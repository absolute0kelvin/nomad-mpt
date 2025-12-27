/**
 * @file nomad_mpt.cpp
 * @brief MonadDB MPT - Pure C API Implementation
 * 
 * This file provides the implementation of the C API defined in nomad_mpt.h.
 * It wraps the C++ MonadDB implementation for use by languages with C FFI.
 */

#include "nomad_mpt.h"

#include <category/mpt/db.hpp>
#include <category/mpt/node.hpp>
#include <category/mpt/state_machine.hpp>
#include <category/mpt/compute.hpp>
#include <category/mpt/config.hpp>
#include <category/mpt/nibbles_view.hpp>
#include <category/mpt/update.hpp>
#include <category/mpt/trie.hpp>
#include <category/mpt/ondisk_db_config.hpp>
#include <category/core/keccak.hpp>
#include <category/core/bytes.hpp>

#include <quill/Quill.h>

#include <cstring>
#include <cstdlib>
#include <memory>
#include <filesystem>

// 使用 monad 命名空间
using namespace monad;

/* ============================================================
 * Global Initialization
 * ============================================================ */

namespace {
    struct QuillInitializer {
        QuillInitializer() {
            quill::Config cfg;
            cfg.enable_console_colours = false;
            quill::configure(cfg);
            quill::start();
        }
    };
    [[maybe_unused]] static QuillInitializer quill_init;
}

/* ============================================================
 * EthereumStateMachine - 以太坊标准状态机
 * ============================================================ */

namespace {

struct LeafDataCompute {
    static byte_string compute(mpt::Node const& node) {
        return byte_string{node.value()};
    }
};

using EthMerkleCompute = mpt::MerkleComputeBase<LeafDataCompute>;

/// EthereumStateMachine - 以太坊兼容状态机
/// 要求使用 32 字节固定长度的 key（以太坊标准）
/// 支持批量更新和嵌套更新（存储 trie）
class EthereumStateMachine final : public mpt::StateMachine {
    static constexpr size_t cache_depth = 8;
    size_t depth_{0};
    mutable EthMerkleCompute compute_;
    
public:
    std::unique_ptr<mpt::StateMachine> clone() const override {
        auto cloned = std::make_unique<EthereumStateMachine>();
        cloned->depth_ = depth_;
        return cloned;
    }
    
    void down(unsigned char) override { ++depth_; }
    void up(size_t n) override { depth_ -= n; }
    
    mpt::Compute& get_compute() const override { return compute_; }
    
    bool cache() const override { return depth_ < cache_depth; }
    bool compact() const override { return true; }
    bool is_variable_length() const override { return false; }  // 固定 32 字节 key
};

} // anonymous namespace

/* ============================================================
 * Internal Types
 * ============================================================ */

struct NomadDb {
    std::unique_ptr<mpt::Db> db;
    std::unique_ptr<mpt::StateMachine> state_machine;
    bool is_on_disk;
    bool is_read_only;
    
    NomadDb() : is_on_disk(false), is_read_only(false) {}
};

struct NomadNode {
    std::shared_ptr<mpt::Node> node;
};

struct NomadFifo {
    NomadDb* db;
    // TODO: 完整实现需要 ck_fifo 队列
};

/* ============================================================
 * Version Info
 * ============================================================ */

const char* nomad_version(void) {
    return "0.1.0";
}

/* ============================================================
 * Memory Helpers
 * ============================================================ */

void nomad_bytes_free(NomadBytes* bytes) {
    if (bytes && bytes->data) {
        std::free(bytes->data);
        bytes->data = nullptr;
        bytes->len = 0;
    }
}

static NomadBytes make_bytes(byte_string_view data) {
    NomadBytes result = {nullptr, 0};
    if (data.empty()) {
        return result;
    }
    
    result.data = static_cast<uint8_t*>(std::malloc(data.size()));
    if (result.data) {
        std::memcpy(result.data, data.data(), data.size());
        result.len = data.size();
    }
    return result;
}

/* ============================================================
 * Database Lifecycle
 * ============================================================ */

NomadError nomad_db_open_memory(NomadDb** db_out) {
    if (!db_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        auto* db = new NomadDb();
        db->state_machine = std::make_unique<EthereumStateMachine>();
        db->db = std::make_unique<mpt::Db>(*db->state_machine);
        db->is_on_disk = false;
        db->is_read_only = false;
        *db_out = db;
        return NOMAD_OK;
    } catch (const std::bad_alloc&) {
        return NOMAD_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

NomadError nomad_db_open_disk(
    const char* path,
    bool create,
    uint64_t history_length,
    NomadDb** db_out
) {
    if (!path || !db_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        mpt::OnDiskDbConfig config;
        config.append = !create;
        config.compaction = true;
        config.dbname_paths.push_back(std::filesystem::path(path));
        config.file_size_db = 4;  // 4GB
        config.sq_thread_cpu = std::nullopt;
        
        if (history_length > 0) {
            config.fixed_history_length = history_length;
        } else {
            config.fixed_history_length = 40;
        }
        
        auto* db = new NomadDb();
        db->state_machine = std::make_unique<EthereumStateMachine>();
        db->db = std::make_unique<mpt::Db>(*db->state_machine, config);
        db->is_on_disk = true;
        db->is_read_only = false;
        *db_out = db;
        return NOMAD_OK;
    } catch (const std::bad_alloc&) {
        return NOMAD_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

void nomad_db_close(NomadDb* db) {
    delete db;
}

bool nomad_db_is_on_disk(const NomadDb* db) {
    if (!db || !db->db) return false;
    return db->is_on_disk;
}

bool nomad_db_is_read_only(const NomadDb* db) {
    if (!db) return true;
    return db->is_read_only;
}

/* ============================================================
 * Synchronous Read Operations
 * ============================================================ */

NomadError nomad_db_find(
    const NomadDb* db,
    const uint8_t* key,
    size_t key_len,
    uint64_t version,
    NomadNode** node_out
) {
    if (!db || !db->db || !node_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        byte_string_view key_bytes{key, key_len};
        mpt::NibblesView key_view{key_bytes};
        
        // 注意：db->find(key, version) 只支持磁盘模式
        // 内存模式下需要使用 find(NodeCursor, key, version)
        if (!db->is_on_disk) {
            // 内存模式：不支持直接按 key 查询
            // 用户需要保持 root 节点引用，使用 nomad_db_find_from_root
            *node_out = nullptr;
            return NOMAD_ERR_NOT_SUPPORTED;
        }
        
        auto result = db->db->find(key_view, version);
        
        if (result.has_error() || !result.value().node) {
            *node_out = nullptr;
            return NOMAD_OK;
        }
        
        auto* node = new NomadNode();
        node->node = result.value().node;
        *node_out = node;
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

NomadError nomad_db_find_from_root(
    const NomadDb* db,
    const NomadNode* root,
    const uint8_t* key,
    size_t key_len,
    uint64_t version,
    NomadNode** node_out
) {
    if (!db || !db->db || !root || !root->node || !node_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        byte_string_view key_bytes{key, key_len};
        mpt::NibblesView key_view{key_bytes};
        
        mpt::NodeCursor cursor{root->node};
        auto result = db->db->find(cursor, key_view, version);
        
        if (result.has_error() || !result.value().node) {
            *node_out = nullptr;
            return NOMAD_OK;
        }
        
        auto* node = new NomadNode();
        node->node = result.value().node;
        *node_out = node;
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

NomadError nomad_node_get_value(const NomadNode* node, NomadBytes* value_out) {
    if (!node || !value_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    if (!node->node || !node->node->has_value()) {
        return NOMAD_ERR_NOT_FOUND;
    }
    
    try {
        auto value = node->node->value();
        *value_out = make_bytes(value);
        if (!value_out->data && !value.empty()) {
            return NOMAD_ERR_OUT_OF_MEMORY;
        }
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

bool nomad_node_has_value(const NomadNode* node) {
    if (!node || !node->node) return false;
    return node->node->has_value();
}

NomadError nomad_node_get_hash(const NomadNode* node, uint8_t* hash_out) {
    if (!node || !hash_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    if (!node->node) {
        return NOMAD_ERR_NOT_FOUND;
    }
    
    try {
        EthMerkleCompute compute;
        unsigned char buffer[532];
        
        unsigned len = compute.compute(buffer, node->node.get());
        
        if (len < KECCAK256_SIZE) {
            keccak256(buffer, len, hash_out);
        } else {
            std::memcpy(hash_out, buffer, KECCAK256_SIZE);
        }
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

NomadNode* nomad_node_clone(const NomadNode* node) {
    if (!node) return nullptr;
    
    try {
        auto* clone = new NomadNode();
        clone->node = node->node;
        return clone;
    } catch (...) {
        return nullptr;
    }
}

void nomad_node_free(NomadNode* node) {
    delete node;
}

/* ============================================================
 * Synchronous Write Operations
 * ============================================================ */

/// UpdateList 构建辅助
class UpdateStorage {
    std::vector<std::unique_ptr<mpt::Update>> updates_;
    
public:
    mpt::Update& add() {
        updates_.push_back(std::make_unique<mpt::Update>());
        return *updates_.back();
    }
};

static mpt::UpdateList build_update_list(
    const NomadUpdate* updates,
    size_t count,
    UpdateStorage& storage
) {
    mpt::UpdateList list;
    
    for (size_t i = count; i > 0; --i) {
        const auto& raw = updates[i - 1];
        auto& update = storage.add();
        
        byte_string_view key_bytes{raw.key, raw.key_len};
        update.key = mpt::NibblesView{key_bytes};
        
        if (raw.type == NOMAD_UPDATE_PUT && raw.value != nullptr) {
            update.value = byte_string_view{raw.value, raw.value_len};
        } else {
            update.value = std::nullopt;
        }
        
        update.version = 0;  // 由外部指定
        
        list.push_front(update);
    }
    
    return list;
}

NomadError nomad_db_upsert(
    NomadDb* db,
    const NomadNode* root,
    const NomadUpdate* updates,
    size_t updates_len,
    uint64_t version,
    NomadNode** new_root_out
) {
    if (!db || !db->db || !new_root_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    if (updates_len > 0 && !updates) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        mpt::Node::SharedPtr root_node;
        if (root && root->node) {
            root_node = root->node;
        }
        
        UpdateStorage storage;
        auto update_list = build_update_list(updates, updates_len, storage);
        
        auto new_root = db->db->upsert(
            root_node,
            std::move(update_list),
            version,
            true,   // enable_compaction
            true,   // can_write_to_fast
            false   // write_root
        );
        
        auto* node = new NomadNode();
        node->node = std::move(new_root);
        *new_root_out = node;
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

/* ============================================================
 * Version Management
 * ============================================================ */

uint64_t nomad_db_get_latest_version(const NomadDb* db) {
    if (!db || !db->db) return 0;
    try {
        if (db->is_on_disk) {
            return db->db->get_latest_version();
        }
        return 0;
    } catch (...) {
        return 0;
    }
}

uint64_t nomad_db_get_earliest_version(const NomadDb* db) {
    if (!db || !db->db) return 0;
    try {
        if (db->is_on_disk) {
            return db->db->get_earliest_version();
        }
        return 0;
    } catch (...) {
        return 0;
    }
}

uint64_t nomad_db_get_history_length(const NomadDb* db) {
    if (!db || !db->db) return 0;
    try {
        return db->db->get_history_length();
    } catch (...) {
        return 0;
    }
}

NomadError nomad_db_load_root(const NomadDb* db, uint64_t version, NomadNode** root_out) {
    if (!db || !db->db || !root_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        auto root = db->db->load_root_for_version(version);
        if (root) {
            auto* node = new NomadNode();
            node->node = std::move(root);
            *root_out = node;
        } else {
            *root_out = nullptr;
        }
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_INTERNAL;
    }
}

bool nomad_db_version_is_valid(const NomadDb* db, uint64_t version) {
    if (!db || !db->db) return false;
    
    if (!db->is_on_disk) {
        return true;
    }
    
    try {
        auto earliest = db->db->get_earliest_version();
        auto latest = db->db->get_latest_version();
        return version >= earliest && version <= latest;
    } catch (...) {
        return false;
    }
}

/* ============================================================
 * Async FIFO Operations
 * ============================================================ */

NomadError nomad_fifo_create(NomadDb* db, NomadFifo** fifo_out) {
    if (!db || !fifo_out) {
        return NOMAD_ERR_NULL_POINTER;
    }
    
    try {
        auto* fifo = new NomadFifo();
        fifo->db = db;
        *fifo_out = fifo;
        return NOMAD_OK;
    } catch (...) {
        return NOMAD_ERR_OUT_OF_MEMORY;
    }
}

void nomad_fifo_start(NomadFifo* fifo, size_t num_workers) {
    (void)fifo;
    (void)num_workers;
    // TODO: 实现 FIFO worker
}

void nomad_fifo_stop(NomadFifo* fifo) {
    (void)fifo;
}

void nomad_fifo_destroy(NomadFifo* fifo) {
    delete fifo;
}

void nomad_fifo_submit_find(
    NomadFifo* fifo,
    const uint8_t* key,
    size_t key_len,
    uint64_t version,
    uint64_t user_data_lo,
    uint64_t user_data_hi
) {
    (void)fifo; (void)key; (void)key_len;
    (void)version; (void)user_data_lo; (void)user_data_hi;
}

void nomad_fifo_submit_traverse(
    NomadFifo* fifo,
    const uint8_t* prefix,
    size_t prefix_len,
    uint64_t version,
    uint32_t limit,
    uint64_t user_data_lo,
    uint64_t user_data_hi
) {
    (void)fifo; (void)prefix; (void)prefix_len;
    (void)version; (void)limit; (void)user_data_lo; (void)user_data_hi;
}

bool nomad_fifo_poll(NomadFifo* fifo, NomadCompletion* completion_out) {
    (void)fifo; (void)completion_out;
    return false;
}

bool nomad_fifo_poll_traverse(NomadFifo* fifo, NomadCompletion* completion_out) {
    (void)fifo; (void)completion_out;
    return false;
}

bool nomad_fifo_poll_large_value(
    NomadFifo* fifo,
    uint64_t* user_data_lo,
    uint64_t* user_data_hi,
    NomadBytes* data_out
) {
    (void)fifo; (void)user_data_lo; (void)user_data_hi; (void)data_out;
    return false;
}
