# loom

极简的 DAG 构建执行器，带内容哈希增量。

## 特性

| 能力 | 说明 |
| --- | --- |
| DAG 调度 | `build.txt` 描述节点与依赖，多线程执行，失败传播，`--retry` 重试 |
| 内容哈希 DB | `FNV-1a` 内容指纹，mtime+size 短路；内容没变就不重建 |
| 增量计划 | 命令/输入/输出指纹陈旧才运行，脏节点沿 DAG 传播到后代 |
| 并行调度 | `-j N`，调度器只运行计划中活跃的节点，inactive 节点直接视为完成 |

## 构建

```sh
cc -std=c11 -O2 -Wall -Wextra -pthread -Iinclude -c src/*.c   
xmake
```

自举：

```sh
./loom -f build.txt -j6 --logdir .build
```

## 用法

```sh
./loom -f build.txt -j8 --logdir .build
```

选项：

| 选项 | 含义 |
| --- | --- |
| `-f FILE` | 构建图文件（必填） |
| `-j N` | 并发任务数，默认 1 |
| `--retry N` | 失败节点重试次数，默认 0 |
| `--cwd DIR` | 命令执行目录 |
| `--logdir DIR` | 日志与指纹 DB 目录，默认 `.build` |
| `--stop` | 失败后停止调度（默认 keep-going） |

## build.txt 格式

```
# 节点序号: 命令 [< 依赖节点序号...]
0: cc -O2 -c a.c -o a.o
1: cc -O2 -c b.c -o b.o
2: cc a.o b.o -o app < 0 1
```

- 输出文件取自命令里的 `-o` 值。
- 输入文件取自命令里带 `.` 或 `/` 的普通 token（排除 `-o` 及其值和自身输出）。
- 依赖顺序始终由 `<` 决定。
