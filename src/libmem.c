/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Helpers for transactional page replacement.
 * Implementations appear later in this file.
 */
static int peek_victim_page(struct mm_struct *mm,
                            addr_t *retpgn,
                            struct pcb_t *caller);

static int remove_fifo_page(struct mm_struct *mm,
                            addr_t pgn);

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  if(mm == NULL || rg_elmt == NULL)
    return -1;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  struct vm_area_struct *rg_node = get_vma_by_num(mm, rg_elmt->vmaid);

  if (rg_node == NULL)
    return -1;

  rg_elmt->rg_next = rg_node->vm_freerg_list;
  /* Enlist the new region */
  rg_node->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
    if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return NULL;

    return &mm->symrgtbl[rgid];
}
/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma;
  struct vm_rg_struct *newrg;

  addr_t inc_sz;
  addr_t old_sbrk;

  if (caller == NULL || caller->mm == NULL || alloc_addr == NULL || size == 0 || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    caller->mm->symrgtbl[rgid].vmaid = vmaid;

    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

#ifdef MM64
  inc_sz = ((size + PAGING64_PAGESZ - 1) / PAGING64_PAGESZ) * PAGING64_PAGESZ;
#else
  inc_sz = PAGING_PAGE_ALIGNSZ(size);
#endif

  if (inc_sz < size) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  old_sbrk = cur_vma->sbrk;

  struct sc_regs regs;
  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  regs.a3 = inc_sz;

  if(_syscall(caller->krnl, caller->pid, 17, &regs) <0){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  } /* SYSCALL 17 sys_memmap */

  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  caller->mm->symrgtbl[rgid].vmaid = vmaid;

  *alloc_addr = old_sbrk;

  if (inc_sz > size){
    newrg = malloc(sizeof(struct vm_rg_struct));
    if (newrg != NULL)
    {
      newrg->rg_start = old_sbrk + size;
      newrg->rg_end = old_sbrk + inc_sz;
      newrg->vmaid = vmaid;
      newrg->rg_next = NULL;
      if (enlist_vm_freerg_list(caller->mm, newrg) != 0)
        free(newrg);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  struct vm_area_struct *cur_vma ;
  struct vm_rg_struct *rgnode;
  struct vm_rg_struct *freerg_node;

  if (caller == NULL || caller->mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (cur_vma == NULL){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode == NULL || rgnode->rg_start >= rgnode->rg_end || rgnode->vmaid != vmaid){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

#ifdef MM64
  addr_t start_pgn = PAGING64_PGN(rgnode->rg_start);
  addr_t end_pgn   = PAGING64_PGN(rgnode->rg_end - 1);
#else
  addr_t start_pgn = PAGING_PGN(rgnode->rg_start);
  addr_t end_pgn   = PAGING_PGN(rgnode->rg_end - 1);
#endif

  for (addr_t pgn = start_pgn; pgn <= end_pgn; pgn++) {
    uint64_t pte = pte_get_entry(caller->mm, pgn, caller);
    if (PAGING_PAGE_PRESENT(pte) && dma_frame_is_pinned(PAGING_FPN(pte))) {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
  }

  freerg_node = malloc(sizeof(struct vm_rg_struct));
  if (freerg_node == NULL){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  freerg_node->vmaid = vmaid;
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  /*enlist the obsoleted memory region */
  if (enlist_vm_freerg_list(caller->mm, freerg_node) != 0) {
    free(freerg_node);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  rgnode->vmaid = -1;
  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  if(proc == NULL || proc->mm == NULL || size == 0 || reg_index >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  addr_t  addr;
  int vmaid = 0;
  addr_t alloc_sz = size;

#ifdef MM64
  vmaid = mm64_vmaid_for_size(size);
  if(vmaid < 0)
  {
    return -1;
  }
  alloc_sz = mm64_align_size(vmaid, size);
  if(alloc_sz < size)
  {
    return -1;
  }
#endif

  int val = __alloc(proc, vmaid, reg_index, alloc_sz, &addr);
  if (val == -1)
  {
    return -1;
  }

#ifdef IODUMP
  printf("PHYSICAL MEMORY AFTER ALLOCATION\n");
  printf("liballoc: allocated %lu bytes at 0x%016lx for reg_index %u\n",
       (unsigned long)alloc_sz,
       (unsigned long)addr,
       (unsigned int)reg_index);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); 
#endif
  MEMPHY_dump(proc->krnl->mram);
#endif
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  if(proc == NULL || proc->mm == NULL || reg_index >= PAGING_MAX_SYMTBL_SZ)
  {
    return -1;
  }

  struct vm_rg_struct *rgnode = get_symrg_byid(proc->mm, reg_index);
  if(rgnode == NULL || rgnode->rg_start >= rgnode->rg_end || rgnode->vmaid < 0)
  {
    return -1;
  }

  int val = __free(proc, rgnode->vmaid, reg_index);
  if (val == -1)
  {
    return -1;
  }

#ifdef IODUMP
  printf("PHYSICAL MEMORY AFTER FREEING\n");
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); 
#endif
  MEMPHY_dump(proc->krnl->mram);
#endif
  return 0;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */

int pg_getpage(struct mm_struct *mm, addr_t pgn,
               int *fpn, struct pcb_t *caller)
{
    if (mm == NULL || fpn == NULL ||
        caller == NULL || caller->mm == NULL ||
        caller->mm != mm)
        return -1;

    uint64_t pte = pte_get_entry(mm, pgn, caller);

    /*
     * A non-present PTE is not necessarily a swapped page.
     * Reject unmapped pages instead of reading an invalid
     * swap frame.
     */
    if (!PAGING_PAGE_PRESENT(pte) &&
        !(pte & PAGING_PTE_SWAPPED_MASK))
        return -1;

    if (!PAGING_PAGE_PRESENT(pte)) {
        /* The page is marked SWAPPED: restore it from swap. */

    addr_t vicpgn;
    addr_t vicpte;
    addr_t vicfpn, swpfpn;
    struct sc_regs regs;
    addr_t tgtfpn = PAGING_SWP(pte);
    /* TODO Initialize the target frame storing our variable */
    //  addr_t tgtfpn

    /* TODO: Play with your paging theory here */
    /* Find victim page */

    /* Reserve swap space before removing a victim from FIFO. */
    if (MEMPHY_get_freefp(caller->krnl->active_mswp,
                          &swpfpn) != 0) {
        return -1;
    }

    /* Select the oldest resident page that is not DMA-pinned. */
    if (find_victim_page(caller->mm, &vicpgn, caller) != 0) {
        MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
        return -1;
    }

    vicpte = pte_get_entry(caller->mm, vicpgn, caller);
    vicfpn = PAGING_FPN(vicpte);
    /* TODO: Implement swap frame from MEMRAM to MEMSWP and vice versa*/

    /* TODO copy victim frame to swap
     * SWP(vicfpn <--> swpfpn)
     * SYSCALL 1 sys_memmap
     */
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = vicfpn;
    regs.a3 = swpfpn;
    _syscall(caller->krnl, caller->pid, 17, &regs);

    /* Update page table */
    // pte_set_swap(...);

    /* Update its online status of the target page */
    // pte_set_fpn(...);
    __swap_cp_page(caller->krnl->active_mswp, tgtfpn, caller->krnl->mram,
                   vicfpn);
    MEMPHY_put_freefp(caller->krnl->active_mswp, tgtfpn);
    pte_set_swap(caller, vicpgn, 0, swpfpn);
    pte_set_fpn(caller->mm, pgn, vicfpn, caller);
    enlist_pgn_node(&caller->mm->fifo_pgn, pgn);
  }

  *fpn = PAGING_FPN(pte_get_entry(caller->mm, pgn, caller));

  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller)
{
#ifdef MM64
  addr_t pgn = PAGING64_PGN(addr);
#else
  int pgn = PAGING_PGN(addr);
#endif
  int fpn;

  addr_t phyaddr;
  addr_t off;
  struct sc_regs regs;

  if(mm == NULL || data == NULL || caller == NULL)
    return -1;  

#ifdef MM64
  if(PAGING64_IS_PAGEZERO_ADDR(addr))
    return -1;
#endif

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

#ifdef MM64
  off = addr % PAGING64_PAGESZ;
  phyaddr = ((addr_t)fpn * PAGING64_PAGESZ) + off;
#else
  off = addr % PAGING_PAGESZ;
  phyaddr = ((addr_t)fpn * PAGING_PAGESZ) + off;
#endif

//int phyaddr = ...

  /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */
  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  regs.a3 = 0;

  if(_syscall(caller->krnl, caller->pid, 17, &regs) < 0) {
    return -1;
  }

  *data = (BYTE)regs.a3;
  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
#ifdef MM64
  addr_t pgn = PAGING64_PGN(addr);
#else
  int pgn = PAGING_PGN(addr);
#endif
  int fpn;

  addr_t phyaddr;
  addr_t off;
  struct sc_regs regs;

  if(mm == NULL || caller == NULL)
    return -1;

#ifdef MM64
  if(PAGING64_IS_PAGEZERO_ADDR(addr))
    return -1;
#endif

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

#ifdef MM64
  off = addr % PAGING64_PAGESZ;
  phyaddr = ((addr_t)fpn * PAGING64_PAGESZ) + off;
#else
  off = addr % PAGING_PAGESZ;
  phyaddr = ((addr_t)fpn * PAGING_PAGESZ) + off;
#endif

  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */
  memset(&regs, 0, sizeof(regs));
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = value;

  if(_syscall(caller->krnl, caller->pid, 17, &regs) < 0) {
    return -1;
  }

  return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_area_struct *cur_vma;

  if(caller == NULL || caller->mm == NULL || data == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  cur_vma = get_vma_by_num(caller->mm, vmaid);

  if(currg == NULL || cur_vma == NULL || currg->vmaid != vmaid || offset >= (currg->rg_end - currg->rg_start)){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  int ret = pg_getval(caller->mm, currg->rg_start + offset, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libread - PAGING-based read a region memory */

int libread(struct pcb_t *proc, uint32_t source, addr_t offset, uint32_t *destination)
{
  BYTE data = 0;
  int val;

  if (proc == NULL || proc -> mm == NULL || destination == NULL)
    return -1;

  struct vm_rg_struct *rgnode = get_symrg_byid(proc->mm, source);
  if (rgnode == NULL || rgnode->rg_start >= rgnode->rg_end || rgnode->vmaid < 0 || offset >= (rgnode->rg_end - rgnode->rg_start))
    return -1;

  val = __read(proc, rgnode->vmaid, source, offset, &data);

  if (val != 0)
    return val;

  *destination = data;

#ifdef IODUMP
#ifdef PAGETBL_DUMP
    print_pgtbl(proc, 0, -1);
#endif
#endif

    return 0;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);

  if(caller == NULL || caller->mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  int ret;

  if (currg == NULL || cur_vma == NULL || currg->vmaid != vmaid || offset >= (currg->rg_end - currg->rg_start)) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  ret = pg_setval(caller->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  if(proc == NULL || proc->mm == NULL || destination >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  struct vm_rg_struct *rgnode = get_symrg_byid(proc->mm, destination);
  if(rgnode == NULL || rgnode->rg_start >= rgnode->rg_end || rgnode->vmaid < 0 || offset >= (rgnode->rg_end - rgnode->rg_start))
    return -1;  
    
  int val = __write(proc, rgnode->vmaid, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); 
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t *caller, uint32_t size, uint32_t reg_index)
{
    if (caller == NULL || caller->krnl == NULL ||
        caller->krnl->mm == NULL || size == 0 ||
        reg_index >= PAGING_MAX_SYMTBL_SZ)
        return -1;

    addr_t addr;
    int ret = (int)__kmalloc(caller, 0, reg_index, (addr_t)size, &addr);


if (ret == 0) {
#ifdef MM64
    addr_t pgn = PAGING64_PGN(addr);
    addr_t pte = pte_get_entry(caller->krnl->mm, pgn, caller);

    printf("[KMALLOC TEST] pid=%u rgid=%u size=%u va=0x%llx "
           "present=%d fpn=%llu\n",
           caller->pid,
           reg_index,
           size,
           (unsigned long long)addr,
           !!PAGING_PAGE_PRESENT(pte),
           (unsigned long long)PAGING_FPN(pte));

    if (!PAGING_PAGE_PRESENT(pte)) {
        fprintf(stderr,
                "[KMALLOC TEST] FAIL: kernel PTE not present\n");
        return -1;
    }
#else
    printf("[KMALLOC TEST] pid=%u rgid=%u size=%u\n",
           caller->pid, reg_index, size);
#endif
    return 0;
}

fprintf(stderr,
        "[KMALLOC TEST] FAIL pid=%u rgid=%u size=%u\n",
        caller->pid, reg_index, size);
return -1;
}

/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */


addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid,
                 addr_t size, addr_t *alloc_addr)
{
    if (caller == NULL || caller->krnl == NULL ||
        caller->krnl->mm == NULL ||
        alloc_addr == NULL || size == 0 ||
        rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return (addr_t)-1;

    pthread_mutex_lock(&kmm_lock);

    struct krnl_t *krnl = caller->krnl;
    struct vm_area_struct *cur_vma =
        get_vma_by_num(krnl->mm, vmaid);

    if (cur_vma == NULL) {
        pthread_mutex_unlock(&kmm_lock);
        return (addr_t)-1;
    }

    /* Try reusing an available kernel region. */
    struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

    while (rgit != NULL) {
        if (rgit->rg_start <= rgit->rg_end &&
            size <= rgit->rg_end - rgit->rg_start) {

            addr_t start = rgit->rg_start;
            rgit->rg_start += size;

            krnl->mm->symrgtbl[rgid].rg_start = start;
            krnl->mm->symrgtbl[rgid].rg_end = start + size;
            krnl->mm->symrgtbl[rgid].vmaid = vmaid;

            *alloc_addr = start;

            pthread_mutex_unlock(&kmm_lock);
            return 0;
        }

        rgit = rgit->rg_next;
    }

    /* Allocate new physical frames for the kernel region. */
#ifdef MM64
    addr_t align_sz = ((size - 1) / PAGING64_PAGESZ + 1) * PAGING64_PAGESZ;
    int incpgnum = (int)(align_sz / PAGING64_PAGESZ);
#else
    addr_t align_sz = PAGING_PAGE_ALIGNSZ(size);
    int incpgnum = (int)(align_sz / PAGING_PAGESZ);
#endif

    addr_t old_sbrk = cur_vma->sbrk;
    struct vm_rg_struct rgnode;

    if (vm_map_ram(caller, cur_vma->vm_start,
                   cur_vma->vm_end, old_sbrk,
                   incpgnum, &rgnode) != 0) {
        pthread_mutex_unlock(&kmm_lock);
        return (addr_t)-1;
    }

    cur_vma->sbrk = old_sbrk + align_sz;

    if (cur_vma->sbrk > cur_vma->vm_end)
        cur_vma->vm_end = cur_vma->sbrk;

    krnl->mm->symrgtbl[rgid].rg_start = old_sbrk;
    krnl->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
    krnl->mm->symrgtbl[rgid].vmaid = vmaid;

    *alloc_addr = old_sbrk;

    pthread_mutex_unlock(&kmm_lock);
    return 0;
}


#ifdef MM64
/*
 * Resolve one resident user virtual address to a physical address.
 *
 * This is a translation helper only.
 * It does not pin the frame against SWAP/FREE.
 */
static int __attribute__((unused))
dma_translate_user_va(struct pcb_t *caller,
                      addr_t va,
                      uint64_t *physical_addr)
{
    uint64_t pte;
    uint64_t fpn;
    uint64_t page_offset;
    uint64_t pa;

    if (caller == NULL || caller->mm == NULL ||
        caller->krnl == NULL || caller->krnl->mram == NULL ||
        physical_addr == NULL)
        return -1;

    if (PAGING64_IS_KERNEL_ADDR(va))
        return -1;

    pte = pte_get_entry(caller->mm,
                        PAGING64_PGN(va),
                        caller);

    if (!PAGING_PAGE_PRESENT(pte))
        return -1;

    fpn = PAGING_FPN(pte);
    page_offset = (uint64_t)va % PAGING64_PAGESZ;

    pa = fpn * PAGING64_PAGESZ + page_offset;

    if (pa >= (uint64_t)caller->krnl->mram->maxsz)
        return -1;

    *physical_addr = pa;
    return 0;
}
#endif


/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */

int libmem_dma_memcpy(struct pcb_t *caller,
                      uint32_t src,
                      uint32_t dst,
                      uint32_t offset,
                      uint32_t size)
{
    struct vm_rg_struct *src_rg;
    struct vm_rg_struct *dst_rg;
    addr_t src_start, dst_start;
    addr_t src_length, dst_length;

if (caller == NULL || caller->mm == NULL ||
    caller->krnl == NULL ||
    caller->krnl->mm == NULL)
    return -1;

    if (src >= PAGING_MAX_SYMTBL_SZ ||
        dst >= PAGING_MAX_SYMTBL_SZ ||
        size == 0)
        return -1;

pthread_mutex_lock(&mmvm_lock);
pthread_mutex_lock(&kmm_lock);

src_rg = get_symrg_byid(caller->mm, (int)src);
dst_rg = get_symrg_byid(caller->krnl->mm, (int)dst);

if (src_rg == NULL || dst_rg == NULL ||
    src_rg->rg_start >= src_rg->rg_end ||
    dst_rg->rg_start >= dst_rg->rg_end ||
    src_rg->vmaid < 0 || dst_rg->vmaid < 0) {
    pthread_mutex_unlock(&kmm_lock);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
}

    src_length = src_rg->rg_end - src_rg->rg_start;
    dst_length = dst_rg->rg_end - dst_rg->rg_start;

    /* Avoid integer overflow by checking with subtraction. */
if ((addr_t)offset > src_length ||
    (addr_t)offset > dst_length ||
    (addr_t)size > src_length - (addr_t)offset ||
    (addr_t)size > dst_length - (addr_t)offset) {
    pthread_mutex_unlock(&kmm_lock);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
}

    src_start = src_rg->rg_start + (addr_t)offset;
    dst_start = dst_rg->rg_start + (addr_t)offset;

#ifdef MM64
    /* m2s: source must be user, destination must be kernel. */
    if (PAGING64_IS_KERNEL_ADDR(src_start) ||
        !PAGING64_IS_KERNEL_ADDR(dst_start) ||
        (addr_t)size - 1 > KERNEL_BASE_ADDR - 1 - src_start ||
        (addr_t)size - 1 > (addr_t)-1 - dst_start) {
        pthread_mutex_unlock(&kmm_lock);
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
#endif

#ifdef MM64
    uint64_t src_pa = 0;
    uint64_t dst_pa = 0;
    uint64_t request_id = 0;
    uint64_t src_fpn = 0;
    uint64_t dst_fpn = 0;

    /*
     * Initial implementation:
     * - 4-byte aligned transfers only
     * - one source page and one destination page
     * - both pages must already be resident in RAM
     */
    if (((uint64_t)src_start & 3U) != 0 ||
        ((uint64_t)dst_start & 3U) != 0 ||
        (size & 3U) != 0 ||
        size > PAGING64_PAGESZ - PAGING64_OFFST(src_start) ||
        size > PAGING64_PAGESZ - PAGING64_OFFST(dst_start)) {
pthread_mutex_unlock(&kmm_lock);
pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }

    /*
     * Keep mmvm_lock while translating, pinning and
     * submitting. This serializes with __read, __write
     * and __free in the current libmem implementation.
     */
    if (mm64_dma_pin_user_page(caller, src_start, &src_pa) != 0) {
pthread_mutex_unlock(&kmm_lock);
pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }

    src_fpn = src_pa / PAGING64_PAGESZ;

if (mm64_dma_pin_kernel_page(caller, dst_start, &dst_pa) != 0) {
    dma_unpin_frame(src_fpn);
    pthread_mutex_unlock(&kmm_lock);
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
}
    dst_fpn = dst_pa / PAGING64_PAGESZ;

    if (dma_submit_pinned_with_id(
            caller->pid,
            src_pa,
            dst_pa,
            (uint64_t)size,
            &request_id) != 0) {
        /* Queue did not take ownership of the pins. */
        dma_unpin_frame(dst_fpn);
        dma_unpin_frame(src_fpn);
pthread_mutex_unlock(&kmm_lock);
pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }

    /*
     * DMA worker now owns the pins.
     * CPU does not wait for RTL completion here.
     */
    printf("[DMA PROCESS] PID=%u request=%llu submitted\n",
           caller->pid,
           (unsigned long long)request_id);

pthread_mutex_unlock(&kmm_lock);
pthread_mutex_unlock(&mmvm_lock);
    return 0;
#else
pthread_mutex_unlock(&kmm_lock);
pthread_mutex_unlock(&mmvm_lock);
    return -1;
#endif
}

int libkmem_copy_from_user(struct pcb_t *caller,
                           uint32_t source,
                           uint32_t destination,
                           uint32_t offset,
                           uint32_t size)
{
    if (caller == NULL || caller->mm == NULL ||
        caller->krnl == NULL || caller->krnl->mm == NULL ||
        size == 0 || source >= PAGING_MAX_SYMTBL_SZ ||
        destination >= PAGING_MAX_SYMTBL_SZ)
        return -1;

    pthread_mutex_lock(&mmvm_lock);
    pthread_mutex_lock(&kmm_lock);

    struct vm_rg_struct *src_rg =
        get_symrg_byid(caller->mm, source);
    struct vm_rg_struct *dst_rg =
        get_symrg_byid(caller->krnl->mm, destination);

    if (src_rg == NULL || dst_rg == NULL ||
        src_rg->rg_start >= src_rg->rg_end ||
        dst_rg->rg_start >= dst_rg->rg_end ||
        offset > src_rg->rg_end - src_rg->rg_start ||
        offset > dst_rg->rg_end - dst_rg->rg_start ||
        size > src_rg->rg_end - src_rg->rg_start - offset ||
        size > dst_rg->rg_end - dst_rg->rg_start - offset) {
        pthread_mutex_unlock(&kmm_lock);
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }

    for (uint32_t i = 0; i < size; i++) {
        BYTE value;

        if (pg_getval(caller->mm,
                      src_rg->rg_start + offset + i,
                      &value, caller) != 0 ||
            __write_kernel_mem(caller, 0, destination,
                               offset + i, value) != 0) {
            pthread_mutex_unlock(&kmm_lock);
            pthread_mutex_unlock(&mmvm_lock);
            return -1;
        }
    }

    pthread_mutex_unlock(&kmm_lock);
    pthread_mutex_unlock(&mmvm_lock);
    return 0;
}


int libkmem_copy_to_user(struct pcb_t *caller,
                         uint32_t source,
                         uint32_t destination,
                         uint32_t offset,
                         uint32_t size)
{
    if (caller == NULL || caller->mm == NULL ||
        caller->krnl == NULL || caller->krnl->mm == NULL ||
        size == 0 || source >= PAGING_MAX_SYMTBL_SZ ||
        destination >= PAGING_MAX_SYMTBL_SZ)
        return -1;

    pthread_mutex_lock(&mmvm_lock);
    pthread_mutex_lock(&kmm_lock);

    struct vm_rg_struct *src_rg =
        get_symrg_byid(caller->krnl->mm, source);
    struct vm_rg_struct *dst_rg =
        get_symrg_byid(caller->mm, destination);

    if (src_rg == NULL || dst_rg == NULL ||
        src_rg->rg_start >= src_rg->rg_end ||
        dst_rg->rg_start >= dst_rg->rg_end ||
        offset > src_rg->rg_end - src_rg->rg_start ||
        offset > dst_rg->rg_end - dst_rg->rg_start ||
        size > src_rg->rg_end - src_rg->rg_start - offset ||
        size > dst_rg->rg_end - dst_rg->rg_start - offset) {
        pthread_mutex_unlock(&kmm_lock);
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }

    for (uint32_t i = 0; i < size; i++) {
        BYTE value;

        if (__read_kernel_mem(caller, 0, source,
                              offset + i, &value) != 0 ||
            pg_setval(caller->mm,
                      dst_rg->rg_start + offset + i,
                      value, caller) != 0) {
            pthread_mutex_unlock(&kmm_lock);
            pthread_mutex_unlock(&mmvm_lock);
            return -1;
        }
    }

    pthread_mutex_unlock(&kmm_lock);
    pthread_mutex_unlock(&mmvm_lock);
    return 0;
}

#ifdef MM64
static int kernel_va_to_pa(struct pcb_t *caller,
                           addr_t va,
                           addr_t *pa)
{
    if (caller == NULL || caller->krnl == NULL ||
        caller->krnl->mm == NULL ||
        caller->krnl->mram == NULL || pa == NULL ||
        !PAGING64_IS_KERNEL_ADDR(va))
        return -1;

    addr_t pte = pte_get_entry(caller->krnl->mm,
                               PAGING64_PGN(va), caller);

    if (!PAGING_PAGE_PRESENT(pte))
        return -1;

    addr_t physical =
        (addr_t)PAGING_FPN(pte) * PAGING64_PAGESZ +
        PAGING64_OFFST(va);

    if (physical >= (addr_t)caller->krnl->mram->maxsz)
        return -1;

    *pa = physical;
    return 0;
}
#endif

/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
#ifdef MM64
    if (caller == NULL || caller->krnl == NULL ||
        caller->krnl->mm == NULL || data == NULL ||
        rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return -1;

    struct vm_rg_struct *rg =
        get_symrg_byid(caller->krnl->mm, rgid);

    struct vm_area_struct *vma =
        get_vma_by_num(caller->krnl->mm, vmaid);

    if (rg == NULL || vma == NULL ||
        rg->rg_start >= rg->rg_end ||
        rg->vmaid != vmaid ||
        offset >= rg->rg_end - rg->rg_start)
        return -1;

    addr_t pa;
    if (kernel_va_to_pa(caller, rg->rg_start + offset, &pa) != 0)
        return -1;

    struct sc_regs regs = {0};
    regs.a1 = SYSMEM_IO_READ;
    regs.a2 = pa;

    if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
        return -1;

    *data = (BYTE)regs.a3;
    return 0;
#else
    return -1;
#endif
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
#ifdef MM64
    if (caller == NULL || caller->krnl == NULL ||
        caller->krnl->mm == NULL ||
        rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return -1;

    struct vm_rg_struct *rg =
        get_symrg_byid(caller->krnl->mm, rgid);

    struct vm_area_struct *vma =
        get_vma_by_num(caller->krnl->mm, vmaid);

    if (rg == NULL || vma == NULL ||
        rg->rg_start >= rg->rg_end ||
        rg->vmaid != vmaid ||
        offset >= rg->rg_end - rg->rg_start)
        return -1;

    addr_t pa;
    if (kernel_va_to_pa(caller, rg->rg_start + offset, &pa) != 0)
        return -1;

    struct sc_regs regs = {0};
    regs.a1 = SYSMEM_IO_WRITE;
    regs.a2 = pa;
    regs.a3 = value;

    return (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
           ? -1 : 0;
#else
    return -1;
#endif
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

   return 0;
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

  return 0;
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++)
  {
    pte = caller->mm->pgd[pagenum];

    if (PAGING_PAGE_PRESENT(pte))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*
 * Find the oldest resident page without removing it from FIFO.
 * Used to prepare a swap transaction before committing changes.
 */
static int peek_victim_page(struct mm_struct *mm,
                            addr_t *retpgn,
                            struct pcb_t *caller)
{
    struct pgn_t *pg;
    int found = 0;

    if (mm == NULL || retpgn == NULL ||
        caller == NULL || caller->mm != mm)
        return -1;

    for (pg = mm->fifo_pgn; pg != NULL; pg = pg->pg_next) {
        addr_t pte = pte_get_entry(mm, pg->pgn, caller);

        if (PAGING_PAGE_PRESENT(pte) &&
            !dma_frame_is_pinned(PAGING_FPN(pte))) {
            *retpgn = pg->pgn;
            found = 1;
        }
    }

    return found ? 0 : -1;
}


/*
 * Remove exactly one specified page from the FIFO list.
 * Unlike find_victim_page(), this function does not select
 * another victim.
 *
 * Caller must serialize modifications to mm->fifo_pgn.
 */
static int remove_fifo_page(struct mm_struct *mm,
                            addr_t pgn)
{
    struct pgn_t **link;

    if (mm == NULL)
        return -1;

    link = &mm->fifo_pgn;

    while (*link != NULL) {
        struct pgn_t *node = *link;

        if (node->pgn == pgn) {
            *link = node->pg_next;
            free(node);
            return 0;
        }

        link = &node->pg_next;
    }

    return -1;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */

int find_victim_page(struct mm_struct *mm,
                     addr_t *retpgn,
                     struct pcb_t *caller)
{
    struct pgn_t *pg;
    struct pgn_t *prev = NULL;
    struct pgn_t *victim = NULL;
    struct pgn_t *victim_prev = NULL;

    if (mm == NULL || retpgn == NULL ||
        caller == NULL || caller->mm != mm)
        return -1;

    pg = mm->fifo_pgn;

    /*
     * Preserve FIFO order: select the oldest
     * resident page whose frame is not DMA-pinned.
     */
    while (pg != NULL) {
        addr_t pte = pte_get_entry(mm, pg->pgn, caller);

        if (PAGING_PAGE_PRESENT(pte) &&
            !dma_frame_is_pinned(PAGING_FPN(pte))) {
            victim = pg;
            victim_prev = prev;
        }

        prev = pg;
        pg = pg->pg_next;
    }

    if (victim == NULL)
        return -1;

    *retpgn = victim->pgn;

    if (victim_prev == NULL)
        mm->fifo_pgn = victim->pg_next;
    else
        victim_prev->pg_next = victim->pg_next;

    free(victim);
    return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{

  if(caller == NULL || caller->mm == NULL || newrg == NULL || size <= 0)
    return -1;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if(cur_vma == NULL)
    return -1;

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
