// Buffer cache.
//
// Each hash bucket contains cached disk blocks and has its own lock.
// A separate lock serializes cache misses and buffer migration. Buffers
// with no references are recycled using a Clock second-chance policy.
// Bucket locks protect lists, reference counts, and access bits. Each
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
  uchar accessed[NBUF]; // Clock 访问位，由 buffer 所属桶的锁保护
  uint hand;            // 下一个要检查的 buffer，由 addlock 保护
  struct spinlock addlock;
  struct bucket buckets[NBUCKETS];
} bcache;

static char locknames[NBUCKETS][16];

void
binit(void)
{
  struct buf *b;

  // 初始化替换锁
  initlock(&bcache.addlock, "bcache.miss");
  bcache.hand = 0;
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
    bcache.accessed[b - bcache.buf] = 0;
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
  uint index, source;
  int scanned, cleared;

  // Cache hits
  acquire(&bcache.buckets[n].lock);
  b = blookup(dev, blockno);
  if(b){
    b->refcnt++;
    bcache.accessed[b - bcache.buf] = 1;
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
    bcache.accessed[b - bcache.buf] = 1;
    release(&bcache.buckets[n].lock);
    release(&bcache.addlock);
    acquiresleep(&b->lock);
    return b;
  }
  release(&bcache.buckets[n].lock);

  victim = 0;
  while(victim == 0){
    cleared = 0;
    for(scanned = 0; scanned < NBUF; scanned++){
      index = bcache.hand;
      bcache.hand = (bcache.hand + 1) % NBUF;
      b = &bcache.buf[index];
      source = b->blockno % NBUCKETS;
      acquire(&bcache.buckets[source].lock);
      if(b->refcnt == 0){
        if(bcache.accessed[index]){
          // 最近使用过，清除访问位并给一次第二机会
          bcache.accessed[index] = 0;
          cleared = 1;
        } else {
          // 同一桶锁内检查、预留并摘链
          b->refcnt = 1;
          for(link = &bcache.buckets[source].head;
              *link != b; link = &(*link)->next)
            ;
          *link = b->next;
          victim = b;
        }
      }
      release(&bcache.buckets[source].lock);
      if(victim)
        break;
    }
    // 扫描一圈没有可替换块，也没有清位的空闲块，说明当时均被引用
    if(victim == 0 && !cleared){
      release(&bcache.addlock);
      panic("bget: no buffers");
    }
  }

  // 替换缓存块
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;

  acquire(&bcache.buckets[n].lock);
  bcache.accessed[victim - bcache.buf] = 1;
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
  uint n;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  n = b->blockno % NBUCKETS;
  acquire(&bcache.buckets[n].lock);
  b->refcnt--;
  release(&bcache.buckets[n].lock);
}

void
bpin(struct buf *b)
{
  uint n = b->blockno % NBUCKETS;

  acquire(&bcache.buckets[n].lock);
  b->refcnt++;
  bcache.accessed[b - bcache.buf] = 1;
  release(&bcache.buckets[n].lock);
}

void
bunpin(struct buf *b)
{
  uint n = b->blockno % NBUCKETS;

  acquire(&bcache.buckets[n].lock);
  b->refcnt--;
  release(&bcache.buckets[n].lock);
}
