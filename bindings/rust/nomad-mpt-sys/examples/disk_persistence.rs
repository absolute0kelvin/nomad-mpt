//! MonadDB 持久化示例
//!
//! 运行: cargo run --example disk_persistence
//!
//! 演示:
//! - 基本读写操作
//! - 多版本状态管理
//! - 嵌套 Trie（以太坊账户存储）
//! - 批量写入性能
//! - 统计信息

use nomad_mpt_sys::{Db, Update};
use std::fs;

fn main() {
    println!("=== MonadDB 持久化测试 ===\n");
    
    // 检查 huge pages
    let has_hugepages = check_hugepages();
    
    if !has_hugepages {
        println!("⚠️  未配置 huge pages，将使用内存模式演示");
        println!("\n配置磁盘模式方法:");
        println!("  sudo sysctl -w vm.nr_hugepages=128");
        println!("  或永久配置: echo 'vm.nr_hugepages=128' | sudo tee -a /etc/sysctl.conf\n");
    }
    
    // 注意：当前 MonadDB 磁盘模式需要额外的系统配置
    // 这里直接使用内存模式进行演示
    println!("注意：磁盘模式需要额外的系统配置，本示例使用内存模式演示核心功能\n");
    
    // 使用内存模式演示所有功能
    test_memory_mode();
    
    println!("\n=== 所有测试完成 ===");
}

/// 内存模式完整演示
fn test_memory_mode() {
    println!("========== 内存模式演示 ==========\n");
    
    let mut db = Db::open_memory().expect("Failed to open memory db");
    
    println!("   ✓ 内存数据库打开成功");
    println!("   is_on_disk: {}", db.is_on_disk());
    println!("   is_read_only: {}", db.is_read_only());
    
    // 1. 基本写入
    println!("\n--- 1. 基本写入 ---");
    let accounts: Vec<([u8; 32], Vec<u8>)> = vec![
        (hex_to_bytes32("0x1111111111111111111111111111111111111111111111111111111111111111"), 
         b"account_1_balance:1000".to_vec()),
        (hex_to_bytes32("0x2222222222222222222222222222222222222222222222222222222222222222"),
         b"account_2_balance:2000".to_vec()),
        (hex_to_bytes32("0x3333333333333333333333333333333333333333333333333333333333333333"),
         b"account_3_balance:3000".to_vec()),
    ];
    
    let updates: Vec<Update> = accounts.iter()
        .map(|(k, v)| Update::put(k, v))
        .collect();
    
    let root = db.upsert(&updates, 1).expect("upsert failed");
    
    println!("   写入 {} 个账户", accounts.len());
    println!("   State Root: 0x{}", hex(&root.root_hash()));
    
    // 2. 多版本演示
    println!("\n--- 2. 多版本演示 ---");
    let key = hex_to_bytes32("0x1111111111111111111111111111111111111111111111111111111111111111");
    let mut prev_root = root;
    
    for version in 2u64..=5 {
        let new_balance = format!("account_1_balance:{}", 1000 + version * 100);
        let updates = vec![Update::put(&key, new_balance.as_bytes())];
        
        let new_root = db.upsert_with_root(Some(&prev_root), &updates, version)
            .expect("upsert failed");
        
        println!("   Version {}: Root = 0x{}", version, hex(&new_root.root_hash()[..8]));
        prev_root = new_root;
    }
    
    // 3. 嵌套 Trie（以太坊账户存储）
    println!("\n--- 3. 嵌套 Trie（以太坊账户存储）---");
    let account_addr = hex_to_bytes32("0xdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    let account_data = b"nonce:5,balance:1000000";
    
    let slot0: [u8; 32] = [0x00; 32];
    let slot1: [u8; 32] = [0x01; 32];
    
    let storage_updates = vec![
        Update::put(&slot0, b"storage_slot_0"),
        Update::put(&slot1, b"storage_slot_1"),
    ];
    
    let account_update = Update::put(&account_addr, account_data)
        .with_nested(storage_updates);
    
    let nested_root = db.upsert_with_root(Some(&prev_root), &[account_update], 6)
        .expect("nested upsert failed");
    
    println!("   账户地址: 0xdeadbeef...");
    println!("   存储槽数量: 2");
    println!("   World State Root: 0x{}", hex(&nested_root.root_hash()));
    
    // 4. 大批量写入
    println!("\n--- 4. 大批量写入性能 ---");
    let count = 5000usize;
    let keys: Vec<[u8; 32]> = (0..count).map(|i| {
        let mut key = [0u8; 32];
        key[0..4].copy_from_slice(&(i as u32).to_be_bytes());
        key
    }).collect();
    
    let values: Vec<Vec<u8>> = (0..count).map(|i| {
        format!("value_{:06}_padding", i).into_bytes()
    }).collect();
    
    let updates: Vec<Update> = keys.iter().zip(values.iter())
        .map(|(k, v)| Update::put(k, v))
        .collect();
    
    let start = std::time::Instant::now();
    let batch_root = db.upsert(&updates, 7).expect("batch upsert failed");
    let elapsed = start.elapsed();
    
    println!("   写入 {} 条记录: {:?}", count, elapsed);
    println!("   吞吐量: {:.0} ops/sec", count as f64 / elapsed.as_secs_f64());
    println!("   Batch State Root: 0x{}", hex(&batch_root.root_hash()[..8]));
    
    // 5. 统计信息
    println!("\n--- 5. 统计信息 ---");
    let stats = db.stats();
    println!("   latest_version: {}", stats.latest_version);
    println!("   earliest_version: {}", stats.earliest_version);
    println!("   history_length: {}", stats.history_length);
    println!("   is_on_disk: {}", stats.is_on_disk);
    println!("   is_read_only: {}", stats.is_read_only);
    println!("   finalized_version: {}", 
        if stats.finalized_version == u64::MAX { "未设置".to_string() }
        else { stats.finalized_version.to_string() });
    
    // 6. Prefetch（内存模式返回 0）
    println!("\n--- 6. Prefetch ---");
    let prefetched = db.prefetch(&batch_root);
    println!("   预加载节点: {} (内存模式预期为 0)", prefetched);
    
    println!("\n   ✓ 内存模式演示完成");
}

fn check_hugepages() -> bool {
    if let Ok(content) = fs::read_to_string("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages") {
        let nr: i32 = content.trim().parse().unwrap_or(0);
        println!("检测到 {} 个 2MB huge pages", nr);
        return nr > 0;
    }
    false
}

/// 将十六进制字符串转换为 32 字节数组
fn hex_to_bytes32(s: &str) -> [u8; 32] {
    let s = s.trim_start_matches("0x");
    let mut bytes = [0u8; 32];
    for i in 0..32 {
        bytes[i] = u8::from_str_radix(&s[i*2..i*2+2], 16).unwrap_or(0);
    }
    bytes
}

/// 将字节数组转换为十六进制字符串
fn hex(data: &[u8]) -> String {
    data.iter().map(|b| format!("{:02x}", b)).collect()
}
