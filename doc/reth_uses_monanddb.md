# Reth Core Architecture: Payload Processing and State Root Calculation in Engine Tree

This document introduces the payload processing and state root calculation pipeline in Reth's **Engine Tree path**, detailing the data flow and collaboration patterns between the execution engine (`reth_evm`), state root calculation tasks (`MultiProofTask` / `SparseTrieTask`), execution cache (`ExecutionCache`), and the underlying database (MDBX).

> **Scope**: This document primarily covers the `PayloadProcessor` pipeline in **Live Sync** scenarios within Engine Tree. Pipeline batch sync mode is briefly introduced in [Section 3.2](#32-batch-sync-mode-pipeline-sync---optimized-for-throughput), but follows a different implementation path.

## 1. Core Architecture Diagram

```text
                                  ┌──────────────────────────┐
                                  │      Engine API / RPC    │
                                  └───────────▲──────────────┘
                                              │ State Root (verify/return)
                                              │
                      ┌───────────────────────┴───────────────────────┐
                      │   Merklizer (MultiProofTask / SparseTrieTask) │
                      │  ┌─────────────────────────────────────────┐  │
                      │  │          SparseStateTrie (state machine)│  │
                      │  └───────────────────▲─────────────────────┘  │
                      └──────────▲───────────┼───────────┬────────────┘
                                 │           │           │
                 EvmState stream │           │           │ Active Read
                 (state hook)    │  Prefetch │           │ (Hashed*/Trie tables)
                                 │  Proofs   │           ▼
    ┌────────────────────────────┴─┐  │  ┌───┴──────────────────────────┐
    │   Execution Engine (reth_evm)│  │  │   Execution Cache            │
    │  ┌─────────────────────────┐ │  │  │  - Account / Storage / Code  │
    │  │     Read-your-writes    │◄┼──┼──┤    read cache + prewarm +    │
    │  └─────────────────────────┘ │  │  │    cross-block reuse         │
    └──────────▲──────────┬────────┘  │  └─────────────┬────────────────┘
               │          │           │                │
   Active Read │          │ Produce   │                │ Batch Write (Tree Path)
   (PlainState)│          │ changes   │                │ (Engine Tree persistence)
               │          │(BundleState)               │
               │          ▼           │                ▼
    ┌──────────┴──────────────────────┼────────────────────────────────┐
    │              Underlying Storage │(MDBX / reth_db)                │
    │  ┌────────────┐ ┌────────────┐ ┌┴────────────┐ ┌───────────────┐ │
    │  │ PlainState │ │ ChangeSets │ │HashedState  │ │AccountsTrie / │ │
    │  │(flat state)│ │ (reverts)  │ │(hash shadow)│ │ StoragesTrie  │ │
    │  └────────────┘ └────────────┘ └─────────────┘ └───────────────┘ │
    └──────────────────────────────────────────────────────────────────┘

                      ┌───────────────────────────────────────────────┐
                      │         Prewarm Task (Optimistic Parallel)    │
                      │  - Each tx executed twice: Prewarm + Official │
                      │  - Core purpose: Fill ExecutionCache,         │
                      │    eliminate disk I/O                         │
                      │  - Side effect: Send PrefetchProofs for MPT   │
                      └───────────────────────────────────────────────┘
```

---

## 2. Component Responsibilities

### 2.1 Execution Engine (`reth_evm`)
*   **Code Location**: `crates/evm/`
*   **Role**: Producer. Responsible for running EVM bytecode and processing transaction logic.
*   **Read Strategy**: **Direct access to flat tables.** For maximum performance, the execution engine does not consult the Trie tree, but reads raw balances and data directly from MDBX's `PlainAccountState` / `PlainStorageState` tables or memory cache.
*   **Write Strategy**: **Memory-first.** Modifications are not written directly to the database, but accumulated in `BundleState`.

### 2.2 Execution Cache

> **⚠️ Naming Note**: There are **two `ExecutionCache` with the same name but different responsibilities** in Engine Tree code, which can be confusing.

| Location | Responsibility | Description |
|----------|----------------|-------------|
| `cached_state.rs` | Provider-layer read-through cache | Caches account/storage/code to reduce DB I/O |
| `payload_processor/mod.rs` | Cross-block cache slot manager | `Arc<RwLock<Option<SavedCache>>>`, manages reusability |

*   **Code Location**: `crates/engine/tree/src/tree/cached_state.rs` (Provider-layer cache)
*   **Role**: Read cache (read-through). Caches commonly used **account info / storage slots / contract bytecode** to reduce DB I/O during execution.
*   **Important Clarifications**:
    *   **`ExecutionCache` is NOT the source of "Read-your-writes" semantics.** The ability for subsequent transactions within the same block to see writes from previous transactions comes from the in-memory state during EVM execution (e.g., `BundleState` / `EvmState` inside `reth_revm`), not from this read cache.
    *   `ExecutionCache` primarily serves "read acceleration" and "prewarming"; after execution, block output can be inserted into the cache for reuse by the next block.
    *   It is a **provider-layer read-through cache** that can be reused by prewarm/execution, but does not guarantee intra-transaction consistency semantics.
*   **Scope**: Can serve a single block or span multiple blocks in chain-tip continuous payload scenarios (via `SavedCache` reuse).

### 2.3 Merklizer (Trie Tasks - `MultiProofTask` / `SparseTrieTask`)
*   **Code Location**: `crates/engine/tree/src/tree/payload_processor/`
*   **Role**: Consumer / async accountant. Responsible for calculating the Merkle Root (state root) to satisfy block header verification.
*   **Data Structure**: Internally drives **`SparseStateTrie`** (`crates/trie/sparse/src/state.rs`). This is a sparse in-memory tree capable of handling "Blind Nodes".
*   **Core Subcomponent - `MultiproofManager`**:
    *   **Code Location**: `crates/engine/tree/src/tree/payload_processor/multiproof.rs`
    *   **Responsibility**: Coordinates multiproof distribution. Handles deduplication of proof targets, routes work to storage or account worker pools, caches storage roots of missed leaves to avoid redundant computation (common for hot ERC-20 contracts).
    *   **Dual-Channel Design**: Uses two independent crossbeam channels—control channel (`tx`/`rx`) for orchestrating messages (PrefetchProofs, StateUpdate, EmptyProof, FinishedStateUpdates), and proof result channel (`proof_result_tx`/`proof_result_rx`) for workers to return computed results directly.
    *   **Chunking**: When `chunk_size` is configured and multiple workers are available (`available_account_workers > 1` or `available_storage_workers > 1`), `MultiProofTask` chunks proof targets and distributes them in parallel to the worker pool for increased parallelism. Chunking logic applies to both `on_prefetch_proof()` and `on_state_update()` paths.
*   **Message Types** (`MultiProofMessage`):
    *   `PrefetchProofs(targets)`: **From prewarm task**, prefetches proof targets for potentially needed trie paths. **Note: Only sent for the 2nd and subsequent transactions** (`index > 0`), not sent for the 1st transaction.
    *   `StateUpdate(source, EvmState)`: From execution side's state hook, carries transaction state changes.
    *   `EmptyProof`: Fast path, used when all targets have already been prefetched.
    *   `FinishedStateUpdates`: Signal, **automatically sent when `StateHookSender` is dropped**, indicating no more state updates.
*   **Collaboration Pattern**: Asynchronously collaborates with the execution engine via Channel:
    *   Execution side sends `MultiProofMessage::StateUpdate(StateChangeSource, EvmState)` via state hook.
    *   **Prewarm task** sends `MultiProofMessage::PrefetchProofs(targets)` to prefetch proofs (see `prewarm.rs`).
    *   `MultiProofTask` converts/hashes `EvmState` to `HashedPostState` and coordinates multiproof generation through `MultiproofManager` (reading proof nodes from DB/trie tables when necessary).
    *   Then assembles `(multiproof + state update)` into `SparseTrieUpdate` and sends to `SparseTrieTask`, which drives `SparseStateTrie` updates and produces the final state root (and optional `TrieUpdates`).

### 2.4 Prewarm Task (Dual Execution Architecture)

> **Core Design**: In Engine Tree mode, **each transaction is executed twice by the EVM**—this is Reth's core performance optimization strategy.

| Execution | Nature | Environment | Core Purpose |
| :--- | :--- | :--- | :--- |
| **1st (Prewarm)** | Optimistic parallel | Independent Worker thread | **Fill cache**: Pull state from disk to memory, prefetch MPT Proof nodes |
| **2nd (Official)** | Deterministic serial | Main execution thread | **Compute results**: Produce final `BundleState`, Receipts, trigger MPT updates |

*   **Code Location**: `crates/engine/tree/src/tree/payload_processor/prewarm.rs`
*   **Role**: Optimistic pre-execution. Eliminates the slowest part of the main sync path—**disk I/O wait**—through "redundant execution".
*   **Why execute twice?**
    *   **State dependency conflicts**: During parallel Prewarm, transaction N runs on the block's initial state without knowing the results of transactions 1~N-1, so Prewarm may produce incorrect results.
    *   **But accessed Keys are usually correct**: Even if data values are wrong, the "address/storage slot" Keys accessed by Prewarm are usually correct. Reth only needs to know which Keys will be accessed to preload them from disk to memory.
    *   **Architectural simplicity**: Compared to complex conflict detection/rollback logic in Block-STM and similar schemes, Reth trades "running once more" for a **single-threaded, sequential, deterministic** main execution thread, ensuring stability.
*   **Collaboration Pattern**:
    *   **Transaction flow "split in two"**: `PayloadProcessor` sends each transaction to both Prewarm channel and official execution channel (see code below).
    *   Uses `ExecutionCache` (`SavedCache`) for prewarm execution, filling accessed state into cache.
    *   **Conditional `PrefetchProofs` sending**: Only sends `MultiProofMessage::PrefetchProofs(targets)` **after the first transaction** (`index > 0`) and when multiproof channel exists. Not sent for the first transaction.
    *   **Cache saving requires explicit handoff**: Saving warmed caches depends on receiving a `Terminate` event with the final `BundleState` from main execution; prewarm completion alone does not automatically persist the cache.
*   **Optimization Effects**:
    1.  **Eliminate disk I/O** (primary benefit): During the second execution, all needed `Account` and `Storage` are already in `ExecutionCache`, with nearly zero disk access.
    2.  **Improve multiproof hit rate** (secondary benefit): For the 2nd and subsequent transactions, prefetched proof targets can reduce state root calculation latency.
    3.  **Hit precompile cache**: Results of complex cryptographic computations (e.g., `ecRecover`) are also cached.

> **Code Evidence 1**: Transaction flow "split in two" (`payload_processor/mod.rs` lines 312-321)
> ```rust
> self.executor.spawn_blocking(move || {
>     for tx in transactions {
>         let tx = tx.map(|tx| WithTxEnv { tx_env: tx.to_tx_env(), tx: Arc::new(tx) });
>         // 1. Send to Prewarm task (parallel first execution)
>         if let Ok(tx) = &tx {
>             let _ = prewarm_tx.send(tx.clone());
>         }
>         // 2. Send to official execution task (serial second execution)
>         let _ = execute_tx.send(tx);
>     }
> });
> ```

> **Code Evidence 2**: Conditional `PrefetchProofs` sending (`prewarm.rs` lines 498-506)
> ```rust
> if index > 0 {
>     let (targets, storage_targets) = multiproof_targets_from_state(res.state);
>     let _ = sender.send(PrewarmTaskEvent::Outcome { proof_targets: Some(targets) });
> }
> ```

### 2.5 Underlying Storage (MDBX)
*   **Code Location**: `crates/storage/db-api/src/tables/mod.rs` (table definitions)
*   **Role**: Single source of truth.
*   **Table Design**: Uses a "read-write separation" index design:
    *   **`PlainState`** (`PlainAccountState` / `PlainStorageState`): Supports execution speed, O(1) state queries.
    *   **`HashedState`** (`HashedAccounts` / `HashedStorages`): Supports MPT tree updates, sorted by hash.
    *   **`ChangeSets`** (`AccountChangeSets` / `StorageChangeSets`): Supports rollback and incremental hashing.
    *   **`*History`** (`AccountsHistory` / `StoragesHistory`): History index tables, combined with `ChangeSets` to support RPC historical state queries.
    *   **`*Trie`** (`AccountsTrie` / `StoragesTrie`): Stores MPT intermediate node hashes, supports state root verification and incremental updates.

---

## 3. Data Flow: From Execution to Persistence

Reth has two main sync paths with **completely different** state root calculation methods:

### 3.1 Live Sync Mode (Engine Tree Path) - Optimized for Low Latency

> **This document primarily describes this path.**

1.  **Prewarm** (optimistic parallel): `PrewarmCacheTask` **executes all transactions in parallel** in independent Worker threads (first execution). The core purpose is to pull state from disk to `ExecutionCache`, eliminating disk I/O for subsequent main execution. Additionally sends `PrefetchProofs` to `MultiProofTask` for **the 2nd and subsequent transactions** to prefetch MPT proofs.
2.  **Execute** (deterministic serial): `reth_evm` **executes all transactions serially** in the main execution thread (second execution). At this point, state is already in `ExecutionCache`, with nearly zero disk I/O. Changes accumulate in memory state (`BundleState`), and `EvmState` update streams are produced **per-transaction** via state hook.
3.  **Dispatch**: Execution side sends `MultiProofMessage::StateUpdate(StateChangeSource::Transaction(i), EvmState)` to background tasks via memory channel.
4.  **Compute**: `MultiProofTask` generates multiproof (when necessary) and assembles updates into `SparseTrieUpdate`; `SparseTrieTask` drives `SparseStateTrie` to update memory structures/hashes.
5.  **Finish Signal**: When `StateHookSender` is dropped, `FinishedStateUpdates` signal is automatically sent, notifying background tasks that execution is complete.
6.  **Return**: Computed `stateRoot` is returned to Engine API / block header verification logic.
7.  **Persist**: After verification passes, Engine Tree persistence logic **typically** hands execution output (`BundleState`) and trie updates to the persistence layer for batch writing to MDBX.

**Key Characteristics**:
*   Uses **channel async collaboration** (`MultiProofTask` / `SparseTrieTask`)
*   State root calculation runs **in parallel** with execution
*   **prewarm + PrefetchProofs** significantly improves multiproof hit rate
*   Persistence is handled by Engine Tree persistence logic (not Pipeline Stage)

### 3.2 Batch Sync Mode (Pipeline Sync) - Optimized for Throughput

> **This is a separate implementation path that does not use `MultiProofTask` / channel mechanism.**

1.  **Batch Execute** (`ExecutionStage`): Runs thousands of blocks consecutively, only reading/writing `PlainAccountState` / `PlainStorageState` and `AccountChangeSets` / `StorageChangeSets`, **completely not updating Trie**.
2.  **Centralized Hashing** (`AccountHashingStage` / `StorageHashingStage`): Reads `ChangeSets`, batch updates `HashedAccounts` / `HashedStorages`.
3.  **Merkle Stage** (`MerkleStage`): **Directly consumes `ChangeSets` and `Hashed*` tables from database**, reconstructs and computes the final `stateRoot` in one pass, then verifies against block header.

> **`MerkleStage` Details**: `MerkleStage` is actually an **enum** with multiple variants:
> - `MerkleStage::Execution`: Computes state root during forward execution
> - `MerkleStage::Unwind`: Restores trie state during rollback
>
> And can switch between **incremental** and **rebuild** modes based on thresholds.

**Key Characteristics**:
*   **Does not use channels**, but Stage sequential execution
*   Execution and Merkle calculation run **serially**
*   Persistence is handled by each Stage's Writer (`StateWriter`, etc.)


## 4. Core Database Tables

### 4.1 PlainState (Flat State Tables)
*   **Code Reference**: `PlainAccountState` and `PlainStorageState` in `crates/storage/db-api/src/tables/mod.rs`
*   **Contents**:
    *   `PlainAccountState`: `Address` -> `Account` (Balance, Nonce, CodeHash).
    *   `PlainStorageState`: `Address` + `SlotKey` -> `StorageEntry`.
*   **Function**: Provides the fastest $O(1)$ state reads.
*   **Readers**: **State Provider** (`crates/storage/provider/src/providers/state/latest.rs`). Queries balances and contract data by raw address during transaction execution.
*   **Writers**:
    *   **Tree Path**: Engine Tree persistence logic
    *   **Pipeline Path**: `ExecutionStage` via `StateWriter`

### 4.2 ChangeSets (Change Record Tables)
*   **Code Reference**: `AccountChangeSets` and `StorageChangeSets`
*   **Contents**: `BlockNumber` -> `Address/Slot` + `OldValue`.
*   **Functions**:
    1.  **Rollback**: When chain reorganization occurs, restores `PlainState` to old versions using this table.
    2.  **Incremental Driver**: In Pipeline mode, drives `HashingStage` / `MerkleStage` to compute hashes.
*   **Readers**: **Rollback logic, Pipeline's Hashing/Merkle Stages, historical state queries**.
*   **Writers**:
    *   **Tree Path**: Engine Tree persistence logic (execution produces `BundleState`, persisted by upper layer)
    *   **Pipeline Path**: `ExecutionStage` via `StateWriter::write_state_reverts()`

### 4.3 History Index Tables
*   **Code Reference**: `AccountsHistory` and `StoragesHistory`
*   **Contents**: Sharded index recording which blocks each address/slot changed in.
*   **Function**: Combined with `ChangeSets` to support **RPC historical state queries** (e.g., `eth_call` at block N).
*   **Readers**: **Historical State Provider** (`crates/storage/provider/src/providers/state/historical.rs`). First queries History table to locate block, then reads values from ChangeSets.
*   **Writers**: **Pipeline's `IndexAccountHistoryStage` / `IndexStorageHistoryStage`**.

### 4.4 HashedState (Hash Shadow Tables)
*   **Code Reference**: `HashedAccounts` and `HashedStorages`
*   **Contents**:
    *   `HashedAccounts`: `Keccak256(Address)` -> `Account`.
    *   `HashedStorages`: `Keccak256(Address)` + `Keccak256(Slot)` -> `StorageEntry`.
*   **Function**: Serves as the **leaf node data source** for the MPT tree. Since MPT paths are hash-based, this table pre-computes the hash mapping.
*   **Readers**: **Merklizer / Trie Cursor** (`crates/trie/db/src/hashed_cursor.rs`).
*   **Writers**:
    *   **Tree Path**: Engine Tree persistence logic (via `write_hashed_state()`)
    *   **Pipeline Path**: `AccountHashingStage` / `StorageHashingStage`

### 4.5 Trie Node Tables
*   **Code Reference**: `AccountsTrie` and `StoragesTrie` (and corresponding `*TrieChangeSets`)
*   **Contents**: `HashedPath (Nibbles)` -> `BranchNodeCompact` / `StorageTrieEntry`.
*   **Function**: Stores MPT **intermediate node hashes**, supports incremental state root calculation.
*   **Readers**: **Trie Cursor** (`crates/trie/db/src/trie_cursor.rs`). When computing Root, if encountering "blind nodes" not in memory, loads intermediate hashes from this table for reuse.
*   **Writers**:
    *   **Tree Path**: Engine Tree persistence logic (writes `TrieUpdates`)
    *   **Pipeline Path**: `MerkleStage`

---

## 5. BundleState and EvmState Details

`BundleState` and `EvmState` are core structures for expressing state changes during execution.

### 5.1 Code Origin
*   **`BundleState`** and **`EvmState`** are both **native types from the `revm` library**, re-exported through `reth_revm`:
    *   `reth_revm::db::BundleState` (actually from `revm::database::BundleState`)
    *   `reth_revm::state::EvmState` (actually from `revm`'s state module)
*   Re-export code in `crates/revm/src/lib.rs`: `pub use revm::{database as db, ...}`
*   `MultiProofMessage::StateUpdate` actually carries `EvmState` (see `crates/engine/tree/src/tree/payload_processor/multiproof.rs`).

### 5.2 Differences and Relationships
*   **`EvmState`**: Represents state changes produced by **a single transaction** during EVM execution. Streamed in real-time **per-transaction** (`StateChangeSource::Transaction(i)`) to background multiproof tasks via state hook.
*   **`BundleState`**: Accumulated state changes packaging structure (for execution/commit), essentially an in-memory "change bundle". Used for cache updates and final commits.

In the current Engine Tree state root calculation pipeline, **the incremental payload sent to background tasks via channel is `EvmState` (not `BundleState`)**; `BundleState` is still used for cache updates and final commits.

> **Granularity Note**: State hook call granularity is typically per-transaction, i.e., called once after each transaction completes `on_state(StateChangeSource::Transaction(i), &state)`.

### 5.3 BundleState Contents
1.  **State**: A Map containing `Address -> BundleAccount`. `BundleAccount` records the account's latest `AccountInfo` and all storage changes (`StorageSlot`) for that account.
2.  **Contracts**: A Map recording newly created contracts' `B256(CodeHash) -> Bytecode`.

### 5.4 Data Flow Division
*   **Execution → State Root Task**: Channel sends `EvmState` (state hook output), `MultiProofTask` hashes and coordinates multiproof, then hands to `SparseTrieTask` to update `SparseStateTrie`.
*   **Execution → Cache/Commit**: `BundleState` is used to express the final change set after execution, for cache updates and persistence/commit.


## 6. Collaboration Flow Summary (Engine Tree Path)

> The following table describes the collaboration flow for the **Engine Tree (Live Sync) path**. Pipeline path uses a different Stage sequential execution mechanism.

| Phase | Action Description | Components Involved | Tables/Data Structures |
| :--- | :--- | :--- | :--- |
| **0. Prewarm** (optimistic parallel) | **First execution**: Prewarm task executes all transactions in parallel in Worker threads, **core purpose** is filling `ExecutionCache` with state to eliminate disk I/O; additionally sends `PrefetchProofs` for 2nd+ transactions to prefetch MPT proofs. | `PrewarmCacheTask` | `ExecutionCache` + `MultiProofMessage::PrefetchProofs(targets)` |
| **1. Execute** (deterministic serial) | **Second execution**: Main execution thread executes all transactions serially, state is already in `ExecutionCache` (nearly zero disk I/O); changes accumulate in `BundleState` / memory state. | `reth_evm` + `ExecutionCache` | `PlainAccountState` / `PlainStorageState` + `BundleState` |
| **2. Dispatch** | Via state hook, **per-transaction** streams `EvmState` updates to background multiproof task. | `PayloadHandle::state_hook` | `MultiProofMessage::StateUpdate(StateChangeSource::Transaction(i), EvmState)` |
| **3. Verify** | `MultiProofTask` coordinates multiproof distribution via `MultiproofManager` (dedup, route to worker pool) and generates `SparseTrieUpdate`; `SparseTrieTask` updates sparse tree and computes state root. | `MultiProofTask` + `MultiproofManager` + `SparseTrieTask` + `SparseStateTrie` | `SparseTrieUpdate` + multiproof + `AccountsTrie` / `StoragesTrie` |
| **4. Finish Signal** | `StateHookSender` **automatically sends `FinishedStateUpdates` when dropped**, notifying background tasks execution is complete. | `StateHookSender::drop()` | `MultiProofMessage::FinishedStateUpdates` |
| **5. Commit** | After verification passes, Engine Tree persistence logic batch writes final results to MDBX. | Engine Tree persistence logic | `BundleState` / `TrieUpdates` -> MDBX (Plain*/Hashed*/Trie tables) |

---

## 7. Two Paths Comparison

| Dimension | Engine Tree Path (Live Sync) | Pipeline Path (Batch Sync) |
| :--- | :--- | :--- |
| **State Root Calculation** | `MultiProofTask` + `SparseTrieTask` + channel async | `MerkleStage` (enum with Execution/Unwind variants) directly consumes DB |
| **Execution vs Merkle** | Parallel (async channel) | Serial (Stage sequential execution) |
| **Prewarm/Prefetch** | ✅ Dual execution: `PrewarmCacheTask` (fill cache + `PrefetchProofs`) | ❌ None |
| **Persistence Responsibility** | Engine Tree persistence logic | Each Stage's `StateWriter` |
| **Use Case** | Chain-tip live sync, low latency | Historical block batch sync, high throughput |
| **Code Entry** | `crates/engine/tree/` | `crates/stages/stages/src/stages/` |

---

## 8. MonadDB Integration Proposal (Experimental)

> **Background**: MonadDB is a high-performance versioned MPT engine with built-in Merkle Patricia Trie structure, supporting historical state queries by `version` (= block_number). Replacing the existing MDBX state tables with MonadDB can significantly simplify the architecture.

### 8.1 Architecture Diagram

```text
┌─────────────────────────────────────────────────────────────────────────────────┐
│                 Engine Tree Architecture with MonadDB Integration               │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│                              ┌──────────────────────────┐                       │
│                              │      Engine API / RPC    │                       │
│                              └───────────▲──────────────┘                       │
│                                          │ State Root (verify/return)           │
│                                          │                                      │
│  ┌───────────────────────────────────────┴────────────────────────────────────┐ │
│  │                          BundleState → Update                              │ │
│  │                                  │                                         │ │
│  │                                  ▼                                         │ │
│  │                    db.upsert_with_root(prev_root, updates, block_number)   │ │
│  │                                  │                                         │ │
│  │                                  ▼                                         │ │
│  │                           state_root returned                              │ │
│  └────────────────────────────────────────────────────────────────────────────┘ │
│                                          ▲                                      │
│                                          │                                      │
│  ┌───────────────────────────────────────┴────────────────────────────────────┐ │
│  │                      Official Execution (serial deterministic)             │ │
│  │                                                                            │ │
│  │   for tx in transactions {                                                 │ │
│  │       result = evm.execute(tx, state_provider);  // query via FifoHandle   │ │
│  │       bundle_state.merge(result);                                          │ │
│  │   }                                                                        │ │
│  └───────────────────────────────────────┬────────────────────────────────────┘ │
│                                          │                                      │
│                     StateRequest         │         StateRequest                 │
│                     (key, version,       │         (key, version,               │
│                      oneshot_tx)         │          oneshot_tx)                 │
│                          │               │               │                      │
│  ┌───────────────────────┴───────────────┴───────────────┴────────────────────┐ │
│  │                        request_tx (mpsc channel)                           │ │
│  │                      multiple producers → single consumer                  │ │
│  └───────────────────────────────────────┬────────────────────────────────────┘ │
│                                          │                                      │
│                                          ▼                                      │
│  ┌────────────────────────────────────────────────────────────────────────────┐ │
│  │                           FifoCtrl Thread                                  │ │
│  │  ┌──────────────────────────────────────────────────────────────────────┐  │ │
│  │  │  pending_requests: HashMap<u128, oneshot::Sender<Option<Vec<u8>>>>   │  │ │
│  │  │                                                                      │  │ │
│  │  │  loop {                                                              │  │ │
│  │  │    // 1. Collect requests → submit_find_value()                      │  │ │
│  │  │    // 2. Poll results → poll_batch() → return via oneshot            │  │ │
│  │  │  }                                                                   │  │ │
│  │  └──────────────────────────────────────────────────────────────────────┘  │ │
│  └───────────────────────────────────────┬────────────────────────────────────┘ │
│                                          │                                      │
│                                          ▼                                      │
│  ┌────────────────────────────────────────────────────────────────────────────┐ │
│  │                              MonadDB                                       │ │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────────┐ │ │
│  │  │   AsyncFifo     │  │  Versioned MPT  │  │  Built-in Merkle Root       │ │ │
│  │  │  (io_uring +    │  │  (query by      │  │  calculation                │ │ │
│  │  │   ck_fifo)      │  │   version)      │  │  (upsert_with_root returns  │ │ │
│  │  │                 │  │                 │  │   state_root directly)      │ │ │
│  │  └─────────────────┘  └─────────────────┘  └─────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                 │
│                                          ▲                                      │
│                                          │                                      │
│  ┌───────────────────────────────────────┴────────────────────────────────────┐ │
│  │                   Prewarm Worker Pool (optimistic parallel execution)      │ │
│  │                                                                            │ │
│  │   // Execute transactions in parallel, fill MonadDB internal cache         │ │
│  │   // Dynamically discovered storage accesses query via FifoHandle          │ │
│  │   // Results may be incorrect, but cache is prewarmed                      │ │
│  └────────────────────────────────────────────────────────────────────────────┘ │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 Architecture Changes Comparison

| Component | Current Architecture (MDBX) | MonadDB Architecture | Change Description |
| :--- | :--- | :--- | :--- |
| **State Storage** | 4 tables: `PlainState` / `ChangeSets` / `HashedState` / `*Trie` | **MonadDB single storage** | Greatly simplified, versioned queries replace ChangeSets |
| **State Root Calculation** | `MultiProofTask` + `SparseTrieTask` + channel async | `upsert_with_root()` **sync return** | Remove Merklizer, MonadDB has built-in MPT |
| **Prewarm Purpose** | Fill `ExecutionCache` + send `PrefetchProofs` | **Only fill MonadDB internal cache** | Remove MPT prefetch logic |
| **State Reading** | `LatestStateProvider` reads `PlainState` table | `MonadStateProvider` via `FifoHandle` | Async batch I/O |
| **Historical State Query** | `HistoricalStateProvider` reads `ChangeSets` + `*History` | `submit_find_value(key, version)` | Native versioned query support |
| **Persistence** | `StateWriter` writes multiple tables | `update_finalized_version()` | Simplified to single API |

### 8.3 Core Components

#### 8.3.1 FifoCtrl Thread

**Responsibility**: Wraps MonadDB's `AsyncFifo`, providing a Rust-friendly async state query interface.

```rust
/// State read request
pub struct StateRequest {
    pub key: Vec<u8>,                              // keccak256(address) or nested key
    pub version: u64,                              // block_number - 1
    pub response_tx: oneshot::Sender<Option<Vec<u8>>>,
}

/// FifoCtrl handle, can be cloned to multiple consumers
#[derive(Clone)]
pub struct FifoHandle {
    request_tx: mpsc::UnboundedSender<StateRequest>,
}

impl FifoHandle {
    /// Async query
    pub async fn get(&self, key: Vec<u8>, version: u64) -> Option<Vec<u8>>;
    
    /// Sync query (blocking)
    pub fn get_blocking(&self, key: Vec<u8>, version: u64) -> Option<Vec<u8>>;
}
```

**Workflow**:
1. Collect requests from `request_rx` (non-blocking batch collection)
2. Call `fifo.submit_find_value()` to submit to MonadDB
3. Call `fifo.poll_batch()` to get results
4. Return to requesters via `oneshot` channel

#### 8.3.2 MonadStateProvider

**Responsibility**: Implements `StateProvider` trait as EVM's state backend.

```rust
pub struct MonadStateProvider {
    fifo: FifoHandle,
    version: u64,  // query version (typically block_number - 1)
}

impl StateProvider for MonadStateProvider {
    fn basic_account(&self, address: &Address) -> Result<Option<Account>> {
        let key = keccak256(address);
        let data = self.fifo.get_blocking(key.to_vec(), self.version);
        // Decode and return
    }
    
    fn storage(&self, address: &Address, slot: &B256) -> Result<Option<U256>> {
        let key = storage_key(address, slot);  // nested key
        let data = self.fifo.get_blocking(key.to_vec(), self.version);
        // Decode and return
    }
}
```

#### 8.3.3 State Hook Directly Produces Update (Whole Block Write)

**Optimization Idea**: Remove `BundleState` intermediate state, let State Hook directly convert `EvmState` to `Update` and accumulate, then write entire block to MonadDB after all transactions complete.

```text
Original flow (with intermediate state):
  EVM → EvmState → BundleState (accumulate) → bundle_state_to_updates() → Update → MonadDB

Optimized flow (no intermediate state):
  EVM → EvmState → evm_state_to_updates() → Vec<Update> (accumulate) → MonadDB
```

**Implementation**:

```rust
/// Convert single transaction's EvmState directly to Update
fn evm_state_to_updates(state: &EvmState) -> Vec<Update> {
    state.iter().map(|(address, account)| {
        let account_key = keccak256(address);
        
        // Storage slot changes (nested updates)
        let storage_updates: Vec<Update> = account.storage.iter()
            .map(|(slot, value)| {
                let slot_key = keccak256(slot);
                if value.present_value.is_zero() {
                    Update::delete(&slot_key)
                } else {
                    Update::put(&slot_key, &value.present_value.to_be_bytes::<32>())
                }
            })
            .collect();
        
        Update::put(&account_key, &encode_account(&account.info))
            .with_nested(storage_updates)
    }).collect()
}

/// Optimized State Hook: directly produces Update and accumulates
fn create_state_hook(
    update_accumulator: Arc<Mutex<Vec<Update>>>,
) -> impl OnStateHook {
    move |_source: StateChangeSource, state: &EvmState| {
        let updates = evm_state_to_updates(state);
        update_accumulator.lock().unwrap().extend(updates);
    }
}

/// Execute block (whole block write)
fn execute_block(block: &Block, db: &mut Db, prev_root: &Node) -> [u8; 32] {
    let update_accumulator = Arc::new(Mutex::new(Vec::new()));
    let state_hook = create_state_hook(update_accumulator.clone());
    
    // Execute all transactions serially, State Hook produces Update per-tx and accumulates
    for tx in &block.transactions {
        evm.execute_with_hook(tx, &state_hook);
    }
    
    // Execution complete, write entire block to MonadDB
    let all_updates = update_accumulator.lock().unwrap();
    let new_root = db.upsert_with_root(
        Some(prev_root),
        &all_updates,
        block.number,
    ).unwrap();
    
    new_root.root_hash()
}
```

**Benefits**:
- Remove `BundleState` intermediate accumulation structure
- Remove `bundle_state_to_updates()` conversion step
- Whole block write leverages MonadDB's batch optimization

#### 8.3.4 Simplified Prewarm

**Responsibility**: Retain optimistic parallel execution, but remove MPT prefetch logic.

```rust
fn prewarm_task(
    fifo: FifoHandle,
    transactions: Receiver<Transaction>,
    version: u64,
) {
    let provider = MonadStateProvider::new(fifo, version);
    
    // Execute transactions in parallel
    transactions.par_iter().for_each(|tx| {
        // Optimistic execution, results may be incorrect
        // But state accessed via FifoHandle will be cached by MonadDB
        let _ = execute_transaction(&provider, tx);
    });
    
    // Note: No longer sends PrefetchProofs since MonadDB has built-in MPT
}
```

### 8.4 Execution Flow Comparison

#### Current Flow (MDBX + Merklizer)

```text
Tx flow ──┬──► Prewarm (parallel) ──► ExecutionCache + PrefetchProofs
          │                                │
          │                                ▼
          │                          MultiProofTask ◄─── channel ───┐
          │                                │                        │
          └──► Official Execution (serial) ──► EvmState ────────────┘
                      │
                      ▼
                BundleState
                      │
                      ▼
          StateWriter writes 4 tables ──► MDBX
```

#### MonadDB Flow (Optimized)

```text
Tx flow ──┬──► Prewarm (parallel) ──► Query via FifoHandle ──► MonadDB internal cache warmup
          │
          │
          └──► Official Execution (serial) ──► Query via FifoHandle (cache hit)
                      │
                      ▼
                State Hook produces Update per-tx
                      │
                      ▼
                Vec<Update> accumulate (no BundleState intermediate)
                      │
                      ▼
          db.upsert_with_root(prev_root, updates, block_number)
                      │
                      ▼
                state_root returned
                      │
                      ▼
          db.update_finalized_version(finalized_block)  // after consensus confirmation
```

### 8.5 Required Code Changes

| Module | File Path | Change Description |
| :--- | :--- | :--- |
| **Add FifoCtrl** | `crates/engine/tree/src/tree/fifo_ctrl.rs` | Implement `FifoCtrl` thread and `FifoHandle` |
| **Add MonadStateProvider** | `crates/storage/provider/src/providers/state/monad.rs` | Implement `StateProvider` based on FifoHandle |
| **Modify PayloadProcessor** | `crates/engine/tree/src/tree/payload_processor/mod.rs` | Remove `MultiProofTask` / `SparseTrieTask`, change to whole block write to MonadDB |
| **Modify State Hook** | `crates/engine/tree/src/tree/payload_processor/mod.rs` | State Hook directly produces `Update` and accumulates (remove `BundleState` intermediate) |
| **Add EvmState Conversion** | `crates/engine/tree/src/tree/monad_adapter.rs` | Implement `evm_state_to_updates()` (per-tx conversion, not whole block) |
| **Simplify Prewarm** | `crates/engine/tree/src/tree/payload_processor/prewarm.rs` | Remove `PrefetchProofs` logic, retain optimistic execution |
| **Modify Persistence Logic** | `crates/engine/tree/src/tree/persistence.rs` | Change to call `update_finalized_version()` |
| **Optional: Remove MDBX State Tables** | `crates/storage/db-api/src/tables/mod.rs` | Remove `PlainState` / `ChangeSets` / `HashedState` / `*Trie` table definitions |

### 8.6 Considerations

1. **Execution vs State Root Parallelism**:
   - Current architecture: Execution and Merkle calculation run **in parallel** (via channel)
   - MonadDB architecture: **Sync** call to `upsert_with_root()` after all transactions complete
   - **Impact**: Single block latency may slightly increase, but architecture is greatly simplified

2. **Per-Transaction State Root**:
   - If needed (e.g., tracing), need to call `upsert_with_root()` after each transaction
   - This reduces efficiency, recommended only in debug mode

3. **Fork Scenarios (Reorg)**:
   - MonadDB supports `rewind_to_version()` for rollback
   - Same version with multiple roots scenario needs `find_from_root()` instead of FIFO

4. **History Pruning**:
   - MonadDB's `history_length` config controls how many historical versions to retain
   - Versions beyond this are automatically cleaned, needs alignment with Reth's pruning strategy

### 8.7 Expected Benefits

| Dimension | Expected Benefit |
| :--- | :--- |
| **Code Complexity** | Greatly reduced: Remove Merklizer (`MultiProofTask` / `SparseTrieTask` / `MultiproofManager`) |
| **Storage Efficiency** | Reduce table count: 4 tables → 1 MonadDB instance |
| **I/O Performance** | MonadDB's `io_uring` + `ck_fifo` async I/O may outperform MDBX sync read/write |
| **Historical Query** | Native versioned query support, no need for ChangeSets + History indexes |
| **State Root Calculation** | MonadDB built-in MPT, `upsert_with_root()` returns directly |

