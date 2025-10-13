cmake_minimum_required(VERSION 3.16)

project(tcp_echo_server LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(concurrencpp SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
FetchContent_MakeAvailable(concurrencpp)

include(../../cmake/coroutineOptions.cmake)

add_executable(tcp_echo_server source/main.cpp)

target_compile_features(tcp_echo_server PRIVATE cxx_std_20)

target_link_libraries(tcp_echo_server PRIVATE concurrencpp::concurrencpp)

# Ensure ASIO headers are discoverable (vendored under include/concurrencpp/net/asio)
target_include_directories(tcp_echo_server PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../include/concurrencpp/net/asio)

# Optimize for performance: O3 and native tuning (GCC/Clang)
target_compile_options(tcp_echo_server PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-O3 -march=native -mtune=native>
)

target_coroutine_options(tcp_echo_server)