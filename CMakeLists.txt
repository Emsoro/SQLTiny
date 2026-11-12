cmake_minimum_required(VERSION 3.20)

# fmt 静态编译: vcpkg manifest 默认使用 x64-windows-static (库 + CRT 均静态),
# 产物不再依赖 fmt.dll。必须放在 project() 之前 —— vcpkg toolchain 在 project() 时读取该变量。
# 需要动态链接时须成对覆盖 VCPKG_TARGET_TRIPLET 与 CMAKE_MSVC_RUNTIME_LIBRARY。
if(WIN32 AND CMAKE_TOOLCHAIN_FILE AND NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet")
endif()

project(SQLTiny VERSION 1.2.0 LANGUAGES CXX)

# MSVC 静态 CRT: 与 x64-windows-static 的 VCPKG_CRT_LINKAGE=static 保持一致,
# 避免 CMake 默认 /MD 与 toolchain 的 /MT 冲突 (LNK2038 / D9025)。
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- vcpkg ----------
find_package(fmt CONFIG REQUIRED)
find_package(GTest CONFIG QUIET)

enable_testing()

# ---------- Library ----------
set(SQLTINY_SOURCES
  src/types/value.cpp
  src/parser/lexer.cpp
  src/parser/parser.cpp
  src/parser/ast.cpp
  src/storage/index_avl.cpp
  src/storage/table.cpp
  src/storage/persist.cpp
  src/storage/wal_manager.cpp
  src/storage/snapshot.cpp
  src/catalog/catalog.cpp
  src/core/session.cpp
  src/core/transaction.cpp
  src/executor/expr.cpp
  src/executor/executor.cpp
  src/net/mysql_proto.cpp
  src/net/mysql_server.cpp
)

add_library(sqltiny STATIC ${SQLTINY_SOURCES})
target_include_directories(sqltiny PUBLIC src)
target_link_libraries(sqltiny PUBLIC fmt::fmt)

# Windows 网络
if(WIN32)
  target_link_libraries(sqltiny PUBLIC ws2_32)
endif()

# ---------- MySQL 服务器可执行 ----------
add_executable(sqltiny_server src/main.cpp)
target_link_libraries(sqltiny_server PRIVATE sqltiny)

# ---------- 测试 ----------
function(add_smoke_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE sqltiny)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_smoke_test(smoke_m1       tests/smoke_m1.cpp)
add_smoke_test(smoke_m2       tests/smoke_m2.cpp)
add_smoke_test(smoke_m3       tests/smoke_m3.cpp)
add_smoke_test(smoke_m4       tests/smoke_m4.cpp)
add_smoke_test(smoke_m25_e2e  tests/smoke_m25_e2e.cpp)
add_smoke_test(smoke_m5_wal   tests/smoke_m5_wal.cpp)
add_smoke_test(smoke_m6_isolation tests/smoke_m6_isolation.cpp)
add_smoke_test(smoke_m6_hsqldb_matrix tests/smoke_m6_hsqldb_matrix.cpp)
add_smoke_test(smoke_m6_session_db tests/smoke_m6_session_db.cpp)
add_smoke_test(dbx_probe     tests/dbx_probe.cpp)

if(GTest_FOUND)
  function(add_sqltiny_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE sqltiny GTest::gtest GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()
  add_sqltiny_test(test_executor tests/test_executor.cpp)
  add_sqltiny_test(test_mysql_proto tests/test_mysql_proto.cpp)
endif()

message(STATUS "=== SQLTiny build config ===")
message(STATUS "  GTest:  ${GTest_FOUND}")
message(STATUS "  Config: ${CMAKE_BUILD_TYPE}")
