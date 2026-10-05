add_rules("mode.debug", "mode.release")

target("loom")
    set_kind("binary")
    add_files("src/*.c")
    add_includedirs("include")