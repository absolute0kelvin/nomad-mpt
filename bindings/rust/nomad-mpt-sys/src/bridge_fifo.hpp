// MonadDB FFI - Async FIFO Bridge (using Concurrency Kit + Boost.Fiber)
//
// 提供 Rust/Go 与 C++ MonadDB 之间的异步通信通道
//
// 架构说明：
// - 使用 MonadDB 的 PriorityPool (Boost.Fiber) 作为 worker
// - 单个 OS 线程 + 多个 fiber，高效处理并发请求
// - Request FIFO: MPMC 队列（多 Rust 线程 submit → 多 fiber dequeue）
// - Completion FIFO: MPMC 队列（多 fiber enqueue → Rust poll，用于 Find 结果）
// - Traverse FIFO: MPMC 队列（多 fiber enqueue → Rust poll，用于 Traverse 结果）
// - Large Value FIFO: MPMC 队列（多 fiber enqueue → Rust poll，用于 >256B 大值）

#pragma once

#include "ck_wrapper.h"  // C wrapper for ck_fifo

#include <category/core/fiber/priority_pool.hpp>

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>

// Forward declaration
namespace monad::mpt {
    class Db;
}

namespace monad::ffi {

class DbHandle;

// ============================================================
// 请求/响应数据结构
// ============================================================

/// 请求类型
enum RequestType : uint8_t {
    REQ_FIND_VALUE = 1,     // 获取 value
    REQ_FIND_NODE = 2,      // 获取 Node（含 Merkle hash）
    REQ_TRAVERSE = 3,       // 遍历子树
    REQ_SHUTDOWN = 255,     // 关闭 worker
};

/// 请求数据
/// user_data 按小端打包：lo = user_data & 0xffffffffffffffff, hi = user_data >> 64
struct alignas(8) Request {
    uint64_t user_data_lo;  // 小端低 64 位
    uint64_t user_data_hi;  // 小端高 64 位
    uint64_t version;       // block_id / version
    RequestType type;       // 请求类型
    uint8_t key_len;        // key 长度 (字节)
    uint8_t _pad[2];
    uint32_t traverse_limit;// traverse 最大返回数量
    uint8_t key[32];        // key 数据
};
static_assert(sizeof(Request) == 64, "Request size mismatch");

/// 请求节点（包含 FIFO entry）
/// 现在 request FIFO 使用 MPMC，entry 需要使用 MPMC entry 尺寸（24 字节）
struct RequestNode {
    alignas(16) uint8_t entry[24];  // ck_fifo_mpmc_entry_t
    Request req;
};

/// 响应状态
enum ResultStatus : uint8_t {
    STATUS_OK = 0,
    STATUS_NOT_FOUND = 1,
    STATUS_ERROR = 2,
    STATUS_TRAVERSE_MORE = 3,   // traverse 还有更多结果
    STATUS_TRAVERSE_END = 4,    // traverse 结束
};

/// 完成数据
struct alignas(8) Completion {
    uint64_t user_data_lo;  // 小端低 64 位
    uint64_t user_data_hi;  // 小端高 64 位
    ResultStatus status;    // 结果状态
    uint8_t _pad[3];
    uint32_t value_len;     // value 长度 (0xFFFFFFFF = 大值)
    uint8_t value[256];     // 内联小值
    uint8_t merkle_hash[32];// node.data()
};
static_assert(sizeof(Completion) == 312, "Completion size mismatch");

/// 完成节点（使用 MPMC entry，24 字节）
struct CompletionNode {
    alignas(16) uint8_t entry[24];  // ck_fifo_mpmc_entry_t (larger than SPSC)
    Completion comp;
};

/// Traverse 节点（复用 Completion 结构，但来自独立队列）
typedef CompletionNode TraverseNode;

/// 大值节点（用于 >256 字节的值）
struct LargeValueNode {
    alignas(16) uint8_t entry[24];  // ck_fifo_mpmc_entry_t
    uint64_t user_data_lo;
    uint64_t user_data_hi;
    uint32_t len;
    uint8_t data[];  // 柔性数组
};

// ============================================================
// FifoManager - 异步 FIFO 管理器
// ============================================================

// Forward declaration
class FifoTraverseMachine;

class FifoManager {
    // 允许 TraverseMachine 访问私有方法
    friend class FifoTraverseMachine;
    
public:
    explicit FifoManager(mpt::Db& db);
    ~FifoManager();
    
    // 禁止拷贝
    FifoManager(const FifoManager&) = delete;
    FifoManager& operator=(const FifoManager&) = delete;
    
    // 启动/停止 worker 线程
    void start(size_t num_workers = 1);
    void stop();
    
    // === 单个操作 ===
    RequestNode* alloc_request();
    void free_request(RequestNode* node);
    void submit(RequestNode* node);
    
    CompletionNode* poll_completion();
    void free_completion(CompletionNode* node);
    
    TraverseNode* poll_traverse();
    void free_traverse(TraverseNode* node);
    
    LargeValueNode* poll_large_value();
    void free_large_value(LargeValueNode* node);
    
    // === 批量操作 ===
    size_t alloc_request_batch(RequestNode** out, size_t count);
    void submit_batch(RequestNode** nodes, size_t count);
    size_t poll_completion_batch(CompletionNode** out, size_t max_count);
    void free_completion_batch(CompletionNode** nodes, size_t count);
    size_t poll_traverse_batch(TraverseNode** out, size_t max_count);
    void free_traverse_batch(TraverseNode** nodes, size_t count);
    
private:
    void worker_fiber();
    void process_find(const Request& req);
    void process_traverse(const Request& req);
    void post_completion(Completion&& comp);
    void post_traverse(Completion&& comp);
    void post_large_value(uint64_t user_data_lo, uint64_t user_data_hi, const uint8_t* data, size_t len);
    
    mpt::Db& db_;
    
    // === Request FIFO (MPMC) ===
    // 多 Rust 线程 submit → 多 fiber dequeue
    ck_fifo_mpmc_wrapper_t* request_fifo_;
    
    // === Response FIFOs (MPMC) ===
    // 多 fiber 写入，单 Rust 消费者读取
    ck_fifo_mpmc_wrapper_t* completion_fifo_;  // C++ → Rust/Go (Find 小值)
    ck_fifo_mpmc_wrapper_t* traverse_fifo_;    // C++ → Rust/Go (Traverse 结果)
    ck_fifo_mpmc_wrapper_t* large_value_fifo_; // C++ → Rust/Go (大值)
    
    // Fiber pool（单线程 + 多 fiber）
    std::unique_ptr<fiber::PriorityPool> pool_;
    
    std::atomic<bool> running_{false};
    size_t num_workers_{0};
};

// ============================================================
// FFI 接口
// ============================================================

extern "C" {
    // 创建/销毁
    FifoManager* fifo_create(DbHandle* db);
    void fifo_destroy(FifoManager* mgr);
    
    // 启动/停止
    void fifo_start(FifoManager* mgr, size_t num_workers);
    void fifo_stop(FifoManager* mgr);
    
    // === 单个操作 ===
    RequestNode* fifo_alloc_request(FifoManager* mgr);
    void fifo_free_request(FifoManager* mgr, RequestNode* node);
    void fifo_submit(FifoManager* mgr, RequestNode* node);
    
    CompletionNode* fifo_poll_completion(FifoManager* mgr);
    void fifo_free_completion(FifoManager* mgr, CompletionNode* node);
    
    TraverseNode* fifo_poll_traverse(FifoManager* mgr);
    void fifo_free_traverse(FifoManager* mgr, TraverseNode* node);
    
    LargeValueNode* fifo_poll_large_value(FifoManager* mgr);
    void fifo_free_large_value(FifoManager* mgr, LargeValueNode* node);
    
    // === 批量操作 ===
    size_t fifo_alloc_request_batch(FifoManager* mgr, RequestNode** out, size_t count);
    void fifo_submit_batch(FifoManager* mgr, RequestNode** nodes, size_t count);
    size_t fifo_poll_completion_batch(FifoManager* mgr, CompletionNode** out, size_t max_count);
    void fifo_free_completion_batch(FifoManager* mgr, CompletionNode** nodes, size_t count);
    size_t fifo_poll_traverse_batch(FifoManager* mgr, TraverseNode** out, size_t max_count);
    void fifo_free_traverse_batch(FifoManager* mgr, TraverseNode** nodes, size_t count);
}

} // namespace monad::ffi

