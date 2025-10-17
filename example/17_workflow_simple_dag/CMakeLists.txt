cmake_minimum_required(VERSION 3.16)

project(workflow_simple_dag LANGUAGES CXX)

add_executable(workflow_simple_dag
        source/main.cpp)

target_link_libraries(workflow_simple_dag PRIVATE concurrencpp::concurrencpp)

target_compile_features(workflow_simple_dag PRIVATE cxx_std_20)