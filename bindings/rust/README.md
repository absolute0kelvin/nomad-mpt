# nomad-mpt-sys

Rust FFI bindings for MonadDB's Merkle Patricia Trie (MPT) engine.

## 核心概念

在使用 MonadDB 之前，需要理解以下核心概念：

### Version（版本号）= Block Number（区块高度）

MonadDB 使用 `version` 来索引状态。**对于区块链场景，version 就是区块高度**：

```
version = 100  →  Block 100 的世界状态
version = 101  →  Block 101 的世界状态
version = 102  →  Block 102 的世界状态
```

每个 version 对应一个完整的 MPT 状态快照。

### Node（节点）= State Root（状态根）

`Node` 是 MPT 的根节点抽象，包含：
- **Merkle Root Hash**：32 字节 Keccak256 哈希（= 以太坊 `stateRoot`）
- 对整个状态树的引用

```rust
let root: Node = db.upsert(...)?;
let state_root: [u8; 32] = root.root_hash();  // 这就是区块头的 stateRoot
```

### Finalized Version（已确认版本）

区块链中有两类区块：
- **Pending/Unfinalized**：最新的区块，可能被 reorg
- **Finalized**：已被共识确认，不可逆

```
Block 100 ✓ finalized
Block 101 ✓ finalized  
Block 102 ✓ finalized  ← finalized_version = 102
Block 103   pending
Block 104   pending    ← latest_version = 104
```

MonadDB 的 `finalized_version` 用于：
1. **崩溃恢复**：重启后恢复到 finalized 状态
2. **历史裁剪**：超过 `history_length` 的旧版本会被清理

### prev_root（上一个区块的状态根）

处理区块 N 时，需要基于区块 N-1 的状态：

```rust
// 处理 Block 104
let prev_root = db.load_root(103)?;  // 加载 Block 103 的 root（不是 finalized!）
let new_root = db.upsert_with_root(Some(&prev_root), &updates, 104)?;
```

**重要**：`prev_root` 是**上一个区块**的 root，不是 finalized 的 root。

## 完整示例：PoS 区块处理流程

以下是一个典型 PoS 链（无 reorg）的区块处理流程：

```rust
use nomad_mpt_sys::{Db, DbConfig, Update, AsyncFifo, ResultStatus};

/// 区块执行器
struct BlockExecutor {
    db: Db,
    fifo: AsyncFifo,
    current_root: Option<Node>,
}

impl BlockExecutor {
    fn new(db_path: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let mut db = Db::open(
            DbConfig::disk(db_path)
                .with_create(true)
                .with_history_length(10000)  // 保留 10000 个历史版本
        )?;
        
        // 创建异步 FIFO 用于并发读取
        let fifo = db.create_async_fifo()?;
        fifo.start(4);  // 4 个 worker fiber
        
        // 加载最新状态
        let latest = db.latest_version();
        let current_root = if latest > 0 {
            Some(db.load_root(latest)?)
        } else {
            None
        };
        
        Ok(Self { db, fifo, current_root })
    }
    
    /// 处理一个区块
    fn process_block(&mut self, block: &Block) -> Result<[u8; 32], Error> {
        let block_number = block.number;
        
        // ============================================================
        // 阶段 1: 异步读取 - 执行交易时并发查询状态
        // ============================================================
        
        let mut pending_reads: HashMap<u128, TxContext> = HashMap::new();
        let mut all_updates: Vec<Update> = Vec::new();
        
        for (tx_idx, tx) in block.transactions.iter().enumerate() {
            // 提交异步读取请求
            // version = block_number - 1，因为我们要读取上一个区块的状态
            let user_data = tx_idx as u128;
            
            // 读取发送方账户
            self.fifo.submit_find_value(&tx.from, block_number - 1, user_data);
            
            // 读取接收方账户
            self.fifo.submit_find_value(&tx.to, block_number - 1, user_data);
            
            pending_reads.insert(user_data, TxContext::new(tx));
        }
        
        // 轮询读取结果，执行交易逻辑
        while !pending_reads.is_empty() {
            // 批量轮询
            let results = self.fifo.poll_batch(64);
            
            for result in results {
                let tx_ctx = pending_reads.get_mut(&result.user_data).unwrap();
                
                match result.status {
                    ResultStatus::Ok => {
                        // 找到值，更新交易上下文
                        tx_ctx.add_state(result.merkle_hash, result.value);
                    }
                    ResultStatus::NotFound => {
                        // 账户不存在，使用默认值
                        tx_ctx.add_state(result.merkle_hash, None);
                    }
                    ResultStatus::Error => {
                        return Err(Error::ReadFailed);
                    }
                    _ => {}
                }
                
                // 检查是否收集完所有需要的状态
                if tx_ctx.is_ready() {
                    // 执行交易，生成状态变更
                    let changes = tx_ctx.execute()?;
                    
                    // 收集 Update
                    for (account, data, storage_changes) in changes {
                        let storage_updates: Vec<Update> = storage_changes
                            .into_iter()
                            .map(|(slot, value)| Update::put(&slot, &value))
                            .collect();
                        
                        all_updates.push(
                            Update::put(&account, &data)
                                .with_nested(storage_updates)
                        );
                    }
                    
                    pending_reads.remove(&result.user_data);
                }
            }
            
            // 让出 CPU，避免忙等
            std::thread::yield_now();
        }
        
        // ============================================================
        // 阶段 2: 同步写入 - 提交区块的状态变更
        // ============================================================
        
        // 方式 A: 批量写入（推荐，性能更好）
        let new_root = self.db.upsert_with_root(
            self.current_root.as_ref(),  // 基于上一个区块的 root
            &all_updates,                 // 整个区块的所有变更
            block_number,                 // version = block_number
        )?;
        
        // 方式 B: 增量写入（如果需要每笔交易后的中间状态）
        // let mut current = self.current_root.clone();
        // for tx_updates in per_tx_updates {
        //     current = Some(self.db.upsert_with_root(
        //         current.as_ref(),
        //         &tx_updates,
        //         block_number,  // 同一个 version
        //     )?);
        // }
        // let new_root = current.unwrap();
        
        let state_root = new_root.root_hash();
        
        // 更新当前 root
        self.current_root = Some(new_root);
        
        // 验证状态根
        assert_eq!(state_root, block.header.state_root);
        
        Ok(state_root)
    }
    
    /// 确认区块已 finalized
    fn finalize(&mut self, finalized_block: u64) -> Result<(), Error> {
        // ============================================================
        // 阶段 3: Finalize - 标记区块为不可逆
        // ============================================================
        
        self.db.update_finalized_version(finalized_block)?;
        
        // 此时：
        // - finalized_block 及之前的区块数据已安全
        // - 崩溃重启后会恢复到 finalized_block 状态
        // - 超过 history_length 的旧版本会被清理
        
        Ok(())
    }
}
```

### 流程图解

```
                        ┌─────────────────────────────────────────────┐
                        │              Block N 处理流程                │
                        └─────────────────────────────────────────────┘

┌──────────────┐     ┌──────────────────────────────────────────────────┐
│   Block N    │     │  阶段 1: 异步读取（FIFO）                          │
│              │     │                                                  │
│  tx1, tx2... │────►│  fifo.submit_find_value(key, version=N-1, ...)  │
│              │     │  fifo.poll_batch() → 执行交易逻辑                 │
└──────────────┘     │  收集所有 Update                                 │
                     └───────────────────────┬──────────────────────────┘
                                             │
                                             ▼
                     ┌──────────────────────────────────────────────────┐
                     │  阶段 2: 同步写入（upsert）                        │
                     │                                                  │
                     │  db.upsert_with_root(prev_root, updates, N)      │
                     │  返回 new_root，包含 stateRoot                    │
                     └───────────────────────┬──────────────────────────┘
                                             │
                                             ▼
                     ┌──────────────────────────────────────────────────┐
                     │  阶段 3: Finalize（稍后，共识确认时）              │
                     │                                                  │
                     │  db.update_finalized_version(finalized_block)    │
                     │  数据安全落盘，可进行历史裁剪                      │
                     └──────────────────────────────────────────────────┘
```

### 状态时间线

```
Block:     98      99      100     101     102     103     104
           ─────────────────────────────────────────────────────►
                                    │               │
                                    │               └── latest_version (刚处理)
                                    │
                                    └── finalized_version (共识确认)
                                    
读取 Block 104 时:
  - 交易执行需要读取 version = 103 的状态
  - prev_root = db.load_root(103)
  - new_root = db.upsert_with_root(prev_root, updates, 104)
  
稍后 Block 101 被 finalize:
  - db.update_finalized_version(101)
```

## 特性

| 功能 | 状态 | 说明 |
|------|------|------|
| 内存模式 | ✅ | 快速测试和开发 |
| 磁盘模式 | ✅ | 持久化存储（需要 huge pages） |
| 批量更新 | ✅ | 高效的多 key 事务 |
| 嵌套 Trie | ✅ | 支持以太坊账户存储 |
| Merkle Proof | ✅ | 生成状态证明 |
| 异步 Find | ✅ | ck_fifo 高并发查询 |
| 异步 Traverse | ✅ | 子树遍历 |

## 系统要求

- **操作系统**: Linux (需要 `io_uring`)
- **架构**: x86_64 (AVX2/AVX512) 或 ARM64 (NEON/SHA3)
- **编译器**: Clang 19+ 推荐
- **依赖**:
  - `liburing-dev`
  - `libboost-fiber-dev`
  - `libboost-context-dev`
  - `libgmp-dev`
  - `libzstd-dev`

### 磁盘模式配置

磁盘模式需要配置 huge pages：

```bash
# 配置 512 个 2MB huge pages (1GB 内存)
echo 512 | sudo tee /proc/sys/vm/nr_hugepages

# 持久化配置
echo "vm.nr_hugepages=512" | sudo tee -a /etc/sysctl.conf
```

## 安装

添加到 `Cargo.toml`:

```toml
[dependencies]
nomad-mpt-sys = { path = "path/to/nomad-mpt/bindings/rust/nomad-mpt-sys" }
```

构建：

```bash
cd nomad-mpt/bindings/rust
cargo build --release
```

## API 参考

### 数据库操作

```rust
// 打开数据库
let db = Db::open_memory()?;                    // 内存模式
let db = Db::open_disk("/path/to/db")?;         // 磁盘模式（需要 huge pages）
let db = Db::open(DbConfig::disk(path)          // 自定义配置
    .with_create(true)
    .with_history_length(10000))?;

// 版本查询
let latest = db.latest_version();               // 最新写入的版本
let earliest = db.earliest_version();           // 最早可查询的版本
let finalized = db.finalized_version();         // 已确认的版本

// 加载指定版本的 root
let root = db.load_root(version)?;

// 按版本查询（磁盘模式）
let value = db.find(&key, version)?;

// 从指定 root 查询（内存/磁盘都支持）
let node = db.find_from_root(&root, &key, version)?;
```

### 写入操作

```rust
// 方式 1: 从空树开始（创世区块）
let root = db.upsert(&updates, version)?;

// 方式 2: 基于已有状态增量更新（常规区块）
let root = db.upsert_with_root(Some(&prev_root), &updates, version)?;
```

### 增量写入 vs 批量写入

MonadDB 支持两种写入模式：

**批量写入**（推荐）：收集所有更新，一次 upsert
```rust
let mut all_updates = Vec::new();
for tx in block.transactions {
    all_updates.extend(execute_tx(tx));
}
let root = db.upsert_with_root(Some(&prev_root), &all_updates, block_number)?;
```

**增量写入**：每笔交易后立即 upsert
```rust
let mut current_root = prev_root;
for tx in block.transactions {
    let updates = execute_tx(tx);
    current_root = db.upsert_with_root(Some(&current_root), &updates, block_number)?;
}
// current_root 是最终状态
```

| 模式 | 性能 | 适用场景 |
|------|------|---------|
| **批量写入** | ⚡ 更快（MPT 路径合并优化） | 大多数场景 |
| **增量写入** | 稍慢（每次遍历 MPT） | 需要中间状态、调试 |

**注意**：两种模式都使用相同的 `version` 号，只有最后一个 root 会被持久化索引。

### Update 构建

```rust
// 插入/更新
Update::put(&key, &value)

// 删除
Update::delete(&key)

// 嵌套更新（以太坊账户 + 存储槽）
Update::put(&account_key, &account_data)
    .with_nested(vec![
        Update::put(&slot0, &value0),
        Update::put(&slot1, &value1),
    ])
```

### 版本管理

```rust
// 标记版本为 finalized（崩溃恢复点）
db.update_finalized_version(block_number)?;

// 回滚到指定版本
db.rewind_to_version(block_number)?;
```

### 异步 FIFO API

```rust
// 创建 FIFO
let fifo = db.create_async_fifo()?;
fifo.start(4);  // 启动 4 个 worker fiber

// 提交查询
fifo.submit_find_value(&key, version, user_data);
fifo.submit_find_node(&key, version, user_data);  // 包含 Merkle hash
fifo.submit_traverse(&prefix, version, limit, user_data);

// 轮询结果
while let Some(result) = fifo.poll() {
    match result.status {
        ResultStatus::Ok => { /* 找到 */ }
        ResultStatus::NotFound => { /* 未找到 */ }
        ResultStatus::Error => { /* 错误 */ }
        _ => {}
    }
}

// 批量操作
let requests: Vec<(&[u8], u64, u128)> = vec![
    (&key1, version, 1),
    (&key2, version, 2),
];
fifo.submit_find_batch(&requests);
let results = fifo.poll_batch(64);

// 停止（Drop 时自动调用）
fifo.stop();
```

### Node 操作

```rust
// 获取 Merkle 根哈希（32 字节，= 以太坊 stateRoot）
let state_root: [u8; 32] = node.root_hash();

// 检查是否有值（叶子节点）
if node.has_value() {
    let value: Option<Vec<u8>> = node.value();
}

// 获取 Merkle 数据（用于生成 proof）
let merkle_data: Vec<u8> = node.data();

// 克隆节点
let cloned = node.clone();
```

## 内存模式 vs 磁盘模式

| 功能 | 内存模式 | 磁盘模式 |
|------|---------|---------|
| `db.find(key, version)` | ❌ 不支持 | ✅ 支持 |
| `db.find_from_root(root, key, version)` | ✅ 支持 | ✅ 支持 |
| 异步 FIFO | ⚠️ 需要磁盘模式的 version 索引 | ✅ 完全支持 |
| 持久化 | ❌ 关闭即丢失 | ✅ 自动持久化 |
| Huge Pages | 不需要 | 必需 |
| 使用场景 | 测试、临时计算 | 生产环境 |

## 分叉场景（PoW/Reorg）

虽然本 README 以 PoS 无分叉链为例，MonadDB 也支持分叉：

```rust
// 从同一个 root 分叉
let root_a = db.load_root(100)?;
let root_b = db.upsert_with_root(Some(&root_a), &updates_b, 101)?;
let root_c = db.upsert_with_root(Some(&root_a), &updates_c, 101)?;

// 两个竞争的 tip
// 用 find_from_root 查询（不能用 FIFO，因为 version 冲突）
let node_b = db.find_from_root(&root_b, &key, 101)?;
let node_c = db.find_from_root(&root_c, &key, 101)?;
```

**注意**：FIFO API 基于 version 查询，不支持同一 version 有多个 root 的场景。

## 示例

```bash
# 基本用法
cargo run --release --example basic

# 异步 FIFO
cargo run --release --example async_fifo

# 磁盘持久化
cargo run --release --example disk_persistence
```

## 运行测试

```bash
cargo test --release
```

## 性能

在 ARM64 (Apple M4) 上的参考性能：

- 批量写入: ~2.5M records/sec
- 同步查询: ~1M queries/sec (缓存命中)
- 异步查询: ~500K queries/sec (批量，冷缓存)

## 许可证

GPL-3.0

## 相关链接

- [MonadDB](https://github.com/monad/monad)
- [FFI 开发计划](../../docs/ffi_plan.md)
