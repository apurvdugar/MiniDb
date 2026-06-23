cmake_minimum_required(VERSION 3.16)
project(MiniDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Source files ──────────────────────────────────────────────
set(MINIDB_SOURCES
    src/storage/page.cpp
    src/storage/disk_manager.cpp
    src/storage/buffer_pool.cpp
    src/storage/heap_file.cpp
    src/index/bplus_tree.cpp
    src/catalog/catalog.cpp
    src/execution/parser.cpp
    src/execution/operators.cpp
    src/execution/executor.cpp
    src/optimizer/optimizer.cpp
    src/transaction/lock_manager.cpp
    src/transaction/txn_manager.cpp
    src/recovery/wal.cpp
)

# ── MiniDB static library (shared by main + benchmark) ──────
add_library(minidb_lib STATIC ${MINIDB_SOURCES})
target_include_directories(minidb_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ── Main executable ─────────────────────────────────────────
add_executable(minidb src/main.cpp)
target_link_libraries(minidb PRIVATE minidb_lib)

# ── Benchmark executable ────────────────────────────────────
add_executable(benchmark benchmarks/benchmark.cpp)
target_link_libraries(benchmark PRIVATE minidb_lib)

# ── Platform-specific threading ─────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(minidb_lib PUBLIC Threads::Threads)
