/*
 * @Role B - standalone test / demo of the 64-bit 5-level page table (mm64.c)
 *           and of the macOS-style user layout (PAGEZERO + SMALL/MEDIUM/LARGE)
 *
 *   make mm64test && ./mm64test
 *
 * The test links only mm64.o. It brings its own tiny MEMPHY frame allocator
 * and DMA pin stubs, so it does not depend on the other memory modules.
 */
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#if !defined(MM64)
int main(void) { printf("mm64test needs #define MM64 in include/os-cfg.h\n"); return 1; }
#else

static int nr_pass = 0, nr_fail = 0;

#define CHECK(cond, ...)                                    \
  do {                                                      \
    if (cond) { nr_pass++; printf("  [PASS] "); }           \
    else      { nr_fail++; printf("  [FAIL] "); }           \
    printf(__VA_ARGS__);                                    \
    printf("\n");                                           \
  } while (0)

/* ------------------------------------------------------------------ */
/* Minimal MEMPHY used only by this test (frames 0..n-1 in a free list)  */
/* ------------------------------------------------------------------ */
int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
  struct framephy_struct *fp = mp->free_fp_list;
  if (fp == NULL)
    return -1;
  *retfpn = fp->fpn;
  mp->free_fp_list = fp->fp_next;
  free(fp);
  return 0;
}

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
  struct framephy_struct *fp = malloc(sizeof(struct framephy_struct));
  fp->fpn = fpn;
  fp->fp_next = mp->free_fp_list;
  mp->free_fp_list = fp;
  return 0;
}

int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
  if (mp == NULL || addr >= (addr_t)mp->maxsz) return -1;
  *value = mp->storage[addr];
  return 0;
}

int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
  if (mp == NULL || addr >= (addr_t)mp->maxsz) return -1;
  mp->storage[addr] = data;
  return 0;
}

/* DMA pin stubs (the real ones live in os.c) */
static int nr_pins = 0;
int dma_pin_frame(uint64_t fpn) { (void)fpn; nr_pins++; return 0; }
int dma_unpin_frame(uint64_t fpn) { (void)fpn; nr_pins--; return 0; }
int dma_frame_is_pinned(uint64_t fpn) { (void)fpn; return nr_pins > 0; }

static void test_memphy_init(struct memphy_struct *mp, int nframes)
{
  int i;
  memset(mp, 0, sizeof(*mp));
  mp->maxsz = nframes * PAGING64_PAGESZ;
  mp->storage = calloc(mp->maxsz, 1);
  mp->rdmflg = 1;
  for (i = nframes - 1; i >= 0; i--) /* head = frame 0 */
    MEMPHY_put_freefp(mp, i);
}

static int nr_free_frames(struct memphy_struct *mp)
{
  int n = 0;
  struct framephy_struct *fp;
  for (fp = mp->free_fp_list; fp != NULL; fp = fp->fp_next)
    n++;
  return n;
}

/* ------------------------------------------------------------------ */
/* Simulated kernel + processes                                         */
/* ------------------------------------------------------------------ */
#define RAM_FRAMES 16
#define SWP_FRAMES 32

/* first page of each user zone */
#define VA_S PAGING64_SMALL_START
#define VA_M PAGING64_MEDIUM_START
#define VA_L PAGING64_LARGE_START

static struct krnl_t krnl;
static struct memphy_struct mram, mswp[PAGING_MAX_MMSWP];
static struct memphy_struct *mswp_ptr[PAGING_MAX_MMSWP];

static struct pcb_t *new_proc(uint32_t pid)
{
  struct pcb_t *p = calloc(1, sizeof(struct pcb_t));
  p->pid = pid;
  p->krnl = &krnl;
  p->mm = malloc(sizeof(struct mm_struct));
  init_mm(p->mm, p);
  return p;
}

static uint64_t ntables(struct mm_struct *mm)
{
  return pgtbl_size_bytes(mm) / PAGING64_TBL_SZ;
}

/* MMU translation used by the demo: VA -> PA (returns -1 if not present) */
static int translate(struct pcb_t *p, addr_t va, addr_t *pa)
{
  addr_t pte = pte_get_entry(p->mm, PAGING64_PGN(va), p);
  if (!PAGING_PAGE_PRESENT(pte))
    return -1;
  *pa = PAGING64_PHYADDR(PAGING_FPN(pte), PAGING64_OFFST(va));
  return 0;
}

/* T15: several "CPUs" map pages into their own tree and into the shared
 * kernel half at the same time */
#define MT_THREADS 4
#define MT_PAGES   32

struct mt_arg {
  struct pcb_t *proc;
  int id;
  int err;
};

static void *mt_worker(void *arg)
{
  struct mt_arg *a = arg;
  struct vm_rg_struct rg;
  int j;

  for (j = 0; j < MT_PAGES; j++)
  {
    addr_t kva = KERNEL_BASE_ADDR + (addr_t)(1 + a->id * MT_PAGES + j) * PAGING64_PAGESZ;
    addr_t uva = VA_S + (addr_t)j * PAGING64_PAGESZ;
    if (vm_map_ram(a->proc, 0, 0, kva, 1, &rg) != 0)
      a->err++;
    if (vm_map_ram(a->proc, 0, 0, uva, 1, &rg) != 0)
      a->err++;
  }
  return NULL;
}

static void reset_stat(struct mm_struct *mm)
{
  mm->pgstat.nr_walk = mm->pgstat.nr_mem_access = 0;
  mm->pgstat.nr_pwc_hit = mm->pgstat.nr_fault = 0;
  mm->pgstat.pwc_valid = 0;
}

int main(void)
{
  struct pcb_t *p1, *p2;
  struct vm_rg_struct rg;
  addr_t pgd, p4d, pud, pmd, pt, pa, pte, va;
  int i, free_before;

  printf("==== Role B: 64-bit 5-level paging + macOS layout test ====\n");

  /* system: 1 RAM + 4 SWAP, kernel mm + kernel page table */
  test_memphy_init(&mram, RAM_FRAMES);
  for (i = 0; i < PAGING_MAX_MMSWP; i++)
  {
    test_memphy_init(&mswp[i], SWP_FRAMES);
    mswp_ptr[i] = &mswp[i];
  }
  memset(&krnl, 0, sizeof(krnl));
  krnl.mram = &mram;
  krnl.mswp = mswp_ptr;
  krnl.active_mswp = mswp_ptr[0];
  krnl.mm = malloc(sizeof(struct mm_struct));
  init_mm(krnl.mm, NULL);

  /* ---------------------------------------------------------------- */
  printf("\n[T1] Address split (example of the assignment, Table 2)\n");
  va = 0x39ffe1d9c9000ULL;
  get_pd_from_address(va, &pgd, &p4d, &pud, &pmd, &pt);
  printf("  VA 0x%" PRIx64 " -> PGD 0x%03" PRIx64 " P4D 0x%03" PRIx64 " PUD 0x%03" PRIx64
         " PMD 0x%03" PRIx64 " PT 0x%03" PRIx64 " OFF 0x%03" PRIx64 "\n",
         va, pgd, p4d, pud, pmd, pt, (uint64_t)PAGING64_OFFST(va));
  CHECK(pgd == 0x03 && p4d == 0x13f && pud == 0x1f8 && pmd == 0x0ec && pt == 0x1c9,
        "indices = 0x03 / 0x13f / 0x1f8 / 0x0ec / 0x1c9 as in the spec");
  {
    addr_t a[5];
    get_pd_from_pagenum(PAGING64_PGN(va), &a[0], &a[1], &a[2], &a[3], &a[4]);
    CHECK(a[0] == pgd && a[1] == p4d && a[2] == pud && a[3] == pmd && a[4] == pt,
          "get_pd_from_pagenum(pgn) gives the same indices as get_pd_from_address(va)");
  }
  get_pd_from_address(0xFFFFFFFFFFFFFFFFULL, &pgd, &p4d, &pud, &pmd, &pt);
  CHECK(pgd == 0x1ff && p4d == 0x1ff && pud == 0x1ff && pmd == 0x1ff && pt == 0x1ff,
        "each index is 9 bits (max 0x1ff = 511 -> 512 entries per table)");
  CHECK(PAGING64_TBL_SZ == 4096, "one table = 512 entries x 8 B = %d B = 1 page",
        (int)PAGING64_TBL_SZ);

  /* ---------------------------------------------------------------- */
  printf("\n[T2] init_mm: macOS-style zones, kernel keeps one VMA\n");
  p1 = new_proc(1);
  p2 = new_proc(2);
  CHECK(p1->mm->pgd != NULL && ntables(p1->mm) == 1 && p1->mm->pt == NULL,
        "a new process owns only its PGD (1 table = 4096 B)");
  {
    struct vm_area_struct *v0 = p1->mm->mmap;
    struct vm_area_struct *v1 = v0 ? v0->vm_next : NULL;
    struct vm_area_struct *v2 = v1 ? v1->vm_next : NULL;
    CHECK(v0 && v0->vm_id == 0 && v0->vm_start == 0x10000 && v0->vm_end == 0x100000 &&
          v0->sbrk == v0->vm_start, "vma0 SMALL  = [0x10000, 0x100000), sbrk at start");
    CHECK(v1 && v1->vm_id == 1 && v1->vm_start == 0x100000 && v1->vm_end == 0x500000,
          "vma1 MEDIUM = [0x100000, 0x500000)");
    CHECK(v2 && v2->vm_id == 2 && v2->vm_start == 0x500000 && v2->vm_end == 0x1000000 &&
          v2->vm_next == NULL, "vma2 LARGE  = [0x500000, 0x1000000), end of list");
    CHECK(v0 && v1 && v2 && v0->vm_mm == p1->mm && v2->vm_mm == p1->mm &&
          v0->vm_freerg_list && v0->vm_freerg_list->vmaid == 0 &&
          v2->vm_freerg_list && v2->vm_freerg_list->vmaid == 2,
          "each zone owns its free-region list tagged with its vmaid");
    CHECK(v0 && v0->vm_start >= PAGING64_PAGEZERO_END, "no VMA overlaps PAGEZERO [0x0, 0x1000)");
  }
  CHECK(krnl.mm->mmap->vm_start == KERNEL_BASE_ADDR && krnl.mm->mmap->vm_next == NULL,
        "kernel mm: one VMA at KERNEL_BASE_ADDR 0x%016" PRIx64, (uint64_t)KERNEL_BASE_ADDR);
  CHECK(init_krnl_pgtbl(&krnl) == 0 && krnl.krnl_pgd == krnl.mm->pgd && krnl.krnl_pt != NULL,
        "kernel tree built at boot: krnl_pgd -> p4d -> pud -> pmd -> pt");
  CHECK(PAGING64_ADDR_PGD(KERNEL_BASE_ADDR) == PAGING64_KERNEL_PGD_IDX &&
        krnl.krnl_pgd[PAGING64_KERNEL_PGD_IDX] != 0,
        "kernel half starts at PGD index %d", PAGING64_KERNEL_PGD_IDX);

  /* ---------------------------------------------------------------- */
  printf("\n[T3] Layout helpers: zone by size, quantum, PAGEZERO\n");
  CHECK(mm64_vmaid_for_size(1) == PAGING64_VMA_SMALL && mm64_vmaid_for_size(512) == PAGING64_VMA_SMALL,
        "1 B and 512 B -> SMALL");
  CHECK(mm64_vmaid_for_size(513) == PAGING64_VMA_MEDIUM && mm64_vmaid_for_size(4096) == PAGING64_VMA_MEDIUM,
        "513 B and 4096 B -> MEDIUM");
  CHECK(mm64_vmaid_for_size(4097) == PAGING64_VMA_LARGE && mm64_vmaid_for_size(0) == -1,
        "4097 B -> LARGE, 0 B -> -1");
  CHECK(mm64_align_size(PAGING64_VMA_SMALL, 100) == 112 &&
        mm64_align_size(PAGING64_VMA_MEDIUM, 300) == 512 &&
        mm64_align_size(PAGING64_VMA_LARGE, 5000) == 8192 &&
        mm64_align_size(PAGING64_VMA_SMALL, 16) == 16 && mm64_align_size(7, 10) == 0,
        "alignment: 100->112 (16 B), 300->512 (256 B), 5000->8192 (4 KB), bad id -> 0");
  CHECK(mm64_is_pagezero_addr(0) && mm64_is_pagezero_addr(0xfff) && !mm64_is_pagezero_addr(0x1000) &&
        PAGING64_IS_PAGEZERO_ADDR(0x10) && !PAGING64_IS_PAGEZERO_ADDR(KERNEL_BASE_ADDR),
        "PAGEZERO = [0x0, 0x1000): 0x0 and 0xfff yes, 0x1000 and kernel no");
  CHECK(mm64_range_in_pagezero(0xff0, 0x1010) && !mm64_range_in_pagezero(0x1000, 0x2000),
        "a range crossing 0x1000 touches PAGEZERO, [0x1000,0x2000) does not");

  /* ---------------------------------------------------------------- */
  printf("\n[T4] PAGEZERO is never mapped\n");
  free_before = nr_free_frames(&mram);
  {
    uint64_t tbl_before = ntables(p1->mm), faults = p1->mm->pgstat.nr_fault;
    CHECK(vm_map_ram(p1, 0, 0x1000, 0, 1, &rg) != 0, "vm_map_ram at VA 0 is refused");
    CHECK(nr_free_frames(&mram) == free_before && ntables(p1->mm) == tbl_before,
          "no frame taken and no table created");
    CHECK(pte_set_fpn(p1->mm, 0, 3, p1) == -1 && pte_get_entry(p1->mm, 0, p1) == 0,
          "pte_set_fpn / pte_get_entry on pgn 0 fail (NULL-pointer page)");
    CHECK(p1->mm->pgstat.nr_fault > faults, "PAGEZERO accesses counted as faults");
  }

  /* ---------------------------------------------------------------- */
  printf("\n[T5] vm_map_ram: P1 maps 3 pages at the start of SMALL\n");
  free_before = nr_free_frames(&mram);
  CHECK(vm_map_ram(p1, VA_S, VA_S + 0x3000, VA_S, 3, &rg) == 0, "vm_map_ram returns 0");
  CHECK(rg.rg_start == VA_S && rg.rg_end == VA_S + 0x3000 && rg.vmaid == PAGING64_VMA_SMALL,
        "ret_rg = [0x10000, 0x13000), vmaid 0 (SMALL)");
  CHECK(nr_free_frames(&mram) == free_before - 3, "3 RAM frames were taken");
  CHECK(ntables(p1->mm) == 5, "tables now: PGD+P4D+PUD+PMD+PT = 5 (20 KB)");
  for (i = 0; i < 3; i++)
  {
    pte = pte_get_entry(p1->mm, PAGING64_PGN(VA_S) + i, p1);
    CHECK(PAGING_PAGE_PRESENT(pte) && !(pte & PAGING_PTE_SWAPPED_MASK),
          "pgn 0x%" PRIx64 " present -> fpn %" PRIu64,
          (uint64_t)(PAGING64_PGN(VA_S) + i), (uint64_t)PAGING_FPN(pte));
  }
  CHECK(translate(p1, VA_S + 0x1234, &pa) == 0 &&
        pa == PAGING64_PHYADDR(PAGING_FPN(pte_get_entry(p1->mm, PAGING64_PGN(VA_S) + 1, p1)), 0x234),
        "MMU: VA 0x11234 -> PA 0x%" PRIx64 " (frame of page 0x11 + offset 0x234)", (uint64_t)pa);
  CHECK(p1->mm->fifo_pgn && p1->mm->fifo_pgn->pgn == PAGING64_PGN(VA_S) + 2,
        "user pages enlisted in fifo_pgn for page replacement");

  /* ---------------------------------------------------------------- */
  printf("\n[T6] Zone boundaries are half-open [start, end)\n");
  CHECK(vm_map_ram(p1, VA_M, VA_M + 0x1000, VA_M, 1, &rg) == 0 && rg.vmaid == PAGING64_VMA_MEDIUM,
        "first page of MEDIUM (0x100000) gets vmaid 1, not SMALL's 0");
  CHECK(ntables(p1->mm) == 5,
        "SMALL and MEDIUM pages share the same PT (both in the first 2 MB): still 5 tables");
  CHECK(vm_map_ram(p1, VA_L, VA_L + 0x1000, VA_L, 1, &rg) == 0 && rg.vmaid == PAGING64_VMA_LARGE,
        "first page of LARGE (0x500000) gets vmaid 2");
  CHECK(ntables(p1->mm) == 6, "LARGE starts in the 3rd 2 MB region: +1 PT -> 6 tables = 24 KB");

  /* ---------------------------------------------------------------- */
  printf("\n[T7] Isolation: P2 also maps the first SMALL page\n");
  CHECK(vm_map_ram(p2, VA_S, VA_S + 0x1000, VA_S, 1, &rg) == 0, "P2 vm_map_ram OK");
  {
    addr_t pa1 = 0, pa2 = 0;
    translate(p1, VA_S + 0x10, &pa1);
    translate(p2, VA_S + 0x10, &pa2);
    CHECK(pa1 != pa2, "same VA 0x10010 -> P1 PA 0x%" PRIx64 ", P2 PA 0x%" PRIx64
          " (separate trees)", (uint64_t)pa1, (uint64_t)pa2);
  }

  /* ---------------------------------------------------------------- */
  printf("\n[T8] Sparse address: P1 maps 1 page at VA 0x39ffe1d9c9000\n");
  {
    uint64_t before = ntables(p1->mm);
    CHECK(vm_map_ram(p1, 0, 0, 0x39ffe1d9c9000ULL, 1, &rg) == 0, "vm_map_ram OK");
    CHECK(ntables(p1->mm) == before + 4,
          "only 4 new tables (P4D,PUD,PMD,PT) for that far region");
  }
  CHECK(translate(p1, 0x39ffe1d9c9abcULL, &pa) == 0 && PAGING64_OFFST(pa) == 0xabc,
        "VA 0x39ffe1d9c9abc -> PA 0x%" PRIx64, (uint64_t)pa);
  printf("  note: a flat 1-level table for 2^45 pages would need 2^45 x 8 B = 256 TiB\n");

  /* ---------------------------------------------------------------- */
  printf("\n[T9] Look-up of an unmapped page never allocates tables\n");
  {
    uint64_t before = ntables(p1->mm), faults = p1->mm->pgstat.nr_fault;
    pte = pte_get_entry(p1->mm, PAGING64_PGN(0x00007fff00000000ULL), p1);
    CHECK(pte == 0 && ntables(p1->mm) == before, "PTE = 0, table count unchanged");
    CHECK(p1->mm->pgstat.nr_fault == faults + 1, "counted as a fault (page fault path)");
  }

  /* ---------------------------------------------------------------- */
  printf("\n[T10] Swap: pte_set_swap / pte_set_fpn / __swap_cp_page\n");
  {
    addr_t pgn = PAGING64_PGN(VA_S) + 2;
    addr_t fpn = PAGING_FPN(pte_get_entry(p1->mm, pgn, p1));
    addr_t swpfpn;
    BYTE b;
    MEMPHY_write(&mram, fpn * PAGING64_PAGESZ + 7, 'X');
    MEMPHY_get_freefp(&mswp[0], &swpfpn);
    CHECK(__swap_cp_page(&mram, fpn, &mswp[0], swpfpn) == 0 &&
          MEMPHY_read(&mswp[0], swpfpn * PAGING64_PAGESZ + 7, &b) == 0 && b == 'X',
          "page content copied RAM frame %" PRIu64 " -> SWAP frame %" PRIu64,
          (uint64_t)fpn, (uint64_t)swpfpn);
    CHECK(pte_set_swap(p1, pgn, 0, swpfpn) == 0, "pte_set_swap(pgn 0x12, type 0, off %" PRIu64 ")",
          (uint64_t)swpfpn);
    pte = pte_get_entry(p1->mm, pgn, p1);
    CHECK(!PAGING_PAGE_PRESENT(pte) && (pte & PAGING_PTE_SWAPPED_MASK) &&
          PAGING_PTE_SWPTYP(pte) == 0 && PAGING_SWP(pte) == swpfpn,
          "PTE: present=0 swapped=1 type=0 off=%" PRIu64, (uint64_t)PAGING_SWP(pte));
    CHECK(translate(p1, VA_S + 0x2000, &pa) == -1, "MMU refuses VA 0x12000 (page is not in RAM)");
    CHECK(pte_set_fpn(p1->mm, pgn, fpn, p1) == 0 && translate(p1, VA_S + 0x2000, &pa) == 0,
          "pte_set_fpn brings page 0x12 back to frame %" PRIu64, (uint64_t)fpn);
    MEMPHY_put_freefp(&mswp[0], swpfpn);
  }

  /* ---------------------------------------------------------------- */
  printf("\n[T11] Kernel space (VA >= 0x%016" PRIx64 ") is shared by every process\n",
         (uint64_t)KERNEL_BASE_ADDR);
  {
    uint64_t user_tbl = ntables(p1->mm);
    addr_t kpgn = PAGING64_PGN(KERNEL_BASE_ADDR);
    CHECK(vm_map_ram(p1, 0, 0, KERNEL_BASE_ADDR, 1, &rg) == 0, "P1 maps 1 kernel page");
    CHECK(ntables(p1->mm) == user_tbl, "no table added to P1's private tree");
    CHECK(pte_get_entry(p1->mm, kpgn, p1) == pte_get_entry(p2->mm, kpgn, p2) &&
          PAGING_PAGE_PRESENT(pte_get_entry(p2->mm, kpgn, p2)),
          "P2 sees the very same kernel PTE (fpn %" PRIu64 ")",
          (uint64_t)PAGING_FPN(pte_get_entry(p2->mm, kpgn, p2)));
    CHECK(p1->mm->pgd[PAGING64_KERNEL_PGD_IDX] == 0, "P1 PGD[256] stays empty (routed to krnl_pgd)");
    CHECK(p1->mm->fifo_pgn->pgn != kpgn, "kernel page is not a swap victim candidate");
  }

  /* ---------------------------------------------------------------- */
  printf("\n[T12] Canonical address check\n");
  CHECK(pte_set_fpn(p1->mm, PAGING64_PGN(0x0100000000000000ULL), 1, p1) == -1,
        "VA 0x0100000000000000 (bits 63..57 != bit 56) is rejected");
  CHECK(PAGING64_IS_CANONICAL(0x00FFFFFFFFFFF000ULL) && PAGING64_IS_CANONICAL(0xFF00000000000000ULL),
        "0x00FF..F000 (user top) and 0xFF00..0000 (kernel base) are canonical");

  /* ---------------------------------------------------------------- */
  printf("\n[T13] DMA pinning refuses PAGEZERO\n");
  {
    uint64_t dpa = 0;
    int pins = nr_pins;
    CHECK(mm64_dma_pin_user_page(p1, 0x10, &dpa) == -1 && nr_pins == pins,
          "mm64_dma_pin_user_page(VA 0x10) = -1, nothing pinned");
    CHECK(mm64_dma_pin_user_page(p1, VA_S + 0x20, &dpa) == 0 && nr_pins == pins + 1 &&
          PAGING64_OFFST(dpa) == 0x20,
          "a resident SMALL address is pinned (PA 0x%" PRIx64 ")", (uint64_t)dpa);
    dma_unpin_frame(dpa / PAGING64_PAGESZ);
  }

  /* ---------------------------------------------------------------- */
  printf("\n[T14] Memory accesses: page-walk cache vs full walk\n");
  reset_stat(p1->mm);
  for (i = 0; i < 100; i++)
    translate(p1, VA_S + (addr_t)(i % 3) * PAGING64_PAGESZ + i, &pa);
  printf("  100 translations inside one 2MB region (SMALL):\n");
  print_pgtbl_stat(p1->mm, p1->pid);
  CHECK(p1->mm->pgstat.nr_walk == 100 && p1->mm->pgstat.nr_pwc_hit == 99 &&
        p1->mm->pgstat.nr_mem_access == 5 + 99,
        "1 full walk (5 accesses) + 99 cache hits (1 access) = 104 vs 500 without cache");
  reset_stat(p1->mm);
  for (i = 0; i < 100; i++)
    translate(p1, (i % 2) ? VA_M + 0x40 : VA_S + 0x40, &pa);
  printf("  100 translations alternating SMALL / MEDIUM (same PT):\n");
  print_pgtbl_stat(p1->mm, p1->pid);
  CHECK(p1->mm->pgstat.nr_pwc_hit == 99, "SMALL and MEDIUM share a PT -> 99 hits");
  reset_stat(p1->mm);
  for (i = 0; i < 100; i++)
    translate(p1, (i % 2) ? 0x39ffe1d9c9000ULL : VA_S, &pa);
  printf("  100 translations alternating between two far regions:\n");
  print_pgtbl_stat(p1->mm, p1->pid);
  CHECK(p1->mm->pgstat.nr_pwc_hit == 0 && p1->mm->pgstat.nr_mem_access == 500,
        "worst case: every walk misses the cache -> 5 accesses each");

  /* ---------------------------------------------------------------- */
  printf("\n[T15] vmap_pgd_memset reserves PTEs without frames\n");
  CHECK(vmap_pgd_memset(p1, VA_L + 0x200000, 2) == 0, "reserve 2 pages at VA 0x700000 (LARGE)");
  pte = pte_get_entry(p1->mm, PAGING64_PGN(VA_L + 0x200000), p1);
  CHECK(pte == PAGING64_PTE_RESERVED_PATTERN && !PAGING_PAGE_PRESENT(pte),
        "PTE = 0x%08" PRIx64 " (reserved, not present)", (uint64_t)pte);
  CHECK(vmap_pgd_memset(p1, 0, 1) == -1, "reserving PAGEZERO is refused");

  /* ---------------------------------------------------------------- */
  printf("\n[T16] Out of memory is all-or-nothing\n");
  free_before = nr_free_frames(&mram);
  CHECK(vm_map_ram(p2, 0, 0, VA_L, free_before + 1, &rg) != 0,
        "asking %d frames with %d free fails", free_before + 1, free_before);
  CHECK(nr_free_frames(&mram) == free_before, "no frame leaked (%d still free)", free_before);

  /* ---------------------------------------------------------------- */
  printf("\n[T17] print_pgtbl(P1, 0, -1): pages, statistics and layout\n");
  /* in the OS, inc_vma_limit() raises sbrk after vm_map_ram(); do the same
   * here so the layout lines show how much of each zone is in use */
  p1->mm->mmap->sbrk = VA_S + 0x3000;
  p1->mm->mmap->vm_next->sbrk = VA_M + 0x1000;
  p1->mm->mmap->vm_next->vm_next->sbrk = VA_L + 0x1000;
  CHECK(print_pgtbl(p1, 0, (addr_t)-1) == 0, "dump printed");

  /* ---------------------------------------------------------------- */
  printf("\n[T18] free_pcb_pgtbl: process exit gives everything back\n");
  free_before = nr_free_frames(&mram);
  CHECK(free_pcb_pgtbl(p1) == 0 && p1->mm == NULL, "P1 address space released");
  CHECK(nr_free_frames(&mram) == free_before + 6, "6 RAM frames of P1 returned (%d free)",
        nr_free_frames(&mram));
  CHECK(PAGING_PAGE_PRESENT(pte_get_entry(p2->mm, PAGING64_PGN(KERNEL_BASE_ADDR), p2)),
        "kernel page still mapped for P2 (kernel tree is not freed by a process)");
  free_pcb_pgtbl(p2);

  /* ---------------------------------------------------------------- */
  printf("\n[T19] %d threads map %d kernel + %d user pages each, concurrently\n",
         MT_THREADS, MT_PAGES, MT_PAGES);
  {
    pthread_t th[MT_THREADS];
    struct mt_arg arg[MT_THREADS];
    int nframes = 2 * MT_THREADS * MT_PAGES + 16, err = 0, dup = 0, missing = 0, t, j;
    char *seen = calloc(nframes, 1);

    test_memphy_init(&mram, nframes);
    for (t = 0; t < MT_THREADS; t++)
    {
      arg[t].proc = new_proc(10 + t);
      arg[t].id = t;
      arg[t].err = 0;
      pthread_create(&th[t], NULL, mt_worker, &arg[t]);
    }
    for (t = 0; t < MT_THREADS; t++)
    {
      pthread_join(th[t], NULL);
      err += arg[t].err;
    }

    /* every page mapped once, every frame used by exactly one page */
    for (t = 0; t < MT_THREADS; t++)
      for (j = 0; j < MT_PAGES; j++)
      {
        addr_t kpgn = PAGING64_PGN(KERNEL_BASE_ADDR) + 1 + t * MT_PAGES + j;
        addr_t e[2];
        int k;
        e[0] = pte_get_entry(arg[0].proc->mm, kpgn, arg[0].proc);
        e[1] = pte_get_entry(arg[t].proc->mm, PAGING64_PGN(VA_S) + j, arg[t].proc);
        for (k = 0; k < 2; k++)
        {
          if (!PAGING_PAGE_PRESENT(e[k])) { missing++; continue; }
          if (seen[PAGING_FPN(e[k])]++) dup++;
        }
      }
    CHECK(err == 0 && missing == 0, "all %d mappings done (errors=%d, missing=%d)",
          2 * MT_THREADS * MT_PAGES, err, missing);
    CHECK(dup == 0, "no RAM frame handed out twice (duplicates=%d)", dup);
    CHECK(nr_free_frames(&mram) == 16, "free frames left = %d (expected 16)", nr_free_frames(&mram));
    printf("  kernel tree statistics (shared by the 4 processes):\n");
    print_pgtbl_stat(krnl.mm, 0);
    for (t = 0; t < MT_THREADS; t++)
      free_pcb_pgtbl(arg[t].proc);
    free(seen);
  }

  printf("\n==== RESULT: %d passed, %d failed ====\n", nr_pass, nr_fail);
  return nr_fail ? 1 : 0;
}
#endif
