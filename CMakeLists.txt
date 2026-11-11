cmake_minimum_required(VERSION 3.20)
project(SQLTiny LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- vcpkg (已预装 fmt;gtest/spdlog 网络恢复后再拉) ----------
find_package(fmt CONFIG REQUIRED)

# ---------- Optional: GoogleTest (若存在则自动启用) ----------
find_package(GTest CONFIG QUIET)

enable_testing()

# ---------- Library ----------
set(SQLTINY_SOURCES
  src/types/value.cpp
  src/parser/lexer.cpp
  src/storage/index_avl.cpp
)

add_library(sqltiny STATIC ${SQLTINY_SOURCES})
target_include_directories(sqltiny PUBLIC src)
target_link_libraries(sqltiny PUBLIC fmt::fmt)

# ---------- Tests ----------
if(GTest_FOUND)
  function(add_sqltiny_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE sqltiny GTest::gtest GTest::gtest_main)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  add_sqltiny_test(test_value   tests/test_value.cpp)
  add_sqltiny_test(test_avl     tests/test_avl.cpp)
  add_sqltiny_test(test_lexer   tests/test_lexer.cpp)
else()
  message(STATUS "GTest not found — formal tests disabled. Run `vcpkg install gtest` when network is available.")
endif()

# ---------- Debug helpers ----------
add_executable(dump_tokens tests/dump_tokens.cpp)
target_link_libraries(dump_tokens PRIVATE sqltiny)

add_executable(smoke_m1 tests/smoke_m1.cpp)
target_link_libraries(smoke_m1 PRIVATE sqltiny)
add_test(NAME smoke_m1 COMMAND smoke_m1)
