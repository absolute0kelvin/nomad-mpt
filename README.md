# nomad-mpt

`nomad-mpt` provides multi-language bindings for the MonadDB MPT engine. The core C++ implementation lives upstream at [category-labs/monad](https://github.com/category-labs/monad).

## Upstream Source

- **C++ core**: mirrored from upstream `category-labs/monad`, keeping Ethereum state compatibility and performance tuning (io_uring, Boost.Fiber, etc.).

## Bindings and Maturity

| Language | Maturity | Notes | Docs |
| --- | --- | --- | --- |
| Rust (`bindings/rust/nomad-mpt-sys`) | ✅ Production-ready | Sync API complete; async ck_fifo (find/traverse/large values) implemented with examples and integration tests | [`bindings/rust/README.md`](bindings/rust/README.md) |
| Go (`bindings/go`) | ⏳ Sync stable; async experimental | Sync read/write API usable; async FIFO currently stub/experimental placeholder for future C API hookup | [`bindings/go/README.md`](bindings/go/README.md) |

## Quick Links

- Rust usage & examples: `bindings/rust/README.md`
- Go build & usage: `bindings/go/README.md`
- Upstream C++: <https://github.com/category-labs/monad>

## Status

- Rust binding: both sync and async (ck_fifo) paths are usable today.
- Go binding: sync API is production-ready; async FIFO is a no-op stub kept for API compatibility—expect updates.


