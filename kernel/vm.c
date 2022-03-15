#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

pagetable_t     kernel_pagetable; // the kernel's page table.

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// create a direct-map page table for the kernel.
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);  
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
// 根据所给的 L2 页表地址，输入虚拟地址返回对应的 L0 页表的 PTE 的地址。
// 当计算得到的 PTE 无效时，若 alloc 不为 0 且 kalloc 可以分配一个 page，
// 那么重新分配下级页表，将下级页表物理地址转换后赋值本级页表的 PTE 并有效性设置为 1。
// 输入 va 或是 PGROUNDDOWN(va) 没有影响。PX(level, va) 下两个没区别，本就是要右移的。
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      // 如果该 PTE 有效性为 1 的话，计算得到下一级页表的地址
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      // 有效性为 0 的话就给这个 PTE 分配一个下级页表
      // alloc 为 0 或分配页表出错时直接 return 0，报错返回。
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;

      // 给下级页表的 4096 个字节赋值 0
      memset(pagetable, 0, PGSIZE);
      // 本级页表的 PTE 赋值为下级页表物理地址转换后的 PTE
      // 本级页表的 PTE 设置有效性为 1。
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  // 此时 pagetable 为 L0 页表。
  // 返回对应的 L0 页表的 PTE 的地址
  // 如果 L0 页表是新分配的，那么该 PTE 的有效性为 0。
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  // lab2 part2: A kernel page table per process.
  // 是否要同步更新所有进程中的 k_pagetable 不懂
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
/*
|                                                |
|                                                |
==== 有等号的是 PGSIZE 的整数倍 ---------------- |---------------
|                                                |         |
|-------- va -----------对应--------- pa --------|         |
|                        |                       |         |
|                        |                       |    PGSIZE 4096 bit = 2^12
|---------------------offset---------------------|    offset 也是 12 位
|                        |                       |         |
|                        |                       |         |
==== PGROUNDDOWN(va) --对应---- pa - offset -----|---------------
|                                                |         |
|                                                |         |
|                                                |         |
|                                                |         |
|                                                |         |
|                                                |         |
|                                                |         |
==== n * PGSIZE ---------------------------------|---------------
|                                                |
虚拟地址                                     物理地址
*/
// 本函数以 pagetable 为 L2 页表，以 PGROUNDDOWN(va) 为虚拟地址，用 walk() 创建 L1、L0 页表，
// 并在 L0 页表上创建 PPN 为 pa - offset 的 PTE，并设置 PTE 的属性为 perm 且有效。
// 使得 PGROUNDDOWN(va) 经 L2 页表 pagetable 转换后得到 pa - offset，如上图。
// 考虑 offset 的存在，等价于实现了在 L2 页表 pagetable 上使虚拟地址 va 指向 pa。
// 
// 总结，使 pagetable 上，虚拟地址 va 开始的 size 空间指向 pa 开始的物理地址的 size 空间，PTE 属性为 perm | PTE_V。
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    // 根据提供的 L2 页表地址为虚拟地址 a 分配三级页表，并返回 L0 页表的 PTE 的地址。
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;

    // 返回的 PTE 有效性为 1 说明，该页表不是新分配的，需要 remap。
    if(*pte & PTE_V) 
      panic("remap");
    
    // 物理地址 pa 清除 offset 后转换为 PTE 并设置 RWXU 等属性为 perm，设置有效性为 1。
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
// 虚拟地址 va 开始的 npages 个页面对应的 L0-PTE 都设置为 0，
// 如果 do_free 为 1 则将该 PTE 对应的物理 page 释放
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
// 将 kalloc 返回的填满了 0x05 的 Page 全部赋值 0 后返回
// 为什么 kalloc 不直接全部赋值 0后返回呢 不懂
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// kvmmap for process's k_pagetable
void kvmmapProcKernelPagetable(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm){
  if(mappages(pagetable, va, sz, pa, perm) != 0)
    panic("kvmmapProcKernelPagetable");
}

// kvminit() for process
pagetable_t kvmcreate(){
  pagetable_t pagetable;
  int i;

  pagetable = uvmcreate();

  // 因为 pagetable[0] 是 run 的 next 指针吗，所以 pagetable[1] 开始 不懂
  for(i = 1; i < PTENUMPERPAGE; i++){
    pagetable[i] = kernel_pagetable[i];
  }

  // uart registers
  kvmmapProcKernelPagetable(pagetable, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmapProcKernelPagetable(pagetable, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmapProcKernelPagetable(pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmapProcKernelPagetable(pagetable, PLIC, PLIC, 0x400000, PTE_R | PTE_W);
  
  /* 为什么要去掉这三部分呢 不懂
  // map kernel text executable and read-only.
  kvmmapProcKernelPagetable(pagetable, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmapProcKernelPagetable(pagetable, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmapProcKernelPagetable(pagetable, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
 */  
  
  return pagetable;
}

// 释放页表不释放叶子物理内存
void freewalkWithoutPhysicalMemory(pagetable_t pagetable_L2){
  // for(int i = 0; i < 512; i++){
  //   pte_t pte = pagetable_L2[i];
  //   if(pte & PTE_V){
  //     // this PTE points to a lower-level page table.
  //     uint64 child = PTE2PA(pte);
  //     freewalkWithoutPhysicalMemory((pagetable_t)child, depth - 1);
  //     pagetable_L2[i] = 0;
  //   }
  // }
  // if(depth >= 0) kfree((void*)pagetable_L2);

  for(int L2_i = 0; L2_i < PTENUMPERPAGE; L2_i++){
    pte_t pte_L2 = pagetable_L2[L2_i];
    if(pte_L2 & PTE_V){
      // this PTE points to a lower-level page table.
      pagetable_t pagetable_L1 = (pagetable_t)PTE2PA(pte_L2);

      for(int L1_i = 0; L1_i < PTENUMPERPAGE; L1_i++){
        pte_t pte_L1 = pagetable_L1[L1_i];
        if(pte_L1 & PTE_V){
          // this PTE points to a lower-level page table.
          pagetable_t pagetable_L0 = (pagetable_t)PTE2PA(pte_L1);
          kfree((void*)pagetable_L0);
        }          
      }

      kfree((void*)pagetable_L1);
    }
  }
  kfree((void*)pagetable_L2);

  // pte_t pte = pagetable_L2[0];
  // pagetable_t level1 = (pagetable_t) PTE2PA(pte);
  // for (int i = 0; i < PTENUMPERPAGE; i++) {
  //   pte_t pte = level1[i];
  //   if (pte & PTE_V) {
  //     uint64 level2 = PTE2PA(pte);
  //     kfree((void *) level2);
  //     level1[i] = 0;
  //   }
  // }
  // kfree((void *) level1);
  // kfree((void *) pagetable_L2);

}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// recursively: 递归地; leaf: 叶。
// Recursively free page-table pages.
// All leaf mappings must already have been removed.
// 递归地释放 pagetable 下的所有 page 和 pagetable 
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    // (pte & (PTE_R|PTE_W|PTE_X) 这个有什么用呢，不懂。
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

void vmprintHelper(pagetable_t pagetable, int depth) {
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if (pte & PTE_V) {
      for ( int depth_i = 0; depth_i < depth; depth_i++) {
        printf(".. ");
      }

      uint64 child = PTE2PA(pte);
      printf("%d: pte %p pa %p\n", i, pte, child);

      if(depth == 3)  continue;
      else{
        
        // 此处奇奇怪怪，不懂

        // vmprintHelper((pagetable_t)child, depth+1);
        // vmprintHelper((pagetable_t)child, depth++);
        depth++;
        vmprintHelper((pagetable_t)child, depth);
        depth--;
      }
    }
  }
}

void vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  vmprintHelper(pagetable, 1);
}
