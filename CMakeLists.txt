cmake_minimum_required(VERSION 3.10)

project(MainProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
add_executable(main main.cpp)
target_link_libraries(main PRIVATE Threads::Threads)
