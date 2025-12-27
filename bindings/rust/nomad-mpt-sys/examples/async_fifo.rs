//! 异步 FIFO 测试示例
//!
//! 演示 ck_fifo 批量异步操作
//!
//! 注意：异步 find 只支持磁盘模式（需要 huge pages）
//! 内存模式下应使用同步 API

use nomad_mpt_sys::{Db, Update};
use std::time::Instant;

fn main() {
    println!("=== MonadDB 异步 FIFO 测试 ===\n");
    
    // 1. 创建数据库并写入测试数据
    println!("[1] 创建内存数据库并写入测试数据...");
    let mut db = Db::open_memory().expect("Failed to open database");
    
    // 写入 1000 条测试数据
    let num_records = 1000;
    
    // 先创建所有 key-value 对，保持它们的生命周期
    let kv_pairs: Vec<(Vec<u8>, Vec<u8>)> = (0..num_records)
        .map(|i| {
            let key = format!("key{:08}", i);
            let value = format!("value{:08}", i);
            (key.into_bytes(), value.into_bytes())
        })
        .collect();
    
    // 创建 Update 引用
    let updates: Vec<Update> = kv_pairs.iter()
        .map(|(k, v)| Update::put(k, v))
        .collect();
    
    let root = db.upsert(&updates, 1).expect("Failed to upsert");
    println!("   ✅ 写入 {} 条记录，root hash: {}",
        num_records,
        hex(&root.data())
    );
    
    // 2. 创建异步 FIFO（基础设施测试）
    println!("\n[2] 创建异步 FIFO 通道...");
    let fifo = db.create_async_fifo().expect("Failed to create async fifo");
    fifo.start(4);  // 4 个 worker 线程
    println!("   ✅ FIFO 已启动，4 个 worker 线程");
    
    // 3. 测试 FIFO 基础设施
    println!("\n[3] FIFO 基础设施说明:");
    println!("   ⚠️  注意：db.find() 只支持磁盘模式");
    println!("   ⚠️  内存模式下请使用同步 API");
    println!("   ✅ FIFO 基础设施已就绪");
    
    // 4. 演示内存模式同步批量写入性能
    println!("\n[4] 内存模式同步批量写入性能测试...");
    
    let batch_size = 100;
    let num_batches = 10;
    let start = Instant::now();
    
    for batch in 0..num_batches {
        // 创建 key-value 对
        let batch_kv: Vec<(Vec<u8>, Vec<u8>)> = (0..batch_size)
            .map(|i| {
                let idx = batch * batch_size + i;
                let key = format!("newkey{:08}", idx);
                let value = format!("newvalue{:08}", idx);
                (key.into_bytes(), value.into_bytes())
            })
            .collect();
        
        // 创建 Update 引用
        let updates: Vec<Update> = batch_kv.iter()
            .map(|(k, v)| Update::put(k, v))
            .collect();
        
        let _ = db.upsert(&updates, (batch + 2) as u64);
    }
    
    let elapsed = start.elapsed();
    let total_ops = num_batches * batch_size;
    let ops_per_sec = (total_ops as f64) / elapsed.as_secs_f64();
    
    println!("   ✅ 完成 {} 次 upsert (共 {} 条记录)", num_batches, total_ops);
    println!("   ⏱️  总耗时: {:?}", elapsed);
    println!("   📊 吞吐量: {:.0} records/sec", ops_per_sec);
    
    // 5. 停止 FIFO
    println!("\n[5] 停止 FIFO...");
    fifo.stop();
    println!("   ✅ FIFO 已停止");
    
    // 显式 drop FIFO 和 DB，确保顺序正确
    drop(fifo);
    drop(db);
    
    // 6. 磁盘模式说明
    println!("\n[6] 磁盘模式异步查询说明:");
    println!("   要使用异步 find，需要:");
    println!("   1. 配置系统 huge pages:");
    println!("      echo 512 | sudo tee /proc/sys/vm/nr_hugepages");
    println!("   2. 使用磁盘模式打开数据库:");
    println!("      let db = Db::open(DbConfig::disk(\"/path/to/db\"))?;");
    println!("   3. 创建异步 FIFO 并查询:");
    println!("      let fifo = db.create_async_fifo()?;");
    println!("      fifo.start(4);");
    println!("      let id = fifo.submit_find_value(key, version);");
    println!("      if let Some(result) = fifo.poll() {{ ... }}");
    
    println!("\n=== 测试完成 ===");
}

fn hex(data: &[u8]) -> String {
    if data.is_empty() {
        return "(empty)".to_string();
    }
    data.iter()
        .take(16)
        .map(|b| format!("{:02x}", b))
        .collect::<Vec<_>>()
        .join("")
        + if data.len() > 16 { "..." } else { "" }
}
