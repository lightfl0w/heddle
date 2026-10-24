build(#{ dir: "out" });

target_config(#{ arch: "armv7em", abi: "eabihf", float: "hard" });
toolchain(#{ name: "cross", cc: "arm-none-eabi-gcc", based_on: "gcc" });
pkg(#{ name: "freertos", version: "10.5.1" });

fn firmware(board, chip) {
    target(#{
        name: "fw_" + board,
        type: "exe",
        src: ["src/" + chip + ".c"],
        linker_script: "boards/" + board + ".ld",
        entry: "reset_handler",
    });
}

let BOARDS = #{ nucleo: "f4", bluepill: "f1" };

for name in BOARDS.keys() {
    firmware(name, BOARDS[name]);
}
