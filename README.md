# heddle

构建工具。读 `heddle.star` 或 `heddle.toml`，生成依赖图，增量执行。

引擎是 loom（`src/core`），负责 DAG 调度、内容哈希增量、头文件扫描和 CAS 缓存。

## 构建

```sh
xmake
```

产出 `build/linux/x86_64/release/heddle` 和同目录的 `loom`。

## 用法

在工程目录里直接跑目标名：

```sh
heddle app
```

也可写成 `heddle build app`。子命令：

```sh
heddle init           # 生成一个新工程
heddle check          # 只做配置静态检查
heddle toolchains     # 列出探测到的工具链
heddle tool install   # 恢复工具链 + 依赖，写/修 heddle.lock
heddle tool plan      # 只打印包解析计划
heddle env verify     # 校验环境与锁文件一致
heddle run TARGET     # 构建后运行
heddle test [TARGET]  # 构建并运行声明的测试
heddle watch [TARGET] # 盯着源文件，变了就重建
heddle install        # 安装构建产物
heddle uninstall      # 卸载
heddle vcpkg ...      # 读 vcpkg port / 导入已构建的树
heddle migrate        # 从 CMake / XMake 迁移
```

工程根目录默认是当前目录，用 `-C` 切换：

```sh
heddle -C sub app
heddle -C /path/to/proj app
```

完整选项：

| 选项 | 含义 |
| --- | --- |
| `-C DIR` | 工程根目录，默认 `.` |
| `-j N` | 并发数 |
| `-t NAME` | 指定工具链，默认自动探测 |
| `-v`, `--verbose` | 打印工具链与 `N ran, M cached` |
| `--cache DIR` | CAS 根目录 |
| `--remote URL` | 远程 CAS，目录或 `http(s)://` |
| `--no-cache` | 关闭缓存 |
| `--registry DIR\|URL` | 包 registry，默认 `$HEDDLE_REGISTRY` |
| `--offline` | 不访问 registry |
| `--prefix DIR` | 安装前缀 |
| `--destdir DIR` | 安装暂存目录 |
| `--dry-run` | 只打印，不执行 |
| `--compile-db[=FILE]` | 生成 `compile_commands.json` |

## 新建工程

```sh
heddle init                 # 交互选择类型
heddle init embedded fw     # 指定类型和目录
heddle init lib --force     # 覆盖已存在的文件
heddle init exe app --star  # 生成 heddle.star 而不是 heddle.toml
heddle init --list          # 列出类型
```

| 类型 | 生成内容 |
| --- | --- |
| `exe` | 本机可执行，一个 `src/main.c` |
| `lib` | 静态库 + 公共头 + 安装规则 |
| `embedded` | 交叉目标 + 链接脚本 + `entry`，架构用 `--arch` 选 |
| `baremetal` | 引导扇区 + 内核 + 磁盘镜像，演示 `raw` / `custom` |

`embedded` 支持 `armv7em` `armv7m` `armv6m` `armv8m` `cortex-m7` `aarch64`
`riscv32imac` `riscv64` `xtensa` `avr` `msp430`。生成的 `[target] arch`
会自动推出编译器前缀和编译参数。

`--star` 生成 `heddle.star`（`exe` 和 `lib`），不加就是 `heddle.toml`。
默认不覆盖已有文件，`--force` 才覆盖。

## 工具链与依赖

`heddle tool install` 同时恢复工具链和依赖，落到工程内的隔离 store。

```toml
[toolchain]
cross-gcc = "arm-none-eabi@12.2.0"
qemu      = "8.0.0"

[dependencies]
zlib   = "pkgconfig"      # 交给 host 的 pkg-config
libssl = "1.1.1"          # 交给 vcpkg（或 heddle registry）

[target]
arch  = "armv7em"
abi   = "eabihf"
float = "hard"
```

```sh
heddle tool plan      # 看解析结果，不动文件
heddle tool install   # 恢复工具链 + 依赖，写 heddle.lock
heddle app            # 用被管理的交叉编译器和依赖构建
heddle env verify     # 校验当前环境与锁一致
```

细节见 [工具链与依赖](docs/packages.md)。

## 配置

工程用 `heddle.star`（Starlark 子集）或 `heddle.toml`。两者都在时 `.star` 优先，
`.toml` 作为回退。

### heddle.star

```python
project(name = "firmware", build_dir = "out", default_toolchain = "arm-none-eabi")

toolchain(name = "arm-none-eabi", family = "gcc",
          cc = "arm-none-eabi-gcc", ar = "arm-none-eabi-ar")

nucleo = platform(name = "nucleo_f4", arch = "armv7em", abi = "eabihf",
                  float = "hard", toolchain = "arm-none-eabi")
bluepill = platform(name = "bluepill_f1", arch = "armv7m", abi = "eabi",
                    float = "soft", toolchain = "arm-none-eabi")

freertos = package(name = "freertos", version = "10.5.1")

def firmware(board, chip, platform):
    return target(
        name = "fw_" + board,
        type = "exe",
        src = ["src/" + chip + ".c"],
        deps = [freertos],
        platform = platform,
        linker_script = "boards/" + board + ".ld",
        entry = "reset_handler",
    )

BOARDS = {"nucleo": ("f4", nucleo), "bluepill": ("f1", bluepill)}

for board, (chip, platform) in BOARDS.items():
    firmware(board, chip, platform)
```

`project` / `toolchain` / `platform` / `package` 返回对象，可以赋给变量、
放进列表和字典，再传给 `target`。`deps` 里放 package 对象是包依赖（链接
`-l`），放字符串是依赖另一个 target。一个 `.star` 里可以有多个 platform，
构建哪个 target 就按它绑定的 platform 推 arch/abi/float：

```sh
heddle fw_nucleo     # -mcpu=cortex-m4 -mfloat-abi=hard
heddle fw_bluepill   # -mcpu=cortex-m3 -mfloat-abi=soft
```

`build(dir = ...)` 和 `target_config(...)` 也认，等价于 `project`/`platform`
的简化写法，适合单配置工程。

能用的东西：

| 功能 | 写法 |
| --- | --- |
| 函数 | `def f(a, b): return ...` |
| 循环 | `for x in list:`，`for k, v in dict.items():`，`for k, (a, b) in ...` |
| 条件 | `if` / `elif` / `else` |
| 列表、元组、字典 | `[1, 2]`，`(1, 2)`，`{"a": 1}` |
| 下标、切片 | `L[0]`，`L[1:3]` |
| 字符串 | `"a" + "b"`，`"v" + str(n)` |
| 内置 | `glob("src/*.c")`，`len`，`range`，`str` |
| 声明 | `project` `platform` `toolchain` `package` `target` `install` `build` `target_config` |

尾随逗号（`[1, 2,]`、`{"a": 1,}`）也认。

`while`、`import`、`class`、`lambda`、`global` 不支持，写了会报错并指出行号。

`target(...)` 的键：`name` `type` `src` `inc` `deps` `cflags` `ldflags`
`linker_script` `entry` `out` `platform` `start_group` `whole_archive`。

### heddle.toml

```toml
[build]
dir = "out"              # 产物目录，默认 "build"
toolchain = "auto"       # 自动探测，默认值

[toolchain.host]
cc = "cc"
cflags = ["-O2", "-Wall"]

[toolchain.debug]        # 用 -t debug 选它
based_on = "host"        # 继承 host 预设，再覆盖下面的字段
cflags = ["-O0", "-g3", "-Wall"]

[target.util]            # 目标名就是 section 名
type = "staticlib"
src = ["src/util/util.c"]
inc = ["src/util"]

[target.app]
type = "exe"
src = ["app/main.c"]
inc = ["src/util"]
deps = ["util"]          # 依赖另一个目标，自动拓扑排序
```

每个目标：

| 键 | 含义 |
| --- | --- |
| `type` | 必填，见下 |
| `src` | 必填，源文件，每个生成一个 .o，支持 `*` `**` `?` 通配 |
| `inc` | 头文件搜索目录 |
| `deps` | 依赖的目标名 |
| `cflags` | 本目标额外的编译选项 |
| `ldflags` | 本目标额外的链接选项 |
| `start_group` | 布尔，静态库循环依赖时用 `--start-group` 包住 `deps`（GNU 链接器） |
| `whole_archive` | 布尔，强制链入 `deps` 静态库的全部成员 |

`cflags` 和工具链的 `cflags` 叠加，工具链的在前。

`src` 里的通配在构建时展开，按路径去重排序，重名不会编译两次：

```toml
[target.app]
type = "exe"
src = ["app/*.c", "src/**/*.c"]
```

#### type

`type` 是目标种类，产物格式和后缀由工具链的 `objext`/`binext`/`libext`/`dllext`
决定，跟 `.exe` 这个后缀没关系。Linux 上 `type = "exe"` 出来的是 ELF。

| type | 别名 | 含义 | host/gcc/clang | mingw |
| --- | --- | --- | --- | --- |
| `exe` | `executable` `bin` | 可执行文件 | `out/app`（ELF） | `out/app.exe` |
| `staticlib` | `lib` | 静态库 | `out/libNAME.a` | `out/libNAME.a` |
| `sharedlib` | `dylib` `so` | 动态库 | `out/libNAME.so` | `out/NAME.dll` |
| `raw` | — | 单个源直接产出二进制，不链接 | `format = "bin"` 时是裸二进制 | 同 |
| `custom` | — | 跑一条自定义命令 | 需 `cmd` 与 `out` | 同 |

`raw` 用于引导扇区这类不链接的产物：

```toml
[target.mbr]
type = "raw"
src = ["src/mbr.asm"]
format = "bin"
out = "out/mbr.bin"
```

`custom` 的 `cmd` 交给 shell 执行：

```toml
[target.image]
type = "custom"
cmd = "cat out/mbr.bin out/kernel > out/os.img"
out = "out/os.img"
deps = ["mbr", "kernel"]
```

### 语言与插件

源文件按扩展名选编译器：

| 扩展名 | 工具 | 说明 |
| --- | --- | --- |
| `.c` | `cc` | C |
| `.cc` `.cpp` `.cxx` `.c++` | `cxx` | C++ |
| `.asm` | `as` | NASM，`-f` 由 `[target] arch` 决定（`elf`/`elf32`/`elf64`） |
| `.S` `.s` | `cc` | GAS |

链接脚本用 `linker_script` 声明，改脚本会触发重链接。它只声明意图，
具体参数由工具链的 `family` 决定：

```toml
[target.kernel]
type = "exe"
src = ["src/boot.asm", "src/kernel.c"]
linker_script = "src/kernel.ld"
entry = "reset_handler"
ldflags = ["-nostdlib", "-m32"]
```

加一门新语言在配置里声明：

```toml
[lang.mine]
ext = "mylang"
cmd = "mycompiler"
args = ["-O2", "{src}", "{out}"]
out = ".o"
```

`args` 支持 `{src}` `{out}` `{format}` `{root}` 占位符。`out` 是对象后缀。

### 工具链

不写 `[build] toolchain` 就是自动探测：按 `gcc`、`clang`、`tcc`、`msvc` 顺序，
取第一个 `PATH` 里存在的编译器。`auto` 和 `native` 等价于省略。

```
$ heddle --toolchains
detected toolchains (auto uses the first match):
  gcc      ok     cc
  clang    ok     clang
  tcc      -      tcc
  msvc     -      cl
```

想固定工具链就直接写：

```toml
[toolchain.myarm]
based_on = "gcc"
cc = "arm-none-eabi-gcc"
ar = "arm-none-eabi-ar"
cflags = ["-mcpu=cortex-m4", "-O2"]
```

预设：`host` `linux` `gcc` `clang` `macos` `mingw` `armcc` `iar` `msvc`。
可覆盖字段：`family` `cc` `cxx` `ar` `ld` `cflags` `ldflags` `objext` `binext`
`libext` `dllpre` `dllext` `soflag` `platform`。

找不到编译器或工具链名写错会直接报错。

### family

`family` 决定链接命令的方言，取值 `gnu` `armcc` `iar` `msvc`。预设自带，
也可以在 `[toolchain.X]` 里改。

同一个 `linker_script`，换个 `family` 就是另一套参数：

```toml
[target.kernel]
type = "exe"
src = ["src/*.c"]
linker_script = "kernel.ld"
entry = "reset_handler"
```

| family | 链接脚本参数 | entry |
| --- | --- | --- |
| `gnu` | `-T kernel.ld` | `-e reset_handler` |
| `armcc` | `--scatter=kernel.sct` | `--entry=reset_handler` |
| `iar` | `--config=kernel.icf` | `--entry=reset_handler` |
| `msvc` | `/DEF:kernel.def` | `/ENTRY:reset_handler` |

`armcc` 和 `iar` 拿到 `.ld` 时会先转成 `.sct`/`.icf`，转换是一个构建步骤，
改了 `.ld` 会重链。

### 多包

子目录带自己的 `heddle.toml` 就是一个包。根配置里列出：

```toml
[package]
deps = ["vendor/greet"]
```

子包的目标和根配置共用一个命名空间，根里可以直接 `deps = ["greet"]`。

## 静态检查

`--check` 在生成图之前检查，任何一条不过就报错退出：

- 依赖的目标存在，且不是 `exe`
- 目标之间无环
- 源文件存在、路径不出工程、扩展名受支持
- 一个源文件没有被两个目标同时声明
- `inc` 目录存在

```
$ heddle check
heddle: 2 targets ok, toolchain=gcc platform=gcc
  greet            staticlib  src=1 inc=1 deps=0
  hello            exe        src=1 inc=1 deps=1
```

## 产物与中间文件

host 工具链下：

| type | 产物 | 命令 |
| --- | --- | --- |
| `exe` | `out/NAME` | `cc -o out/NAME objs... libs...` |
| `staticlib` | `out/libNAME.a` | `ar rcs -o out/libNAME.a objs...` |
| `sharedlib` | `out/libNAME.so` | `cc -shared -o out/libNAME.so objs... libs...` |

链接时 `deps` 里的库自动加到命令行。

生成的图在 `.heddle/TARGET.graph`，缓存和指纹也在 `.heddle/`。

## 测试

用 `[target.NAME.test]` 声明，`heddle test` 构建后运行并校验：

```toml
[target.tmath]
type = "exe"
src = ["src/tmath.c"]
deps = ["math"]

[target.tmath.test]
expect = 0            # 期望退出码，默认 0
stdout = "sum=5"      # 可选，stdout 必须包含这段文字
args   = ["--fast"]   # 可选，传给被测程序的参数
```

```sh
heddle test           
```

输出：

```
[tmath] build tmath ... ok (exit 0)
[tfail] build tfail ... FAIL (exit 3, want 0)
heddle: 1 passed, 1 failed
```

## 监视

改完就编，不用手动重跑：

```sh
heddle watch            # 用默认 target
heddle watch app        # 指定 target
heddle watch --interval 0.2 app   # 轮询间隔（秒，默认 0.5）
```

它先构建一次，然后每 `interval` 轮询一次源文件、头文件和 `heddle.toml`/`heddle.star`
的修改时间与大小，一旦变化就重新加载工程并增量重建：

```
heddle: watching . every 500ms, Ctrl-C to stop
heddle: ok (exit 0)
heddle: change detected, rebuilding
heddle: ok (exit 0)
```

## 安装

在 target 上声明安装意图，不写脚本：

```toml
[target.kernel]
type = "exe"
src = ["src/kernel.c"]

[target.kernel.install]
bin = "out/kernel"

[target.mylib.install]
lib = "out/libmylib.a"
include = ["include/**/*.h"]
share = ["resources/*.md"]
etc = ["rootfs/etc/*.conf"]
rootfs = "rootfs"
```

```sh
heddle install                      # 用 [install] prefix
heddle install --prefix=/opt/myos   # 指定前缀
heddle install --destdir=./pkg      # 暂存到 ./pkg，不碰系统
heddle install kernel               # 只装一个 target
heddle install --dry-run            # 只打印计划
heddle uninstall --destdir=./pkg    # 按安装日志删
```

| 键 | 目标位置 |
| --- | --- |
| `bin` | `<prefix>/bin/`，取文件名 |
| `lib` | `<prefix>/lib/`，取文件名 |
| `include` | `<prefix>/include/`，保留通配基准目录下的相对路径 |
| `share` | `<prefix>/share/<target>/`，取文件名 |
| `etc` | `<prefix>/etc/<target>/`，取文件名 |
| `rootfs` | 整个目录树镜像到 `<prefix>/rootfs/` |
| `sysroot_lib` | `<sysroot>/lib/`，sysroot 取 `[target] sysroot` |
| `sysroot_include` | `<sysroot>/include/` |

`--destdir DIR` 时全部落在 `DIR` 下（`DIR/<prefix>/...`），不动系统。
不写 `--prefix` 也不写 `[install] prefix` 会报错，不会默认装到 `/usr/local`。

安装会写一份日志到 `.heddle/install/<hash>.log`，记录装了哪些文件。
`heddle uninstall` 只删日志里的文件，日志按 `destdir + prefix` 区分，
卸 A 目录不会碰 B 目录。

`.star` 工程里用 `install(target = "...", bin = [...], ...)` 声明，键名相同。

## vcpkg

`heddle vcpkg` 读 vcpkg 的 port 信息，或把已构建的 `installed/` 树收进
heddle 的 store 和锁文件。

```sh
heddle vcpkg show ports/zlib                 # 读 vcpkg.json
heddle vcpkg triplet x64-linux               # 打印对应的 [target] 片段
heddle vcpkg check .                         # 判断是不是 vcpkg registry
heddle vcpkg import ports/zlib --from installed --triplet x64-linux
```

`import` 把 `installed/<triplet>/{include,lib,bin,share}` 收进
`<store>/library/<name>/<version>/`，算出内容哈希，并更新 `heddle.lock`。
之后工程里像普通依赖一样声明：

```toml
[dependencies]
zlib = "1.3.1"
```

构建时自动加 `-I<store>/.../include` 和 `-L<store>/.../lib -l<真实库名>`。
库名从 `lib/` 里的文件名推出来（`libz.a` → `-lz`），所以 port 名和库名
不一致也没关系（`zlib` port 的库是 `libz`）。

| 命令 | 含义 |
| --- | --- |
| `vcpkg show DIR` | 读 `vcpkg.json`，打印名字、版本、依赖 |
| `vcpkg triplet NAME` | 打印 triplet 对应的 `[target]` 片段 |
| `vcpkg check DIR` | 检测 vcpkg registry（`ports/` + `versions/`） |
| `vcpkg import DIR --from TREE --triplet T` | 收进 store 并写锁 |

支持的 triplet 映射：`x64-linux` `x86-linux` `arm64-linux` `arm-linux`
`x64-windows` `x64-windows-static` `x64-osx` `arm64-osx`
`thumbv7m-none-eabi` `thumbv7em-none-eabihf` `arm-none-eabi`。

heddle 不跑 CMake，所以不能直接从 vcpkg registry 构建 port；用 `import` 把 vcpkg 编好的树接进来。

## 从 CMake / XMake 迁移

```sh
heddle migrate                  # 读 CMakeLists.txt 或 xmake.lua
heddle migrate ./proj           # 指定目录
heddle migrate --from xmake     # 目录里两个都有时强制选一个
heddle migrate --dry-run        # 只打印解析结果
heddle migrate --out my.toml    # 换个输出名
heddle migrate --star           # 生成 heddle.star
```

（`--from cmake` / `--from xmake`。）生成的清单里已有的同名文件不会覆盖。

### 导入 CMake

```sh
heddle migrate .                       # 自动：cmake api，失败回退文本解析
heddle migrate . --cmake-build ./b     # 指定 File API 用的构建目录
heddle migrate . --config Release      # 多配置生成器
heddle migrate . --cmake-arg -G --cmake-arg Ninja   # 透传给cmake 的参数（可重复）
heddle migrate . --no-cmake-api        # 强制文本解析
```

| CMake | XMake | heddle |
| --- | --- | --- |
| `add_executable(a ...)` | `set_kind("binary")` | `type = "exe"` |
| `add_library(a STATIC ...)` | `set_kind("static")` | `type = "staticlib"` |
| `add_library(a SHARED ...)` | `set_kind("shared")` | `type = "sharedlib"` |
| `target_sources` / 列表参数 | `add_files(...)` | `src = [...]` |
| `target_include_directories` | `add_includedirs(...)` | `inc = [...]` |
| `target_link_libraries` | `add_deps(...)` / `add_links(...)` | `deps = [...]` |
| `target_compile_definitions` | `add_defines(...)` | `cflags = [...]` |
| `target_compile_options` | `add_cxflags(...)` | `cflags = [...]` |
| `target_link_options` | `add_ldflags(...)` | `ldflags = [...]` |
| `-T file` / `-Wl,-T,file` | `add_ldflags("-T", "file")` | `linker_script = "file"` |
| — | `add_syslinks(...)` | `ldflags = ["-l..."]` |

`target_link_libraries` 里不是本工程 target 的名字（`pthread`、`m`）会变成
`-lpthread`、`-lm`。`-T` 会识别成语义化的 `linker_script`。

> 注意：heddle 要求一个源文件只属于一个 target，而 CMake 允许同一源编进多个target。遇到这种文件，迁移会打印提示，你需要手动拆分（否则 `heddle check` 会拒绝）

转不了的会列出来，不会静默丢掉（比如 xmake 的 `remove_files` 和全局的`include_directories`）。

生成的清单可以直接 `heddle <target>` 构建，也可以 `heddle check`。

## 自测

```sh
bash tests/run.sh
```

## 源码

```
xmake.lua                heddle 和 loom 两个 target
include/heddle/          heddle 头文件
include/core/            loom 头文件
src/heddle/              heddle 主体（cmake_api.c = CMake File API 导入）
src/core/                loom 引擎
tests/                   测试工程和脚本
docs/                    文档
build.txt                loom 自身的构建图，用于自举
```

## 文档

- [语言插件](docs/language-plugins.md)：内置语言、声明新语言、占位符、C 接口
- [工具链与依赖](docs/packages.md)：工具链与依赖管理、交叉 target 推导、锁文件、离线缓存、环境校验
