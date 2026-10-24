project(#{ name: "firmware", build_dir: "out", default_toolchain: "arm-none-eabi" });

toolchain(#{ name: "arm-none-eabi", family: "gcc", cc: "arm-none-eabi-gcc", ar: "arm-none-eabi-ar" });

let nucleo_f4 = platform(#{ name: "nucleo_f4", arch: "armv7em", abi: "eabihf", float: "hard", toolchain: "arm-none-eabi" });
let bluepill_f1 = platform(#{ name: "bluepill_f1", arch: "armv7m", abi: "eabi", float: "soft", toolchain: "arm-none-eabi" });

let freertos = pkg(#{ name: "freertos", version: "10.5.1" });

fn firmware(board, chip, plat, dep) {
    target(#{
        name: "fw_" + board,
        type: "exe",
        src: ["src/" + chip + ".c"],
        deps: [dep],
        platform: plat,
        entry: "reset_handler",
        linker_script: "boards/" + board + ".ld",
    });
}

let BOARDS = #{ "nucleo": ["f4", nucleo_f4], "bluepill": ["f1", bluepill_f1] };

for board in BOARDS.keys() {
    let info = BOARDS[board];
    firmware(board, info[0], info[1], freertos);
}
