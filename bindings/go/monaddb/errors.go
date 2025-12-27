// Package monaddb provides Go bindings for MonadDB MPT (Merkle Patricia Trie).
//
// MonadDB is a high-performance authenticated key-value store used by Monad blockchain.
// This package wraps the C API to provide idiomatic Go access.
package monaddb

import (
	"errors"
	"fmt"
)

// Error codes matching C API (nomad_mpt.h)
const (
	ErrCodeOK                 = 0
	ErrCodeNullPointer        = 1
	ErrCodeInvalidArgument    = 2
	ErrCodeNotFound           = 3
	ErrCodeIO                 = 4
	ErrCodeVersionOutOfRange  = 5
	ErrCodeNotSupported       = 6
	ErrCodeOutOfMemory        = 7
	ErrCodeInternal           = 255
)

// Standard errors
var (
	ErrNullPointer       = errors.New("monaddb: null pointer")
	ErrInvalidArgument   = errors.New("monaddb: invalid argument")
	ErrNotFound          = errors.New("monaddb: key not found")
	ErrIO                = errors.New("monaddb: I/O error")
	ErrVersionOutOfRange = errors.New("monaddb: version out of range")
	ErrNotSupported      = errors.New("monaddb: operation not supported")
	ErrOutOfMemory       = errors.New("monaddb: out of memory")
	ErrInternal          = errors.New("monaddb: internal error")
)

// codeToError converts a C error code to a Go error
func codeToError(code int) error {
	switch code {
	case ErrCodeOK:
		return nil
	case ErrCodeNullPointer:
		return ErrNullPointer
	case ErrCodeInvalidArgument:
		return ErrInvalidArgument
	case ErrCodeNotFound:
		return ErrNotFound
	case ErrCodeIO:
		return ErrIO
	case ErrCodeVersionOutOfRange:
		return ErrVersionOutOfRange
	case ErrCodeNotSupported:
		return ErrNotSupported
	case ErrCodeOutOfMemory:
		return ErrOutOfMemory
	default:
		return fmt.Errorf("monaddb: unknown error code %d", code)
	}
}


