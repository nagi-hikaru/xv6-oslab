// Buffer cache.
//
// Each hash bucket contains cached disk blocks and has its own lock.
// A separate lock serializes cache misses and buffer migration. Buffers
// with no references are recycled by their last-release timestamps.
// Bucket locks protect lists, reference counts, and timestamps. Each
// buffer has a sleep lock that protects its contents across disk I/O.
//
// Interface:
// * bread returns a referenced buffer owned exclusively by the caller.
// * bwrite writes a buffer owned by the caller to disk.
// * brelse releases the buffer; do not use it afterwards.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKETS 13 // 哈希桶数量

struct bucket {
  struct spinlock lock;
  struct buf *head;
};

struct {
  struct buf buf[NBUF];
  uint timestamps[NBUF];
  struct spinlock addlock;
  struct bucket buckets[NBUCKETS];
} bcache;

static char locknames[NBUCKETS][16];

// 获取当前时间戳
static uint
now(void)
{
  uint timestamp;

  acquire(&tickslock);
  timestamp = ticks;
  release(&tickslock);
  return timestamp;
}

void
binit(void)
{
  struct buf *b;
  uint timestamp = now();

  // 初始化替换锁
  initlock(&bcache.addlock, "bcache.miss");
  // 初始化哈希桶锁
  for(int i = 0; i < NBUCKETS; i++){
    int n = snprintf(locknames[i], sizeof(locknames[i]) - 1,
                     "bcache_%d", i);
    locknames[i][n] = '\0';
    initlock(&bcache.buckets[i].lock, locknames[i]);
    bcache.buckets[i].head = 0;
  }

  // 初始化缓存块
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->dev = (uint)-1;
    b->blockno = 0;
    b->valid = 0;
    b->refcnt = 0;
    bcache.timestamps[b - bcache.buf] = timestamp;
    b->next = bcache.buckets[0].head;
    bcache.buckets[0].head = b;
  }
}

// 调用者必须含有对应桶的锁
static struct buf*
blookup(uint dev, uint blockno)
{
  struct buf *b;

  for(b = bcache.buckets[blockno % NBUCKETS].head; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno)
      return b;
  }
  return 0;
}

// Return a referenced buffer owned exclusively by the caller.
static struct buf*
bget(uint dev, uint blockno)
{
  uint n = blockno % NBUCKETS;
  struct buf *b, *victim;
  struct buf **link;
  uint oldest;  // 保存候选块的时间戳快照，用于比较和重新检查
  int source;   // 保存候选块原来所在的桶编号

  // Cache hits
  acquire(&bcache.buckets[n].lock);
  b = blookup(dev, blockno);
  if(b){
    b->refcnt++;
    release(&bcache.buckets[n].lock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.buckets[n].lock);

  // 同一时间只允许一个未命中请求为新的磁盘块分配缓存
  acquire(&bcache.addlock);
  // 再次检查等待锁期间其他进程有没有创建
  acquire(&bcache.buckets[n].lock);
  b = blookup(dev, blockno);
  if(b){
    b->refcnt++;
    release(&bcache.buckets[n].lock);
    release(&bcache.addlock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.buckets[n].lock);

  for(;;){
    victim = 0;
    oldest = 0;
    source = -1;

    for(int i = 0; i < NBUCKETS; i++){
      acquire(&bcache.buckets[i].lock);
      for(b = bcache.buckets[i].head; b; b = b->next){
        uint timestamp = bcache.timestamps[b - bcache.buf];
        if(b->refcnt == 0 && (victim == 0 || timestamp < oldest)){
          victim = b;
          oldest = timestamp;
          source = i;
        }
      }
      release(&bcache.buckets[i].lock);
    }

    if(victim == 0){
      release(&bcache.addlock);
      panic("bget: no buffers");
    }

    acquire(&bcache.buckets[source].lock);
    // 检查缓存块没有被修改
    if(victim->refcnt != 0 ||
       bcache.timestamps[victim - bcache.buf] != oldest){
      release(&bcache.buckets[source].lock);
      continue;
    }

    // 预留引用，并从原桶删除
    victim->refcnt = 1;
    for(link = &bcache.buckets[source].head;
        *link != victim; link = &(*link)->next)
      ;
    *link = victim->next;
    release(&bcache.buckets[source].lock);
    break;
  }

  // 替换缓存块
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;

  acquire(&bcache.buckets[n].lock);
  victim->next = bcache.buckets[n].head;
  bcache.buckets[n].head = victim;
  release(&bcache.buckets[n].lock);
  release(&bcache.addlock);

  acquiresleep(&victim->lock);
  return victim;
}

// Return an exclusively owned buf with the indicated block's contents.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid){
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk. The caller must own b.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release the sleep lock, then drop our reference under the bucket lock.
// Our reference keeps the buffer in its bucket until the decrement.
void
brelse(struct buf *b)
{
  uint timestamp, n;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  timestamp = now();
  n = b->blockno % NBUCKETS;
  acquire(&bcache.buckets[n].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    bcache.timestamps[b - bcache.buf] = timestamp;
  release(&bcache.buckets[n].lock);
}

void
bpin(struct buf *b)
{
  uint n = b->blockno % NBUCKETS;

  acquire(&bcache.buckets[n].lock);
  b->refcnt++;
  release(&bcache.buckets[n].lock);
}

void
bunpin(struct buf *b)
{
  uint timestamp = now();
  uint n = b->blockno % NBUCKETS;

  acquire(&bcache.buckets[n].lock);
  b->refcnt--;
  if(b->refcnt == 0)
    bcache.timestamps[b - bcache.buf] = timestamp;
  release(&bcache.buckets[n].lock);
}
