#ifndef MM64_H
#define MM64_H

#include "mm.h"
#define MM64_BITS_PER_LONG 64

#define PAGING64_CPU_BUS_WIDTH 57 /* 57 bit bus - 5-level paging, 128 PiB VA */
#define PAGING64_PAGESZ  4096      /* 4KB or 12-bits PAGE OFFSET */

#define GENMASK64(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (MM64_BITS_PER_LONG  - (h) - 1)))

#define PAGING64_MAX_PGN  (DIV_ROUND_UP(BIT_ULL(21),PAGING64_PAGESZ))
#define PAGING64_PAGE_ALIGNSZ(sz) (DIV_ROUND_UP(sz,PAGING64_PAGESZ)*PAGING64_PAGESZ)

/*
 * @Role B - 5-level page table geometry
 *   Each level is indexed by 9 bits -> every directory/table has 512 entries.
 *   One entry is 8 bytes (addr_t) -> one table = 512 * 8 = 4096 B = 1 page.
 */
#define PAGING64_LEVELS          5
#define PAGING64_LVL_PGD         0
#define PAGING64_LVL_P4D         1
#define PAGING64_LVL_PUD         2
#define PAGING64_LVL_PMD         3
#define PAGING64_LVL_PT          4
#define PAGING64_IDX_BITS        9
#define PAGING64_PTRS_PER_TBL    (1 << PAGING64_IDX_BITS)              /* 512 */
#define PAGING64_IDX_MASK        (PAGING64_PTRS_PER_TBL - 1)           /* 0x1ff */
#define PAGING64_TBL_SZ          (PAGING64_PTRS_PER_TBL * sizeof(addr_t)) /* 4KB */

/* OFFSET */
#define PAGING64_ADDR_OFFST_HIBIT 11
#define PAGING64_ADDR_OFFST_LOBIT 0

/* PT */
#define PAGING64_ADDR_PT_HIBIT 20
#define PAGING64_ADDR_PT_LOBIT 12
#define PAGING64_ADDR_PT_SHIFT 12

/* PMD */
#define PAGING64_ADDR_PMD_HIBIT 29
#define PAGING64_ADDR_PMD_LOBIT 21

/* PUD */
#define PAGING64_ADDR_PUD_HIBIT 38
#define PAGING64_ADDR_PUD_LOBIT 30

/* P4D */
#define PAGING64_ADDR_P4D_HIBIT 47
#define PAGING64_ADDR_P4D_LOBIT 39

/* PGD */
#define PAGING64_ADDR_PGD_HIBIT 56
#define PAGING64_ADDR_PGD_LOBIT 48

/* Masks */
#define PAGING64_ADDR_OFFST_MASK  GENMASK64(PAGING64_ADDR_OFFST_HIBIT,PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT_MASK  GENMASK64(PAGING64_ADDR_PT_HIBIT,PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD_MASK  GENMASK64(PAGING64_ADDR_PMD_HIBIT,PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD_MASK  GENMASK64(PAGING64_ADDR_PUD_HIBIT,PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D_MASK  GENMASK64(PAGING64_ADDR_P4D_HIBIT,PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD_MASK  GENMASK64(PAGING64_ADDR_PGD_HIBIT,PAGING64_ADDR_PGD_LOBIT)

/* Extract PGD Entry */
#define PAGING64_ADDR_OFFST(addr) ((((addr_t)(addr))&PAGING64_ADDR_OFFST_MASK)>>PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT(addr)    ((((addr_t)(addr))&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD(addr)   ((((addr_t)(addr))&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD(addr)   ((((addr_t)(addr))&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D(addr)   ((((addr_t)(addr))&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD(addr)   ((((addr_t)(addr))&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT)

/*
 * @Role B - helpers for the other modules (libmem, mm-vm, kernel memory)
 *   In MM64 mode use these instead of the 32-bit PAGING_PGN/PAGING_OFFST
 *   (those assume a 22-bit bus with 256B pages).
 */
#define PAGING64_PGN(addr)        (((addr_t)(addr)) >> PAGING64_ADDR_PT_SHIFT)
#define PAGING64_OFFST(addr)      PAGING64_ADDR_OFFST(addr)
#define PAGING64_PGN_TO_ADDR(pgn) (((addr_t)(pgn)) << PAGING64_ADDR_PT_SHIFT)
#define PAGING64_PHYADDR(fpn,off) ((((addr_t)(fpn)) << PAGING64_ADDR_PT_SHIFT) | (addr_t)(off))

/*
 * @Role B - canonical address & user/kernel split (57-bit, like Linux LA57)
 *   bits 63..57 must be copies of bit 56:
 *     user space   : 0x0000000000000000 - 0x00FFFFFFFFFFFFFF (PGD idx   0..255)
 *     kernel space : 0xFF00000000000000 - 0xFFFFFFFFFFFFFFFF (PGD idx 256..511)
 *   The kernel half is resolved through krnl->krnl_pgd, which is shared by
 *   every process -> kernel space is identical for all processes.
 */
#define KERNEL_BASE_ADDR          0xFF00000000000000ULL
#define PAGING64_KERNEL_PGD_IDX   (PAGING64_PTRS_PER_TBL / 2)            /* 256 */
#define PAGING64_IS_CANONICAL(addr) \
	((((addr_t)(addr)) >> 56) == 0x00ULL || (((addr_t)(addr)) >> 56) == 0xFFULL)
#define PAGING64_IS_KERNEL_ADDR(addr) (((addr_t)(addr)) >= KERNEL_BASE_ADDR)

/* Pattern used by vmap_pgd_memset() to mark reserved (not yet backed) PTEs.
 * It only lives in the user-defined bits 15..27, so PRESENT/SWAPPED stay 0. */
#define PAGING64_PTE_RESERVED_PATTERN (0xdeadbeefULL & PAGING_PTE_USRNUM_MASK)

/* Error code returned by alloc_pages_range() when RAM has not enough frames */
#define PAGING64_ERR_OOM  (-3000)

/*
 * @Role B - macOS-style user address space layout (HK261, spec page 31)
 *
 *   Every range is [start, end): addr >= start && addr < end.
 *
 *   0x0000000000000000  PAGEZERO  4 KB  no access (catches NULL pointers)
 *   0x0000000000001000  reserved for __TEXT/__LINKEDIT (code is not kept
 *                       in RAM by this simulator), never mapped
 *   0x0000000000010000  vma0 SMALL    960 KB  size <= 512 B,   align 16 B
 *   0x0000000000100000  vma1 MEDIUM     4 MB  size <= 4096 B,  align 256 B
 *   0x0000000000500000  vma2 LARGE     11 MB  size >  4096 B,  align 4096 B
 *   0x0000000001000000  end of the user heap zones (16 MB)
 *
 *   The kernel mm keeps one VMA that starts at KERNEL_BASE_ADDR.
 */
#define PAGING64_PAGEZERO_START   0x0000000000000000ULL
#define PAGING64_PAGEZERO_END     0x0000000000001000ULL

#define PAGING64_VMA_SMALL        0
#define PAGING64_VMA_MEDIUM       1
#define PAGING64_VMA_LARGE        2
#define PAGING64_USER_NR_VMA      3

#define PAGING64_SMALL_START      0x0000000000010000ULL
#define PAGING64_SMALL_END        0x0000000000100000ULL
#define PAGING64_MEDIUM_START     0x0000000000100000ULL
#define PAGING64_MEDIUM_END       0x0000000000500000ULL
#define PAGING64_LARGE_START      0x0000000000500000ULL
#define PAGING64_LARGE_END        0x0000000001000000ULL

/* Size thresholds: size <= SMALL_MAX -> Small, <= MEDIUM_MAX -> Medium,
 * larger -> Large */
#define PAGING64_SMALL_MAX_SZ     512
#define PAGING64_MEDIUM_MAX_SZ    4096

/* Allocation alignment (quantum) of each zone */
#define PAGING64_SMALL_ALIGN      16
#define PAGING64_MEDIUM_ALIGN     256
#define PAGING64_LARGE_ALIGN      PAGING64_PAGESZ

/* Round @sz up to @a (a power of two) */
#define PAGING64_ALIGN_UP(sz, a) \
	((((addr_t)(sz)) + ((addr_t)(a) - 1)) & ~((addr_t)(a) - 1))

/* PAGEZERO check usable in macros and #if-free code */
#define PAGING64_IS_PAGEZERO_ADDR(addr) \
	(((addr_t)(addr)) < (addr_t)PAGING64_PAGEZERO_END)

/* @Role B - extra 5-level paging services */
/* Take @req_pgnum physically consecutive, zero-filled RAM frames (ascending
 * in @frm_lst); vm_map_ram() uses it for every kernel address (KMALLOC).
 * Return 0 or PAGING64_ERR_OOM. */
addr_t alloc_contig_pages_range(struct pcb_t *caller, int req_pgnum,
                                struct framephy_struct **frm_lst);
int init_krnl_pgtbl(struct krnl_t *krnl);
int free_pcb_pgtbl(struct pcb_t *caller);
/*
 * Resolve and pin one resident user page.
 *
 * On success, caller must later unpin the frame using
 * dma_unpin_frame(physical_addr / PAGING64_PAGESZ).
 */
int mm64_dma_pin_user_page(struct pcb_t *caller,
                           addr_t user_va,
                           uint64_t *physical_addr);
int mm64_dma_pin_kernel_page(struct pcb_t *caller,
                             addr_t kernel_va,
                             uint64_t *physical_addr);
int print_pgtbl_stat(struct mm_struct *mm, uint32_t pid);
uint64_t pgtbl_size_bytes(struct mm_struct *mm);

/* @Role B - layout helpers (macOS-style zones) */
int mm64_is_pagezero_addr(addr_t addr);            /* 1 if addr in PAGEZERO  */
int mm64_range_in_pagezero(addr_t start, addr_t end); /* 1 if [start,end) touches it */
int mm64_vmaid_for_size(addr_t size);              /* SMALL/MEDIUM/LARGE, -1 if 0 */
addr_t mm64_vma_align(int vmaid);                  /* quantum of a zone, 0 if bad */
addr_t mm64_align_size(int vmaid, addr_t size);    /* size rounded to the quantum */
const char *mm64_vma_name(int vmaid);
int print_mm_layout(struct mm_struct *mm, uint32_t pid);

#endif
