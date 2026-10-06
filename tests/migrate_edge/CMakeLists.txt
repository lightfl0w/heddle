add_executable(kernel src/main.c)
target_link_options(kernel PRIVATE -T kernel.ld -nostdlib)
target_link_libraries(kernel PRIVATE pthread m)
