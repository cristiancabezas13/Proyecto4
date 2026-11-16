// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#define NPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
static struct {
  struct spinlock lock;
  int count[NPAGES];
} refs;

static int
pageindex(uint64 pa)
{
  if (pa % PGSIZE || pa < (uint64)end || pa >= PHYSTOP)
    panic("pageindex");
  return (pa - KERNBASE) / PGSIZE;
}

void
krefinc(uint64 pa)
{
  int i = pageindex(pa);
  acquire(&refs.lock);
  if (refs.count[i] < 1)
    panic("krefinc: unowned page");
  refs.count[i]++;
  release(&refs.lock);
}

int
krefcount(uint64 pa)
{
  int i = pageindex(pa);
  acquire(&refs.lock);
  int n = refs.count[i];
  release(&refs.lock);
  return n;
}

// All paths nesting these locks take kmem before refs.
void
kmemstat(uint64 *freepages, uint64 *sharedpages)
{
  *freepages = *sharedpages = 0;
  acquire(&kmem.lock);
  acquire(&refs.lock);
  for (struct run *r = kmem.freelist; r; r = r->next)
    (*freepages)++;
  for (int i = 0; i < NPAGES; i++)
    if (refs.count[i] > 1)
      (*sharedpages)++;
  release(&refs.lock);
  release(&kmem.lock);
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&refs.lock, "refs");
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE) {
    // Boot is single-threaded: seed ownership before the first kfree.
    refs.count[pageindex((uint64)p)] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  int i = pageindex((uint64)pa);
  acquire(&kmem.lock);
  acquire(&refs.lock);
  if (refs.count[i] < 1)
    panic("kfree: no reference");
  int remaining = --refs.count[i];
  release(&refs.lock);
  if (remaining) {
    release(&kmem.lock);
    return;
  }

  // Fill with junk only after the last owner releases the page.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r) {
    kmem.freelist = r->next;
    acquire(&refs.lock);
    refs.count[pageindex((uint64)r)] = 1;
    release(&refs.lock);
  }
  release(&kmem.lock);

  if (r)
    memset((char *)r, 5, PGSIZE); // fill with junk
  return (void *)r;
}
