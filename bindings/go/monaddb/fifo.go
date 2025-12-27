package monaddb

/*
#include "nomad_mpt.h"
#include <stdlib.h>
*/
import "C"
import (
	"runtime"
	"unsafe"
)

// EXPERIMENTAL: The async FIFO API is currently experimental and not fully
// implemented. The underlying C API provides stub implementations that do not
// perform actual async operations.
//
// For production use, prefer the synchronous API (Put, FindFromRoot, etc.).
// For high-throughput scenarios requiring async operations, consider using
// the Rust bindings which have a complete ck_fifo-based implementation.

// ResultStatus represents the status of an async operation result.
type ResultStatus int

const (
	StatusOK           ResultStatus = 0
	StatusNotFound     ResultStatus = 1
	StatusError        ResultStatus = 2
	StatusTraverseMore ResultStatus = 3
	StatusTraverseEnd  ResultStatus = 4
)

// Completion represents the result of an async operation.
type Completion struct {
	UserDataLo uint64
	UserDataHi uint64
	Status     ResultStatus
	Value      []byte       // Inline value (up to 256 bytes)
	MerkleHash [32]byte
}

// Fifo provides async operations using lock-free FIFO queues.
// It is thread-safe and designed for high-concurrency scenarios.
//
// WARNING: This is currently a stub implementation. The async operations
// do not actually perform any work. Use synchronous methods instead.
type Fifo struct {
	ptr *C.NomadFifo
	db  *DB
}

// IsImplemented returns true if the async FIFO is fully implemented.
// Currently returns false as the C API provides only stub implementations.
func (f *Fifo) IsImplemented() bool {
	return false
}

// CreateFifo creates an async FIFO for the database.
func (db *DB) CreateFifo() (*Fifo, error) {
	if db.ptr == nil {
		return nil, ErrNullPointer
	}
	
	var ptr *C.NomadFifo
	code := C.nomad_fifo_create(db.ptr, &ptr)
	if err := codeToError(int(code)); err != nil {
		return nil, err
	}
	
	fifo := &Fifo{ptr: ptr, db: db}
	runtime.SetFinalizer(fifo, (*Fifo).Destroy)
	return fifo, nil
}

// Start starts the FIFO worker fibers.
// numWorkers is the recommended number of worker fibers (typically 4-8).
func (f *Fifo) Start(numWorkers int) {
	if f.ptr == nil {
		return
	}
	C.nomad_fifo_start(f.ptr, C.size_t(numWorkers))
}

// Stop stops all worker fibers.
func (f *Fifo) Stop() {
	if f.ptr == nil {
		return
	}
	C.nomad_fifo_stop(f.ptr)
}

// Destroy destroys the FIFO and releases all resources.
func (f *Fifo) Destroy() {
	if f.ptr != nil {
		C.nomad_fifo_destroy(f.ptr)
		f.ptr = nil
		runtime.SetFinalizer(f, nil)
	}
}

// SubmitFind submits an async find request.
// Results can be retrieved using Poll().
//
// Parameters:
//   - key: The key to look up
//   - version: The version to query
//   - userDataLo, userDataHi: 128-bit user data returned in the completion
//
// Note: The key is copied internally, so it's safe to modify after this call returns.
func (f *Fifo) SubmitFind(key []byte, version uint64, userDataLo, userDataHi uint64) {
	if f.ptr == nil {
		return
	}
	
	var keyPtr *C.uint8_t
	if len(key) > 0 {
		// Allocate C memory for key to avoid CGO pointer rules violation
		// The C side is responsible for freeing this memory after processing
		keyPtr = (*C.uint8_t)(C.malloc(C.size_t(len(key))))
		if keyPtr == nil {
			return // Out of memory, silently fail
		}
		copy(unsafe.Slice((*byte)(unsafe.Pointer(keyPtr)), len(key)), key)
	}
	
	C.nomad_fifo_submit_find(
		f.ptr,
		keyPtr,
		C.size_t(len(key)),
		C.uint64_t(version),
		C.uint64_t(userDataLo),
		C.uint64_t(userDataHi),
	)
	
	// Note: The C implementation should free keyPtr after processing the request.
	// Since the current C implementation is a stub, we free it here to prevent leaks.
	if keyPtr != nil {
		C.free(unsafe.Pointer(keyPtr))
	}
}

// SubmitTraverse submits an async traverse request.
// Results can be retrieved using PollTraverse().
//
// Parameters:
//   - prefix: Key prefix to traverse
//   - version: The version to query
//   - limit: Maximum number of results (0 = default 4096)
//   - userDataLo, userDataHi: 128-bit user data returned in completions
//
// Note: The prefix is copied internally, so it's safe to modify after this call returns.
func (f *Fifo) SubmitTraverse(prefix []byte, version uint64, limit uint32, userDataLo, userDataHi uint64) {
	if f.ptr == nil {
		return
	}
	
	var prefixPtr *C.uint8_t
	if len(prefix) > 0 {
		// Allocate C memory for prefix to avoid CGO pointer rules violation
		// The C side is responsible for freeing this memory after processing
		prefixPtr = (*C.uint8_t)(C.malloc(C.size_t(len(prefix))))
		if prefixPtr == nil {
			return // Out of memory, silently fail
		}
		copy(unsafe.Slice((*byte)(unsafe.Pointer(prefixPtr)), len(prefix)), prefix)
	}
	
	C.nomad_fifo_submit_traverse(
		f.ptr,
		prefixPtr,
		C.size_t(len(prefix)),
		C.uint64_t(version),
		C.uint32_t(limit),
		C.uint64_t(userDataLo),
		C.uint64_t(userDataHi),
	)
	
	// Note: The C implementation should free prefixPtr after processing the request.
	// Since the current C implementation is a stub, we free it here to prevent leaks.
	if prefixPtr != nil {
		C.free(unsafe.Pointer(prefixPtr))
	}
}

// Poll retrieves a completion from the find queue (non-blocking).
// Returns nil if no completion is available.
func (f *Fifo) Poll() *Completion {
	if f.ptr == nil {
		return nil
	}
	
	var cComp C.NomadCompletion
	if !C.nomad_fifo_poll(f.ptr, &cComp) {
		return nil
	}
	
	comp := &Completion{
		UserDataLo: uint64(cComp.user_data_lo),
		UserDataHi: uint64(cComp.user_data_hi),
		Status:     ResultStatus(cComp.status),
	}
	
	// Copy value if present
	if cComp.value_len > 0 {
		comp.Value = C.GoBytes(unsafe.Pointer(&cComp.value[0]), C.int(cComp.value_len))
	}
	
	// Copy merkle hash
	for i := 0; i < 32; i++ {
		comp.MerkleHash[i] = byte(cComp.merkle_hash[i])
	}
	
	return comp
}

// PollTraverse retrieves a completion from the traverse queue (non-blocking).
// Returns nil if no completion is available.
func (f *Fifo) PollTraverse() *Completion {
	if f.ptr == nil {
		return nil
	}
	
	var cComp C.NomadCompletion
	if !C.nomad_fifo_poll_traverse(f.ptr, &cComp) {
		return nil
	}
	
	comp := &Completion{
		UserDataLo: uint64(cComp.user_data_lo),
		UserDataHi: uint64(cComp.user_data_hi),
		Status:     ResultStatus(cComp.status),
	}
	
	// Copy value if present
	if cComp.value_len > 0 {
		comp.Value = C.GoBytes(unsafe.Pointer(&cComp.value[0]), C.int(cComp.value_len))
	}
	
	// Copy merkle hash
	for i := 0; i < 32; i++ {
		comp.MerkleHash[i] = byte(cComp.merkle_hash[i])
	}
	
	return comp
}

// LargeValue represents a value larger than 256 bytes.
type LargeValue struct {
	UserDataLo uint64
	UserDataHi uint64
	Data       []byte
}

// PollLargeValue retrieves a large value (>256 bytes) from the queue.
// Returns nil if no large value is available.
func (f *Fifo) PollLargeValue() *LargeValue {
	if f.ptr == nil {
		return nil
	}
	
	var userDataLo, userDataHi C.uint64_t
	var bytes C.NomadBytes
	
	if !C.nomad_fifo_poll_large_value(f.ptr, &userDataLo, &userDataHi, &bytes) {
		return nil
	}
	
	result := &LargeValue{
		UserDataLo: uint64(userDataLo),
		UserDataHi: uint64(userDataHi),
	}
	
	if bytes.data != nil && bytes.len > 0 {
		result.Data = C.GoBytes(unsafe.Pointer(bytes.data), C.int(bytes.len))
		C.nomad_bytes_free(&bytes)
	}
	
	return result
}


