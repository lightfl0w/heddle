build(dir = "out")

SRC = ["src/util.c", "src/math.c", "src/main.c"]

def static_lib(name, sources, deps):
    return target(
        name = name,
        type = "staticlib",
        src = sources,
        inc = ["include"],
        deps = deps,
    )

static_lib("util", SRC[0:1], [])
static_lib("math", SRC[1:2], ["util"])

target(
    name = "app",
    type = "exe",
    src = SRC[2:3],
    inc = ["include"],
    deps = ["math", "util"],
    cflags = ["-DLEVEL=3"],
)
