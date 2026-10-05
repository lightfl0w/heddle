add_rules("mode.debug", "mode.release")

set_languages("gnu11")
add_cxflags("-Wall", "-Wextra")

target("loom")
    set_kind("binary")
    add_files("src/core/*.c")
    add_includedirs("include/core")
    add_syslinks("pthread")

target("heddle")
    set_kind("binary")
    add_files("src/heddle/*.c")
    add_files("src/core/*.c")
    remove_files("src/core/main.c")
    add_includedirs("include/core", "include/heddle")
    add_syslinks("pthread")
