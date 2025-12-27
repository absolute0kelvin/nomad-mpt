package monaddb

/*
#cgo CFLAGS: -I${SRCDIR}

// 链接 MonadDB FFI 库（需要先运行 scripts/build.sh）
// 库文件位于 lib/ 目录，由构建脚本从 Rust 构建产物复制
#cgo LDFLAGS: -L${SRCDIR}/lib -Wl,--start-group
#cgo LDFLAGS: -lnomad_mpt -lmonad_ffi
#cgo LDFLAGS: -lquill -lblake3 -lkeccak
#cgo LDFLAGS: -Wl,--end-group
#cgo LDFLAGS: -lstdc++ -lm -luring -lgmp -lcrypto -lzstd -larchive
#cgo LDFLAGS: -lboost_fiber -lboost_context -lboost_stacktrace_backtrace -lbacktrace

#include "nomad_mpt.h"
#include <stdlib.h>
*/
import "C"
import (
	"runtime"
	"unsafe"
)

// DB represents a MonadDB database instance.
// It is safe for concurrent reads but NOT safe for concurrent writes.
type DB struct {
	ptr *C.NomadDb
}

// OpenMemory opens an in-memory database.
// The database will be lost when closed.
func OpenMemory() (*DB, error) {
	var ptr *C.NomadDb
	code := C.nomad_db_open_memory(&ptr)
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	db := &DB{ptr: ptr}
	runtime.SetFinalizer(db, (*DB).Close)
	return db, nil
}

// OpenDisk opens a disk-based database.
//
// Parameters:
//   - path: Database directory path
//   - create: If true, create database if it doesn't exist
//   - historyLength: Number of historical versions to retain (0 = default)
func OpenDisk(path string, create bool, historyLength uint64) (*DB, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))
	
	var ptr *C.NomadDb
	code := C.nomad_db_open_disk(cPath, C.bool(create), C.uint64_t(historyLength), &ptr)
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	db := &DB{ptr: ptr}
	runtime.SetFinalizer(db, (*DB).Close)
	return db, nil
}

// Close closes the database and releases all resources.
func (db *DB) Close() error {
	if db.ptr != nil {
		C.nomad_db_close(db.ptr)
		db.ptr = nil
		runtime.SetFinalizer(db, nil)
	}
	return nil
}

// IsOnDisk returns true if the database is using disk storage.
func (db *DB) IsOnDisk() bool {
	if db.ptr == nil {
		return false
	}
	return bool(C.nomad_db_is_on_disk(db.ptr))
}

// IsReadOnly returns true if the database is read-only.
func (db *DB) IsReadOnly() bool {
	if db.ptr == nil {
		return true
	}
	return bool(C.nomad_db_is_read_only(db.ptr))
}

// LatestVersion returns the latest version number.
func (db *DB) LatestVersion() uint64 {
	if db.ptr == nil {
		return 0
	}
	return uint64(C.nomad_db_get_latest_version(db.ptr))
}

// EarliestVersion returns the earliest available version number.
func (db *DB) EarliestVersion() uint64 {
	if db.ptr == nil {
		return 0
	}
	return uint64(C.nomad_db_get_earliest_version(db.ptr))
}

// HistoryLength returns the history retention length.
func (db *DB) HistoryLength() uint64 {
	if db.ptr == nil {
		return 0
	}
	return uint64(C.nomad_db_get_history_length(db.ptr))
}

// VersionIsValid checks if a version is valid.
func (db *DB) VersionIsValid(version uint64) bool {
	if db.ptr == nil {
		return false
	}
	return bool(C.nomad_db_version_is_valid(db.ptr, C.uint64_t(version)))
}

// Find looks up a key at a specific version.
// Returns nil if the key is not found.
//
// Note: This function only works in disk mode. For in-memory databases,
// use FindFromRoot() instead.
func (db *DB) Find(key []byte, version uint64) (*Node, error) {
	if db.ptr == nil {
		return nil, ErrNullPointer
	}
	
	var keyPtr *C.uint8_t
	if len(key) > 0 {
		keyPtr = (*C.uint8_t)(unsafe.Pointer(&key[0]))
	}
	
	var nodePtr *C.NomadNode
	code := C.nomad_db_find(db.ptr, keyPtr, C.size_t(len(key)), C.uint64_t(version), &nodePtr)
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	if nodePtr == nil {
		return nil, nil // Not found
	}
	
	node := &Node{ptr: nodePtr}
	runtime.SetFinalizer(node, (*Node).Free)
	return node, nil
}

// FindFromRoot looks up a key starting from a root node.
// This works in both memory and disk modes.
//
// Parameters:
//   - root: The root node to search from (typically from Upsert result)
//   - key: The key to look up
//   - version: Version number
//
// Returns nil if the key is not found.
func (db *DB) FindFromRoot(root *Node, key []byte, version uint64) (*Node, error) {
	if db.ptr == nil {
		return nil, ErrNullPointer
	}
	if root == nil || root.ptr == nil {
		return nil, ErrNullPointer
	}
	
	var keyPtr *C.uint8_t
	if len(key) > 0 {
		keyPtr = (*C.uint8_t)(unsafe.Pointer(&key[0]))
	}
	
	var nodePtr *C.NomadNode
	code := C.nomad_db_find_from_root(db.ptr, root.ptr, keyPtr, C.size_t(len(key)), C.uint64_t(version), &nodePtr)
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	if nodePtr == nil {
		return nil, nil // Not found
	}
	
	node := &Node{ptr: nodePtr}
	runtime.SetFinalizer(node, (*Node).Free)
	return node, nil
}

// LoadRoot loads the root node for a specific version.
func (db *DB) LoadRoot(version uint64) (*Node, error) {
	if db.ptr == nil {
		return nil, ErrNullPointer
	}
	
	var nodePtr *C.NomadNode
	code := C.nomad_db_load_root(db.ptr, C.uint64_t(version), &nodePtr)
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	if nodePtr == nil {
		return nil, nil
	}
	
	node := &Node{ptr: nodePtr}
	runtime.SetFinalizer(node, (*Node).Free)
	return node, nil
}

// UpdateType represents the type of update operation.
type UpdateType int

const (
	UpdatePut    UpdateType = 0
	UpdateDelete UpdateType = 1
)

// Update represents a single update operation.
type Update struct {
	Type  UpdateType
	Key   []byte
	Value []byte // nil for delete
}

// Upsert applies updates to the database.
// Returns the new root node.
func (db *DB) Upsert(root *Node, updates []Update, version uint64) (*Node, error) {
	if db.ptr == nil {
		return nil, ErrNullPointer
	}
	
	if len(updates) == 0 {
		return root, nil
	}
	
	// 使用 C 分配内存来存储 updates，避免 CGO 指针规则问题
	// C.malloc 分配的内存不受 Go GC 管理
	cUpdatesPtr := (*C.NomadUpdate)(C.malloc(C.size_t(len(updates)) * C.size_t(unsafe.Sizeof(C.NomadUpdate{}))))
	if cUpdatesPtr == nil {
		return nil, ErrOutOfMemory
	}
	defer C.free(unsafe.Pointer(cUpdatesPtr))
	
	// 将 cUpdatesPtr 转换为 slice 以便索引访问
	cUpdates := unsafe.Slice(cUpdatesPtr, len(updates))
	
	// 为 key 和 value 分配 C 内存
	var cKeys []*C.uint8_t
	var cValues []*C.uint8_t
	defer func() {
		for _, p := range cKeys {
			if p != nil {
				C.free(unsafe.Pointer(p))
			}
		}
		for _, p := range cValues {
			if p != nil {
				C.free(unsafe.Pointer(p))
			}
		}
	}()
	
	for i, u := range updates {
		cUpdates[i]._type = C.NomadUpdateType(u.Type)
		
		if len(u.Key) > 0 {
			keyPtr := (*C.uint8_t)(C.malloc(C.size_t(len(u.Key))))
			if keyPtr == nil {
				return nil, ErrOutOfMemory
			}
			cKeys = append(cKeys, keyPtr)
			// 复制 key 数据到 C 内存
			copy(unsafe.Slice((*byte)(unsafe.Pointer(keyPtr)), len(u.Key)), u.Key)
			cUpdates[i].key = keyPtr
			cUpdates[i].key_len = C.size_t(len(u.Key))
		}
		
		if len(u.Value) > 0 {
			valuePtr := (*C.uint8_t)(C.malloc(C.size_t(len(u.Value))))
			if valuePtr == nil {
				return nil, ErrOutOfMemory
			}
			cValues = append(cValues, valuePtr)
			// 复制 value 数据到 C 内存
			copy(unsafe.Slice((*byte)(unsafe.Pointer(valuePtr)), len(u.Value)), u.Value)
			cUpdates[i].value = valuePtr
			cUpdates[i].value_len = C.size_t(len(u.Value))
		}
	}
	
	var rootPtr *C.NomadNode
	if root != nil {
		rootPtr = root.ptr
	}
	
	var newRootPtr *C.NomadNode
	code := C.nomad_db_upsert(
		db.ptr,
		rootPtr,
		cUpdatesPtr,
		C.size_t(len(updates)),
		C.uint64_t(version),
		&newRootPtr,
	)
	
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	if newRootPtr == nil {
		return nil, nil
	}
	
	newRoot := &Node{ptr: newRootPtr}
	runtime.SetFinalizer(newRoot, (*Node).Free)
	return newRoot, nil
}

// Put is a convenience method to insert a single key-value pair.
func (db *DB) Put(root *Node, key, value []byte, version uint64) (*Node, error) {
	return db.Upsert(root, []Update{{Type: UpdatePut, Key: key, Value: value}}, version)
}

// Delete is a convenience method to delete a single key.
func (db *DB) Delete(root *Node, key []byte, version uint64) (*Node, error) {
	return db.Upsert(root, []Update{{Type: UpdateDelete, Key: key}}, version)
}


