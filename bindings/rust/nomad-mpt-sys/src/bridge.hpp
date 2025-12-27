// MonadDB FFI Bridge
// C++ 侧的类型定义和函数声明

#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

#include "rust/cxx.h"

// Forward declarations from MonadDB
namespace monad::mpt {
    class Db;
    struct Node;
    struct StateMachine;
}

namespace monad::ffi {

// ============================================================
// Opaque Handle Types
// ============================================================

/// 数据库句柄，包装 Db 实例和相关状态
class DbHandle {
    std::unique_ptr<mpt::Db> db_;
    std::unique_ptr<mpt::StateMachine> state_machine_;
    
public:
    DbHandle(std::unique_ptr<mpt::Db> db, std::unique_ptr<mpt::StateMachine> sm);
    ~DbHandle();
    
    // 禁止拷贝
    DbHandle(const DbHandle&) = delete;
    DbHandle& operator=(const DbHandle&) = delete;
    
    // 允许移动
    DbHandle(DbHandle&&) noexcept;
    DbHandle& operator=(DbHandle&&) noexcept;
    
    mpt::Db& get() { return *db_; }
    const mpt::Db& get() const { return *db_; }
};

/// 节点句柄，包装 shared_ptr<Node>
class NodeHandle {
    std::shared_ptr<mpt::Node> ptr_;
    
public:
    explicit NodeHandle(std::shared_ptr<mpt::Node> p);
    ~NodeHandle();
    
    // 允许拷贝（shared_ptr）
    NodeHandle(const NodeHandle&);
    NodeHandle& operator=(const NodeHandle&);
    
    std::shared_ptr<mpt::Node>& get() { return ptr_; }
    const std::shared_ptr<mpt::Node>& get() const { return ptr_; }
    
    bool is_valid() const { return ptr_ != nullptr; }
};

// ============================================================
// FFI Functions - Db 生命周期
// ============================================================

/// 打开内存数据库
std::unique_ptr<DbHandle> db_open_memory();

/// 打开磁盘数据库（读写模式）
/// 
/// - db_path: 数据库文件路径
/// - create: 如果不存在是否创建
/// - history_length: 历史版本保留数量（0 表示自动）
std::unique_ptr<DbHandle> db_open_disk_rw(
    rust::Str db_path,
    bool create,
    uint64_t history_length
);

// 注意: db_open_disk_ro (只读磁盘模式) 尚未实现，需要 RODb 支持

/// 关闭数据库（确保数据持久化）
void db_close(std::unique_ptr<DbHandle> db);

/// 检查数据库是否是磁盘模式
bool db_is_on_disk(const DbHandle& db);

// ============================================================
// 共享类型（由 cxx 从 Rust 侧生成）
// ============================================================

// RawUpdate 由 cxx 自动生成，不需要手动定义
// 前向声明以便在函数签名中使用
struct RawUpdate;

// ============================================================
// FFI Functions - 同步读写
// ============================================================

/// 查找 key 对应的节点
/// 返回: NodeHandle，如果不存在返回空的 NodeHandle
std::unique_ptr<NodeHandle> db_find(
    const DbHandle& db,
    rust::Slice<const uint8_t> key,
    uint64_t version
);

/// 批量更新
/// 
/// - root: 当前根节点（可以为 nullptr，表示从空树开始）
/// - updates: 更新数组
/// - updates_len: 更新数量
/// - version: 目标版本号
/// 
/// 返回新的根节点
std::unique_ptr<NodeHandle> db_upsert(
    DbHandle& db,  // 需要可变引用
    const NodeHandle* root,
    const RawUpdate* updates,
    size_t updates_len,
    uint64_t version
);

// ============================================================
// FFI Functions - 元数据
// ============================================================

/// 获取最新版本号
uint64_t db_get_latest_version(const DbHandle& db);

/// 获取最早版本号  
uint64_t db_get_earliest_version(const DbHandle& db);

/// 加载指定版本的根节点
std::unique_ptr<NodeHandle> db_load_root_for_version(
    const DbHandle& db,
    uint64_t version
);

/// 获取历史保留长度
uint64_t db_get_history_length(const DbHandle& db);

/// 更新 finalized 版本（磁盘模式）
/// 
/// finalized 版本表示已被共识确认的版本，用于 rewind 恢复
void db_update_finalized_version(DbHandle& db, uint64_t version);

/// 获取 finalized 版本（磁盘模式）
/// 
/// 返回 UINT64_MAX 表示未设置或内存模式
uint64_t db_get_finalized_version(const DbHandle& db);

// ============================================================
// FFI Functions - Rollback & Prune（仅磁盘模式）
// ============================================================

/// 回滚到指定版本
/// 
/// - version: 目标版本号（必须在 [earliest_version, latest_version] 范围内）
/// 
/// 注意：这是破坏性操作，会丢弃目标版本之后的所有数据
void db_rewind_to_version(DbHandle& db, uint64_t version);

/// 检查版本是否有效（在磁盘上存在）
bool db_version_is_valid(const DbHandle& db, uint64_t version);

// 注意: db_clear (清空磁盘数据库) 已移除，需要直接访问 UpdateAux
// 请使用 CLI 工具: monad_mpt --clear /path/to/database

// ============================================================
// FFI Functions - Node 操作
// ============================================================

/// 克隆节点
std::unique_ptr<NodeHandle> node_clone(const NodeHandle& node);

/// 节点是否有值
bool node_has_value(const NodeHandle& node);

/// 获取值的长度
size_t node_value_len(const NodeHandle& node);

/// 复制值到缓冲区，返回实际复制的字节数
size_t node_copy_value(const NodeHandle& node, rust::Slice<uint8_t> out);

/// 获取 Merkle 数据长度
size_t node_data_len(const NodeHandle& node);

/// 复制 Merkle 数据到缓冲区
size_t node_copy_data(const NodeHandle& node, rust::Slice<uint8_t> out);

/// 计算节点的 Merkle 根哈希（32 字节 Keccak256）
/// 
/// - out: 输出缓冲区，必须至少 32 字节
/// 
/// 返回实际写入的字节数（成功时为 32）
size_t node_compute_root_hash(const NodeHandle& node, rust::Slice<uint8_t> out);

// ============================================================
// FFI Functions - 性能优化
// ============================================================

/// 预加载节点到缓存（仅 RW 磁盘模式有效）
/// 
/// 遍历根节点下的所有可缓存节点，将它们加载到内存中。
/// 
/// - root: 要预加载的根节点
/// 
/// 返回加载的节点数量
size_t db_prefetch(DbHandle& db, const NodeHandle& root);

/// 检查数据库是否只读
bool db_is_read_only(const DbHandle& db);

// ============================================================
// 共享类型 - 统计信息
// ============================================================

// DbStats 由 cxx 从 Rust 侧生成

/// 获取数据库统计信息
/// 
/// 返回结构包含：
/// - latest_version: 最新版本号
/// - earliest_version: 最早版本号
/// - history_length: 历史保留长度
/// - is_on_disk: 是否磁盘模式
/// - is_read_only: 是否只读
/// - finalized_version: finalized 版本
void db_get_stats(
    const DbHandle& db,
    uint64_t& latest_version,
    uint64_t& earliest_version,
    uint64_t& history_length,
    bool& is_on_disk,
    bool& is_read_only,
    uint64_t& finalized_version
);

} // namespace monad::ffi

