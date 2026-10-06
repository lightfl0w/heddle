build(dir = "out")

target(name = "app", type = "exe", src = ["src/main.c"])
install(target = "app", bin = ["out/app*"])

target(name = "mylib", type = "staticlib", src = ["src/lib.c"], inc = ["include"])
install(target = "mylib", lib = ["out/libmylib.a"], include = ["include/*.h"])
