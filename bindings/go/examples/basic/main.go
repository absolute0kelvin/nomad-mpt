// Example: Basic MonadDB operations in Go
//
// This example demonstrates:
// - Opening an in-memory database
// - Inserting key-value pairs (using 32-byte Ethereum-style keys)
// - Querying data
// - Batch operations
// - Merkle hash verification
//
// Build and run:
//   cd nomad-mpt/bindings/go
//   ./scripts/build.sh  # 首次运行需要构建
//   go run examples/basic/main.go
package main

import (
	"fmt"
	"log"

	"github.com/monad/nomad-mpt-go/monaddb"
)

// makeKey32 creates a 32-byte key (Ethereum-style)
func makeKey32(prefix byte, suffix byte) []byte {
	key := make([]byte, 32)
	key[0] = prefix
	key[31] = suffix
	return key
}

func main() {
	fmt.Println("=== MonadDB Go 绑定示例 ===\n")

	// 1. Open in-memory database
	fmt.Println("1. 打开内存数据库...")
	db, err := monaddb.OpenMemory()
	if err != nil {
		log.Fatalf("Failed to open database: %v", err)
	}
	defer db.Close()
	fmt.Println("   ✓ 数据库打开成功")

	// 2. Single put operation (32-byte key)
	fmt.Println("\n2. 单条插入测试...")
	helloKey := makeKey32(0x01, 0x00)  // 32-byte key
	root, err := db.Put(nil, helloKey, []byte("world"), 1)
	if err != nil {
		log.Fatalf("Failed to put: %v", err)
	}
	
	hashHex, _ := root.HashHex()
	fmt.Printf("   ✓ 插入成功, Root: %s\n", hashHex)

	// 3. Query the data (使用 FindFromRoot，因为内存模式需要提供 root)
	fmt.Println("\n3. 查询数据...")
	node, err := db.FindFromRoot(root, helloKey, 1)
	if err != nil {
		log.Fatalf("Failed to find: %v", err)
	}
	
	if node == nil {
		log.Fatal("Key not found!")
	}
	
	value, err := node.Value()
	if err != nil {
		log.Fatalf("Failed to get value: %v", err)
	}
	fmt.Printf("   ✓ 查询成功: key[0x01...] -> %s\n", string(value))

	// 4. Batch operations (32-byte keys)
	fmt.Println("\n4. 批量操作测试...")
	updates := make([]monaddb.Update, 10)
	for i := 0; i < 10; i++ {
		updates[i] = monaddb.Update{
			Type:  monaddb.UpdatePut,
			Key:   makeKey32(byte(i+0x10), byte(i)),  // 32-byte key
			Value: []byte(fmt.Sprintf("value_%03d", i)),
		}
	}
	
	root, err = db.Upsert(root, updates, 2)
	if err != nil {
		log.Fatalf("Failed to upsert: %v", err)
	}
	
	hashHex, _ = root.HashHex()
	fmt.Printf("   ✓ 批量插入 10 条, Root: %s\n", hashHex)

	// 5. Verify batch data (使用 FindFromRoot)
	fmt.Println("\n5. 验证批量数据...")
	for i := 0; i < 10; i++ {
		key := makeKey32(byte(i+0x10), byte(i))
		node, err := db.FindFromRoot(root, key, 2)
		if err != nil {
			log.Fatalf("Failed to find key[0x%02x...]: %v", i+0x10, err)
		}
		if node == nil {
			log.Fatalf("key[0x%02x...] not found", i+0x10)
		}
		value, _ := node.Value()
		expected := fmt.Sprintf("value_%03d", i)
		if string(value) != expected {
			log.Fatalf("key[0x%02x...]: expected %s, got %s", i+0x10, expected, string(value))
		}
	}
	fmt.Println("   ✓ 所有 10 条数据验证通过")

	// 6. Delete operation
	fmt.Println("\n6. 删除操作测试...")
	keyToDelete := makeKey32(0x15, 5)  // key for i=5
	root, err = db.Delete(root, keyToDelete, 3)
	if err != nil {
		log.Fatalf("Failed to delete: %v", err)
	}
	
	node, err = db.FindFromRoot(root, keyToDelete, 3)
	if err != nil {
		log.Fatalf("Failed to find after delete: %v", err)
	}
	if node != nil && node.HasValue() {
		log.Fatal("key should be deleted!")
	}
	fmt.Println("   ✓ 删除成功")

	// 7. Database info
	fmt.Println("\n7. 数据库信息...")
	fmt.Printf("   Latest Version:   %d\n", db.LatestVersion())
	fmt.Printf("   Earliest Version: %d\n", db.EarliestVersion())
	fmt.Printf("   History Length:   %d\n", db.HistoryLength())
	fmt.Printf("   Is On Disk:       %v\n", db.IsOnDisk())
	fmt.Printf("   Is Read Only:     %v\n", db.IsReadOnly())

	fmt.Println("\n=== 所有测试完成 ===")
}
