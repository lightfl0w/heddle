# heddle

构建工具。读 `heddle.toml`，生成依赖图，增量执行。

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
heddle env verify     # 校验环境与锁文件完全一致
heddle install        # 安装构建产物
heddle uninstall      # 卸载
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
| `--offline` | 禁止访问 registry |
| `--prefix DIR` | 安装前缀 |
| `--destdir DIR` | 安装暂存目录 |
| `--dry-run` | 只打印安装计划 |

## 新建工程

```sh
heddle init                 # 交互选择类型
heddle init embedded fw     # 指定类型和目录
heddle init lib --force     # 覆盖已存在的文件
heddle init --list          # 列出类型
```

| 类型 | 生成内容 |
| --- | --- |
| `exe` | 本机可执行，一个 `src/main.c` |
| `lib` | 静态库 + 公共头 + 安装规则 |
| `embedded` | 交叉目标（`--arch` 选架构）+ 链接脚本 + `entry` |
| `baremetal` | 引导扇区 + 内核 + 磁盘镜像，演示 `raw` / `custom` |

`embedded` 的架构用 `--arch` 指定，不指定就交互选择：

```sh
heddle init embedded fw --arch=armv7em
```

支持 `armv7em` `armv7m` `armv6m` `armv8m` `cortex-m7` `aarch64`
`riscv32imac` `riscv64` `xtensa` `avr` `msp430`。
生成的 `[target] arch` 会自动推出编译器前缀和编译参数，不用手写。

默认不覆盖已有文件

## 工具链与依赖的统一管理

`heddle tool install` 同时恢复工具链和所有依赖，落到工程内的隔离 store。

```toml
[toolchain]
cross-gcc = "arm-none-eabi@12.2.0"
qemu      = "8.0.0"

[dependencies]
freertos = "10.5.1"

[target]
arch  = "armv7em"
abi   = "eabihf"
float = "hard"
```

```sh
heddle tool plan      # 看解析结果，不动文件
heddle tool install   # 恢复工具链 + 依赖，写 heddle.lock
heddle app            # 用被管理的交叉编译器和依赖构建
heddle env verify     # 校验当前环境与锁完全一致
```

细节见 [工具链与依赖](docs/packages.md)。

## heddle.toml

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
| `src` | 必填，源文件列表，每个生成一个 .o，支持 `*` `**` `?` 通配 |
| `inc` | 头文件搜索目录 |
| `deps` | 依赖的目标名 |
| `cflags` | 本目标额外的编译选项 |
| `ldflags` | 本目标额外的链接选项 |

`cflags` 和工具链的 `cflags` 叠加，工具链的在前。

`src` 里可以写通配，构建时展开成实际文件：

```toml
[target.app]
type = "exe"
src = ["app/*.c", "src/**/*.c"] 
```

展开后按路径去重排序，重名不会编译两次。

### type

`type` 是目标种类，产物格式和后缀由工具链的 `objext`/`binext`/`libext`/`dllext` 决定，
跟 `.exe` 这个后缀没关系。Linux 上 `type = "exe"` 出来的是 ELF。

| type | 别名 | 含义 | host/gcc/clang | mingw |
| --- | --- | --- | --- | --- |
| `exe` | `executable` `bin` | 可执行文件 | `out/app`（ELF） | `out/app.exe` |
| `staticlib` | `lib` | 静态库 | `out/libNAME.a` | `out/libNAME.a` |
| `sharedlib` | `dylib` `so` | 动态库 | `out/libNAME.so` | `out/NAME.dll` |
| `raw` | — | 单个源直接产出二进制，不链接 | `format = "bin"` 时是裸二进制 | 同 |
| `custom` | — | 跑一条自定义命令 | 需 `cmd` 与 `out` | 同 |

`raw` 用于引导扇区这类不需要链接的产物：

```toml
[target.mbr]
type = "raw"
src = ["src/mbr.asm"]
format = "bin"
out = "out/mbr.bin"
```

`custom` 用于执行自定义命令，`cmd` 里的命令交给 shell 执行：

```toml
[target.image]
type = "custom"
cmd = "cat out/mbr.bin out/kernel > out/os.img"
out = "out/os.img"
deps = ["mbr", "kernel"]
```

### 语言与插件

源文件按扩展名选编译器，内置这些：

| 扩展名 | 工具 | 说明 |
| --- | --- | --- |
| `.c` | `cc` | C |
| `.cc` `.cpp` `.cxx` `.c++` | `cxx` | C++ |
| `.asm` | `as` | NASM，`-f` 由 `[target] arch` 决定（`elf`/`elf32`/`elf64`） |
| `.S` `.s` | `cc` | GAS |

链接脚本用 `linker_script` 声明，改脚本会触发重链接。这是语义声明，
具体参数由当前工具链的 `family` 决定，不写死 `-T`：

```toml
[target.kernel]
type = "exe"
src = ["src/boot.asm", "src/kernel.c"]
linker_script = "src/kernel.ld"
entry = "reset_handler"
ldflags = ["-nostdlib", "-m32"]
```

详见下面的 [family](#family)。

加一门新语言只需在配置里声明：

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

探测到了就填 `cflags`（`-t` 选中的那个 section）；想固定工具链就直接写：

```toml
[toolchain.myarm]
based_on = "gcc"
cc = "arm-none-eabi-gcc"
ar = "arm-none-eabi-ar"
cflags = ["-mcpu=cortex-m4", "-O2"]
```

预设：`host` `linux` `gcc` `clang` `macos` `mingw` `armcc` `iar` `msvc`。
可覆盖字段：`family` `cc` `cxx` `ar` `ld` `cflags` `ldflags` `objext` `binext` `libext` `dllpre` `dllext` `soflag` `platform`。

找不到编译器或工具链名写错会直接报错，不静默回退。

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

`armcc` 和 `iar` 拿到 `.ld` 时会先转成 `.sct`/`.icf`（一个构建步骤，改了 `.ld` 会重链）。

### 多包

子目录带自己的 `heddle.toml` 就是一个包。根配置里列出：

```toml
[package]
deps = ["vendor/greet"] 
```

子包的目标和根配置共用一个命名空间，所以根里可以直接 `deps = ["greet"]`。

## 静态检查

`--check` 在生成图之前检查，任何一条不过就报错退出：

- 依赖的目标存在，且不是 `exe`
- 目标之间无环
- 源文件存在、路径不出工程、扩展名是 `.c` `.cc` `.cpp` `.cxx` `.c++` `.S` 之一
- 一个源文件没有被两个目标同时声明
- `inc` 目录存在

```
$ heddle -C . --check
heddle: 2 targets ok, toolchain=host platform=host
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
| `rootfs` | 把整个目录树镜像到 `<prefix>/rootfs/` |
| `sysroot_lib` | `<sysroot>/lib/`，sysroot 取 `[target] sysroot` |
| `sysroot_include` | `<sysroot>/include/` |

`--destdir DIR` 时全部落在 `DIR` 下（`DIR/<prefix>/...`），不动系统。
不写 `--prefix` 也不写 `[install] prefix` 会直接报错，不会默认装到 `/usr/local`。

安装会写一份日志到 `.heddle/install/<hash>.log`，记录装了哪些文件。
`heddle uninstall` 只删日志里的文件，日志按 `destdir + prefix` 区分，
所以卸 A 目录不会碰 B 目录。

## 测试

```sh
bash tests/run.sh
```

## 源码

```
xmake.lua                heddle 和 loom 两个 target
include/heddle/          heddle 头文件
include/core/            loom 头文件
src/heddle/              heddle 主体
src/core/                loom 引擎
tests/                   测试工程和脚本
docs/                    文档
build.txt                loom 自身的构建图，用于自举
```

## 文档

- [语言插件](docs/language-plugins.md)：内置语言、声明新语言、占位符、C 接口
- [工具链与依赖](docs/packages.md)：统一包管理、交叉 target 推导、锁文件、离线缓存、环境校验
