cmake_minimum_required(VERSION 4.4)
project(audioprog)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(raylib)

add_executable(audioprog main.cpp)
target_link_libraries(audioprog PRIVATE raylib)