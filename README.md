# XV6-OSLAB26-HITSZ

哈尔滨工业大学（深圳）操作系统课程的 xv6 实验实现，基于 RISC-V 版 xv6。`main` 是项目索引，实验源码和测试脚本位于各自的分支。

## 实验分支

| 分支 | 实验内容 |
| --- | --- |
| [`util`](https://github.com/nagi-hikaru/xv6-oslab/tree/util) | 实现 `sleep`、`pingpong` 和 `find`，练习系统调用、管道和目录遍历。 |
| [`syscall`](https://github.com/nagi-hikaru/xv6-oslab/tree/syscall) | 实现非阻塞 `wait` 和 `yield`，记录进程退出状态，并为 `seccomp` 增加系统调用白名单、审计日志和子进程数量限制。 |
| [`pgtbl`](https://github.com/nagi-hikaru/xv6-oslab/tree/pgtbl) | 打印页表，为每个进程建立独立的内核页表，并共享用户地址映射，让内核通过硬件地址翻译读取用户内存。 |
| [`lock`](https://github.com/nagi-hikaru/xv6-oslab/tree/lock) | 实现每 CPU 的物理内存分配、分桶锁和 Clock 缓冲区替换，以及 AI 扩展中的模型分片缓存、KV 缓存持久化与异步预取。 |

## 构建与运行

准备宿主机 C 编译器、`make`、Perl、RISC-V 64 位 GCC 和 binutils、`qemu-system-riscv64`，以及运行评分脚本所需的 Python 3。

以 `util` 为例，克隆后切换到实验分支，再启动 xv6。

```bash
git clone https://github.com/nagi-hikaru/xv6-oslab.git
cd xv6-oslab
git switch util
make qemu
```

在 QEMU 终端按 `Ctrl+A`，松开后按 `X` 退出。回到宿主机终端后，在实验分支运行 `make grade` 执行该分支的评分脚本。

- `syscall` 可用 `make grade-extra` 运行附加测试。
- `lock` 可用 `make grade-ai` 运行 AI 扩展测试。

实验起始代码来自 [HITSZ 课程仓库](https://gitee.com/ftutorials/xv6-oslabs-hitsz)。
