#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    if (argc != 1) {
        fprintf(2, "Usage: pingpong\n");
        exit(1);
    }

    int p2c[2]; // Pipe1: 父 -> 子
    int c2p[2]; // Pipe2: 子 -> 父

    // 创建两个管道
    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        fprintf(2, "Error: pipe creation failed\n");
        exit(1);
    }

    int cur_pid, from_pid;
    char buf[5];

    int pid = fork();
    if (pid < 0) {
        fprintf(2, "Error: fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // 子进程
        close(p2c[1]); // 关闭 Pipe1 的写端
        close(c2p[0]); // 关闭 Pipe2 的读端

        if (read(p2c[0], &from_pid, sizeof(int)) < 0 || read(p2c[0], buf, 4) < 0) {
            fprintf(2, "Error: read failed\n");
            exit(1);
        }
        buf[4] = '\0';
        close(p2c[0]); // 读后关闭 Pipe1 读端

        printf("%d: received %s from pid %d\n", getpid(), buf, from_pid);

        cur_pid = getpid();
        if (write(c2p[1], &cur_pid, sizeof(int)) < 0 || write(c2p[1], "pong", 4) < 0) {
            fprintf(2, "Error: write failed\n");
            exit(1);
        }
        close(c2p[1]); // 写后关闭 Pipe1 写端

        exit(0);
    } else {
        // 父进程
        close(p2c[0]); // 关闭 Pipe1 读端
        close(c2p[1]); // 关闭 Pipe2 写端

        cur_pid = getpid();
        if (write(p2c[1], &cur_pid, sizeof(int)) < 0 || write(p2c[1], "ping", 4) < 0) {
            fprintf(2, "Error: write failed\n");
            exit(1);
        }
        close(p2c[1]); // 写后关闭 Pipe1 写端

        if (read(c2p[0], &from_pid, sizeof(int)) < 0 || read(c2p[0], buf, 4) < 0) {
            fprintf(2, "Error: read failed\n");
            exit(1);
        }
        buf[4] = '\0';
        close(c2p[0]); // 读后关闭 Pipe2 读端

        printf("%d: received %s from pid %d\n", getpid(), buf, from_pid);

        wait(0);
        exit(0);
    }
    exit(0);
}