cmake_minimum_required(VERSION 3.16)
project(demo C)

add_library(util STATIC src/util/util.c)
target_include_directories(util PUBLIC include)

add_library(math STATIC src/math/add.c)
target_include_directories(math PUBLIC include)
target_link_libraries(math PUBLIC util)

add_executable(app src/main.c)
target_include_directories(app PRIVATE include)
target_link_libraries(app PRIVATE math util)
target_compile_definitions(app PRIVATE LEVEL=3)
