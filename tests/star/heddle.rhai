build(#{ dir: "out" });

let SRC = ["src/util.c", "src/math.c", "src/main.c"];

fn static_lib(name, sources, deps) {
    target(#{
        name: name,
        type: "staticlib",
        src: sources,
        inc: ["include"],
        deps: deps,
    });
}

static_lib("util", [SRC[0]], []);
static_lib("math", [SRC[1]], ["util"]);

target(#{
    name: "app",
    type: "exe",
    src: [SRC[2]],
    inc: ["include"],
    deps: ["math", "util"],
    cflags: ["-DLEVEL=3"],
});
