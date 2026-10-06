add_rules("mode.debug", "mode.release")

set_languages("gnu11")
add_cxflags("-Wall", "-Wextra")

local version = os.getenv("VERSION") or "0.0.0-dev"
local commit  = os.getenv("COMMIT") or "unknown"
local btarget = os.getenv("BUILD_TARGET") or "unknown"

add_cxflags('-DBUILD_VERSION="' .. version .. '"')
add_cxflags('-DBUILD_COMMIT="' .. commit .. '"')
add_cxflags('-DBUILD_TARGET="' .. btarget .. '"')

if not is_plat("windows") then
    add_syslinks("pthread")
end

target("loom")
    set_kind("binary")
    add_files("src/core/*.c")
    add_includedirs("include/core")

target("heddle")
    set_kind("binary")
    add_files("src/heddle/*.c")
    add_files("src/core/*.c")
    remove_files("src/core/main.c")
    add_includedirs("include/core", "include/heddle")
