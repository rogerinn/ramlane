cmake_minimum_required(VERSION 3.15)
project(LayoutEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Inclua todos os diretórios necessários
include_directories(include)
include_directories(flatbuffers) # diretório com layout_map_generated.h

# Adiciona o executável
add_executable(main main.cpp src/layout_engine.cpp)

# Agora sim você pode configurar include dirs do target
target_include_directories(main PRIVATE include flatbuffers)
