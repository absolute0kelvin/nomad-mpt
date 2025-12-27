// MonadDB FFI Bridge Implementation

#include "bridge.hpp"
#include "nomad-mpt-sys/src/lib.rs.h"

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

#include <stdexcept>
#include <cstring>
#include <vector>
#include <filesystem>

namespace monad::ffi {

// ============================================================
// 全局初始化
// ============================================================
namespace {
    struct QuillInitializer {
        QuillInitializer() {
            // 配置 quill 日志到 stderr
            quill::Config cfg;
            cfg.enable_console_colours = false;  // 禁用颜色
            quill::configure(cfg);
            quill::start();  // 启动后台日志线程
        }
    };
    
    // 全局初始化器 - 在任何 FFI 调用之前自动执行
    [[maybe_unused]] static QuillInitializer quill_init;
}

// ============================================================
// EthereumStateMachine - 预设的以太坊状态机
// ============================================================

/// 叶子数据计算器 - 以太坊风格
/// 叶子数据 = value + hash (如果有子节点)
struct LeafDataCompute {
    static byte_string compute(mpt::Node const& node) {
        // 返回叶子节点的值
        return byte_string{node.value()};
    }
};

/// 以太坊标准 Merkle Compute (Keccak256)
using EthMerkleCompute = mpt::MerkleComputeBase<LeafDataCompute>;

/// 以太坊标准 StateMachine
class EthereumStateMachine final : public mpt::StateMachine {
    static constexpr size_t cache_depth = 8;   // 缓存前 8 层
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
    bool is_variable_length() const override { return false; }
};

// ============================================================
// DbHandle Implementation
// ============================================================

DbHandle::DbHandle(std::unique_ptr<mpt::Db> db, std::unique_ptr<mpt::StateMachine> sm)
    : db_(std::move(db))
    , state_machine_(std::move(sm))
{}

DbHandle::~DbHandle() = default;

DbHandle::DbHandle(DbHandle&&) noexcept = default;
DbHandle& DbHandle::operator=(DbHandle&&) noexcept = default;

// ============================================================
// NodeHandle Implementation
// ============================================================

NodeHandle::NodeHandle(std::shared_ptr<mpt::Node> p)
    : ptr_(std::move(p))
{}

NodeHandle::~NodeHandle() = default;

NodeHandle::NodeHandle(const NodeHandle& other)
    : ptr_(other.ptr_)
{}

NodeHandle& NodeHandle::operator=(const NodeHandle& other) {
    ptr_ = other.ptr_;
    return *this;
}

// ============================================================
// FFI Functions - Db 生命周期
// ============================================================

std::unique_ptr<DbHandle> db_open_memory() {
    auto state_machine = std::make_unique<EthereumStateMachine>();
    auto db = std::make_unique<mpt::Db>(*state_machine);
    
    return std::make_unique<DbHandle>(std::move(db), std::move(state_machine));
}

std::unique_ptr<DbHandle> db_open_disk_rw(
    rust::Str db_path,
    bool create,
    uint64_t history_length
) {
    // 构建配置
    mpt::OnDiskDbConfig config;
    config.append = !create;  // append=false 表示创建新数据库
    config.compaction = true;
    config.dbname_paths.push_back(std::filesystem::path(std::string(db_path)));
    
    // 设置文件大小（GB），默认 4GB 用于测试，生产环境可能需要更大
    config.file_size_db = 4;  // 4GB
    
    // 禁用 SQPOLL 内核线程（需要 root 权限）
    config.sq_thread_cpu = std::nullopt;
    
    // 设置历史长度
    if (history_length > 0) {
        config.fixed_history_length = history_length;
    } else {
        config.fixed_history_length = 40;  // 默认保留 40 个版本
    }
    
    // 创建状态机和数据库
    auto state_machine = std::make_unique<EthereumStateMachine>();
    auto db = std::make_unique<mpt::Db>(*state_machine, config);
    
    return std::make_unique<DbHandle>(std::move(db), std::move(state_machine));
}

// 注意: db_open_disk_ro 已移除，Rust 侧会直接返回错误

void db_close(std::unique_ptr<DbHandle> db) {
    // UniquePtr 会自动调用析构函数
    // 显式释放确保析构顺序正确
    db.reset();
}

bool db_is_on_disk(const DbHandle& db) {
    return db.get().is_on_disk();
}

// ============================================================
// UpdateList 构建辅助函数
// ============================================================

/// 存储 Update 对象的容器，保持生命周期
/// 
/// UpdateList 使用侵入式链表，需要 Update 对象在整个操作期间保持有效
class UpdateStorage {
    std::vector<std::unique_ptr<mpt::Update>> updates_;
    
public:
    /// 添加一个 Update 并返回引用
    mpt::Update& add() {
        updates_.push_back(std::make_unique<mpt::Update>());
        return *updates_.back();
    }
    
    /// 清空所有 Update
    void clear() {
        updates_.clear();
    }
};

/// 递归构建 UpdateList
/// 
/// @param raw_updates 原始更新数组
/// @param count 数组长度
/// @param storage 存储容器（保持 Update 对象的生命周期）
/// @return UpdateList
static mpt::UpdateList build_update_list(
    const RawUpdate* raw_updates,
    size_t count,
    UpdateStorage& storage
) {
    mpt::UpdateList list;
    
    // 逆序遍历，因为 push_front 会反转顺序
    for (size_t i = count; i > 0; --i) {
        const auto& raw = raw_updates[i - 1];
        
        // 创建 Update 对象
        auto& update = storage.add();
        
        // 设置 key (转换为 NibblesView)
        // key_ptr 指向字节数组，需要转换为 nibbles
        byte_string_view key_bytes{raw.key_ptr, raw.key_len};
        update.key = mpt::NibblesView{key_bytes};
        
        // 设置 value
        if (raw.value_ptr != nullptr) {
            update.value = byte_string_view{raw.value_ptr, raw.value_len};
        } else {
            update.value = std::nullopt;  // 删除操作
        }
        
        // 设置 version
        update.version = raw.version;
        
        // 递归处理嵌套更新
        if (raw.nested_ptr != nullptr && raw.nested_len > 0) {
            update.next = build_update_list(raw.nested_ptr, raw.nested_len, storage);
        }
        
        // 添加到链表
        list.push_front(update);
    }
    
    return list;
}

// ============================================================
// FFI Functions - 同步读写
// ============================================================

std::unique_ptr<NodeHandle> db_find(
    const DbHandle& db,
    rust::Slice<const uint8_t> key,
    uint64_t version
) {
    // 将 key 转换为 byte_string_view，然后隐式转换为 NibblesView
    byte_string_view key_view{key.data(), key.size()};
    
    auto result = db.get().find(mpt::NibblesView{key_view}, version);
    
    if (result.has_error()) {
        // 返回空节点表示未找到
        return std::make_unique<NodeHandle>(nullptr);
    }
    
    // 从 NodeCursor 获取 Node
    auto& cursor = result.value();
    return std::make_unique<NodeHandle>(cursor.node);
}

std::unique_ptr<NodeHandle> db_upsert(
    DbHandle& db,
    const NodeHandle* root,
    const RawUpdate* updates,
    size_t updates_len,
    uint64_t version
) {
    // 获取根节点
    mpt::Node::SharedPtr root_node;
    if (root != nullptr && root->is_valid()) {
        root_node = root->get();
    }
    
    // 构建 UpdateList
    UpdateStorage storage;
    auto update_list = build_update_list(updates, updates_len, storage);
    
    // 执行 upsert
    auto new_root = db.get().upsert(
        root_node,
        std::move(update_list),
        version,
        true,   // enable_compaction
        true,   // can_write_to_fast
        false   // write_root (内存模式不写入)
    );
    
    return std::make_unique<NodeHandle>(std::move(new_root));
}

// ============================================================
// FFI Functions - 元数据
// ============================================================

uint64_t db_get_latest_version(const DbHandle& db) {
    return db.get().get_latest_version();
}

uint64_t db_get_earliest_version(const DbHandle& db) {
    return db.get().get_earliest_version();
}

std::unique_ptr<NodeHandle> db_load_root_for_version(
    const DbHandle& db,
    uint64_t version
) {
    auto root = db.get().load_root_for_version(version);
    return std::make_unique<NodeHandle>(std::move(root));
}

uint64_t db_get_history_length(const DbHandle& db) {
    return db.get().get_history_length();
}

void db_update_finalized_version(DbHandle& db, uint64_t version) {
    if (!db.get().is_on_disk()) {
        throw std::runtime_error("update_finalized_version only supported in on-disk mode");
    }
    db.get().update_finalized_version(version);
}

uint64_t db_get_finalized_version(const DbHandle& db) {
    return db.get().get_latest_finalized_version();
}

// ============================================================
// FFI Functions - Rollback & Prune
// ============================================================

void db_rewind_to_version(DbHandle& db, uint64_t version) {
    // 只有磁盘模式支持 rewind
    if (!db.get().is_on_disk()) {
        throw std::runtime_error("rewind_to_version only supported in on-disk mode");
    }
    
    // 验证版本有效性
    auto earliest = db.get().get_earliest_version();
    auto latest = db.get().get_latest_version();
    if (version < earliest || version > latest) {
        throw std::runtime_error("version out of range");
    }
    
    // 使用 update_finalized_version 更新 finalized 版本
    // 这会触发旧数据的 prune（当超出 history_length 时）
    // 
    // 注意：完整的 rewind（丢弃 version 之后的数据）需要访问 
    // UpdateAux::rewind_to_version，但 Db 类没有公开此方法。
    // 用户可以通过 CLI 工具 `monad_mpt --rewind-to <version>` 实现完整 rollback。
    db.get().update_finalized_version(version);
}

bool db_version_is_valid(const DbHandle& db, uint64_t version) {
    if (!db.get().is_on_disk()) {
        return true;  // 内存模式所有版本都有效
    }
    
    auto earliest = db.get().get_earliest_version();
    auto latest = db.get().get_latest_version();
    return version >= earliest && version <= latest;
}

// 注意: db_clear 已移除，需要使用 CLI 工具

// ============================================================
// FFI Functions - Node 操作
// ============================================================

std::unique_ptr<NodeHandle> node_clone(const NodeHandle& node) {
    return std::make_unique<NodeHandle>(node.get());
}

bool node_has_value(const NodeHandle& node) {
    if (!node.is_valid()) return false;
    return node.get()->has_value();
}

size_t node_value_len(const NodeHandle& node) {
    if (!node.is_valid()) return 0;
    return node.get()->value_len;
}

size_t node_copy_value(const NodeHandle& node, rust::Slice<uint8_t> out) {
    if (!node.is_valid()) return 0;
    
    auto* n = node.get().get();
    size_t len = std::min(static_cast<size_t>(n->value_len), out.size());
    
    if (len > 0) {
        std::memcpy(out.data(), n->value_data(), len);
    }
    
    return len;
}

size_t node_data_len(const NodeHandle& node) {
    if (!node.is_valid()) return 0;
    return node.get()->bitpacked.data_len;
}

size_t node_copy_data(const NodeHandle& node, rust::Slice<uint8_t> out) {
    if (!node.is_valid()) return 0;
    
    auto* n = node.get().get();
    size_t len = std::min(static_cast<size_t>(n->bitpacked.data_len), out.size());
    
    if (len > 0) {
        std::memcpy(out.data(), n->data_data(), len);
    }
    
    return len;
}

size_t node_compute_root_hash(const NodeHandle& node, rust::Slice<uint8_t> out) {
    if (!node.is_valid()) return 0;
    if (out.size() < KECCAK256_SIZE) return 0;
    
    // 使用以太坊标准的 MerkleCompute
    EthMerkleCompute compute;
    
    // 创建临时缓冲区存储 Merkle 数据
    // 最大可能长度：branch RLP = 532 字节
    unsigned char buffer[532];
    
    // 计算节点的 Merkle 数据
    auto* n = node.get().get();
    unsigned len = compute.compute(buffer, n);
    
    // =========================================================
    // 以太坊 MPT Merkle 哈希规则：
    // 
    // 1. 当 RLP 编码长度 < 32 字节时：
    //    - 数据直接内联存储在父节点中（不做哈希）
    //    - 但作为 root hash，我们需要对其做 Keccak256
    //    
    // 2. 当 RLP 编码长度 >= 32 字节时：
    //    - MerkleCompute::compute() 返回的前 32 字节已经是 Keccak256 哈希
    //    - 因为长数据会被哈希后再存储到父节点
    //
    // 参考：Ethereum Yellow Paper, Appendix D (Modified Merkle Patricia Trie)
    // =========================================================
    if (len < KECCAK256_SIZE) {
        // 短数据：计算 Keccak256 得到 root hash
        keccak256(buffer, len, out.data());
    } else {
        // 长数据：前 32 字节已经是哈希值
        std::memcpy(out.data(), buffer, KECCAK256_SIZE);
    }
    
    return KECCAK256_SIZE;
}

// ============================================================
// FFI Functions - 性能优化
// ============================================================

size_t db_prefetch(DbHandle& db, const NodeHandle& root) {
    if (!db.get().is_on_disk()) {
        return 0;  // 内存模式不需要预加载
    }
    
    if (db.get().is_read_only()) {
        return 0;  // 只读模式不支持 prefetch
    }
    
    if (!root.is_valid()) {
        return 0;
    }
    
    return db.get().prefetch(root.get());
}

bool db_is_read_only(const DbHandle& db) {
    return db.get().is_read_only();
}

void db_get_stats(
    const DbHandle& db,
    uint64_t& latest_version,
    uint64_t& earliest_version,
    uint64_t& history_length,
    bool& is_on_disk,
    bool& is_read_only,
    uint64_t& finalized_version
) {
    is_on_disk = db.get().is_on_disk();
    is_read_only = db.get().is_read_only();
    history_length = db.get().get_history_length();
    
    if (is_on_disk) {
        latest_version = db.get().get_latest_version();
        earliest_version = db.get().get_earliest_version();
        finalized_version = db.get().get_latest_finalized_version();
    } else {
        // 内存模式: 版本号从 trie 推断
        // get_latest_version 和 get_earliest_version 在内存模式下会断言失败
        // 我们只能返回默认值
        latest_version = 0;  // 将由 Rust 侧通过其他方式获取
        earliest_version = 0;
        finalized_version = UINT64_MAX;  // 表示未设置
    }
}

} // namespace monad::ffi
