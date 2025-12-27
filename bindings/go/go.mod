module github.com/monad/nomad-mpt-go

go 1.21

// 本模块提供 MonadDB MPT 的 Go 绑定
// 使用 CGO 调用 core/include/nomad_mpt.h 定义的纯 C API
//
// 构建步骤：
//   1. 先构建 Rust FFI: cd ../rust && cargo build --release
//   2. 运行构建脚本: ./scripts/build.sh
//   3. 运行示例: go run examples/basic/main.go

