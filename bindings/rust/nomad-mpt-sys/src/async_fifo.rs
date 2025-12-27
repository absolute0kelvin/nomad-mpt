//! Async FIFO - 基于 Concurrency Kit 的异步通道
//!
//! 提供高性能的异步 find/traverse 操作支持。

use std::ffi::c_void;
use std::ptr::NonNull;

// ============================================================
// FFI 类型定义
// ============================================================

// 编译时验证结构体大小与 C++ 一致
// C++ 定义见 bridge_fifo.hpp
const _: () = {
    assert!(std::mem::size_of::<Request>() == 64, "Request size mismatch with C++");
    assert!(std::mem::size_of::<Completion>() == 312, "Completion size mismatch with C++");
    assert!(std::mem::size_of::<RequestNode>() == 24 + 64, "RequestNode size mismatch");
    assert!(std::mem::size_of::<CompletionNode>() == 24 + 312, "CompletionNode size mismatch");
};

/// 请求类型
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RequestType {
    FindValue = 1,
    FindNode = 2,
    Traverse = 3,
    Shutdown = 255,
}

/// 请求数据（与 C++ Request 对应）
#[repr(C, align(8))]
pub struct Request {
    /// user_data 小端：lo = 低 64 位，hi = 高 64 位
    pub user_data_lo: u64,
    pub user_data_hi: u64,
    pub version: u64,
    pub req_type: u8,
    pub key_len: u8,
    pub _pad: [u8; 2],
    pub traverse_limit: u32,
    pub key: [u8; 32],
}

/// 请求节点
#[repr(C)]
pub struct RequestNode {
    // 与 C++ 一致：MPMC entry 尺寸 24 字节
    pub entry: [u8; 24],  // ck_fifo_mpmc_entry_t
    pub req: Request,
}

/// 结果状态
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResultStatus {
    Ok = 0,
    NotFound = 1,
    Error = 2,
    TraverseMore = 3,
    TraverseEnd = 4,
}

impl From<u8> for ResultStatus {
    fn from(v: u8) -> Self {
        match v {
            0 => Self::Ok,
            1 => Self::NotFound,
            2 => Self::Error,
            3 => Self::TraverseMore,
            4 => Self::TraverseEnd,
            _ => Self::Error,
        }
    }
}

/// 完成数据（与 C++ Completion 对应）
#[repr(C, align(8))]
pub struct Completion {
    pub user_data_lo: u64,
    pub user_data_hi: u64,
    pub status: u8,
    pub _pad: [u8; 3],
    pub value_len: u32,
    pub value: [u8; 256],
    pub merkle_hash: [u8; 32],
}

/// 完成节点（使用 MPMC entry，24 字节）
#[repr(C)]
pub struct CompletionNode {
    pub entry: [u8; 24],  // ck_fifo_mpmc_entry_t（比 SPSC 大）
    pub comp: Completion,
}

/// 大值节点（使用 MPMC entry，24 字节）
#[repr(C)]
pub struct LargeValueNode {
    pub entry: [u8; 24],  // ck_fifo_mpmc_entry_t
    pub user_data_lo: u64,
    pub user_data_hi: u64,
    pub len: u32,
    // data 跟随在后面
}

// ============================================================
// FFI 函数声明
// ============================================================

// FifoManager 在 C++ 侧是不透明类型
#[repr(C)]
pub struct FifoManager {
    _private: [u8; 0],
}

// DbHandle 在 C++ 侧定义
#[repr(C)]
pub struct DbHandleOpaque {
    _private: [u8; 0],
}

extern "C" {
    fn fifo_create(db: *mut DbHandleOpaque) -> *mut FifoManager;
    fn fifo_destroy(mgr: *mut FifoManager);
    fn fifo_start(mgr: *mut FifoManager, num_workers: usize);
    fn fifo_stop(mgr: *mut FifoManager);
    
    // 单个操作
    fn fifo_alloc_request(mgr: *mut FifoManager) -> *mut RequestNode;
    #[allow(dead_code)]  // 保留完整 API，可用于手动内存管理
    fn fifo_free_request(mgr: *mut FifoManager, node: *mut RequestNode);
    fn fifo_submit(mgr: *mut FifoManager, node: *mut RequestNode);
    fn fifo_poll_completion(mgr: *mut FifoManager) -> *mut CompletionNode;
    fn fifo_free_completion(mgr: *mut FifoManager, node: *mut CompletionNode);
    fn fifo_poll_traverse(mgr: *mut FifoManager) -> *mut CompletionNode;
    fn fifo_free_traverse(mgr: *mut FifoManager, node: *mut CompletionNode);
    fn fifo_poll_large_value(mgr: *mut FifoManager) -> *mut LargeValueNode;
    fn fifo_free_large_value(mgr: *mut FifoManager, node: *mut LargeValueNode);
    
    // 批量操作
    fn fifo_alloc_request_batch(mgr: *mut FifoManager, out: *mut *mut RequestNode, count: usize) -> usize;
    fn fifo_submit_batch(mgr: *mut FifoManager, nodes: *const *mut RequestNode, count: usize);
    fn fifo_poll_completion_batch(mgr: *mut FifoManager, out: *mut *mut CompletionNode, max_count: usize) -> usize;
    fn fifo_free_completion_batch(mgr: *mut FifoManager, nodes: *const *mut CompletionNode, count: usize);
    fn fifo_poll_traverse_batch(mgr: *mut FifoManager, out: *mut *mut CompletionNode, max_count: usize) -> usize;
    fn fifo_free_traverse_batch(mgr: *mut FifoManager, nodes: *const *mut CompletionNode, count: usize);
}

// ============================================================
// 高级 Rust API
// ============================================================

/// 查找结果
#[derive(Debug, Clone)]
pub struct FindResult {
    pub user_data: u128,
    pub status: ResultStatus,
    pub value: Option<Vec<u8>>,
    pub has_large_value: bool,
    pub merkle_hash: [u8; 32],
}

/// 大值
#[derive(Debug, Clone)]
pub struct LargeValue {
    pub user_data: u128,
    pub data: Vec<u8>,
}

#[inline]
fn split_ud(user_data: u128) -> (u64, u64) {
    let lo = user_data as u64;
    let hi = (user_data >> 64) as u64;
    (lo, hi)
}

#[inline]
fn combine_ud(lo: u64, hi: u64) -> u128 {
    (hi as u128) << 64 | (lo as u128)
}

/// 异步 FIFO 通道
pub struct AsyncFifo {
    mgr: NonNull<FifoManager>,
}

// Safety: FifoManager 内部使用线程安全的 ck_fifo
unsafe impl Send for AsyncFifo {}
unsafe impl Sync for AsyncFifo {}

impl AsyncFifo {
    /// 从 DbHandle 创建 AsyncFifo
    ///
    /// # Safety
    /// `db_handle` 必须是有效的 DbHandle 指针
    pub unsafe fn from_raw(db_handle: *mut c_void) -> Result<Self, String> {
        let mgr = fifo_create(db_handle as *mut DbHandleOpaque);
        if mgr.is_null() {
            return Err("Failed to create FifoManager".into());
        }
        Ok(Self {
            mgr: NonNull::new_unchecked(mgr),
        })
    }
    
    /// 启动 Worker 线程
    pub fn start(&self, num_workers: usize) {
        unsafe { fifo_start(self.mgr.as_ptr(), num_workers) }
    }
    
    /// 停止 Worker 线程
    pub fn stop(&self) {
        unsafe { fifo_stop(self.mgr.as_ptr()) }
    }
    
    // === 单个操作 ===
    
    /// 提交 find_value 请求，user_data 为业务透传，按小端两段 64bit 回传
    /// 
    /// # 返回
    /// - `true`: 请求成功提交
    /// - `false`: 请求提交失败（内存分配失败）
    pub fn submit_find_value(&self, key: &[u8], version: u64, user_data: u128) -> bool {
        self.submit_find_impl(key, version, user_data, RequestType::FindValue)
    }
    
    /// 提交 find_node 请求（包含 Merkle hash）
    /// 
    /// # 返回
    /// - `true`: 请求成功提交
    /// - `false`: 请求提交失败（内存分配失败）
    pub fn submit_find_node(&self, key: &[u8], version: u64, user_data: u128) -> bool {
        self.submit_find_impl(key, version, user_data, RequestType::FindNode)
    }
    
    fn submit_find_impl(&self, key: &[u8], version: u64, user_data: u128, req_type: RequestType) -> bool {
        let (lo, hi) = split_ud(user_data);
        unsafe {
            let node = fifo_alloc_request(self.mgr.as_ptr());
            if node.is_null() {
                return false;  // 分配失败
            }
            
            let req = &mut (*node).req;
            req.user_data_lo = lo;
            req.user_data_hi = hi;
            req.version = version;
            req.req_type = req_type as u8;
            req.key_len = key.len().min(32) as u8;
            req.key[..req.key_len as usize].copy_from_slice(&key[..req.key_len as usize]);
            
            fifo_submit(self.mgr.as_ptr(), node);
            true
        }
    }
    
    /// 提交 traverse 请求
    /// 
    /// # 参数
    /// - `prefix`: 要遍历的前缀
    /// - `version`: 版本号
    /// - `limit`: 最大返回数量
    /// - `user_data`: 用户数据，会在结果中原样返回
    /// 
    /// # 返回
    /// - `true`: 请求成功提交
    /// - `false`: 请求提交失败（内存分配失败）
    pub fn submit_traverse(&self, prefix: &[u8], version: u64, limit: u32, user_data: u128) -> bool {
        let (lo, hi) = split_ud(user_data);
        unsafe {
            let node = fifo_alloc_request(self.mgr.as_ptr());
            if node.is_null() {
                return false;
            }
            
            let req = &mut (*node).req;
            req.user_data_lo = lo;
            req.user_data_hi = hi;
            req.version = version;
            req.req_type = RequestType::Traverse as u8;
            req.key_len = prefix.len().min(32) as u8;
            req.key[..req.key_len as usize].copy_from_slice(&prefix[..req.key_len as usize]);
            req.traverse_limit = limit;
            
            fifo_submit(self.mgr.as_ptr(), node);
            true
        }
    }
    
    /// 轮询完成（非阻塞）
    pub fn poll(&self) -> Option<FindResult> {
        unsafe {
            let node = fifo_poll_completion(self.mgr.as_ptr());
            if node.is_null() {
                return None;
            }
            
            let result = self.node_to_result(node);
            fifo_free_completion(self.mgr.as_ptr(), node);
            Some(result)
        }
    }

    /// 轮询 Traverse 结果（非阻塞）
    pub fn poll_traverse(&self) -> Option<FindResult> {
        unsafe {
            let node = fifo_poll_traverse(self.mgr.as_ptr());
            if node.is_null() {
                return None;
            }
            
            let result = self.node_to_result(node);
            fifo_free_traverse(self.mgr.as_ptr(), node);
            Some(result)
        }
    }
    
    fn node_to_result(&self, node: *mut CompletionNode) -> FindResult {
        unsafe {
            let comp = &(*node).comp;
            FindResult {
                user_data: combine_ud(comp.user_data_lo, comp.user_data_hi),
                status: ResultStatus::from(comp.status),
                value: if comp.value_len > 0 && comp.value_len != 0xFFFFFFFF {
                    Some(comp.value[..comp.value_len as usize].to_vec())
                } else {
                    None
                },
                has_large_value: comp.value_len == 0xFFFFFFFF,
                merkle_hash: comp.merkle_hash,
            }
        }
    }
    
    /// 轮询大值（非阻塞）
    pub fn poll_large_value(&self) -> Option<LargeValue> {
        unsafe {
            let node = fifo_poll_large_value(self.mgr.as_ptr());
            if node.is_null() {
                return None;
            }
            
            let len = (*node).len as usize;
            let data_ptr = (node as *const u8).add(std::mem::size_of::<LargeValueNode>());
            let data = std::slice::from_raw_parts(data_ptr, len).to_vec();
            
            let result = LargeValue {
                user_data: combine_ud((*node).user_data_lo, (*node).user_data_hi),
                data,
            };
            
            fifo_free_large_value(self.mgr.as_ptr(), node);
            Some(result)
        }
    }
    
    // === 批量操作 ===
    
    /// 批量提交 find 请求
    /// 
    /// # 参数
    /// - `requests`: 请求列表，每个元素为 (key, version, user_data)
    /// 
    /// # 返回
    /// 成功提交的请求数量
    pub fn submit_find_batch(&self, requests: &[(&[u8], u64, u128)]) -> usize {
        let count = requests.len();
        if count == 0 {
            return 0;
        }
        
        let mut nodes: Vec<*mut RequestNode> = vec![std::ptr::null_mut(); count];
        
        unsafe {
            let allocated = fifo_alloc_request_batch(self.mgr.as_ptr(), nodes.as_mut_ptr(), count);
            
            // 只处理成功分配的节点
            let mut valid_count = 0usize;
            for (i, (key, version, user_data)) in requests.iter().enumerate() {
                if i >= allocated || nodes[i].is_null() {
                    continue;  // 跳过分配失败的节点
                }
                
                let (lo, hi) = split_ud(*user_data);
                let req = &mut (*nodes[i]).req;
                req.user_data_lo = lo;
                req.user_data_hi = hi;
                req.version = *version;
                req.req_type = RequestType::FindValue as u8;
                req.key_len = key.len().min(32) as u8;
                req.key[..req.key_len as usize].copy_from_slice(&key[..req.key_len as usize]);
                valid_count += 1;
            }
            
            // 只提交非空节点
            if valid_count > 0 {
                // 过滤出有效节点
                let valid_nodes: Vec<*mut RequestNode> = nodes.iter()
                    .take(allocated)
                    .filter(|n| !n.is_null())
                    .copied()
                    .collect();
                
                if !valid_nodes.is_empty() {
                    fifo_submit_batch(self.mgr.as_ptr(), valid_nodes.as_ptr(), valid_nodes.len());
                }
            }
            
            valid_count
        }
    }
    
    /// 批量轮询完成
    pub fn poll_batch(&self, max: usize) -> Vec<FindResult> {
        if max == 0 {
            return vec![];
        }
        
        let mut nodes: Vec<*mut CompletionNode> = vec![std::ptr::null_mut(); max];
        let mut results = Vec::new();
        
        unsafe {
            let count = fifo_poll_completion_batch(self.mgr.as_ptr(), nodes.as_mut_ptr(), max);
            
            for i in 0..count {
                if !nodes[i].is_null() {
                    results.push(self.node_to_result(nodes[i]));
                }
            }
            
            fifo_free_completion_batch(self.mgr.as_ptr(), nodes.as_ptr(), count);
        }
        
        results
    }

    /// 批量轮询 Traverse 结果
    pub fn poll_traverse_batch(&self, max: usize) -> Vec<FindResult> {
        if max == 0 {
            return vec![];
        }
        
        let mut nodes: Vec<*mut CompletionNode> = vec![std::ptr::null_mut(); max];
        let mut results = Vec::new();
        
        unsafe {
            let count = fifo_poll_traverse_batch(self.mgr.as_ptr(), nodes.as_mut_ptr(), max);
            
            for i in 0..count {
                if !nodes[i].is_null() {
                    results.push(self.node_to_result(nodes[i]));
                }
            }
            
            fifo_free_traverse_batch(self.mgr.as_ptr(), nodes.as_ptr(), count);
        }
        
        results
    }
}

impl Drop for AsyncFifo {
    fn drop(&mut self) {
        unsafe {
            fifo_stop(self.mgr.as_ptr());
            fifo_destroy(self.mgr.as_ptr());
        }
    }
}

// ============================================================
// Db 集成
// ============================================================

impl crate::Db {
    /// 创建异步 FIFO 通道
    ///
    /// # Example
    /// ```ignore
    /// let mut db = Db::open_memory()?;
    /// let fifo = db.create_async_fifo()?;
    /// fifo.start(4);  // 4 worker threads
    /// 
    /// let id = fifo.submit_find_value(b"key", 1);
    /// // ... later ...
    /// if let Some(result) = fifo.poll() {
    ///     println!("Result: {:?}", result);
    /// }
    /// ```
    pub fn create_async_fifo(&mut self) -> Result<AsyncFifo, String> {
        // 获取 DbHandle 的原始指针
        // cxx::UniquePtr.as_mut() 返回 Option<Pin<&mut T>>
        // 使用 Pin::get_unchecked_mut 获取可变引用，然后转换为原始指针
        let db_ref = self.inner.as_mut()
            .ok_or("Database not initialized")?;
        
        // Safety: DbHandle 不依赖 Pin 的保证（不是自引用类型）
        let db_ptr = unsafe { 
            std::pin::Pin::get_unchecked_mut(db_ref) as *mut _ as *mut c_void
        };
        
        unsafe { AsyncFifo::from_raw(db_ptr) }
    }
}

