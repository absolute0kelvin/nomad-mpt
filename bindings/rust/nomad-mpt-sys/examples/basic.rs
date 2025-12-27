//! 基本用法示例
//! 
//! 运行: cargo run --example basic
//!
//! 演示内存模式的完整功能：
//! - 单条更新
//! - 批量更新
//! - 嵌套 trie 更新（以太坊账户存储）
//! - Merkle hash 验证

use nomad_mpt_sys::{Db, DbConfig, Update};
use std::path::Path;

fn main() {
    println!("=== MonadDB FFI 完整测试 ===\n");
    
    // 1. 基本单条更新
    test_single_update();
    
    // 2. 批量更新测试
    test_batch_update();
    
    // 3. 嵌套 trie 测试（以太坊账户存储）
    test_nested_trie();
    
    // 4. Merkle hash 一致性测试
    test_merkle_consistency();
    
    // 5. 磁盘模式测试
    test_disk_mode();
    
    // 6. 统计信息测试
    test_stats();
    
    println!("\n=== 所有测试完成 ===");
}

fn test_single_update() {
    println!("========== 1. 单条更新测试 ==========\n");
    
    let mut db = Db::open_memory().expect("Failed to open db");
    println!("   ✓ 数据库打开成功");
    
    let key: [u8; 32] = [0x01; 32];
    let value = b"hello world";
    
    let updates = vec![Update::put(&key, value)];
    
    match db.upsert(&updates, 1) {
        Ok(root) => {
            println!("   ✓ 单条更新成功");
            let data = root.data();
            println!("   根节点数据: {} 字节", data.len());
            if data.len() == 32 {
                println!("   Merkle hash: 0x{}", hex(&data));
            }
        }
        Err(e) => {
            println!("   ✗ 更新失败: {}", e);
        }
    }
}

fn test_batch_update() {
    println!("\n========== 2. 批量更新测试 ==========\n");
    
    let mut db = Db::open_memory().expect("Failed to open db");
    
    // 创建 10 个不同的 key-value 对
    let mut updates = Vec::new();
    let keys: Vec<[u8; 32]> = (0..10).map(|i| {
        let mut key = [0u8; 32];
        key[0] = i as u8;
        key[31] = (255 - i) as u8;
        key
    }).collect();
    
    let values: Vec<Vec<u8>> = (0..10).map(|i| {
        format!("value_{:02}", i).into_bytes()
    }).collect();
    
    for i in 0..10 {
        updates.push(Update::put(&keys[i], &values[i]));
    }
    
    println!("   批量插入 {} 条记录...", updates.len());
    
    let root1 = match db.upsert(&updates, 1) {
        Ok(root) => {
            println!("   ✓ 批量更新成功");
            let data = root.data();
            if data.len() == 32 {
                println!("   State Root: 0x{}", hex(&data));
            }
            root
        }
        Err(e) => {
            println!("   ✗ 批量更新失败: {}", e);
            return;
        }
    };
    
    // 增量更新：修改部分 key，添加新 key
    println!("\n   增量更新（修改 3 条 + 新增 2 条）...");
    
    let mut updates2 = Vec::new();
    
    // 修改 key[0], key[1], key[2]
    updates2.push(Update::put(&keys[0], b"modified_0"));
    updates2.push(Update::put(&keys[1], b"modified_1"));
    updates2.push(Update::put(&keys[2], b"modified_2"));
    
    // 新增 2 个 key
    let new_key1: [u8; 32] = [0xAA; 32];
    let new_key2: [u8; 32] = [0xBB; 32];
    updates2.push(Update::put(&new_key1, b"new_value_1"));
    updates2.push(Update::put(&new_key2, b"new_value_2"));
    
    let root2 = match db.upsert_with_root(Some(&root1), &updates2, 2) {
        Ok(root) => {
            println!("   ✓ 增量更新成功");
            let data = root.data();
            if data.len() == 32 {
                println!("   New State Root: 0x{}", hex(&data));
            }
            root
        }
        Err(e) => {
            println!("   ✗ 增量更新失败: {}", e);
            return;
        }
    };
    
    // 删除测试 - 需要传入当前的 root 节点
    println!("\n   删除操作测试...");
    let updates3 = vec![Update::delete(&keys[0])];
    
    match db.upsert_with_root(Some(&root2), &updates3, 3) {
        Ok(root) => {
            println!("   ✓ 删除操作成功");
            let data = root.data();
            if data.len() == 32 {
                println!("   After Delete Root: 0x{}", hex(&data));
            }
        }
        Err(e) => {
            println!("   ✗ 删除操作失败: {}", e);
        }
    }
}

fn test_nested_trie() {
    println!("\n========== 3. 嵌套 Trie 测试 (以太坊账户存储) ==========\n");
    
    let mut db = Db::open_memory().expect("Failed to open db");
    
    // 模拟以太坊账户地址 (20 字节，这里用 32 字节填充)
    let account_addr: [u8; 32] = {
        let mut addr = [0u8; 32];
        // 模拟地址: 0xdead...beef
        addr[0..4].copy_from_slice(&[0xde, 0xad, 0xbe, 0xef]);
        addr
    };
    
    // 账户值 (RLP 编码的 nonce, balance, storageRoot, codeHash)
    // 这里简化为一个固定值
    let account_value = b"account_data_placeholder";
    
    // 存储槽更新 (嵌套 trie)
    let storage_slot_0: [u8; 32] = [0x00; 32];  // 存储槽 0
    let storage_slot_1: [u8; 32] = [0x01; 32];  // 存储槽 1
    let storage_value_0 = b"storage_value_slot_0";
    let storage_value_1 = b"storage_value_slot_1";
    
    // 构建嵌套更新
    let storage_updates = vec![
        Update::put(&storage_slot_0, storage_value_0),
        Update::put(&storage_slot_1, storage_value_1),
    ];
    
    // 账户更新，包含嵌套的存储更新
    let account_update = Update::put(&account_addr, account_value)
        .with_nested(storage_updates);
    
    println!("   账户地址: 0x{}", hex(&account_addr[0..4]));
    println!("   存储槽数量: 2");
    
    match db.upsert(&[account_update], 1) {
        Ok(root) => {
            println!("   ✓ 嵌套更新成功");
            let data = root.data();
            if data.len() == 32 {
                println!("   World State Root: 0x{}", hex(&data));
            }
        }
        Err(e) => {
            println!("   ✗ 嵌套更新失败: {}", e);
        }
    }
}

fn test_merkle_consistency() {
    println!("\n========== 4. Merkle Hash 一致性测试 ==========\n");
    
    // 相同的更新应该产生相同的 Merkle root
    let key: [u8; 32] = [0x42; 32];
    let value = b"consistency_test";
    
    let mut roots = Vec::new();
    
    for i in 0..3 {
        let mut db = Db::open_memory().expect("Failed to open db");
        let updates = vec![Update::put(&key, value)];
        
        match db.upsert(&updates, 1) {
            Ok(root) => {
                roots.push(root.data());
            }
            Err(e) => {
                println!("   ✗ 第 {} 次更新失败: {}", i + 1, e);
                return;
            }
        }
    }
    
    // 验证所有 root 相同
    let all_same = roots.windows(2).all(|w| w[0] == w[1]);
    
    if all_same && roots[0].len() == 32 {
        println!("   ✓ Merkle hash 一致性验证通过");
        println!("   Root: 0x{}", hex(&roots[0]));
    } else {
        println!("   ✗ Merkle hash 不一致!");
        for (i, root) in roots.iter().enumerate() {
            println!("     Root {}: 0x{}", i, hex(root));
        }
    }
}

fn test_disk_mode() {
    println!("\n========== 5. 磁盘模式测试 ==========\n");
    
    println!("注意: 磁盘模式需要 huge pages 系统配置");
    
    // 检查 huge pages 是否可用
    let hugepages_path = Path::new("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages");
    if let Ok(content) = std::fs::read_to_string(hugepages_path) {
        let nr_hugepages: i32 = content.trim().parse().unwrap_or(0);
        println!("   当前 huge pages: {}", nr_hugepages);
        if nr_hugepages == 0 {
            println!("   ⚠️  跳过磁盘模式测试 (无 huge pages)");
            return;
        }
    } else {
        println!("   ⚠️  跳过磁盘模式测试 (无法读取配置)");
        return;
    }
    
    let db_path = "/tmp/monad_ffi_test.mpt";
    if Path::new(db_path).exists() {
        std::fs::remove_file(db_path).ok();
    }
    
    let mut db = match Db::open(DbConfig::disk(db_path).with_create(true)) {
        Ok(db) => db,
        Err(e) => {
            println!("   ✗ 打开失败: {}", e);
            return;
        }
    };
    
    println!("   ✓ 磁盘数据库打开成功");
    
    let key: [u8; 32] = [0x01; 32];
    let value = b"disk_test_value";
    
    match db.upsert(&[Update::put(&key, value)], 1) {
        Ok(root) => {
            println!("   ✓ 磁盘模式写入成功");
            println!("   Root: 0x{}", hex(&root.data()));
        }
        Err(e) => {
            println!("   ✗ 写入失败: {}", e);
        }
    }
    
    drop(db);
    std::fs::remove_file(db_path).ok();
}

/// 将字节数组转换为十六进制字符串
fn hex(data: &[u8]) -> String {
    data.iter().map(|b| format!("{:02x}", b)).collect()
}

fn test_stats() {
    println!("\n========== 6. 统计信息测试 ==========\n");
    
    let mut db = Db::open_memory().expect("Failed to open db");
    
    // 先插入一些数据
    let keys: Vec<[u8; 32]> = (0..5).map(|i| [i as u8; 32]).collect();
    let values: Vec<Vec<u8>> = (0..5).map(|i| vec![i as u8; 64]).collect();
    let updates: Vec<Update> = keys.iter().zip(values.iter())
        .map(|(k, v)| Update::put(k, v))
        .collect();
    
    let root = db.upsert(&updates, 1).expect("upsert failed");
    
    // 获取统计信息
    let stats = db.stats();
    
    println!("   统计信息:");
    println!("     latest_version:    {}", stats.latest_version);
    println!("     earliest_version:  {}", stats.earliest_version);
    println!("     history_length:    {}", stats.history_length);
    println!("     is_on_disk:        {}", stats.is_on_disk);
    println!("     is_read_only:      {}", stats.is_read_only);
    println!("     finalized_version: {}", 
        if stats.finalized_version == u64::MAX { "未设置".to_string() } 
        else { stats.finalized_version.to_string() });
    
    // 测试 prefetch（内存模式会返回 0）
    let prefetched = db.prefetch(&root);
    println!("\n   prefetch 预加载节点: {}", prefetched);
    println!("   (内存模式预期返回 0)");
    
    // 验证只读状态
    println!("\n   is_read_only: {}", db.is_read_only());
    
    println!("\n   ✓ 统计信息测试完成");
}

