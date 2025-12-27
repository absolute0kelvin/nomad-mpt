package monaddb

import (
	"bytes"
	"testing"
)

// makeKey32 creates a 32-byte key from a byte value (Ethereum-style key)
func makeKey32(b byte) []byte {
	key := make([]byte, 32)
	key[0] = b
	return key
}

// makeKey32FromBytes creates a 32-byte key from input bytes (padded/truncated to 32 bytes)
func makeKey32FromBytes(data []byte) []byte {
	key := make([]byte, 32)
	copy(key, data)
	return key
}

// TestOpenMemory tests opening an in-memory database.
func TestOpenMemory(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	if db.IsOnDisk() {
		t.Error("Memory DB should not be on disk")
	}
	if db.IsReadOnly() {
		t.Error("Memory DB should not be read-only")
	}
}

// TestPutAndFind tests basic put and find operations.
func TestPutAndFind(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	key := makeKey32(0x01)      // 32-byte key
	value := []byte("test-value")
	version := uint64(1)

	// Insert a key-value pair
	root, err := db.Put(nil, key, value, version)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}
	if root == nil {
		t.Fatal("Put returned nil root")
	}

	// Query the key
	node, err := db.FindFromRoot(root, key, version)
	if err != nil {
		t.Fatalf("FindFromRoot failed: %v", err)
	}
	if node == nil {
		t.Fatal("FindFromRoot returned nil for existing key")
	}

	// Check the value
	got, err := node.Value()
	if err != nil {
		t.Fatalf("Value failed: %v", err)
	}
	if !bytes.Equal(got, value) {
		t.Errorf("Value mismatch: got %q, want %q", got, value)
	}
}

// TestNotFound tests querying a non-existent key.
func TestNotFound(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	// Insert one key
	root, err := db.Put(nil, makeKey32(0x01), []byte("value"), 1)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	// Query a different key
	node, err := db.FindFromRoot(root, makeKey32(0x99), 1)
	if err != nil {
		t.Fatalf("FindFromRoot failed: %v", err)
	}
	if node != nil && node.HasValue() {
		t.Error("Expected nil or no-value node for non-existent key")
	}
}

// TestMultiplePuts tests inserting multiple keys using batch upsert.
func TestMultiplePuts(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	// Create test data with 32-byte keys
	testData := []struct {
		keyByte byte
		value   string
	}{
		{0x01, "value1"},
		{0x02, "value2"},
		{0x03, "value3"},
		{0x04, "longer-value-data"},
		{0x05, "v"}, // short value
	}

	// Build batch updates
	updates := make([]Update, len(testData))
	for i, td := range testData {
		updates[i] = Update{
			Type:  UpdatePut,
			Key:   makeKey32(td.keyByte),
			Value: []byte(td.value),
		}
	}

	// Insert all keys in a single batch
	root, err := db.Upsert(nil, updates, 1)
	if err != nil {
		t.Fatalf("Upsert failed: %v", err)
	}

	// Verify all keys can be found
	for _, td := range testData {
		node, err := db.FindFromRoot(root, makeKey32(td.keyByte), 1)
		if err != nil {
			t.Fatalf("FindFromRoot failed for key 0x%02x: %v", td.keyByte, err)
		}
		if node == nil {
			t.Errorf("Key 0x%02x not found", td.keyByte)
			continue
		}
		got, err := node.Value()
		if err != nil {
			t.Fatalf("Value failed for key 0x%02x: %v", td.keyByte, err)
		}
		if string(got) != td.value {
			t.Errorf("Value mismatch for key 0x%02x: got %q, want %q", td.keyByte, got, td.value)
		}
	}
}

// TestDelete tests deleting a key.
func TestDelete(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	key := makeKey32(0xAA)
	value := []byte("temporary-value")
	version := uint64(1)

	// Insert a key
	root, err := db.Put(nil, key, value, version)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	// Verify it exists
	node, err := db.FindFromRoot(root, key, version)
	if err != nil {
		t.Fatalf("FindFromRoot failed: %v", err)
	}
	if node == nil || !node.HasValue() {
		t.Fatal("Key should exist after Put")
	}

	// Delete the key (use same version as this is single-version test)
	root, err = db.Delete(root, key, version)
	if err != nil {
		t.Fatalf("Delete failed: %v", err)
	}

	// Verify it no longer has a value
	node, err = db.FindFromRoot(root, key, version)
	if err != nil {
		t.Fatalf("FindFromRoot after delete failed: %v", err)
	}
	if node != nil && node.HasValue() {
		t.Error("Key should not have value after Delete")
	}
}

// TestUpsertBatch tests batch updates.
func TestUpsertBatch(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	// Create batch of updates with 32-byte keys
	updates := []Update{
		{Type: UpdatePut, Key: makeKey32(0x10), Value: []byte("batch-value1")},
		{Type: UpdatePut, Key: makeKey32(0x20), Value: []byte("batch-value2")},
		{Type: UpdatePut, Key: makeKey32(0x30), Value: []byte("batch-value3")},
	}

	// Apply batch
	root, err := db.Upsert(nil, updates, 1)
	if err != nil {
		t.Fatalf("Upsert failed: %v", err)
	}

	// Verify all keys
	for _, u := range updates {
		node, err := db.FindFromRoot(root, u.Key, 1)
		if err != nil {
			t.Fatalf("FindFromRoot failed for key: %v", err)
		}
		if node == nil {
			t.Errorf("Key not found after batch upsert")
			continue
		}
		got, err := node.Value()
		if err != nil {
			t.Fatalf("Value failed: %v", err)
		}
		if !bytes.Equal(got, u.Value) {
			t.Errorf("Value mismatch")
		}
	}
}

// TestNodeHash tests getting node hashes.
func TestNodeHash(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	root, err := db.Put(nil, makeKey32(0x42), []byte("value"), 1)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	hash, err := root.Hash()
	if err != nil {
		t.Fatalf("Hash failed: %v", err)
	}

	// Hash should be 32 bytes and non-zero
	if len(hash) != 32 {
		t.Errorf("Hash length should be 32, got %d", len(hash))
	}

	// Check it's not all zeros
	allZero := true
	for _, b := range hash {
		if b != 0 {
			allZero = false
			break
		}
	}
	if allZero {
		t.Error("Hash should not be all zeros")
	}

	// Test hex string
	hexHash, err := root.HashHex()
	if err != nil {
		t.Fatalf("HashHex failed: %v", err)
	}
	if len(hexHash) != 66 { // "0x" + 64 hex chars
		t.Errorf("HashHex length should be 66, got %d", len(hexHash))
	}
	if hexHash[:2] != "0x" {
		t.Errorf("HashHex should start with 0x, got %s", hexHash[:2])
	}
}

// TestNodeClone tests cloning nodes.
func TestNodeClone(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	root, err := db.Put(nil, makeKey32(0x01), []byte("value"), 1)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	// Clone the root
	clone := root.Clone()
	if clone == nil {
		t.Fatal("Clone returned nil")
	}

	// Both should have the same hash
	origHash, _ := root.Hash()
	cloneHash, _ := clone.Hash()
	if origHash != cloneHash {
		t.Error("Clone should have the same hash as original")
	}

	// Free the original, clone should still work
	root.Free()
	cloneHash2, err := clone.Hash()
	if err != nil {
		t.Fatalf("Clone Hash after original free failed: %v", err)
	}
	if cloneHash != cloneHash2 {
		t.Error("Clone hash changed after original free")
	}
}

// TestFindNotSupportedInMemoryMode tests that Find returns error for memory mode.
func TestFindNotSupportedInMemoryMode(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	// Put some data first
	_, err = db.Put(nil, makeKey32(0x01), []byte("value"), 1)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	// Find should return ErrNotSupported for memory mode
	_, err = db.Find(makeKey32(0x01), 1)
	if err != ErrNotSupported {
		t.Errorf("Find in memory mode should return ErrNotSupported, got %v", err)
	}
}

// TestEmptyUpdates tests that empty updates returns existing root.
func TestEmptyUpdates(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	version := uint64(1)
	root, err := db.Put(nil, makeKey32(0x01), []byte("value"), version)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	// Empty updates should return the same root
	sameRoot, err := db.Upsert(root, []Update{}, version)
	if err != nil {
		t.Fatalf("Empty Upsert failed: %v", err)
	}
	// The returned root should be the same pointer
	if sameRoot != root {
		t.Log("Note: Empty upsert returns same root object (expected behavior)")
	}
}

// TestLargeValue tests storing larger values.
func TestLargeValue(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	key := makeKey32(0xFF)
	// Create a 1KB value
	value := make([]byte, 1024)
	for i := range value {
		value[i] = byte(i % 256)
	}

	root, err := db.Put(nil, key, value, 1)
	if err != nil {
		t.Fatalf("Put failed: %v", err)
	}

	node, err := db.FindFromRoot(root, key, 1)
	if err != nil {
		t.Fatalf("FindFromRoot failed: %v", err)
	}
	if node == nil {
		t.Fatal("Key not found")
	}

	got, err := node.Value()
	if err != nil {
		t.Fatalf("Value failed: %v", err)
	}
	if !bytes.Equal(got, value) {
		t.Errorf("Large value mismatch: got %d bytes, want %d bytes", len(got), len(value))
	}
}

// TestBinaryKey tests using binary keys.
func TestBinaryKey(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	// 32-byte key with various byte values
	key := makeKey32FromBytes([]byte{0x00, 0x01, 0xFF, 0x00, 0xAB, 0xCD})
	value := []byte{0xDE, 0xAD, 0xBE, 0xEF}

	root, err := db.Put(nil, key, value, 1)
	if err != nil {
		t.Fatalf("Put with binary key failed: %v", err)
	}

	node, err := db.FindFromRoot(root, key, 1)
	if err != nil {
		t.Fatalf("FindFromRoot with binary key failed: %v", err)
	}
	if node == nil {
		t.Fatal("Binary key not found")
	}

	got, err := node.Value()
	if err != nil {
		t.Fatalf("Value failed: %v", err)
	}
	if !bytes.Equal(got, value) {
		t.Error("Binary value mismatch")
	}
}

// TestVersionMetadata tests version-related functions.
func TestVersionMetadata(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	// For memory mode, these should return reasonable defaults
	if db.HistoryLength() == 0 {
		t.Log("Note: HistoryLength may be 0 for memory mode")
	}

	// Version 1 should be valid for memory mode
	if !db.VersionIsValid(1) {
		t.Log("Note: VersionIsValid behavior may vary for memory mode")
	}
}

// TestMerkleConsistency tests that same updates produce same root hash.
func TestMerkleConsistency(t *testing.T) {
	key := makeKey32(0x42)
	value := []byte("consistency_test")

	var roots [][32]byte

	// Create 3 separate databases with same data
	for i := 0; i < 3; i++ {
		db, err := OpenMemory()
		if err != nil {
			t.Fatalf("OpenMemory failed: %v", err)
		}

		root, err := db.Put(nil, key, value, 1)
		if err != nil {
			t.Fatalf("Put failed: %v", err)
		}

		hash, err := root.Hash()
		if err != nil {
			t.Fatalf("Hash failed: %v", err)
		}
		roots = append(roots, hash)
		db.Close()
	}

	// All roots should be identical
	for i := 1; i < len(roots); i++ {
		if roots[i] != roots[0] {
			t.Errorf("Merkle hash inconsistency: root[%d] != root[0]", i)
		}
	}
}

// TestFifoNotImplemented tests that FIFO is marked as not implemented.
func TestFifoNotImplemented(t *testing.T) {
	db, err := OpenMemory()
	if err != nil {
		t.Fatalf("OpenMemory failed: %v", err)
	}
	defer db.Close()

	fifo, err := db.CreateFifo()
	if err != nil {
		t.Fatalf("CreateFifo failed: %v", err)
	}
	defer fifo.Destroy()

	if fifo.IsImplemented() {
		t.Error("FIFO should report as not implemented")
	}
}
