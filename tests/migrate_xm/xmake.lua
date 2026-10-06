-- demo project in xmake
target("util")
    set_kind("static")
    add_files("src/util/util.c")
    add_includedirs("include")

target("math")
    set_kind("static")
    add_files("src/math/add.c")
    add_includedirs("include")
    add_deps("util")

target("app")
    set_kind("binary")
    add_files("src/main.c")
    add_includedirs("include")
    add_deps("math", "util")
    add_defines("LEVEL=3")
    add_cxflags("-Wall")
