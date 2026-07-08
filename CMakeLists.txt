cmake_minimum_required(VERSION 3.16)
project(irotoiro_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(irotoiro_core
  core/src/engine.cpp
  core/src/bot.cpp
  core/src/search.cpp
)
target_include_directories(irotoiro_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)
target_compile_options(irotoiro_core
  PRIVATE
    -Wall -Wextra -Wpedantic
)

add_executable(irotoiro_core_tests
  core/tests/test_core.cpp
)
target_link_libraries(irotoiro_core_tests PRIVATE irotoiro_core)
target_compile_options(irotoiro_core_tests
  PRIVATE
    -Wall -Wextra -Wpedantic
)

add_executable(irotoiro_bench
  core/bench/bench.cpp
)
target_link_libraries(irotoiro_bench PRIVATE irotoiro_core)
target_compile_options(irotoiro_bench
  PRIVATE
    -O3
    -Wall -Wextra -Wpedantic
)

add_executable(irotoiro_cli
  core/cli/cli.cpp
)
target_link_libraries(irotoiro_cli PRIVATE irotoiro_core)
target_compile_options(irotoiro_cli
  PRIVATE
    -Wall -Wextra -Wpedantic
)

enable_testing()
add_test(NAME irotoiro_core_tests COMMAND irotoiro_core_tests)
