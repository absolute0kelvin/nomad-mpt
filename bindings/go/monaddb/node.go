package monaddb

/*
#include "nomad_mpt.h"
#include <stdlib.h>
#include <string.h>
*/
import "C"
import (
	"runtime"
	"unsafe"
)

// Node represents a node in the Merkle Patricia Trie.
// Nodes are reference-counted and must be freed when no longer needed.
type Node struct {
	ptr *C.NomadNode
}

// Free releases the node's resources.
func (n *Node) Free() {
	if n.ptr != nil {
		C.nomad_node_free(n.ptr)
		n.ptr = nil
		runtime.SetFinalizer(n, nil)
	}
}

// Clone creates a copy of the node (increments reference count).
func (n *Node) Clone() *Node {
	if n.ptr == nil {
		return nil
	}
	
	clonePtr := C.nomad_node_clone(n.ptr)
	if clonePtr == nil {
		return nil
	}
	
	clone := &Node{ptr: clonePtr}
	runtime.SetFinalizer(clone, (*Node).Free)
	return clone
}

// HasValue returns true if the node has a value.
func (n *Node) HasValue() bool {
	if n.ptr == nil {
		return false
	}
	return bool(C.nomad_node_has_value(n.ptr))
}

// Value returns the value stored in the node.
// Returns nil if the node has no value.
func (n *Node) Value() ([]byte, error) {
	if n.ptr == nil {
		return nil, ErrNullPointer
	}
	
	var bytes C.NomadBytes
	code := C.nomad_node_get_value(n.ptr, &bytes)
	if err := codeToError(int(code)); err != nil {
		if err == ErrNotFound {
			return nil, nil
		}
		return nil, err
	}
	
	if bytes.data == nil || bytes.len == 0 {
		return nil, nil
	}
	
	// Copy data to Go slice
	result := C.GoBytes(unsafe.Pointer(bytes.data), C.int(bytes.len))
	
	// Free C memory
	C.nomad_bytes_free(&bytes)
	
	return result, nil
}

// Hash returns the 32-byte Merkle hash of the node.
func (n *Node) Hash() ([32]byte, error) {
	var hash [32]byte
	
	if n.ptr == nil {
		return hash, ErrNullPointer
	}
	
	code := C.nomad_node_get_hash(n.ptr, (*C.uint8_t)(unsafe.Pointer(&hash[0])))
	if err := codeToError(int(code)); err != nil {
		return hash, err
	}
	
	return hash, nil
}

// HashHex returns the Merkle hash as a hex string.
func (n *Node) HashHex() (string, error) {
	hash, err := n.Hash()
	if err != nil {
		return "", err
	}
	
	const hexChars = "0123456789abcdef"
	result := make([]byte, 66) // "0x" + 64 hex chars
	result[0] = '0'
	result[1] = 'x'
	
	for i, b := range hash {
		result[2+i*2] = hexChars[b>>4]
		result[3+i*2] = hexChars[b&0x0f]
	}
	
	return string(result), nil
}


