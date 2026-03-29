cmake_minimum_required(VERSION 3.16)
project(PirateVsZombies CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -fsanitize=address,undefined")

add_executable(server
    server/src/main.cpp
)

target_include_directories(server PRIVATE
    shared/include
    server/src
)

target_link_libraries(server PRIVATE pthread)

find_package(SFML 2.5 COMPONENTS graphics window system QUIET)

if(SFML_FOUND)
    add_executable(client
        client/src/main.cpp
    )

    target_include_directories(client PRIVATE
        shared/include
        client/src
    )

    target_link_libraries(client PRIVATE
        sfml-graphics
        sfml-window
        sfml-system
        pthread
    )
    message(STATUS "SFML exist — enabled")
else()
    message(WARNING "SFML does not exist — disabled")
endif()
