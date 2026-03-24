cmake_minimum_required(VERSION 3.17)
project(game_server CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "")

add_compile_options(
    -Wall -Wextra -Wpedantic
    -O2 -march=native
    -fno-exceptions
)

file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(game_server ${SOURCES})

target_include_directories(game_server PRIVATE src)

target_link_libraries(game_server pthread)
