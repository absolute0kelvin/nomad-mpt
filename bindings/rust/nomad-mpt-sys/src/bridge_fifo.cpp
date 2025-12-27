// MonadDB FFI - Async FIFO Bridge Implementation (Boost.Fiber version)
//
// 架构设计：
// - 使用 MonadDB 的 PriorityPool (Boost.Fiber) 作为 worker
// - 单个 OS 线程 + 多个 fiber，高效处理并发请求
// - Request FIFO: MPMC（多 Rust 线程 submit → 多 fiber dequeue）
// - Completion FIFO: MPMC（多 fiber enqueue → Rust poll，用于 Find 结果）
// - Traverse FIFO: MPMC（多 fiber enqueue → Rust poll，用于 Traverse 结果）
// - LargeValue FIFO: MPMC（多 fiber enqueue → Rust poll，用于 >256B 大值）

#include "bridge_fifo.hpp"
#include "bridge.hpp"

#include <category/mpt/db.hpp>
#include <category/mpt/node.hpp>
#include <category/mpt/nibbles_view.hpp>
#include <category/core/bytes.hpp>

#include <boost/fiber/operations.hpp>

#include <cstdlib>
#include <cstring>

namespace monad::ffi {

// ============================================================
// 内存分配辅助函数
// ============================================================

/// 分配对齐内存（使用 posix_memalign，避免 aligned_alloc 的 size 限制）
/// aligned_alloc 要求 size 是 alignment 的倍数，否则是 UB
/// posix_memalign 没有这个限制
static inline void* aligned_malloc(size_t alignment, size_t size) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return ptr;
}

// ============================================================
// Traverse Machine for FIFO
// ============================================================

class FifoTraverseMachine : public mpt::TraverseMachine {
    FifoManager& mgr_;
    uint64_t lo_;
    uint64_t hi_;
    mpt::Nibbles path_;

public:
    FifoTraverseMachine(FifoManager& mgr, uint64_t lo, uint64_t hi, mpt::NibblesView prefix)
        : mgr_(mgr), lo_(lo), hi_(hi), path_(prefix) {}

    bool down(unsigned char const branch, mpt::Node const& node) override {
        if (MONAD_UNLIKELY(branch == mpt::INVALID_BRANCH)) {
            // Root of traversal
            if (node.has_value()) {
                send_node(node);
            }
            return true;
        }

        path_ = concat(mpt::NibblesView{path_}, branch, node.path_nibble_view());
        if (node.has_value()) {
            send_node(node);
        }
        return true;
    }

    void up(unsigned char const branch, mpt::Node const& node) override {
        auto const path_view = mpt::NibblesView{path_};
        unsigned const prefix_size =
            branch == mpt::INVALID_BRANCH
                ? 0
                : path_view.nibble_size() - node.path_nibbles_len() - 1;
        path_ = path_view.substr(0, prefix_size);
    }

    std::unique_ptr<mpt::TraverseMachine> clone() const override {
        return std::make_unique<FifoTraverseMachine>(*this);
    }

private:
    void send_node(mpt::Node const& node) {
        Completion comp{};
        comp.user_data_lo = lo_;
        comp.user_data_hi = hi_;
        // 使用 STATUS_TRAVERSE_MORE 表示这是遍历中的中间结果
        // 消费者应继续轮询直到收到 STATUS_TRAVERSE_END
        comp.status = STATUS_TRAVERSE_MORE;

        auto value = node.value();
        if (value.size() <= sizeof(comp.value)) {
            comp.value_len = static_cast<uint32_t>(value.size());
            std::memcpy(comp.value, value.data(), value.size());
        } else {
            comp.value_len = 0xFFFFFFFF;
            mgr_.post_large_value(lo_, hi_, (const uint8_t*)value.data(), value.size());
        }

        // 将当前 path (nibbles) 转换为 bytes 存放在 merkle_hash 中
        // 注意：这仅支持最长 32 字节的 key
        size_t nibbles = path_.nibble_size();
        size_t bytes = (nibbles + 1) / 2;
        std::memset(comp.merkle_hash, 0, 32);
        
        if (bytes <= 32) {
            for (size_t i = 0; i < nibbles; ++i) {
                unsigned char nibble = path_.get(static_cast<unsigned>(i));
                if (i % 2 == 0) {
                    comp.merkle_hash[i / 2] = nibble << 4;
                } else {
                    comp.merkle_hash[i / 2] |= nibble & 0x0F;
                }
            }
        } else {
            // Key 超过 32 字节，只复制前 32 字节，并在最后一个字节设置截断标记 0xFF
            // 消费者可以通过检查 merkle_hash[31] == 0xFF 来判断 key 是否被截断
            for (size_t i = 0; i < 64; ++i) {  // 32 bytes = 64 nibbles
                unsigned char nibble = path_.get(static_cast<unsigned>(i));
                if (i % 2 == 0) {
                    comp.merkle_hash[i / 2] = nibble << 4;
                } else {
                    comp.merkle_hash[i / 2] |= nibble & 0x0F;
                }
            }
            comp.merkle_hash[31] = 0xFF;  // 截断标记
        }

        mgr_.post_traverse(std::move(comp));
    }
};

// ============================================================
// FifoManager Implementation (Boost.Fiber version)
// ============================================================

FifoManager::FifoManager(mpt::Db& db) : db_(db) {
    // 使用 MPMC 队列，支持多 fiber 并发 dequeue
    request_fifo_ = ck_fifo_mpmc_wrapper_create();
    completion_fifo_ = ck_fifo_mpmc_wrapper_create();
    traverse_fifo_ = ck_fifo_mpmc_wrapper_create();
    large_value_fifo_ = ck_fifo_mpmc_wrapper_create();
}

FifoManager::~FifoManager() {
    // 确保已停止
    stop();
    
    // 清理 FIFOs
    if (request_fifo_) {
        ck_fifo_mpmc_wrapper_destroy(request_fifo_);
        request_fifo_ = nullptr;
    }
    if (completion_fifo_) {
        ck_fifo_mpmc_wrapper_destroy(completion_fifo_);
        completion_fifo_ = nullptr;
    }
    if (traverse_fifo_) {
        ck_fifo_mpmc_wrapper_destroy(traverse_fifo_);
        traverse_fifo_ = nullptr;
    }
    if (large_value_fifo_) {
        ck_fifo_mpmc_wrapper_destroy(large_value_fifo_);
        large_value_fifo_ = nullptr;
    }
}

void FifoManager::start(size_t num_workers) {
    if (running_.load(std::memory_order_acquire)) {
        return;  // 已经在运行
    }
    
    // 至少 1 个 worker fiber
    if (num_workers == 0) {
        num_workers = 1;
    }
    num_workers_ = num_workers;
    
    running_.store(true, std::memory_order_release);
    
    // 创建 PriorityPool：1 个 OS 线程，num_workers 个 fiber
    // 这样多个 fiber 可以在同一线程上并发处理请求
    // 当一个 fiber 等待 I/O 时，其他 fiber 可以继续执行
    pool_ = std::make_unique<fiber::PriorityPool>(
        1,              // 1 个 OS 线程（可以根据 CPU 核心数调整）
        num_workers     // N 个 fiber
    );
    
    // 启动 worker fibers
    for (size_t i = 0; i < num_workers; ++i) {
        pool_->submit(0, [this] { worker_fiber(); });
    }
}

void FifoManager::stop() {
    // 使用 exchange 确保只执行一次
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;  // 已经停止或从未启动
    }

    // 向 request FIFO 发送 shutdown 请求（每个 fiber 一个）
    // 重要：ck_fifo_mpmc 的 entry 生命周期独立于 value，需要单独分配并在 dequeue 时释放 garbage entry。
    for (size_t i = 0; i < num_workers_; ++i) {
        auto* node = alloc_request();
        if (!node) {
            continue;
        }
        node->req.type = REQ_SHUTDOWN;
        void* entry = ck_fifo_mpmc_wrapper_alloc_entry();
        if (!entry) {
            free_request(node);
            continue;
        }
        ck_fifo_mpmc_wrapper_enqueue(request_fifo_, entry, node);
    }
    
    // PriorityPool 析构时会等待所有 fiber 完成
    // 每个 fiber 要么：
    // - 正在处理请求，完成后检查 running_ 为 false 并退出
    // - 在 yield 后检查 running_ 为 false 并退出
    // - 收到 REQ_SHUTDOWN 直接退出
    pool_.reset();
    num_workers_ = 0;
}

// === 单个操作 ===

RequestNode* FifoManager::alloc_request() {
    auto* node = static_cast<RequestNode*>(aligned_malloc(16, sizeof(RequestNode)));
    if (node) {
        // 初始化为零，避免未定义行为
        std::memset(node, 0, sizeof(RequestNode));
    }
    return node;
}

void FifoManager::free_request(RequestNode* node) {
    std::free(node);
}

void FifoManager::submit(RequestNode* node) {
    // 使用 MPMC enqueue，多 Rust 线程可以并发 submit
    void* entry = ck_fifo_mpmc_wrapper_alloc_entry();
    if (!entry) {
        // entry 分配失败：丢弃请求并释放 node，避免内存泄漏/stop 卡死
        free_request(node);
        return;
    }
    ck_fifo_mpmc_wrapper_enqueue(request_fifo_, entry, node);
}

CompletionNode* FifoManager::poll_completion() {
    void* garbage = nullptr;
    void* value = ck_fifo_mpmc_wrapper_dequeue(completion_fifo_, &garbage);
    if (garbage) {
        ck_fifo_mpmc_wrapper_free_entry(garbage);
    }
    if (value) {
        return reinterpret_cast<CompletionNode*>(value);
    }
    return nullptr;
}

void FifoManager::free_completion(CompletionNode* node) {
    std::free(node);
}

TraverseNode* FifoManager::poll_traverse() {
    void* garbage = nullptr;
    void* value = ck_fifo_mpmc_wrapper_dequeue(traverse_fifo_, &garbage);
    if (garbage) {
        ck_fifo_mpmc_wrapper_free_entry(garbage);
    }
    if (value) {
        return reinterpret_cast<TraverseNode*>(value);
    }
    return nullptr;
}

void FifoManager::free_traverse(TraverseNode* node) {
    std::free(node);
}

LargeValueNode* FifoManager::poll_large_value() {
    void* garbage = nullptr;
    void* value = ck_fifo_mpmc_wrapper_dequeue(large_value_fifo_, &garbage);
    if (garbage) {
        ck_fifo_mpmc_wrapper_free_entry(garbage);
    }
    if (value) {
        return reinterpret_cast<LargeValueNode*>(value);
    }
    return nullptr;
}

void FifoManager::free_large_value(LargeValueNode* node) {
    std::free(node);
}

// === 批量操作 ===

size_t FifoManager::alloc_request_batch(RequestNode** out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        out[i] = alloc_request();
    }
    return count;
}

void FifoManager::submit_batch(RequestNode** nodes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        void* entry = ck_fifo_mpmc_wrapper_alloc_entry();
        if (!entry) {
            // 分配失败则释放对应 request node（否则 Rust 侧不会再管它）
            if (nodes[i]) {
                free_request(nodes[i]);
            }
            continue;  // 分配失败则跳过该节点
        }
        ck_fifo_mpmc_wrapper_enqueue(request_fifo_, entry, nodes[i]);
    }
    // 单次内存屏障确保所有入队操作对 consumer 可见
    ck_wrapper_fence_store();
}

size_t FifoManager::poll_completion_batch(CompletionNode** out, size_t max_count) {
    size_t count = 0;
    
    while (count < max_count) {
        void* garbage = nullptr;
        void* value = ck_fifo_mpmc_wrapper_dequeue(completion_fifo_, &garbage);
        if (garbage) {
            ck_fifo_mpmc_wrapper_free_entry(garbage);
        }
        if (!value) break;
        out[count++] = reinterpret_cast<CompletionNode*>(value);
    }
    
    return count;
}

void FifoManager::free_completion_batch(CompletionNode** nodes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        std::free(nodes[i]);
    }
}

size_t FifoManager::poll_traverse_batch(TraverseNode** out, size_t max_count) {
    size_t count = 0;
    
    while (count < max_count) {
        void* garbage = nullptr;
        void* value = ck_fifo_mpmc_wrapper_dequeue(traverse_fifo_, &garbage);
        if (garbage) {
            ck_fifo_mpmc_wrapper_free_entry(garbage);
        }
        if (!value) break;
        out[count++] = reinterpret_cast<TraverseNode*>(value);
    }
    
    return count;
}

void FifoManager::free_traverse_batch(TraverseNode** nodes, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        std::free(nodes[i]);
    }
}

// === Fiber Worker 实现 ===

void FifoManager::worker_fiber() {
    // 注意：循环中同时检查 running_ 和 REQ_SHUTDOWN，双重保证退出：
    // 1. running_ 为 false 时主动退出（防止 shutdown 消息发送失败导致死锁）
    // 2. 收到 REQ_SHUTDOWN 时立即退出（正常优雅关闭路径）
    while (running_.load(std::memory_order_acquire)) {
        void* garbage = nullptr;
        void* value = ck_fifo_mpmc_wrapper_dequeue(request_fifo_, &garbage);
        if (garbage) {
            ck_fifo_mpmc_wrapper_free_entry(garbage);
        }

        if (!value) {
            // 队列空，yield 让其他 fiber 运行
            // yield 后会再次检查 running_，避免 shutdown 消息丢失时死锁
            boost::this_fiber::yield();
            continue;
        }

        auto* node = reinterpret_cast<RequestNode*>(value);

        switch (node->req.type) {
            case REQ_FIND_VALUE:
            case REQ_FIND_NODE:
                process_find(node->req);
                break;
            case REQ_TRAVERSE:
                process_traverse(node->req);
                break;
            case REQ_SHUTDOWN:
                free_request(node);
                return;  // 退出这个 fiber
        }

        free_request(node);
    }
}

void FifoManager::process_find(const Request& req) {
    Completion comp{};
    comp.user_data_lo = req.user_data_lo;
    comp.user_data_hi = req.user_data_hi;
    
    try {
        // 将 key bytes 转换为 NibblesView
        byte_string_view key_bytes{req.key, req.key_len};
        mpt::NibblesView key_view{key_bytes};
        
        // 调用 db_.find()
        // 在磁盘模式下，这会内部使用 fiber 进行异步 I/O
        // 当 I/O 等待时，当前 fiber 会 yield，让其他 fiber 运行
        auto result = db_.find(key_view, req.version);
        
        if (result.has_error()) {
            comp.status = STATUS_ERROR;
        } else {
            auto& cursor = result.value();
            if (!cursor.node || !cursor.node->has_value()) {
                comp.status = STATUS_NOT_FOUND;
            } else {
                comp.status = STATUS_OK;
                
                // 获取 value
                auto value_data = cursor.node->value_data();
                size_t value_len = cursor.node->value_len;
                
                if (value_len <= sizeof(comp.value)) {
                    comp.value_len = static_cast<uint32_t>(value_len);
                    std::memcpy(comp.value, value_data, value_len);
                } else {
                    // 大值通过 large_value_fifo 传递
                    comp.value_len = 0xFFFFFFFF;
                    post_large_value(req.user_data_lo, req.user_data_hi, value_data, value_len);
                }
                
                // 如果是 FIND_NODE，复制 Merkle hash
                if (req.type == REQ_FIND_NODE) {
                    auto data_ptr = cursor.node->data_data();
                    size_t data_len = cursor.node->bitpacked.data_len;
                    if (data_len == 32) {
                        std::memcpy(comp.merkle_hash, data_ptr, 32);
                    }
                }
            }
        }
    } catch (...) {
        comp.status = STATUS_ERROR;
    }
    
    post_completion(std::move(comp));
}

void FifoManager::process_traverse(const Request& req) {
    try {
        // 1. 找到起始前缀对应的节点
        byte_string_view prefix_bytes{req.key, req.key_len};
        mpt::NibblesView prefix_view{prefix_bytes};
        
        auto result = db_.find(prefix_view, req.version);
        
        if (!result.has_error() && result.value().node) {
            // 2. 从该节点开始遍历
            FifoTraverseMachine machine(*this, req.user_data_lo, req.user_data_hi, prefix_view);
            db_.traverse(result.value(), machine, req.version, req.traverse_limit > 0 ? req.traverse_limit : 4096);
        }
    } catch (...) {
        // 忽略错误，至少发送 END
    }

    // 发送结束标志
    Completion comp{};
    comp.user_data_lo = req.user_data_lo;
    comp.user_data_hi = req.user_data_hi;
    comp.status = STATUS_TRAVERSE_END;
    comp.value_len = 0;
    post_traverse(std::move(comp));
}

void FifoManager::post_completion(Completion&& comp) {
    auto* node = static_cast<CompletionNode*>(aligned_malloc(16, sizeof(CompletionNode)));
    if (!node) return;  // 分配失败时静默失败（生产环境应记录日志）
    node->comp = std::move(comp);
    void* entry = ck_fifo_mpmc_wrapper_alloc_entry();
    if (!entry) {
        std::free(node);
        return;
    }
    ck_fifo_mpmc_wrapper_enqueue(completion_fifo_, entry, node);
}

void FifoManager::post_traverse(Completion&& comp) {
    auto* node = static_cast<TraverseNode*>(aligned_malloc(16, sizeof(TraverseNode)));
    if (!node) return;
    node->comp = std::move(comp);
    void* entry = ck_fifo_mpmc_wrapper_alloc_entry();
    if (!entry) {
        std::free(node);
        return;
    }
    ck_fifo_mpmc_wrapper_enqueue(traverse_fifo_, entry, node);
}

void FifoManager::post_large_value(uint64_t user_data_lo, uint64_t user_data_hi, const uint8_t* data, size_t len) {
    // 防止 len 超过 uint32_t 范围时溢出截断
    // 虽然 MPT value 实际上不太可能超过 4GB，但为了健壮性仍需检查
    if (len > UINT32_MAX) {
        // 超大值无法正确表示，静默丢弃
        // 生产环境应记录日志
        return;
    }
    
    auto* node = static_cast<LargeValueNode*>(aligned_malloc(16, sizeof(LargeValueNode) + len));
    if (!node) return;
    node->user_data_lo = user_data_lo;
    node->user_data_hi = user_data_hi;
    node->len = static_cast<uint32_t>(len);
    std::memcpy(node->data, data, len);
    void* entry = ck_fifo_mpmc_wrapper_alloc_entry();
    if (!entry) {
        std::free(node);
        return;
    }
    ck_fifo_mpmc_wrapper_enqueue(large_value_fifo_, entry, node);
}

// ============================================================
// FFI 实现
// ============================================================

extern "C" {

FifoManager* fifo_create(DbHandle* db) {
    if (!db) return nullptr;
    return new FifoManager(db->get());
}

void fifo_destroy(FifoManager* mgr) {
    delete mgr;
}

void fifo_start(FifoManager* mgr, size_t num_workers) {
    if (mgr) mgr->start(num_workers);
}

void fifo_stop(FifoManager* mgr) {
    if (mgr) mgr->stop();
}

// === 单个操作 ===

RequestNode* fifo_alloc_request(FifoManager* mgr) {
    return mgr ? mgr->alloc_request() : nullptr;
}

void fifo_free_request(FifoManager* mgr, RequestNode* node) {
    if (mgr && node) mgr->free_request(node);
}

void fifo_submit(FifoManager* mgr, RequestNode* node) {
    if (mgr && node) mgr->submit(node);
}

CompletionNode* fifo_poll_completion(FifoManager* mgr) {
    return mgr ? mgr->poll_completion() : nullptr;
}

void fifo_free_completion(FifoManager* mgr, CompletionNode* node) {
    if (mgr && node) mgr->free_completion(node);
}

TraverseNode* fifo_poll_traverse(FifoManager* mgr) {
    return mgr ? mgr->poll_traverse() : nullptr;
}

void fifo_free_traverse(FifoManager* mgr, TraverseNode* node) {
    if (mgr && node) mgr->free_traverse(node);
}

LargeValueNode* fifo_poll_large_value(FifoManager* mgr) {
    return mgr ? mgr->poll_large_value() : nullptr;
}

void fifo_free_large_value(FifoManager* mgr, LargeValueNode* node) {
    if (mgr && node) mgr->free_large_value(node);
}

// === 批量操作 ===

size_t fifo_alloc_request_batch(FifoManager* mgr, RequestNode** out, size_t count) {
    return mgr ? mgr->alloc_request_batch(out, count) : 0;
}

void fifo_submit_batch(FifoManager* mgr, RequestNode** nodes, size_t count) {
    if (mgr) mgr->submit_batch(nodes, count);
}

size_t fifo_poll_completion_batch(FifoManager* mgr, CompletionNode** out, size_t max_count) {
    return mgr ? mgr->poll_completion_batch(out, max_count) : 0;
}

void fifo_free_completion_batch(FifoManager* mgr, CompletionNode** nodes, size_t count) {
    if (mgr) mgr->free_completion_batch(nodes, count);
}

size_t fifo_poll_traverse_batch(FifoManager* mgr, TraverseNode** out, size_t max_count) {
    return mgr ? mgr->poll_traverse_batch(out, max_count) : 0;
}

void fifo_free_traverse_batch(FifoManager* mgr, TraverseNode** nodes, size_t count) {
    if (mgr) mgr->free_traverse_batch(nodes, count);
}

} // extern "C"

} // namespace monad::ffi
