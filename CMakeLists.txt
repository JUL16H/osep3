cmake_minimum_required(VERSION 3.14)

project(osep3 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Build spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Build spdlog tests" FORCE)
add_subdirectory(external/spdlog)

add_executable(test ${CMAKE_CURRENT_SOURCE_DIR}/src/test.cpp)
target_include_directories(test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(test
    PRIVATE
        spdlog::spdlog
)

add_executable(cli ${CMAKE_CURRENT_SOURCE_DIR}/src/cli.cpp)
target_include_directories(cli
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(cli
    PRIVATE
        spdlog::spdlog
)
