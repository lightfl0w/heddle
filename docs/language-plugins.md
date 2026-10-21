# 语言插件

heddle 按源文件扩展名挑编译器。内置 C、C++、NASM、GAS，其他语言在 `heddle.toml` 里声明。

## 内置

| 扩展名 | 工具 | 参数 |
| --- | --- | --- |
| `.c` | `cc` | `-c` |
| `.cc` `.cpp` `.cxx` `.c++` | `cxx` | `-c` |
| `.S` `.s` | `cc` | `-c` |
| `.asm` | `as` | `-f <format>` |

工具名是工具链里的字段，不是写死的路径：

- `cc` → `[toolchain.X] cc`
- `c++` → `[toolchain.X] cxx`
- `nasm` → `[toolchain.X] as`
- `ar` `ld` → 同名字段
- 其他名字原样当可执行文件

换汇编器改工具链就行：

```toml
[toolchain.host]
as = "yasm"
```

## 声明语言

```toml
[lang.mine]
ext    = "mylang"          # 不带点
cmd    = "mycompiler"      # 可执行文件或工具名
args   = ["-O2", "{src}", "{out}"]
out    = ".o"              # 对象后缀，默认 .o
cflags = false             # 是否接收 cflags
fmt    = false             # 是否接收 -f <format>
```

section 名只作标识，匹配看 `ext`。

## 占位符

`args` 里能用 `{src}`（源文件）、`{out}`（目标文件）、`{format}`（目标的 `format` 字段）、`{root}`（工程根）。

写了占位符就完全按你写的拼：

```toml
[lang.copy]
ext  = "txt"
cmd  = "cp"
args = ["{src}", "{out}"]
```

```
cp src/a.txt out/t_a.txt
```

一个都不写时，末尾自动补 `inc` 目录、`-I<root>`、`-o <out>`、`<src>`：

```toml
[lang.simple]
ext  = "sim"
cmd  = "simcc"
args = ["-c"]
```

```
simcc -c -I. -o out/t_a.o src/a.sim
```

## 构建

编译成对象再链接：

```toml
[target.lib]
type = "staticlib"
src = ["src/a.mylang"]
```

单个源直接产出成品，不链接：

```toml
[target.fw]
type = "raw"
src = ["src/fw.mylang"]
format = "bin"
out = "out/fw.bin"
```

`format` 通过 `{format}` 传给编译器；语言声明 `fmt = true` 时还会自动加 `-f <format>`。

## 接口

语言表是进程内的一张表（`heddle/src/lang.rs`），对外只有 `heddle.toml` 里的 `[lang.NAME]` 这一条路：
`load_toml` 读配置，调用 `lang::add(ext, cmd, args, out, cflags, fmt)` 逐条登记。

按扩展名查表用 `lang::for_path`，找不到返回 `None`。先注册先匹配，内置的排在前面，
所以声明同名扩展名不会覆盖内置语言——内置的 C/C++/汇编永远先命中。

上限 32 门语言。
