cmake_minimum_required(VERSION 3.15)
project(LayoutEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include_directories(include)

add_executable(main main.cpp src/layout_engine.cpp)
