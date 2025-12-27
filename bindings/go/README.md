# MonadDB Go 绑定

这个包提供 MonadDB MPT (Merkle Patricia Trie) 的 Go 语言绑定。

## 系统要求

- Linux (x86_64 或 ARM64)
- Go 1.21+
- Rust 1.70+
- 系统依赖：
  ```bash
  # Ubuntu/Debian
  sudo apt install liburing-dev libgmp-dev libzstd-dev libarchive-dev
  sudo apt install libboost-fiber-dev libboost-context-dev libboost-stacktrace-dev
  sudo apt install libbacktrace-dev libssl-dev
  ```

## 构建步骤

### 方式一：使用构建脚本（推荐）

```bash
cd nomad-mpt/bindings/go
./scripts/build.sh
```

构建脚本会：
1. 构建 Rust FFI crate
2. 编译 C API 包装层
3. 复制静态库到 `monaddb/lib/`
4. 构建 Go 包

快速重建（跳过 Rust 编译）：
```bash
./scripts/build.sh --quick
```

## Key 要求

**推荐使用 32 字节固定长度的 Key**（以太坊标准）。

这与以太坊使用 Keccak256 哈希作为 key 的设计一致。Value 长度不限。

```go
// ✅ 推荐：32 字节 key（以太坊标准）
key := make([]byte, 32)
key[0] = 0x01

// ⚠️ 可用但不推荐：变长 key
// key := []byte("hello")  // 虽然可以工作，但不符合以太坊规范
```

> 注：底层使用 EthereumStateMachine，支持批量更新（UpdateList）。
> 技术上支持任意长度 key，但为了与以太坊生态兼容，建议使用 32 字节。

## 使用示例

```go
package main

import (
    "fmt"
    "log"
    
    "github.com/monad/nomad-mpt-go/monaddb"
)

func main() {
    // 打开内存数据库
    db, err := monaddb.OpenMemory()
    if err != nil {
        log.Fatal(err)
    }
    defer db.Close()
    
    // 创建 32 字节 key（以太坊标准）
    key := make([]byte, 32)
    key[0] = 0x01
    
    // 插入数据
    root, err := db.Put(nil, key, []byte("world"), 1)
    if err != nil {
        log.Fatal(err)
    }
    
    // 获取 Merkle 根哈希
    hash, _ := root.HashHex()
    fmt.Printf("Root: %s\n", hash)
    
    // 查询数据（内存模式必须使用 FindFromRoot）
    node, err := db.FindFromRoot(root, key, 1)
    if err != nil {
        log.Fatal(err)
    }
    
    if node != nil {
        value, _ := node.Value()
        fmt.Printf("Value: %s\n", value)
    }
}
```

## 运行示例

```bash
go run examples/basic/main.go
```

## 运行测试

```bash
go test -v ./monaddb/...
```

## API 概览

### DB 操作

| 方法 | 说明 |
|------|------|
| `OpenMemory()` | 打开内存数据库 |
| `OpenDisk(path, create, historyLen)` | 打开磁盘数据库 |
| `Close()` | 关闭数据库 |
| `FindFromRoot(root, key, version)` | 从根节点查找 key（内存/磁盘通用） |
| `Find(key, version)` | 查找 key（仅磁盘模式） |
| `Upsert(root, updates, version)` | 批量更新 |
| `Put(root, key, value, version)` | 插入单条 |
| `Delete(root, key, version)` | 删除单条 |

### 内存模式 vs 磁盘模式

| 特性 | 内存模式 | 磁盘模式 |
|------|----------|----------|
| 数据持久化 | ❌ 关闭即丢失 | ✅ 持久存储 |
| `Find()` | ❌ 返回错误 | ✅ 支持 |
| `FindFromRoot()` | ✅ 必须使用 | ✅ 支持 |
| 需要 huge pages | ❌ 不需要 | ✅ 需要 |

### 异步操作 (Fifo) - 实验性 🔬

> **注意**：异步 FIFO API 目前是**实验性**功能。C API 层只提供了 stub 实现，
> 实际的异步操作不会执行。如需高并发异步操作，请使用 Rust 绑定，
> 其中有基于 `ck_fifo`（Concurrency Kit 无锁队列）的完整实现。

```go
fifo, _ := db.CreateFifo()
defer fifo.Destroy()

// 检查是否已实现
if !fifo.IsImplemented() {
    log.Println("Warning: FIFO is not fully implemented")
}

fifo.Start(4)  // 4 个 worker fiber
defer fifo.Stop()

// 提交异步查询（目前为空操作）
fifo.SubmitFind(key, version, userData, 0)

// 轮询结果（目前总是返回 nil）
if comp := fifo.Poll(); comp != nil {
    // 处理结果
}
```

#### ck_fifo 说明

`ck_fifo` 是 [Concurrency Kit](https://github.com/concurrencykit/ck) 提供的无锁 FIFO 队列实现：
- **无锁设计**：多生产者多消费者，无需互斥锁
- **高并发**：适合大量并发读取请求
- **Rust 绑定完整支持**：通过 `bridge_fifo.cpp` 实现

Go 绑定的 FIFO 接口保留是为了 API 兼容性，未来可能会在 C API 层添加完整实现。

## 线程安全

- `DB`: 支持并发读，**不支持**并发写
- `Fifo`: 接口设计为线程安全（无锁队列）
- `Node`: **不**线程安全，跨 goroutine 使用前需要 `Clone()`

## 架构

```
┌─────────────────────────────────────────────────────────┐
│                     Go Application                       │
├─────────────────────────────────────────────────────────┤
│                   monaddb (CGO)                         │
│  db.go | node.go | fifo.go | errors.go                 │
├─────────────────────────────────────────────────────────┤
│              C API (nomad_mpt.h/.cpp)                   │
│  同步操作已实现 | FIFO 为 stub                          │
├─────────────────────────────────────────────────────────┤
│                MonadDB C++ Core                          │
│  mpt::Db | mpt::Node | StateMachine                     │
└─────────────────────────────────────────────────────────┘

Rust 绑定（完整实现）:
┌─────────────────────────────────────────────────────────┐
│                   Rust Application                       │
├─────────────────────────────────────────────────────────┤
│              nomad-mpt-sys (cxx bridge)                 │
│  bridge.cpp (同步) | bridge_fifo.cpp (ck_fifo 异步)    │
├─────────────────────────────────────────────────────────┤
│                MonadDB C++ Core                          │
└─────────────────────────────────────────────────────────┘
```

## 许可证

Apache 2.0
