// Package monaddb provides Go bindings for MonadDB MPT (Merkle Patricia Trie).
//
// MonadDB is a high-performance authenticated key-value store designed for
// blockchain state storage. It provides:
//
//   - KV Storage: Values stored directly in MPT leaf nodes
//   - Merkle Authentication: Each node maintains a hash for state proofs
//   - Version Control: Historical state access with configurable retention
//
// # Quick Start
//
// Open an in-memory database:
//
//	db, err := monaddb.OpenMemory()
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer db.Close()
//
// Insert data (note: keys must be exactly 32 bytes, like Ethereum addresses):
//
//	key := make([]byte, 32)
//	key[0] = 0x01  // Set key bytes
//	root, err := db.Put(nil, key, []byte("value"), 1)
//	if err != nil {
//	    log.Fatal(err)
//	}
//
// Query data (for in-memory mode, use FindFromRoot with the root node):
//
//	node, err := db.FindFromRoot(root, []byte("key"), 1)
//	if err != nil {
//	    log.Fatal(err)
//	}
//	if node != nil {
//	    value, _ := node.Value()
//	    fmt.Printf("Value: %s\n", value)
//	}
//
// For disk-based databases, you can also use db.Find() directly:
//
//	// Open disk database
//	db, err := monaddb.OpenDisk("/path/to/db", true, 40)
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer db.Close()
//
//	// Query using Find (disk mode only)
//	node, err := db.Find([]byte("key"), version)
//
// # Key Recommendations
//
// MonadDB uses an Ethereum-compatible state machine. For best compatibility:
//   - Keys SHOULD be 32 bytes (256 bits) - matching Ethereum's Keccak256 hashes
//   - Values can be any length
//
// While shorter keys technically work, 32-byte keys are recommended for
// Ethereum ecosystem compatibility.
//
// # Memory Mode vs Disk Mode
//
// In-memory databases require tracking the root node returned by upsert
// operations. Use FindFromRoot() for queries.
//
// Disk databases persist the root automatically and support Find() directly.
//
// # Async Operations (Experimental)
//
// NOTE: The async FIFO API is currently experimental and not fully implemented.
// Use the synchronous API (Put, FindFromRoot) for production use.
//
//	fifo, err := db.CreateFifo()
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer fifo.Destroy()
//
//	fifo.Start(4) // Start 4 worker fibers
//	defer fifo.Stop()
//
//	// Submit requests
//	for i := 0; i < 1000; i++ {
//	    fifo.SubmitFind(keys[i], version, uint64(i), 0)
//	}
//
//	// Poll results
//	for completed := 0; completed < 1000; {
//	    if comp := fifo.Poll(); comp != nil {
//	        completed++
//	        // Process completion...
//	    }
//	}
//
// # Thread Safety
//
//   - DB: Safe for concurrent reads, NOT safe for concurrent writes
//   - Fifo: Fully thread-safe (lock-free queues)
//   - Node: NOT thread-safe; clone before sharing across goroutines
//
// # Building
//
// This package requires pre-built MonadDB libraries. Use the build script:
//
//	cd nomad-mpt/bindings/go/scripts
//	./build.sh
//
// For faster rebuilds (skips Rust compilation if already done):
//
//	./build.sh --quick
//
// Then build and run the example:
//
//	cd nomad-mpt/bindings/go
//	go run examples/basic/main.go
//
// # Dependencies
//
// System libraries required:
//   - liburing (io_uring support)
//   - libgmp (big integers)
//   - libcrypto (OpenSSL)
//   - libzstd (compression)
//   - libarchive
//   - boost_fiber, boost_context (coroutines)
//   - boost_stacktrace_backtrace, libbacktrace (debugging)
//
// On Ubuntu/Debian:
//
//	apt install liburing-dev libgmp-dev libssl-dev libzstd-dev libarchive-dev \
//	    libboost-fiber-dev libboost-context-dev libboost-stacktrace-dev libbacktrace-dev
package monaddb


