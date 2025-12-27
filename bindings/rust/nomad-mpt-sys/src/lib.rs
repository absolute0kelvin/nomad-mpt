//! MonadDB FFI Bindings
//!
//! 提供 MonadDB MPT (Merkle Patricia Trie) 的 Rust FFI 绑定。

pub mod async_fifo;
pub use async_fifo::{AsyncFifo, FindResult, ResultStatus, LargeValue};

#[cxx::bridge(namespace = "monad::ffi")]
pub mod ffi {
    // ============================================================
    // 共享类型 (Rust ↔ C++)
    // ============================================================
    
    /// 原始更新数据，用于跨 FFI 边界传递
    /// 
    /// 这是一个扁平结构，C++ 侧会将其转换为 UpdateList
    #[derive(Debug, Clone)]
    struct RawUpdate {
        /// Key 数据指针
        key_ptr: *const u8,
        /// Key 长度（字节）
        key_len: usize,
        /// Value 数据指针（NULL 表示删除）
        value_ptr: *const u8,
        /// Value 长度（字节）
        value_len: usize,
        /// 版本号
        version: i64,
        /// 嵌套更新指针（用于存储 trie）
        nested_ptr: *const RawUpdate,
        /// 嵌套更新数量
        nested_len: usize,
    }
    
    // ============================================================
    // Opaque C++ Types
    // ============================================================
    
    unsafe extern "C++" {
        include!("nomad-mpt-sys/src/bridge.hpp");
        
        /// 数据库句柄
        type DbHandle;
        
        /// 节点句柄
        type NodeHandle;
        
        // ============================================================
        // Db 生命周期
        // ============================================================
        
        /// 打开内存数据库
        fn db_open_memory() -> Result<UniquePtr<DbHandle>>;
        
        /// 打开磁盘数据库（读写模式）
        fn db_open_disk_rw(
            db_path: &str,
            create: bool,
            history_length: u64,
        ) -> Result<UniquePtr<DbHandle>>;
        
        
        /// 关闭数据库
        fn db_close(db: UniquePtr<DbHandle>);
        
        /// 检查是否是磁盘模式
        fn db_is_on_disk(db: &DbHandle) -> bool;
        
        // ============================================================
        // 同步读取
        // ============================================================
        
        /// 查找 key 对应的节点
        fn db_find(
            db: &DbHandle,
            key: &[u8],
            version: u64,
        ) -> Result<UniquePtr<NodeHandle>>;
        
        /// 批量更新
        /// 
        /// - root: 当前根节点（可以为空，表示从空树开始）
        /// - updates: 更新数组
        /// - version: 目标版本号
        /// 
        /// 返回新的根节点
        unsafe fn db_upsert(
            db: Pin<&mut DbHandle>,
            root: *const NodeHandle,
            updates: *const RawUpdate,
            updates_len: usize,
            version: u64,
        ) -> Result<UniquePtr<NodeHandle>>;
        
        // ============================================================
        // 元数据
        // ============================================================
        
        /// 获取最新版本号
        fn db_get_latest_version(db: &DbHandle) -> u64;
        
        /// 获取最早版本号
        fn db_get_earliest_version(db: &DbHandle) -> u64;
        
        /// 加载指定版本的根节点
        fn db_load_root_for_version(
            db: &DbHandle,
            version: u64,
        ) -> Result<UniquePtr<NodeHandle>>;
        
        /// 获取历史保留长度
        fn db_get_history_length(db: &DbHandle) -> u64;
        
        /// 更新 finalized 版本（磁盘模式）
        fn db_update_finalized_version(db: Pin<&mut DbHandle>, version: u64) -> Result<()>;
        
        /// 获取 finalized 版本（磁盘模式）
        fn db_get_finalized_version(db: &DbHandle) -> u64;
        
        // ============================================================
        // Rollback & Prune（仅磁盘模式）
        // ============================================================
        
        /// 回滚/更新 finalized 版本
        /// 
        /// 这会触发 prune：当版本数超过 history_length 时，旧数据会被清理
        /// 完整的 rollback（丢弃指定版本之后的数据）请使用 CLI 工具
        fn db_rewind_to_version(db: Pin<&mut DbHandle>, version: u64) -> Result<()>;
        
        /// 检查版本是否有效
        fn db_version_is_valid(db: &DbHandle, version: u64) -> bool;
        
        // ============================================================
        // Node 操作
        // ============================================================
        
        /// 克隆节点
        fn node_clone(node: &NodeHandle) -> UniquePtr<NodeHandle>;
        
        /// 节点是否有值
        fn node_has_value(node: &NodeHandle) -> bool;
        
        /// 获取值的长度
        fn node_value_len(node: &NodeHandle) -> usize;
        
        /// 复制值到缓冲区，返回实际复制的字节数
        fn node_copy_value(node: &NodeHandle, out: &mut [u8]) -> usize;
        
        /// 获取 Merkle 数据长度
        fn node_data_len(node: &NodeHandle) -> usize;
        
        /// 复制 Merkle 数据到缓冲区
        fn node_copy_data(node: &NodeHandle, out: &mut [u8]) -> usize;
        
        /// 计算节点的 Merkle 根哈希（32 字节 Keccak256）
        fn node_compute_root_hash(node: &NodeHandle, out: &mut [u8]) -> usize;
        
        // ============================================================
        // 性能优化
        // ============================================================
        
        /// 预加载节点到缓存（仅 RW 磁盘模式）
        fn db_prefetch(db: Pin<&mut DbHandle>, root: &NodeHandle) -> usize;
        
        /// 检查数据库是否只读
        fn db_is_read_only(db: &DbHandle) -> bool;
        
        /// 获取数据库统计信息
        unsafe fn db_get_stats(
            db: &DbHandle,
            latest_version: &mut u64,
            earliest_version: &mut u64,
            history_length: &mut u64,
            is_on_disk: &mut bool,
            is_read_only: &mut bool,
            finalized_version: &mut u64,
        );
    }
}

// ============================================================
// High-level Rust API
// ============================================================

use cxx::UniquePtr;
use std::ptr;

/// 单个更新操作
#[derive(Debug, Clone)]
pub struct Update<'a> {
    /// Key
    pub key: &'a [u8],
    /// Value（None 表示删除）
    pub value: Option<&'a [u8]>,
    /// 嵌套更新（用于存储 trie）
    pub nested: Vec<Update<'a>>,
}

impl<'a> Update<'a> {
    /// 创建插入/更新操作
    pub fn put(key: &'a [u8], value: &'a [u8]) -> Self {
        Self {
            key,
            value: Some(value),
            nested: Vec::new(),
        }
    }
    
    /// 创建删除操作
    pub fn delete(key: &'a [u8]) -> Self {
        Self {
            key,
            value: None,
            nested: Vec::new(),
        }
    }
    
    /// 添加嵌套更新（用于账户存储 trie）
    pub fn with_nested(mut self, nested: Vec<Update<'a>>) -> Self {
        self.nested = nested;
        self
    }
    
    /// 转换为 RawUpdate
    fn to_raw(&self, version: i64, nested_raw: &[ffi::RawUpdate]) -> ffi::RawUpdate {
        ffi::RawUpdate {
            key_ptr: self.key.as_ptr(),
            key_len: self.key.len(),
            value_ptr: self.value.map_or(ptr::null(), |v| v.as_ptr()),
            value_len: self.value.map_or(0, |v| v.len()),
            version,
            nested_ptr: if nested_raw.is_empty() { ptr::null() } else { nested_raw.as_ptr() },
            nested_len: nested_raw.len(),
        }
    }
}

/// 数据库配置
#[derive(Debug, Clone, Default)]
pub struct DbConfig {
    /// 数据库路径（None 表示内存模式）
    pub path: Option<String>,
    /// 是否创建新数据库（仅磁盘模式有效）
    pub create: bool,
    /// 历史版本保留数量（0 表示自动，仅磁盘模式有效）
    pub history_length: u64,
    /// 只读模式
    pub read_only: bool,
}

impl DbConfig {
    /// 创建内存模式配置
    pub fn memory() -> Self {
        Self::default()
    }
    
    /// 创建磁盘模式配置
    pub fn disk(path: impl Into<String>) -> Self {
        Self {
            path: Some(path.into()),
            create: true,
            history_length: 0,
            read_only: false,
        }
    }
    
    /// 设置是否创建新数据库
    pub fn with_create(mut self, create: bool) -> Self {
        self.create = create;
        self
    }
    
    /// 设置历史版本保留数量
    pub fn with_history_length(mut self, length: u64) -> Self {
        self.history_length = length;
        self
    }
    
    /// 设置只读模式
    pub fn with_read_only(mut self, read_only: bool) -> Self {
        self.read_only = read_only;
        self
    }
}

/// MonadDB 数据库
pub struct Db {
    inner: UniquePtr<ffi::DbHandle>,
}

impl Db {
    /// 使用配置打开数据库
    /// 
    /// # 支持的模式
    /// - 内存模式：`DbConfig::memory()`
    /// - 磁盘读写模式：`DbConfig::disk("/path/to/db")`
    /// 
    /// # 未实现的功能
    /// - 只读磁盘模式 (`read_only: true`) 尚未实现，会返回错误
    /// - 清空数据库 (`db_clear`) 已移除，请使用 CLI 工具
    pub fn open(config: DbConfig) -> Result<Self, cxx::Exception> {
        let inner = match &config.path {
            None => {
                // 内存模式
                ffi::db_open_memory()?
            }
            Some(path) => {
                if config.read_only {
                    // 只读磁盘模式需要 RODb 支持，尚未实现
                    // 返回一个明确的错误信息
                    panic!("Read-only disk mode is not yet implemented (requires RODb support). \
                           Use read_only: false or open the database with standard tools.");
                }
                ffi::db_open_disk_rw(path, config.create, config.history_length)?
            }
        };
        Ok(Self { inner })
    }
    
    /// 打开内存数据库
    pub fn open_memory() -> Result<Self, cxx::Exception> {
        Self::open(DbConfig::memory())
    }
    
    /// 打开磁盘数据库（读写模式）
    pub fn open_disk(path: impl Into<String>) -> Result<Self, cxx::Exception> {
        Self::open(DbConfig::disk(path))
    }
    
    /// 检查是否是磁盘模式
    pub fn is_on_disk(&self) -> bool {
        ffi::db_is_on_disk(&self.inner)
    }
    
    /// 查找 key 对应的值
    pub fn find(&self, key: &[u8], version: u64) -> Result<Option<Vec<u8>>, cxx::Exception> {
        let node = ffi::db_find(&self.inner, key, version)?;
        
        if !ffi::node_has_value(&node) {
            return Ok(None);
        }
        
        let len = ffi::node_value_len(&node);
        if len == 0 {
            return Ok(Some(Vec::new()));
        }
        
        let mut buf = vec![0u8; len];
        let copied = ffi::node_copy_value(&node, &mut buf);
        buf.truncate(copied);
        
        Ok(Some(buf))
    }
    
    /// 获取最新版本号
    pub fn latest_version(&self) -> u64 {
        ffi::db_get_latest_version(&self.inner)
    }
    
    /// 获取最早版本号
    pub fn earliest_version(&self) -> u64 {
        ffi::db_get_earliest_version(&self.inner)
    }
    
    /// 获取历史保留长度
    pub fn history_length(&self) -> u64 {
        ffi::db_get_history_length(&self.inner)
    }
    
    /// 检查版本是否有效
    pub fn version_is_valid(&self, version: u64) -> bool {
        ffi::db_version_is_valid(&self.inner, version)
    }
    
    /// 更新 finalized 版本（仅磁盘模式）
    /// 
    /// finalized 版本表示已被共识确认的版本。
    /// 
    /// # 用途
    /// 1. 配合 `rewind_to_latest_finalized` 选项恢复到一致状态
    /// 2. 当版本数超过 `history_length` 时触发自动 prune
    /// 
    /// # 注意
    /// - 仅磁盘模式支持
    /// - 这不是 rollback，不会丢弃指定版本之后的数据
    pub fn update_finalized_version(&mut self, version: u64) -> Result<(), cxx::Exception> {
        ffi::db_update_finalized_version(self.inner.pin_mut(), version)
    }
    
    /// 获取 finalized 版本（仅磁盘模式）
    /// 
    /// # 返回
    /// - 磁盘模式：返回最后设置的 finalized 版本，如果从未设置则返回 `u64::MAX`
    /// - 内存模式：返回 `u64::MAX`
    pub fn finalized_version(&self) -> u64 {
        ffi::db_get_finalized_version(&self.inner)
    }
    
    /// 回滚/更新 finalized 版本并触发 prune
    /// 
    /// # 参数
    /// - `version`: 目标版本，必须在 `[earliest_version, latest_version]` 范围内
    /// 
    /// # 注意
    /// 当前实现调用 `update_finalized_version`，会触发 prune 但不会丢弃后续版本。
    /// 完整 rollback（丢弃后续版本）需要使用 CLI 工具: `monad_mpt --rewind-to <version>`
    pub fn rewind_to_version(&mut self, version: u64) -> Result<(), cxx::Exception> {
        ffi::db_rewind_to_version(self.inner.pin_mut(), version)
    }
    
    /// 加载指定版本的根节点
    pub fn load_root(&self, version: u64) -> Result<Node, cxx::Exception> {
        let inner = ffi::db_load_root_for_version(&self.inner, version)?;
        Ok(Node { inner })
    }
    
    /// 批量更新（从空树开始）
    /// 
    /// # 参数
    /// - `updates`: 更新列表
    /// - `version`: 目标版本号
    /// 
    /// # 返回
    /// 新的根节点
    pub fn upsert(&mut self, updates: &[Update], version: u64) -> Result<Node, cxx::Exception> {
        self.upsert_with_root(None, updates, version)
    }
    
    /// 批量更新（指定根节点）
    /// 
    /// # 参数
    /// - `root`: 当前根节点（None 表示从空树开始）
    /// - `updates`: 更新列表
    /// - `version`: 目标版本号
    /// 
    /// # 返回
    /// 新的根节点
    pub fn upsert_with_root(
        &mut self,
        root: Option<&Node>,
        updates: &[Update],
        version: u64,
    ) -> Result<Node, cxx::Exception> {
        // 构建 RawUpdate 数组
        // 注意：我们需要保持所有嵌套更新的生命周期
        let mut all_nested: Vec<Vec<ffi::RawUpdate>> = Vec::new();
        let mut raw_updates: Vec<ffi::RawUpdate> = Vec::new();
        
        for update in updates {
            // 递归构建嵌套更新
            let nested_raw = build_nested_raw(&update.nested, version as i64, &mut all_nested);
            raw_updates.push(update.to_raw(version as i64, nested_raw));
        }
        
        let root_ptr = root.map_or(ptr::null(), |r| &*r.inner as *const _);
        
        let inner = unsafe {
            ffi::db_upsert(
                self.inner.pin_mut(),
                root_ptr,
                raw_updates.as_ptr(),
                raw_updates.len(),
                version,
            )?
        };
        
        Ok(Node { inner })
    }
    
    /// 预加载节点到缓存（仅 RW 磁盘模式）
    /// 
    /// 遍历根节点下的所有可缓存节点，将它们加载到内存中。
    /// 这可以加速后续的读取操作。
    /// 
    /// # 返回
    /// 加载的节点数量
    /// 
    /// # 注意
    /// - 仅在 RW 磁盘模式下有效
    /// - 内存模式和只读模式返回 0
    pub fn prefetch(&mut self, root: &Node) -> usize {
        ffi::db_prefetch(self.inner.pin_mut(), &root.inner)
    }
    
    /// 检查数据库是否只读
    pub fn is_read_only(&self) -> bool {
        ffi::db_is_read_only(&self.inner)
    }
    
    /// 获取数据库统计信息
    pub fn stats(&self) -> DbStats {
        let mut latest_version = 0u64;
        let mut earliest_version = 0u64;
        let mut history_length = 0u64;
        let mut is_on_disk = false;
        let mut is_read_only = false;
        let mut finalized_version = 0u64;
        
        unsafe {
            ffi::db_get_stats(
                &self.inner,
                &mut latest_version,
                &mut earliest_version,
                &mut history_length,
                &mut is_on_disk,
                &mut is_read_only,
                &mut finalized_version,
            );
        }
        
        DbStats {
            latest_version,
            earliest_version,
            history_length,
            is_on_disk,
            is_read_only,
            finalized_version,
        }
    }
}

/// 数据库统计信息
#[derive(Debug, Clone, Copy)]
pub struct DbStats {
    /// 最新版本号
    pub latest_version: u64,
    /// 最早版本号（仅磁盘模式有效）
    pub earliest_version: u64,
    /// 历史保留长度
    pub history_length: u64,
    /// 是否磁盘模式
    pub is_on_disk: bool,
    /// 是否只读
    pub is_read_only: bool,
    /// Finalized 版本（`u64::MAX` 表示未设置）
    pub finalized_version: u64,
}

/// 递归构建嵌套 RawUpdate
fn build_nested_raw<'a>(
    updates: &[Update],
    version: i64,
    storage: &'a mut Vec<Vec<ffi::RawUpdate>>,
) -> &'a [ffi::RawUpdate] {
    if updates.is_empty() {
        return &[];
    }
    
    let mut raw_updates: Vec<ffi::RawUpdate> = Vec::new();
    
    for update in updates {
        let nested_raw = build_nested_raw(&update.nested, version, storage);
        raw_updates.push(update.to_raw(version, nested_raw));
    }
    
    storage.push(raw_updates);
    storage.last().unwrap()
}

/// MPT 节点
pub struct Node {
    inner: UniquePtr<ffi::NodeHandle>,
}

impl Node {
    /// 节点是否有值
    pub fn has_value(&self) -> bool {
        ffi::node_has_value(&self.inner)
    }
    
    /// 获取节点的值
    pub fn value(&self) -> Option<Vec<u8>> {
        if !self.has_value() {
            return None;
        }
        
        let len = ffi::node_value_len(&self.inner);
        if len == 0 {
            return Some(Vec::new());
        }
        
        let mut buf = vec![0u8; len];
        let copied = ffi::node_copy_value(&self.inner, &mut buf);
        buf.truncate(copied);
        
        Some(buf)
    }
    
    /// 获取节点的 Merkle 数据（用于生成 proof）
    pub fn data(&self) -> Vec<u8> {
        let len = ffi::node_data_len(&self.inner);
        if len == 0 {
            return Vec::new();
        }
        
        let mut buf = vec![0u8; len];
        let copied = ffi::node_copy_data(&self.inner, &mut buf);
        buf.truncate(copied);
        
        buf
    }
    
    /// 计算节点的 Merkle 根哈希（32 字节 Keccak256）
    /// 
    /// 这是以太坊风格的状态根哈希，可以与区块头中的 stateRoot 比对。
    pub fn root_hash(&self) -> [u8; 32] {
        let mut hash = [0u8; 32];
        ffi::node_compute_root_hash(&self.inner, &mut hash);
        hash
    }
}

impl Clone for Node {
    fn clone(&self) -> Self {
        Self {
            inner: ffi::node_clone(&self.inner),
        }
    }
}

// 测试需要在单独的集成测试中运行，因为静态库链接顺序问题
// TODO: 添加集成测试
// #[cfg(test)]
// mod tests {
//     use super::*;
//     
//     #[test]
//     fn test_db_open() {
//         let db = Db::open_rw("{}").expect("Failed to open db");
//         assert_eq!(db.latest_version(), 0);
//     }
// }
