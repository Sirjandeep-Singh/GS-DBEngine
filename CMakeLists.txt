cmake_minimum_required(VERSION 3.16)
project(GS-DBEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linux only — WAL layer uses POSIX fsync()/open()/etc.
if(NOT UNIX)
    message(FATAL_ERROR "GS-DBEngine targets Linux/POSIX only (WAL uses POSIX file APIs).")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ── Core static library (everything except cli/ and tests/) ────────────────
add_library(gsdb_lib STATIC
    src/storage/disk_manager.cpp
    src/storage/header_manager.cpp
    src/storage/buffer_pool.cpp
    src/wal/wal_manager.cpp
    src/catalog/catalog_manager.cpp
    src/btree/btree_node.cpp
    src/btree/btree.cpp
    src/row/serializer.cpp
    src/table/table.cpp
    src/parser/tokenizer.cpp
    src/parser/parser.cpp
    src/executor/executor.cpp
    src/database.cpp
)

target_include_directories(gsdb_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(gsdb_lib PUBLIC -Wall -Wextra)

# ── CLI executable ───────────────────────────────────────────────────────
add_executable(gsdb cli/main.cpp)
target_link_libraries(gsdb PRIVATE gsdb_lib)

# ── Tests ────────────────────────────────────────────────────────────────
enable_testing()

set(TEST_NAMES
    test_storage
    test_wal
    test_catalog
    test_btree_node
    test_btree
    test_row
    test_table
    test_tokenizer
    test_parser
    test_executor
    test_database
)

foreach(test_name ${TEST_NAMES})
    add_executable(${test_name} tests/${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE gsdb_lib)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()