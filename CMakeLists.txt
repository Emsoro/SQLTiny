cmake_minimum_required(VERSION 3.20)

# fmt 静态编译: vcpkg manifest 默认使用 x64-windows-static (库 + CRT 均静态),
# 产物不再依赖 fmt.dll。必须放在 project() 之前 —— vcpkg toolchain 在 project() 时读取该变量。
# 需要动态链接时须成对覆盖 VCPKG_TARGET_TRIPLET 与 CMAKE_MSVC_RUNTIME_LIBRARY。
if(WIN32 AND CMAKE_TOOLCHAIN_FILE AND NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet")
endif()

project(SQLTiny VERSION 2.2.2 LANGUAGES CXX)

# MSVC 静态 CRT: 与 x64-windows-static 的 VCPKG_CRT_LINKAGE=static 保持一致,
# 避免 CMake 默认 /MD 与 toolchain 的 /MT 冲突 (LNK2038 / D9025)。
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime library")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- AddressSanitizer ----------
# MSVC 自带 ASan(/fsanitize=address); 不需要 Clang/LLVM。UBSan 不可用(Clang 专属)。
option(SQLTINY_ASAN "Build with AddressSanitizer (MSVC)" OFF)
if(SQLTINY_ASAN)
  if(NOT MSVC)
    message(FATAL_ERROR "SQLTINY_ASAN requires MSVC (cl.exe)")
  endif()
  # 预编译的 fmt.lib 未开启 ASan; 关闭 STL 容器注解以避免 LNK2038
  # (annotate_vector/annotate_string)。堆/栈越界检测不受影响。
  add_compile_options(
    /fsanitize=address
    /Zi
    /D_DISABLE_VECTOR_ANNOTATION
    /D_DISABLE_STRING_ANNOTATION)
  add_link_options(/INCREMENTAL:NO)
endif()

# ---------- vcpkg ----------
find_package(fmt CONFIG REQUIRED)
find_package(GTest CONFIG QUIET)

enable_testing()

# ASan 运行时 clang_rt.asan_dynamic-x86_64.dll 位于 MSVC bin 目录,不在系统 PATH。
# 拷贝到每个 exe 旁边,使 Python 测试/fuzz 脚本直接 spawn 构建产物时也能加载。
if(SQLTINY_ASAN)
  get_filename_component(SQLTINY_MSVC_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
endif()
function(sqltiny_copy_asan_runtime target)
  if(SQLTINY_ASAN)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${SQLTINY_MSVC_BIN_DIR}/clang_rt.asan_dbg_dynamic-x86_64.dll,${SQLTINY_MSVC_BIN_DIR}/clang_rt.asan_dynamic-x86_64.dll>"
        "$<TARGET_FILE_DIR:${target}>"
      VERBATIM)
  endif()
endfunction()

# ---------- Library ----------
set(SQLTINY_SOURCES
  src/types/value.cpp
  src/parser/lexer.cpp
  src/parser/parser.cpp
  src/parser/ast.cpp
  src/storage/index_key.cpp
  src/storage/index_avl.cpp
  src/storage/index_bptree.cpp
  src/storage/table.cpp
  src/storage/persist.cpp
  src/storage/wal_manager.cpp
  src/storage/snapshot.cpp
  src/catalog/catalog.cpp
  src/cli/sql_io.cpp
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
sqltiny_copy_asan_runtime(sqltiny_server)

# Windows 版本资源:让 exe 属性页(详细信息)带产品名/版本/版权等元数据
if(WIN32)
  enable_language(RC)
  configure_file(src/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
  target_sources(sqltiny_server PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
endif()

# ---------- 测试 ----------
function(add_smoke_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE sqltiny)
  add_test(NAME ${name} COMMAND ${name})
  sqltiny_copy_asan_runtime(${name})
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
add_smoke_test(smoke_sql_keywords tests/smoke_sql_keywords.cpp)
add_smoke_test(smoke_sql_syntax   tests/smoke_sql_syntax.cpp)
add_smoke_test(smoke_cli_io      tests/smoke_cli_io.cpp)
add_smoke_test(dbx_probe     tests/dbx_probe.cpp)

# 索引微基准(AVL vs B+ 树 A/B 对比,不进 ctest)
add_executable(bench_index tests/bench_index.cpp)
target_link_libraries(bench_index PRIVATE sqltiny)
sqltiny_copy_asan_runtime(bench_index)

if(GTest_FOUND)
  function(add_sqltiny_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE sqltiny GTest::gtest GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
    sqltiny_copy_asan_runtime(${name})
  endfunction()
  add_sqltiny_test(test_executor tests/test_executor.cpp)
  add_sqltiny_test(test_mysql_proto tests/test_mysql_proto.cpp)
endif()

message(STATUS "=== SQLTiny build config ===")
message(STATUS "  GTest:  ${GTest_FOUND}")
message(STATUS "  Config: ${CMAKE_BUILD_TYPE}")
