#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

#include "ai_student_impl.h"

enum {
  PREFETCH_EMPTY,   // 槽位为空，可以发起下一条预取
  PREFETCH_PENDING, // 请求已发出，父进程尚未确认收到完整结果
  PREFETCH_READY,   // 父进程已收到完整记录，等待交付给调用者
  PREFETCH_FAILED,  // 读取或通信失败，停止使用并等待清理
};

// 当前框架每个 worker 进程只有一个活动 session。此状态属于当前进程，
// 不覆盖模型 memory 或 KV 的 state 槽位；fork 后父子必须通过管道交付数据。
static struct {
  struct ai_session *owner;
  int pid;
  int request_fd;
  int data_fd;
  int phase;
  int request;
  int next_request;
  char block[AI_KV_FILE_BYTES];
} prefetch;

static int
read_exact(int fd, void *buffer, int length)
{
  char *p = buffer;
  int n;

  while(length > 0) {
    n = read(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static int
write_exact(int fd, const void *buffer, int length)
{
  const char *p = buffer;
  int n;

  while(length > 0) {
    n = write(fd, p, length);
    if(n <= 0)
      return -1;
    p += n;
    length -= n;
  }
  return 0;
}

static void
helper_loop(struct ai_session *session, int request_fd, int data_fd)
{
  char name[AI_STREAM_NAME_LEN];
  int fd;
  int request;
  int expected = 0;
  int status = 1;
  int i;

  // 只保留协议端点，关闭继承的同步 KV fd、结果管道及其他无关 fd
  for(i = 0; i < NOFILE; i++)
    if(i != request_fd && i != data_fd)
      close(i);

  ai_student_kv_name(name, session->worker);
  fd = open(name, O_RDONLY);
  if(fd >= 0) {
    for(;;) {
      // 读取请求编号，-1 表示父进程已结束
      if(read_exact(request_fd, &request, sizeof(request)) < 0)
        break;
      if(request == -1) {
        status = 0;
        break;
      }
      if(request < 0 || request >= session->requests || request != expected)
        break;

      // 读取完整 KV 记录并将请求编号与数据交付给父进程
      if(read_exact(fd, prefetch.block, AI_KV_FILE_BYTES) < 0 ||
         write_exact(data_fd, &request, sizeof(request)) < 0 ||
         write_exact(data_fd, prefetch.block, AI_KV_FILE_BYTES) < 0)
        break;
      expected++;
    }
    close(fd);
  }
  close(request_fd);
  close(data_fd);
  exit(status);
}

int
ai_student_prefetch_begin(struct ai_session *session)
{
  // (LAB3-AI，选做)：建立固定小窗口的 KV 预取状态。
  // 推荐使用轻量辅助进程提前读取下一条记录，让数据进入共享 buffer cache。
  int request_pipe[2];
  int data_pipe[2];
  int pid;

  if(session == 0 || prefetch.owner != 0 || session->worker < 0 ||
     session->worker >= AI_MAX_WORKERS || session->requests <= 0 ||
     session->requests > AI_MAX_REQUESTS)
    return -1;

  // 未获得任何资源时 owner 保持为空
  if(pipe(request_pipe) < 0)
    return -1;
  if(pipe(data_pipe) < 0) {
    close(request_pipe[0]);
    close(request_pipe[1]);
    return -1;
  }
  pid = fork();
  if(pid < 0) {
    close(request_pipe[0]);
    close(request_pipe[1]);
    close(data_pipe[0]);
    close(data_pipe[1]);
    return -1;
  }
  if(pid == 0)
    helper_loop(session, request_pipe[0], data_pipe[1]);

  close(request_pipe[0]);
  close(data_pipe[1]);
  prefetch.owner = session;
  prefetch.pid = pid;
  prefetch.request_fd = request_pipe[1];
  prefetch.data_fd = data_pipe[0];
  prefetch.phase = PREFETCH_EMPTY;
  prefetch.request = -1;
  prefetch.next_request = 0;
  return 0;
}

int
ai_student_prefetch_next(struct ai_session *session, int request)
{
  // (LAB3-AI，选做)：非阻塞地请求预取给定 request 的 KV 记录。
  if(session == 0 || prefetch.owner != session ||
     prefetch.phase != PREFETCH_EMPTY || request < 0 ||
     request >= session->requests || request != prefetch.next_request)
    return -1;

  // 只发送编号
  if(write_exact(prefetch.request_fd, &request, sizeof(request)) < 0) {
    prefetch.phase = PREFETCH_FAILED;
    return -1;
  }
  prefetch.request = request;
  prefetch.phase = PREFETCH_PENDING;
  return 0;
}

int
ai_student_prefetch_wait(struct ai_session *session, int request)
{
  // (LAB3-AI，选做)：等待对应预取完成，且不得改变父 worker 的文件偏移。
  int completed;

  if(session == 0 || prefetch.owner != session ||
     prefetch.phase != PREFETCH_PENDING || request != prefetch.request)
    return -1;

  if(read_exact(prefetch.data_fd, &completed, sizeof(completed)) < 0 ||
     completed != request ||
     read_exact(prefetch.data_fd, prefetch.block, AI_KV_FILE_BYTES) < 0) {
    prefetch.phase = PREFETCH_FAILED;
    return -1;
  }
  // 收到正确编号及完整记录后，对父进程宣告 READY
  prefetch.phase = PREFETCH_READY;
  return 0;
}

int
ai_student_prefetch_restore(struct ai_session *session, int request,
                            struct kv_entry *kv, struct ai_io *io)
{
  // (LAB3-AI，选做)：把已由辅助进程从磁盘读取的完整记录交给父 worker。
  // 这次恢复仍必须对应一次真实文件读取，并且只在完整交付后累计 KV 字节数。
  if(session == 0 || prefetch.owner != session || kv == 0 || io == 0 ||
     prefetch.phase != PREFETCH_READY || request != prefetch.request)
    return -1;

  // 不再同步读取文件；只在完整交付时统计 helper 实际读过的 2048 字节
  memmove(kv, prefetch.block, AI_KV_FILE_BYTES);
  io->kv_read_bytes += AI_KV_FILE_BYTES;
  prefetch.next_request++;
  prefetch.request = -1;
  prefetch.phase = PREFETCH_EMPTY;
  return 0;
}

void
ai_student_prefetch_end(struct ai_session *session)
{
  // (LAB3-AI，选做)：关闭管道、等待辅助进程并释放全部预取资源。
  int stop = -1;

  if(session == 0 || prefetch.owner != session)
    return;

  // EMPTY/READY 时 helper 已发送完记录，正在等待下一编号，可正常通知停止。
  if(prefetch.phase == PREFETCH_EMPTY || prefetch.phase == PREFETCH_READY)
    write_exact(prefetch.request_fd, &stop, sizeof(stop));

  // 必须先关闭端点再 wait，避免 helper 阻塞在写满的数据管道中。
  close(prefetch.data_fd);
  close(prefetch.request_fd);
  if(prefetch.phase == PREFETCH_PENDING || prefetch.phase == PREFETCH_FAILED)
    kill(prefetch.pid);
  // 当前 worker 只创建这一个 helper；wait 回收它，防止僵尸进程。
  wait(0);
  prefetch.owner = 0;
  prefetch.pid = -1;
  prefetch.request_fd = -1;
  prefetch.data_fd = -1;
  prefetch.request = -1;
  prefetch.next_request = 0;
  prefetch.phase = PREFETCH_EMPTY;
}
