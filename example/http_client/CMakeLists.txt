cmake_minimum_required(VERSION 3.16)

project(http_client LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(concurrencpp SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
FetchContent_MakeAvailable(concurrencpp)

include(../../cmake/coroutineOptions.cmake)

add_executable(http_client source/main.cpp)

find_package(Threads REQUIRED)
find_library(URING_LIB NAMES uring DOC "Path to liburing")

target_compile_features(http_client PRIVATE cxx_std_20)

# Link to concurrencpp and Threads; link liburing if present (optional)
target_link_libraries(http_client PRIVATE concurrencpp::concurrencpp Threads::Threads $<$<BOOL:${URING_LIB}>:${URING_LIB}>)

# Ensure ASIO headers are discoverable: local wrappers and upstream ASIO
# - wrappers: include/concurrencpp/net/asio
# - upstream ASIO: asio/asio/include
# Both are needed because wrappers include headers like "asio/detail/config.hpp"
target_include_directories(http_client PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../include/concurrencpp/net
    ${CMAKE_CURRENT_LIST_DIR}/../../asio/asio/include)

# Optimize for performance: O3 and native tuning (GCC/Clang)
target_compile_options(http_client PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3 -march=native -mtune=native>
)

# Enable coroutine options used across project
target_coroutine_options(http_client)

# Ensure io_uring is disabled unless explicitly enabled
# (prevents undefined references when liburing is not linked)
target_compile_definitions(http_client PRIVATE ASIO_DISABLE_LIBURING)