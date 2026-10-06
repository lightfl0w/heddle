project(
    name = "firmware",
    build_dir = "out",
    default_toolchain = "arm-none-eabi",
)

toolchain(
    name = "arm-none-eabi",
    family = "gcc",
    cc = "arm-none-eabi-gcc",
    ar = "arm-none-eabi-ar",
)

nucleo_f4 = platform(
    name = "nucleo_f4",
    arch = "armv7em",
    abi = "eabihf",
    float = "hard",
    toolchain = "arm-none-eabi",
)

bluepill_f1 = platform(
    name = "bluepill_f1",
    arch = "armv7m",
    abi = "eabi",
    float = "soft",
    toolchain = "arm-none-eabi",
)

freertos = package(
    name = "freertos",
    version = "10.5.1",
)

def firmware(board, chip, platform):
    return target(
        name = "fw_" + board,
        type = "exe",
        src = ["src/" + chip + ".c"],
        deps = [freertos],
        platform = platform,
        entry = "reset_handler",
        linker_script = "boards/" + board + ".ld",
    )

BOARDS = {
    "nucleo": ("f4", nucleo_f4),
    "bluepill": ("f1", bluepill_f1),
}

for board, (chip, platform) in BOARDS.items():
    firmware(board, chip, platform)
