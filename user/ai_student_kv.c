#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#include "ai_student_impl.h"

// state[0] 已经被使用
enum {
  KV_PHASE = 1,    // 下标 1：保存 KV 当前阶段
  KV_FD,           // 下标 2：保存文件描述符，-1 表示未打开
  KV_NEXT_WRITE,   // 下标 3：保存下一条应写入的请求编号
  KV_NEXT_READ,    // 下标 4：保存下一条应恢复的请求编号
  KV_CLEANUP_FILE, // 下标 5：保存结束时是否需要清理临时文件的标志
};

enum {
  KV_UNINITIALIZED = 0, // 阶段值 0：尚未初始化或已经清理
  KV_INITIALIZED,       // 阶段值 1：状态已初始化，尚未开始写入
  KV_WRITING,           // 阶段值 2：正在按顺序写入 KV
  KV_STORED,            // 阶段值 3：全部 KV 已写完，写文件已关闭
  KV_READING,           // 阶段值 4：正在按顺序从文件恢复 KV
  KV_FAILED,            // 阶段值 5：文件 I/O 失败，停止读写并等待清理
};

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

int
ai_student_kv_begin(struct ai_session *session)
{
  // (LAB3-AI，附加题二)：初始化当前 worker 的 KV 持久化状态。
  // 可采用每请求文件，也可采用每 worker 顺序流，但不得保留内存副本绕过恢复。
  if(session == 0 || session->worker < 0 ||
     session->worker >= AI_MAX_WORKERS || session->requests <= 0 ||
     session->requests > AI_MAX_REQUESTS ||
     session->state[KV_PHASE] != KV_UNINITIALIZED)
    return -1;

  session->state[KV_FD] = -1;
  session->state[KV_NEXT_WRITE] = 0;
  session->state[KV_NEXT_READ] = 0;
  session->state[KV_CLEANUP_FILE] = 0;
  session->state[KV_PHASE] = KV_INITIALIZED;
  return 0;
}

int
ai_student_kv_begin_write(struct ai_session *session)
{
  // (LAB3-AI，附加题二)：开始 KV 写入阶段并准备所需文件。
  char name[AI_STREAM_NAME_LEN];
  int fd;

  if(session == 0 || session->state[KV_PHASE] != KV_INITIALIZED)
    return -1;

  ai_student_kv_name(name, session->worker);
  session->state[KV_CLEANUP_FILE] = 1;
  fd = open(name, O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0) {
    session->state[KV_PHASE] = KV_FAILED;
    return -1;
  }
  session->state[KV_FD] = fd;
  session->state[KV_NEXT_WRITE] = 0;
  session->state[KV_PHASE] = KV_WRITING;
  return 0;
}

int
ai_student_kv_store(struct ai_session *session, int request,
                    struct kv_entry *kv, struct ai_io *io)
{
  // (LAB3-AI，附加题二)：把当前请求的完整 KV cache 写入 xv6 文件系统。
  // request 必须是 [0, session->requests) 内预期的下一条；非法序号返回 -1，
  // 且不得改变文件位置、下一请求序号、KV 内容或 I/O 统计，后续合法调用仍须可用。
  // 必须循环处理短写；只有 AI_KV_FILE_BYTES 字节全部成功后才更新统计。
  // aiinfer 随后会立即释放原页面，因此不能依赖 kv 指针中的残留数据。
  // 非法请求在 I/O 前拒绝
  if(session == 0 || kv == 0 || io == 0 ||
     session->state[KV_PHASE] != KV_WRITING ||
     session->state[KV_FD] < 0 || request < 0 ||
     request >= session->requests || request != session->state[KV_NEXT_WRITE])
    return -1;

  if(write_exact(session->state[KV_FD], kv, AI_KV_FILE_BYTES) < 0) {
    // 部分写入已改变文件偏移，终止本次会话
    session->state[KV_PHASE] = KV_FAILED;
    return -1;
  }
  io->kv_write_bytes += AI_KV_FILE_BYTES;
  session->state[KV_NEXT_WRITE]++;
  return 0;
}

int
ai_student_kv_end_write(struct ai_session *session)
{
  // (LAB3-AI，附加题二)：结束写阶段，确保文件状态完整并关闭写描述符。
  if(session == 0 || session->state[KV_PHASE] != KV_WRITING ||
     session->state[KV_FD] < 0 ||
     session->state[KV_NEXT_WRITE] != session->requests)
    return -1;

  if(close(session->state[KV_FD]) < 0) {
    session->state[KV_PHASE] = KV_FAILED;
    return -1;
  }
  session->state[KV_FD] = -1;
  session->state[KV_PHASE] = KV_STORED;
  return 0;
}

int
ai_student_kv_begin_read(struct ai_session *session)
{
  // (LAB3-AI，附加题二)：重新打开持久化文件，从文件起点开始恢复。
  char name[AI_STREAM_NAME_LEN];
  int fd;

  if(session == 0 || session->state[KV_PHASE] != KV_STORED)
    return -1;

  ai_student_kv_name(name, session->worker);
  fd = open(name, O_RDONLY);
  if(fd < 0) {
    session->state[KV_PHASE] = KV_FAILED;
    return -1;
  }
  session->state[KV_FD] = fd;
  session->state[KV_NEXT_READ] = 0;
  session->state[KV_PHASE] = KV_READING;
  return 0;
}

int
ai_student_kv_restore(struct ai_session *session, int request,
                      struct kv_entry *kv, struct ai_io *io)
{
  // (LAB3-AI，附加题二)：精确恢复当前请求的完整 KV cache。
  // request 必须是 [0, session->requests) 内预期的下一条；非法序号返回 -1，
  // 且不得改变文件位置、下一请求序号、目标 KV 内容或 I/O 统计，后续合法调用仍须可用。
  // 目标页已被覆盖，必须循环处理短读；全部成功后再更新读取字节数。
  // 单项验证：先运行 modelprep，再运行 aikvtest。
  if(session == 0 || kv == 0 || io == 0 ||
     session->state[KV_PHASE] != KV_READING ||
     session->state[KV_FD] < 0 || request < 0 ||
     request >= session->requests || request != session->state[KV_NEXT_READ])
    return -1;

  if(read_exact(session->state[KV_FD], kv, AI_KV_FILE_BYTES) < 0) {
    session->state[KV_PHASE] = KV_FAILED;
    return -1;
  }
  io->kv_read_bytes += AI_KV_FILE_BYTES;
  session->state[KV_NEXT_READ]++;
  return 0;
}

void
ai_student_kv_end(struct ai_session *session)
{
  // (LAB3-AI，附加题二)：关闭 KV 文件并删除当前 worker 的临时持久化文件。
  char name[AI_STREAM_NAME_LEN];

  // 先检查初始化状态，避免把零初始化的槽位当成需要关闭的 fd 0
  if(session == 0 || session->state[KV_PHASE] == KV_UNINITIALIZED)
    return;
  if(session->state[KV_FD] >= 0)
    close(session->state[KV_FD]);
  session->state[KV_FD] = -1;
  if(session->state[KV_CLEANUP_FILE]) {
    ai_student_kv_name(name, session->worker);
    unlink(name);
  }
  session->state[KV_NEXT_WRITE] = 0;
  session->state[KV_NEXT_READ] = 0;
  session->state[KV_CLEANUP_FILE] = 0;
  session->state[KV_PHASE] = KV_UNINITIALIZED;
}
