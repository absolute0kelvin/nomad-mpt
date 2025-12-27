# MonadDB C API 静态库
# 
# 用于 Go/Python/Java 等语言的 FFI 绑定
#
# 构建方法:
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make

cmake_minimum_required(VERSION 3.20)
project(nomad_mpt_c LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找 MonadDB 主库
set(MONAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../depend/monad")

if(NOT EXISTS "${MONAD_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "MonadDB not found at ${MONAD_DIR}. Run scripts/sync_monad.sh first.")
endif()

# 添加 MonadDB 作为子目录（构建 monad_ffi）
add_subdirectory(${MONAD_DIR} ${CMAKE_BINARY_DIR}/monad)

# C API 实现
add_library(nomad_mpt STATIC
    src/nomad_mpt.cpp
)

target_include_directories(nomad_mpt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MONAD_DIR}
    ${MONAD_DIR}/category
    ${MONAD_DIR}/third_party
    ${MONAD_DIR}/third_party/intx/include
    ${MONAD_DIR}/third_party/evmc/include
    ${MONAD_DIR}/third_party/ethash/include
    ${MONAD_DIR}/third_party/nlohmann_json/single_include
    ${MONAD_DIR}/third_party/immer
    ${MONAD_DIR}/third_party/quill/quill/include
    ${MONAD_DIR}/third_party/cthash/include
    ${MONAD_DIR}/third_party/ankerl
    ${MONAD_DIR}/third_party/concurrentqueue
    ${MONAD_DIR}/third_party/unordered_dense/include
)

target_link_libraries(nomad_mpt PUBLIC monad_ffi)

# 安装
install(TARGETS nomad_mpt DESTINATION lib)
install(FILES include/nomad_mpt.h DESTINATION include)

