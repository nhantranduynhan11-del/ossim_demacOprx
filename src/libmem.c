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

#include "libmem.h"
#include "dma.h"
#include "sysbus.h"
#include "mm64.h"
#include "string.h"
#include "syscall.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

#ifdef MM64
#define CUR_PAGESZ PAGING64_PAGESZ
#define CUR_PAGE_ALIGNSZ(sz) PAGING64_PAGE_ALIGNSZ(sz)
#else
#define CUR_PAGESZ PAGING_PAGESZ
#define CUR_PAGE_ALIGNSZ(sz) PAGING_PAGE_ALIGNSZ(sz)
#endif

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt) {
  struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;

  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  if (rg_node != NULL)
    rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid) {
  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
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
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size,
            addr_t *alloc_addr) {
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  int inc_sz = 0;

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0) {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;

    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/

  /*Attempt to increate limit to get space */
#ifdef MM64
  inc_sz = (uint32_t)(size / (int)PAGING64_PAGESZ);
  inc_sz = inc_sz + 1;
#else
  inc_sz = PAGING_PAGE_ALIGNSZ(size);
#endif
  int old_sbrk;
  inc_sz = inc_sz + 1;

  old_sbrk = cur_vma->sbrk;

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
#ifdef MM64
  regs.a3 = size;
#else
  regs.a3 = PAGING_PAGE_ALIGNSZ(size);
#endif
  _syscall(caller->krnl, caller->pid, 17, &regs); /* SYSCALL 17 sys_memmap */
  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;

  *alloc_addr = old_sbrk;

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
int __free(struct pcb_t *caller, int vmaid, int rgid) {
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode->rg_start == 0 && rgnode->rg_end == 0) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->rg_next = NULL;

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index) {
  addr_t addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1) {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index) {
  int val = __free(proc, 0, reg_index);
  if (val == -1) {
    return -1;
  }
  printf("%s:%d\n", __func__, __LINE__);
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0; // val;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller) {

  uint64_t pte = pte_get_entry(caller->mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(
          pte)) { /* Page is not online, make it actively living */
    addr_t vicpgn;
    addr_t vicpte;
    addr_t vicfpn, swpfpn;
    struct sc_regs regs;
    addr_t tgtfpn = PAGING_SWP(pte);
    /* TODO Initialize the target frame storing our variable */
    //  addr_t tgtfpn

    /* TODO: Play with your paging theory here */
    /* Find victim page */
    if (find_victim_page(caller->mm, &vicpgn) == -1) {
      return -1;
    }

    /* Get free frame in MEMSWP */
    if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1) {
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
int pg_getval(struct mm_struct *mm, int addr, BYTE *data,
              struct pcb_t *caller) {
  int pgn = PAGING_PGN(addr);
  // int off = ...
  int fpn;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  // int phyaddr = ...

  /* TODO
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */

  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, int addr, BYTE value,
              struct pcb_t *caller) {
  int pgn = PAGING_PGN(addr);
  // int off = ...
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  /* TODO
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE
   * SYSCALL 17 sys_memmap
   */

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
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset,
           BYTE *data) {
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

  // struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  /* TODO Invalid memory identify */

  pg_getval(caller->krnl->mm, currg->rg_start + offset, data, caller);

  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(struct pcb_t *proc, // Process executing the instruction
            uint32_t source,    // Index of source register
            addr_t offset,      // Source address = [source] + [offset]
            uint32_t *destination) {
  BYTE data;
  printf("%s:%d\n", __func__, __LINE__);
  int val = __read(proc, 0, source, offset, &data);

  *destination = data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset,
            BYTE value) {
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  if (currg == NULL || cur_vma == NULL) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pg_setval(caller->krnl->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(struct pcb_t *proc,   // Process executing the instruction
             BYTE data,            // Data to be wrttien into memory
             uint32_t destination, // Index of destination register
             addr_t offset) {
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1) {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t *caller, uint32_t size, uint32_t reg_index) {
  /*
   * TODO: provide OS level management
   *       and forward the request to helper
   */
  // addr_t  addr;
  // int val = ...

  /*
   * TODO: provide OS kmem allocation validation
   */
  if (caller == NULL || caller->krnl == NULL || size == 0 ||
      reg_index >= PAGING_MAX_SYMTBL_SZ) {
    return -1;
  }
  addr_t addr;
  int val = (int)__kmalloc(caller, 0, reg_index, (addr_t)size, &addr);
  if (val != 0) {
    return -1;
  }

  return 0;
}

/*validate_mem_rg - validate region and offset inside an mm_struct
 *@mm: target memory management struct (user or kernel)
 *@vmaid: VM area ID
 *@rgid: region ID in symrgtbl
 *@offset: byte offset within the region
 */
static struct vm_rg_struct *validate_mem_rg(struct mm_struct *mm, int vmaid,
                                            int rgid, addr_t offset) {
  if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ) {
    return NULL;
  }

  struct vm_rg_struct *currg = get_symrg_byid(mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(mm, vmaid);

  if (currg == NULL || cur_vma == NULL || currg->rg_start >= currg->rg_end) {
    return NULL;
  }

  if (offset >= currg->rg_end - currg->rg_start) {
    return NULL;
  }

  return currg;
}

/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size,
                 addr_t *alloc_addr) {
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      alloc_addr == NULL || size == 0 || rgid < 0 ||
      rgid >= PAGING_MAX_SYMTBL_SZ) {
    return -1;
  }

  pthread_mutex_lock(&kmm_lock);
  struct krnl_t *krnl = caller->krnl;
  struct vm_area_struct *cur_vma = get_vma_by_num(krnl->mm, vmaid);
  if (cur_vma == NULL) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  /* 1. Try reusing a free region from kernel vm_freerg_list */
  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;
  while (rgit != NULL) {
    if (rgit->rg_start + size <= rgit->rg_end) {
      addr_t start = rgit->rg_start;
      rgit->rg_start += size;

      krnl->mm->symrgtbl[rgid].rg_start = start;
      krnl->mm->symrgtbl[rgid].rg_end = start + size;
      *alloc_addr = start;

      pthread_mutex_unlock(&kmm_lock);
      return 0;
    }
    rgit = rgit->rg_next;
  }

  /* 2. Align size to page boundary and map new frames for kernel */
  addr_t align_sz = CUR_PAGE_ALIGNSZ(size);
  int incpgnum = (int)(align_sz / CUR_PAGESZ);

  addr_t old_sbrk = cur_vma->sbrk;
  struct vm_rg_struct rgnode;
  if ((int)vm_map_ram(caller, cur_vma->vm_start, cur_vma->vm_end, old_sbrk,
                      incpgnum, &rgnode) < 0) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  /* 3. Advance kernel sbrk and vm_end */
  cur_vma->sbrk = old_sbrk + align_sz;
  if (cur_vma->sbrk > cur_vma->vm_end) {
    cur_vma->vm_end = cur_vma->sbrk;
  }

  /* 4. Record allocated region in kernel symrgtbl */
  krnl->mm->symrgtbl[rgid].rg_start = old_sbrk;
  krnl->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&kmm_lock);
  return 0;
}

/*copy_mem_bytes - transfer bytes between user and kernel memory spaces*/
static int copy_mem_bytes(struct pcb_t *caller, uint32_t source,
                          uint32_t destination, uint32_t offset, uint32_t size,
                          int to_user) {
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mm == NULL || size == 0 ||
      source >= PAGING_MAX_SYMTBL_SZ || destination >= PAGING_MAX_SYMTBL_SZ) {
    return -1;
  }

  pthread_mutex_lock(&kmm_lock);
  for (uint32_t i = 0; i < size; i++) {
    BYTE data = 0;
    int rd_ret = to_user
                     ? __read_kernel_mem(caller, 0, (int)source, offset + i, &data)
                     : __read_user_mem(caller, 0, (int)source, offset + i, &data);
    if (rd_ret != 0) {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }

    int wr_ret =
        to_user ? __write_user_mem(caller, 0, (int)destination, offset + i, data)
                : __write_kernel_mem(caller, 0, (int)destination, offset + i, data);
    if (wr_ret != 0) {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  }
  pthread_mutex_unlock(&kmm_lock);
  return 0;
}

/*resolve_dma_rg - locate a region in user space first, then kernel space*/
static struct vm_rg_struct *resolve_dma_rg(struct pcb_t *caller, uint32_t rgid,
                                           uint32_t offset, uint32_t size,
                                           int *is_user) {
  struct vm_rg_struct *rg = validate_mem_rg(caller->mm, 0, (int)rgid, offset);
  if (rg != NULL && size <= (rg->rg_end - rg->rg_start - offset)) {
    *is_user = 1;
    return rg;
  }

  rg = validate_mem_rg(caller->krnl->mm, 0, (int)rgid, offset);
  if (rg != NULL && size <= (rg->rg_end - rg->rg_start - offset)) {
    *is_user = 0;
    return rg;
  }

  return NULL;
}

/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst,
                      uint32_t offset, uint32_t size) {
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mm == NULL || size == 0 || src >= PAGING_MAX_SYMTBL_SZ ||
      dst >= PAGING_MAX_SYMTBL_SZ) {
    return -1;
  }

  pthread_mutex_lock(&kmm_lock);
  int src_is_user = 0;
  int dst_is_user = 0;
  struct vm_rg_struct *src_rg =
      resolve_dma_rg(caller, src, offset, size, &src_is_user);
  struct vm_rg_struct *dst_rg =
      resolve_dma_rg(caller, dst, offset, size, &dst_is_user);

  if (src_rg == NULL || dst_rg == NULL) {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  if (memory_bank_is_available()) {
    if (do_dma(caller, (uint64_t)(src_rg->rg_start + offset),
               (uint64_t)(dst_rg->rg_start + offset), (uint64_t)size) != 0) {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  }

  for (uint32_t i = 0; i < size; i++) {
    BYTE data = 0;
    int rd_ret = src_is_user
                     ? __read_user_mem(caller, 0, (int)src, offset + i, &data)
                     : __read_kernel_mem(caller, 0, (int)src, offset + i, &data);
    if (rd_ret != 0) {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }

    int wr_ret = dst_is_user
                     ? __write_user_mem(caller, 0, (int)dst, offset + i, data)
                     : __write_kernel_mem(caller, 0, (int)dst, offset + i, data);
    if (wr_ret != 0) {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  }

  pthread_mutex_unlock(&kmm_lock);
  return 0;
}

int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source,
                           uint32_t destination, uint32_t offset,
                           uint32_t size) {
  return copy_mem_bytes(caller, source, destination, offset, size, 0);
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source,
                         uint32_t destination, uint32_t offset, uint32_t size) {
  return copy_mem_bytes(caller, source, destination, offset, size, 1);
}

/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@data: output data byte
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset,
                      BYTE *data) {
  if (caller == NULL || caller->krnl == NULL || data == NULL) {
    return -1;
  }

  struct vm_rg_struct *currg =
      validate_mem_rg(caller->krnl->mm, vmaid, rgid, offset);
  if (currg == NULL) {
    return -1;
  }

  return pg_getval(caller->krnl->mm, (int)(currg->rg_start + offset), data,
                   caller);
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset,
                       BYTE value) {
  if (caller == NULL || caller->krnl == NULL) {
    return -1;
  }

  struct vm_rg_struct *currg =
      validate_mem_rg(caller->krnl->mm, vmaid, rgid, offset);
  if (currg == NULL) {
    return -1;
  }

  return pg_setval(caller->krnl->mm, (int)(currg->rg_start + offset), value,
                   caller);
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@data: output data byte
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset,
                    BYTE *data) {
  if (caller == NULL || data == NULL) {
    return -1;
  }

  struct vm_rg_struct *currg = validate_mem_rg(caller->mm, vmaid, rgid, offset);
  if (currg == NULL) {
    return -1;
  }

  return pg_getval(caller->mm, (int)(currg->rg_start + offset), data, caller);
}

/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset,
                     BYTE value) {
  if (caller == NULL) {
    return -1;
  }

  struct vm_rg_struct *currg = validate_mem_rg(caller->mm, vmaid, rgid, offset);
  if (currg == NULL) {
    return -1;
  }

  return pg_setval(caller->mm, (int)(currg->rg_start + offset), value, caller);
}

/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller) {
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++) {
    pte = caller->mm->pgd[pagenum];

    if (PAGING_PAGE_PRESENT(pte)) {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    } else {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn) {
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg) { // if no page --> not found
    return -1;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next) {
    prev = pg;
    pg = pg->pg_next;
  }

  *retpgn = pg->pgn;
  if (prev == NULL)
    mm->fifo_pgn = NULL;
  else
    prev->pg_next = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size,
                       struct vm_rg_struct *newrg) {
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL) {
    if (rgit->rg_start + size <=
        rgit->rg_end) { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end) {
        rgit->rg_start = rgit->rg_start + size;
      } else { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL) {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        } else {                         /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    } else {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
