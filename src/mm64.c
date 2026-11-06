/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm64.c - 64-bit address, 5-level paging
 *
 * @Role B design summary
 *
 *  Virtual address (57-bit canonical, 4KB page):
 *
 *   63..57 | 56..48 | 47..39 | 38..30 | 29..21 | 20..12 | 11..0
 *   sign   |  PGD   |  P4D   |  PUD   |  PMD   |   PT   | OFFSET
 *
 *  - Each level is indexed by 9 bits, so every table has 512 entries of
 *    8 bytes = 4KB (exactly one page).
 *  - Every process owns a tree rooted at mm->pgd (address-space isolation).
 *    The kernel half (VA >= KERNEL_BASE_ADDR, PGD index 256..511) is always
 *    translated through krnl->krnl_pgd, which is shared by every process, so
 *    the kernel space is identical for all processes.
 *  - Directory entries (PGD/P4D/PUD/PMD) hold the address of the next-level
 *    table, 0 means "not present". Leaf entries (PT) use the PTE format of
 *    the assignment (present/swapped/dirty bits, FPN or swap type/offset).
 *  - Demand allocation: a new mm only has its PGD. P4D/PUD/PMD/PT tables are
 *    created the first time a page below them is mapped. Look-ups never
 *    allocate, so an unmapped region costs no memory at all.
 *  - Page-walk cache: mm->pt remembers the PT table of the last walk (one
 *    2MB region). A walk inside the same region costs 1 table access
 *    instead of 5.
 *  - Statistics (mm->pgstat) count tables per level, walks, page-table
 *    memory accesses and cache hits for the report.
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>

#if defined(MM64)

/* Page tables of the kernel half and the frame lists are shared by all
 * CPUs, so every operation of this module runs under this lock.
 * Public functions take the lock, static helpers assume it is held. */
static pthread_mutex_t pgtbl_lock = PTHREAD_MUTEX_INITIALIZER;

static const int pgtbl_lvl_shift[PAGING64_LEVELS] = {
    PAGING64_ADDR_PGD_LOBIT, PAGING64_ADDR_P4D_LOBIT, PAGING64_ADDR_PUD_LOBIT,
    PAGING64_ADDR_PMD_LOBIT, PAGING64_ADDR_PT_LOBIT};

/* A directory entry stores the address of the next-level table */
#define PDE_TO_TBL(e) ((addr_t *)(uintptr_t)(e))
#define TBL_TO_PDE(t) ((addr_t)(uintptr_t)(t))

/*
 * init_pte - Initialize PTE entry
 *   pre=0, swp=0 : empty entry (page not mapped)
 *   swp=0        : page online in RAM frame @fpn
 *   swp=1        : page swapped out to device @swptyp at frame @swpoff
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
  if (pte == NULL)
    return -1;

  *pte = 0; /* always start from an empty entry */

  if (pre == 0 && swp == 0)
    return 0; /* not mapped */

  if (swp == 0)
  { /* Non swap ~ page online */
    if (fpn > PAGING_PTE_FPN_MASK)
      return -1; /* FPN does not fit bits 0-12 of the PTE */

    SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
    SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
  }
  else
  { /* page swapped: not present in RAM, location kept in swap fields */
    SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
    SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
    SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
  }

  if (drt)
    SETBIT(*pte, PAGING_PTE_DIRTY_MASK);

  return 0;
}


/*
 * get_pd_from_address - Parse address to 5 page directory level
 * @addr  : virtual address
 * @pgd   : page global directory index    (bits 56-48)
 * @p4d   : page level 4 directory index   (bits 47-39)
 * @pud   : page upper directory index     (bits 38-30)
 * @pmd   : page middle directory index    (bits 29-21)
 * @pt    : page table index               (bits 20-12)
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
  if (pgd == NULL || p4d == NULL || pud == NULL || pmd == NULL || pt == NULL)
    return -1;

  /* Extract page direactories */
  *pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
  *p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
  *pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
  *pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
  *pt = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

  /* The mapping through the directories is done by pgtbl_walk() */

  return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}

/*
 * pgtbl_alloc_table - allocate one empty table (512 entries = 4KB)
 * @owner : mm_struct charged for the table (statistics), may be NULL
 * @lvl   : level of the new table (PAGING64_LVL_PGD .. PAGING64_LVL_PT)
 */
static addr_t *pgtbl_alloc_table(struct mm_struct *owner, int lvl)
{
  addr_t *tbl = calloc(PAGING64_PTRS_PER_TBL, sizeof(addr_t));

  if (tbl != NULL && owner != NULL)
    owner->pgstat.nr_tbl[lvl]++;

  return tbl;
}

/*
 * pgtbl_root - choose the tree that translates @addr
 *   user half   -> the process tree (mm->pgd)
 *   kernel half -> the kernel tree (krnl->krnl_pgd) shared by all processes
 * @owner returns the mm_struct owning the chosen tree
 */
static addr_t *pgtbl_root(struct mm_struct *mm, struct pcb_t *caller,
                          addr_t addr, struct mm_struct **owner)
{
  if (PAGING64_IS_KERNEL_ADDR(addr) && caller != NULL && caller->krnl != NULL &&
      caller->krnl->krnl_pgd != NULL)
  {
    *owner = caller->krnl->mm;
    return caller->krnl->krnl_pgd;
  }

  if (mm == NULL && caller != NULL)
    mm = caller->mm;

  *owner = mm;
  return (mm != NULL) ? mm->pgd : NULL;
}

/*
 * pgtbl_walk - walk the 5-level tree down to the PTE of @addr
 * @root  : PGD table of the tree
 * @owner : mm_struct owning the tree (statistics + walk cache), NULL = quiet
 * @addr  : virtual address
 * @alloc : 1 = create the missing directory tables (demand allocation)
 *          0 = pure look-up
 * Return the address of the PTE slot in its PT table, or NULL when the
 * address is not canonical or a directory on the way does not exist.
 */
static addr_t *pgtbl_walk(addr_t *root, struct mm_struct *owner, addr_t addr, int alloc)
{
  addr_t idx[PAGING64_LEVELS];
  addr_t tag = PAGING64_PGN(addr) >> PAGING64_IDX_BITS; /* 2MB region id */
  addr_t *tbl = root;
  int lvl;

  if (owner != NULL)
    owner->pgstat.nr_walk++;

  if (root == NULL || !PAGING64_IS_CANONICAL(addr))
  {
    if (owner != NULL)
      owner->pgstat.nr_fault++;
    return NULL;
  }

  get_pd_from_address(addr, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);

  /* Page-walk cache: same 2MB region as the last walk, reuse its PT */
  if (owner != NULL && owner->pgstat.pwc_valid &&
      owner->pgstat.pwc_tag == tag && owner->pt != NULL)
  {
    owner->pgstat.nr_pwc_hit++;
    owner->pgstat.nr_mem_access++; /* only the PTE itself is read */
    return &owner->pt[idx[PAGING64_LVL_PT]];
  }

  /* Full walk: PGD -> P4D -> PUD -> PMD -> PT, one table access per level */
  for (lvl = PAGING64_LVL_PGD; lvl < PAGING64_LVL_PT; lvl++)
  {
    if (owner != NULL)
      owner->pgstat.nr_mem_access++;

    if (tbl[idx[lvl]] == 0)
    {
      addr_t *next;

      if (!alloc)
      { /* the region below is not mapped at all */
        if (owner != NULL)
          owner->pgstat.nr_fault++;
        return NULL;
      }

      next = pgtbl_alloc_table(owner, lvl + 1);
      if (next == NULL)
        return NULL;
      tbl[idx[lvl]] = TBL_TO_PDE(next);
    }

    tbl = PDE_TO_TBL(tbl[idx[lvl]]);

    if (owner != NULL)
    { /* remember the path of this walk */
      switch (lvl + 1)
      {
      case PAGING64_LVL_P4D: owner->p4d = tbl; break;
      case PAGING64_LVL_PUD: owner->pud = tbl; break;
      case PAGING64_LVL_PMD: owner->pmd = tbl; break;
      case PAGING64_LVL_PT:  owner->pt = tbl; break;
      }
    }
  }

  if (owner != NULL)
  {
    owner->pgstat.nr_mem_access++; /* the PTE */
    owner->pgstat.pwc_tag = tag;
    owner->pgstat.pwc_valid = 1;
  }

  return &tbl[idx[PAGING64_LVL_PT]];
}

/*
 * pgtbl_find_pte - locate the PTE slot of page @pgn (lock must be held)
 */
static addr_t *pgtbl_find_pte(struct mm_struct *mm, struct pcb_t *caller,
                              addr_t pgn, int alloc)
{
  struct mm_struct *owner = NULL;
  addr_t addr = PAGING64_PGN_TO_ADDR(pgn);
  addr_t *root = pgtbl_root(mm, caller, addr, &owner);

  return pgtbl_walk(root, owner, addr, alloc);
}


/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t *pte;

  if (caller == NULL)
    return -1;

  pthread_mutex_lock(&pgtbl_lock);
  pte = pgtbl_find_pte(caller->mm, caller, pgn, 1);
  if (pte == NULL)
  {
    pthread_mutex_unlock(&pgtbl_lock);
    return -1;
  }

  /* Swapped out: PRESENT=0, SWAPPED=1, keep where the content lives */
  *pte = 0;
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  pthread_mutex_unlock(&pgtbl_lock);
  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @pte   : target page table entry (PTE)
 * @fpn   : frame page number (FPN)
 * @caller: caller
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t *pte;

  if (fpn > PAGING_PTE_FPN_MASK)
  {
    printf("[mm64] %s: fpn %" PRIu64 " does not fit the PTE FPN field\n",
           __func__, (uint64_t)fpn);
    return -1;
  }

  pthread_mutex_lock(&pgtbl_lock);
  pte = pgtbl_find_pte(mm, caller, pgn, 1);
  if (pte == NULL)
  {
    pthread_mutex_unlock(&pgtbl_lock);
    return -1;
  }

  /* On-line: PRESENT=1, SWAPPED=0, FPN in bits 0-12 */
  *pte = 0;
  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  pthread_mutex_unlock(&pgtbl_lock);
  return 0;
}


/* Get PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @caller: caller
 * @ret    : page table entry (0 when the page is not mapped)
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  addr_t *pte;
  addr_t val = 0;

  pthread_mutex_lock(&pgtbl_lock);
  pte = pgtbl_find_pte(mm, caller, pgn, 0); /* look-up never allocates */
  if (pte != NULL)
    val = *pte;
  pthread_mutex_unlock(&pgtbl_lock);

  return val;
}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  addr_t *pte;

  pthread_mutex_lock(&pgtbl_lock);
  /* clearing an entry never needs to create tables */
  pte = pgtbl_find_pte(mm, caller, pgn, pte_val != 0);
  if (pte == NULL)
  {
    pthread_mutex_unlock(&pgtbl_lock);
    return (pte_val == 0) ? 0 : -1;
  }

  *pte = (addr_t)pte_val;

  pthread_mutex_unlock(&pgtbl_lock);
  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 *   Reserve the page-table entries of [addr, addr + pgnum*PAGESZ): the
 *   directories are created and every empty PTE is set to a reserved
 *   pattern (PRESENT=0, SWAPPED=0, pattern in the user-defined bits).
 *   Entries that already map a page are left unchanged.
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  int pgit = 0;
  addr_t pgn = PAGING64_PGN(addr);
  addr_t *pte;

  if (caller == NULL || pgnum <= 0)
    return -1;

  pthread_mutex_lock(&pgtbl_lock);
  for (pgit = 0; pgit < pgnum; pgit++)
  {
    pte = pgtbl_find_pte(caller->mm, caller, pgn + pgit, 1);
    if (pte == NULL)
      break;
    if (*pte == 0)
      *pte = PAGING64_PTE_RESERVED_PATTERN;
  }
  pthread_mutex_unlock(&pgtbl_lock);

  return (pgit == pgnum) ? 0 : -1;
}

/*
 * vma_id_of - id of the vm area containing @addr (0 if none)
 */
static int vma_id_of(struct mm_struct *mm, addr_t addr)
{
  struct vm_area_struct *vma = (mm != NULL) ? mm->mmap : NULL;

  while (vma != NULL)
  {
    if (addr >= vma->vm_start && addr <= vma->vm_end)
      return (int)vma->vm_id;
    vma = vma->vm_next;
  }
  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 *   Insert one PTE per frame of @frames for the pages starting at @addr.
 *   User pages are enlisted in mm->fifo_pgn for page replacement, kernel
 *   pages are not (kernel memory is not swappable).
 * Return 0 when all @pgnum pages are mapped, -1 otherwise; @ret_rg tells
 * the range that is really mapped.
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given pages are mapped
  struct framephy_struct *fpit = frames;
  int pgit = 0;
  addr_t pgn = PAGING64_PGN(addr);
  int is_krnl = PAGING64_IS_KERNEL_ADDR(addr);
  addr_t *pte;

  if (caller == NULL || ret_rg == NULL)
    return (addr_t)-1;

  /* update the rg_end and rg_start of ret_rg */
  ret_rg->rg_start = addr;
  ret_rg->rg_end = addr;
  ret_rg->vmaid = vma_id_of((is_krnl && caller->krnl) ? caller->krnl->mm : caller->mm, addr);

  /* map range of frame to address space [addr to addr + pgnum*PAGING64_PAGESZ) */
  pthread_mutex_lock(&pgtbl_lock);
  for (pgit = 0; pgit < pgnum && fpit != NULL; pgit++, fpit = fpit->fp_next)
  {
    pte = pgtbl_find_pte(caller->mm, caller, pgn + pgit, 1);
    if (pte == NULL || init_pte(pte, 1, fpit->fpn, 0, 0, 0, 0) != 0)
      break;

    fpit->owner = is_krnl ? (caller->krnl ? caller->krnl->mm : NULL) : caller->mm;

    /* Tracking for later page replacement activities
     * Enqueue new usage page */
    if (!is_krnl && caller->mm != NULL)
      enlist_pgn_node(&caller->mm->fifo_pgn, pgn + pgit);
  }
  pthread_mutex_unlock(&pgtbl_lock);

  ret_rg->rg_end = addr + (addr_t)pgit * PAGING64_PAGESZ;

  return (pgit == pgnum) ? 0 : (addr_t)-1;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 * Return 0, or PAGING64_ERR_OOM (-3000) when RAM has not enough free frames
 * (all-or-nothing: the frames already taken are given back).
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  addr_t fpn;
  int pgit;
  struct framephy_struct *newfp_str = NULL;
  struct framephy_struct *head = NULL, *tail = NULL;
  struct memphy_struct *mram;

  if (frm_lst == NULL)
    return (addr_t)-1;
  *frm_lst = NULL;

  if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL)
    return (addr_t)-1;
  if (req_pgnum <= 0)
    return 0;

  mram = caller->krnl->mram;

  pthread_mutex_lock(&pgtbl_lock);
  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    if (MEMPHY_get_freefp(mram, &fpn) != 0)
      break; /* RAM exhausted */

    newfp_str = malloc(sizeof(struct framephy_struct));
    newfp_str->fpn = fpn;
    newfp_str->owner = caller->mm;
    newfp_str->fp_next = NULL;

    /* keep the frames in allocation order */
    if (tail == NULL)
      head = newfp_str;
    else
      tail->fp_next = newfp_str;
    tail = newfp_str;
  }

  if (pgit < req_pgnum)
  { /* ERROR CODE of obtaining somes but not enough frames: roll back */
    while (head != NULL)
    {
      newfp_str = head->fp_next;
      MEMPHY_put_freefp(mram, head->fpn);
      free(head);
      head = newfp_str;
    }
    pthread_mutex_unlock(&pgtbl_lock);
    return (addr_t)PAGING64_ERR_OOM;
  }
  pthread_mutex_unlock(&pgtbl_lock);

  *frm_lst = head;
  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point (page aligned)
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 * Return 0 on success, (addr_t)-1 on failure (check with != 0, addr_t is unsigned)
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL, *fp, *next;
  int64_t ret_alloc = 0;
  addr_t ret_map;
  int mapped, i;

  if (caller == NULL || ret_rg == NULL)
    return (addr_t)-1;

  if (PAGING64_OFFST(mapstart) != 0)
  {
    printf("[mm64] %s: mapstart 0x%016" PRIx64 " is not page aligned\n",
           __func__, (uint64_t)mapstart);
    mapstart -= PAGING64_OFFST(mapstart);
  }

  if (incpgnum <= 0)
  {
    ret_rg->rg_start = ret_rg->rg_end = mapstart;
    return 0;
  }

  /*@bksysnet: author provides a feasible solution of getting frames
   *FATAL logic in here, wrong behaviour if we have not enough page
   *i.e. we request 1000 frames meanwhile our RAM has size of 3 frames
   *Don't try to perform that case in this simple work, it will result
   *in endless procedure of swap-off to get frame and we have not provide
   *duplicate control mechanism, keep it simple
   */
  ret_alloc = (int64_t)alloc_pages_range(caller, incpgnum, &frm_lst);

  /* Out of memory */
  if (ret_alloc == PAGING64_ERR_OOM)
  {
    printf("[mm64] %s: PID %u out of RAM frames (need %d)\n",
           __func__, caller->pid, incpgnum);
    return (addr_t)-1;
  }

  if (ret_alloc < 0)
    return (addr_t)-1;

  /* it leaves the case of memory is enough but half in ram, half in swap
   * do the swaping all to swapper to get the all in ram */
  ret_map = vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

  /* The frames are now referenced by the PTEs; release the list nodes and
   * give back the frames that could not be mapped (if any) */
  mapped = (int)((ret_rg->rg_end - ret_rg->rg_start) / PAGING64_PAGESZ);
  for (fp = frm_lst, i = 0; fp != NULL; fp = next, i++)
  {
    next = fp->fp_next;
    if (i >= mapped)
    {
      pthread_mutex_lock(&pgtbl_lock);
      MEMPHY_put_freefp(caller->krnl->mram, fp->fpn);
      pthread_mutex_unlock(&pgtbl_lock);
    }
    free(fp);
  }

  return (ret_map == 0) ? 0 : (addr_t)-1;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  int cellidx;
  addr_t addrsrc, addrdst;

  if (mpsrc == NULL || mpdst == NULL)
    return -1;

  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;

    BYTE data;
    MEMPHY_read(mpsrc, addrsrc, &data);
    MEMPHY_write(mpdst, addrdst, data);
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner (NULL = the kernel mm, its area starts at KERNEL_BASE_ADDR)
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct *vma0;
  struct vm_rg_struct *first_rg;
  addr_t base;

  if (mm == NULL)
    return -1;

  /* clean every field: symrgtbl, fifo_pgn, kcpooltbl, statistics ... */
  memset(mm, 0, sizeof(struct mm_struct));

  /* init page table directory: only the root (PGD) exists, the lower
   * levels are allocated on demand by pgtbl_walk() */
  mm->pgd = pgtbl_alloc_table(mm, PAGING64_LVL_PGD);
  if (mm->pgd == NULL)
    return -1;
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt = NULL;

  vma0 = malloc(sizeof(struct vm_area_struct));
  if (vma0 == NULL)
    return -1;

  /* user processes start at VA 0, the kernel mm at the kernel base */
  base = (caller == NULL) ? (addr_t)KERNEL_BASE_ADDR : 0;

  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = base;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  vma0->vm_freerg_list = NULL;
  first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  /* update VMA0 next */
  vma0->vm_next = NULL;

  /* Point vma owner backward */
  vma0->vm_mm = mm;

  /* update mmap (symrgtbl, fifo_pgn, kcpooltbl were cleared above) */
  mm->mmap = vma0;

  return 0;
}

/*
 * init_krnl_pgtbl - build the kernel page table (shared kernel half)
 *   krnl->krnl_pgd is the root of the kernel tree. The directory path of
 *   the first kernel page is created at boot so the kernel half exists
 *   before any process runs (krnl_p4d .. krnl_pt point to it).
 */
int init_krnl_pgtbl(struct krnl_t *krnl)
{
  addr_t *pte;
  addr_t idx[PAGING64_LEVELS];

  if (krnl == NULL)
    return -1;

  pthread_mutex_lock(&pgtbl_lock);

  if (krnl->mm != NULL && krnl->mm->pgd != NULL)
    krnl->krnl_pgd = krnl->mm->pgd; /* the kernel mm owns the kernel tree */
  else
    krnl->krnl_pgd = pgtbl_alloc_table(NULL, PAGING64_LVL_PGD);

  if (krnl->krnl_pgd == NULL)
  {
    pthread_mutex_unlock(&pgtbl_lock);
    return -1;
  }

  pte = pgtbl_walk(krnl->krnl_pgd, krnl->mm, (addr_t)KERNEL_BASE_ADDR, 1);

  get_pd_from_address((addr_t)KERNEL_BASE_ADDR, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);
  krnl->krnl_p4d = PDE_TO_TBL(krnl->krnl_pgd[idx[0]]);
  krnl->krnl_pud = krnl->krnl_p4d ? PDE_TO_TBL(krnl->krnl_p4d[idx[1]]) : NULL;
  krnl->krnl_pmd = krnl->krnl_pud ? PDE_TO_TBL(krnl->krnl_pud[idx[2]]) : NULL;
  krnl->krnl_pt  = krnl->krnl_pmd ? PDE_TO_TBL(krnl->krnl_pmd[idx[3]]) : NULL;

  pthread_mutex_unlock(&pgtbl_lock);

  return (pte != NULL) ? 0 : -1;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->vmaid = 0;
  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[%" PRIu64 "]\n", (uint64_t)fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    printf("rg[%" PRIu64 "->%" PRIu64 "]\n", (uint64_t)rg->rg_start, (uint64_t)rg->rg_end);
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    printf("va[%" PRIu64 "->%" PRIu64 "]\n", (uint64_t)vma->vm_start, (uint64_t)vma->vm_end);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[%" PRIu64 "]-\n", (uint64_t)ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

/*
 * pgtbl_size_bytes - storage used by the page tables of @mm
 */
uint64_t pgtbl_size_bytes(struct mm_struct *mm)
{
  uint64_t ntbl = 0;
  int lvl;

  if (mm == NULL)
    return 0;
  for (lvl = 0; lvl < PAGING64_LEVELS; lvl++)
    ntbl += mm->pgstat.nr_tbl[lvl];
  return ntbl * (uint64_t)PAGING64_TBL_SZ;
}

/*
 * print_pgtbl_stat - one line of statistics for the report
 */
int print_pgtbl_stat(struct mm_struct *mm, uint32_t pid)
{
  struct pgtbl_stat_struct *st;
  double avg;

  if (mm == NULL)
    return -1;

  st = &mm->pgstat;
  avg = st->nr_walk ? (double)st->nr_mem_access / (double)st->nr_walk : 0.0;

  printf(" [PGTBL-STAT] PID %u: tables PGD=%" PRIu64 " P4D=%" PRIu64 " PUD=%" PRIu64
         " PMD=%" PRIu64 " PT=%" PRIu64 " size=%" PRIu64 "B | walks=%" PRIu64
         " pt-mem-access=%" PRIu64 " (%.2f/walk) pwc-hit=%" PRIu64 " faults=%" PRIu64 "\n",
         pid, st->nr_tbl[0], st->nr_tbl[1], st->nr_tbl[2], st->nr_tbl[3], st->nr_tbl[4],
         pgtbl_size_bytes(mm), st->nr_walk, st->nr_mem_access, avg,
         st->nr_pwc_hit, st->nr_fault);
  return 0;
}

/*
 * pgtbl_dump - print every leaf entry of a (sub)tree inside [start, end]
 * @tbl    : table at level @lvl
 * @prefix : virtual address covered by entry 0 of @tbl
 * @from/@to: entry range of @tbl to visit (PGD: user half or kernel half)
 */
static int pgtbl_dump(addr_t *tbl, int lvl, addr_t prefix, int from, int to,
                      addr_t start, addr_t end)
{
  int i, nr = 0;

  for (i = from; i < to; i++)
  {
    addr_t ent = tbl[i];
    addr_t va = prefix | ((addr_t)i << pgtbl_lvl_shift[lvl]);
    addr_t va_last;

    if (ent == 0)
      continue;

    if (lvl == PAGING64_LVL_PGD && i >= PAGING64_KERNEL_PGD_IDX)
      va |= 0xFE00000000000000ULL; /* canonical: copy bit 56 into 63..57 */

    va_last = va + ((((addr_t)1) << pgtbl_lvl_shift[lvl]) - 1);
    if (va_last < start || va > end)
      continue; /* the whole entry is outside the requested range */

    if (lvl < PAGING64_LVL_PT)
    {
      nr += pgtbl_dump(PDE_TO_TBL(ent), lvl + 1, va, 0, PAGING64_PTRS_PER_TBL, start, end);
      continue;
    }

    /* leaf PTE */
    {
      addr_t ix[PAGING64_LEVELS];
      get_pd_from_address(va, &ix[0], &ix[1], &ix[2], &ix[3], &ix[4]);
      printf("  va=0x%016" PRIx64 " pgn=0x%" PRIx64 " [PGD %03" PRIx64 "|P4D %03" PRIx64
             "|PUD %03" PRIx64 "|PMD %03" PRIx64 "|PT %03" PRIx64 "] ",
             (uint64_t)va, (uint64_t)PAGING64_PGN(va), ix[0], ix[1], ix[2], ix[3], ix[4]);
      if (PAGING_PAGE_PRESENT(ent))
        printf("-> fpn %" PRIu64 " (pa=0x%" PRIx64 ")\n", (uint64_t)PAGING_FPN(ent),
               (uint64_t)PAGING64_PHYADDR(PAGING_FPN(ent), 0));
      else if (ent & PAGING_PTE_SWAPPED_MASK)
        printf("-> swapped (type %" PRIu64 ", off %" PRIu64 ")\n",
               (uint64_t)PAGING_PTE_SWPTYP(ent), (uint64_t)PAGING_SWP(ent));
      else
        printf("-> reserved (pte=0x%08" PRIx64 ")\n", (uint64_t)ent);
      nr++;
    }
  }
  return nr;
}

/*
 * print_pgtbl - dump the 5-level page table of @caller inside [start, end]
 *   line 1: the directory tables on the walk of @start (0 = not created)
 *   then  : every mapped page with its 5 indices and its frame
 *   last  : statistics of the tree
 */
int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
  struct mm_struct *mm;
  addr_t idx[PAGING64_LEVELS];
  addr_t *path[PAGING64_LEVELS] = {NULL, NULL, NULL, NULL, NULL};
  struct krnl_t *krnl;
  int lvl, nr;

  if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL)
    return -1;

  mm = caller->mm;
  krnl = caller->krnl;

  pthread_mutex_lock(&pgtbl_lock);

  /* directory tables on the walk of @start (quiet walk, no statistics) */
  get_pd_from_address(start, &idx[0], &idx[1], &idx[2], &idx[3], &idx[4]);
  path[0] = (PAGING64_IS_KERNEL_ADDR(start) && krnl && krnl->krnl_pgd) ? krnl->krnl_pgd : mm->pgd;
  for (lvl = 1; lvl < PAGING64_LEVELS; lvl++)
    if (path[lvl - 1] != NULL && path[lvl - 1][idx[lvl - 1]] != 0)
      path[lvl] = PDE_TO_TBL(path[lvl - 1][idx[lvl - 1]]);

  printf("print_pgtbl: PID %u VA[0x%016" PRIx64 " - 0x%016" PRIx64 "]\n",
         caller->pid, (uint64_t)start, (uint64_t)end);
  printf(" PDG=%016" PRIxPTR " P4D=%016" PRIxPTR " PUD=%016" PRIxPTR
         " PMD=%016" PRIxPTR " PT=%016" PRIxPTR "\n",
         (uintptr_t)path[0], (uintptr_t)path[1], (uintptr_t)path[2],
         (uintptr_t)path[3], (uintptr_t)path[4]);

  /* user half: private to this process */
  nr = pgtbl_dump(mm->pgd, PAGING64_LVL_PGD, 0, 0, PAGING64_KERNEL_PGD_IDX, start, end);

  /* kernel half: shared by every process */
  if (krnl != NULL && krnl->krnl_pgd != NULL && end >= (addr_t)KERNEL_BASE_ADDR)
    nr += pgtbl_dump(krnl->krnl_pgd, PAGING64_LVL_PGD, 0, PAGING64_KERNEL_PGD_IDX,
                     PAGING64_PTRS_PER_TBL, start, end);

  if (nr == 0)
    printf("  (no page mapped)\n");

  print_pgtbl_stat(mm, caller->pid);

  pthread_mutex_unlock(&pgtbl_lock);
  return 0;
}

/*
 * pgtbl_free_tree - release a (sub)tree and the frames it maps
 */
static void pgtbl_free_tree(addr_t *tbl, int lvl, struct krnl_t *krnl,
                            uint64_t *nr_ram, uint64_t *nr_swp)
{
  int i;

  for (i = 0; i < PAGING64_PTRS_PER_TBL; i++)
  {
    addr_t ent = tbl[i];

    if (ent == 0)
      continue;

    if (lvl < PAGING64_LVL_PT)
    {
      pgtbl_free_tree(PDE_TO_TBL(ent), lvl + 1, krnl, nr_ram, nr_swp);
      continue;
    }

    if (krnl == NULL)
      continue;

    if (PAGING_PAGE_PRESENT(ent))
    { /* frame in RAM goes back to the free list */
      MEMPHY_put_freefp(krnl->mram, PAGING_FPN(ent));
      (*nr_ram)++;
    }
    else if (ent & PAGING_PTE_SWAPPED_MASK)
    { /* frame in the swap device given by the swap type */
      addr_t typ = PAGING_PTE_SWPTYP(ent);
      struct memphy_struct *swp = krnl->active_mswp;

      if (krnl->mswp != NULL && typ < PAGING_MAX_MMSWP && krnl->mswp[typ] != NULL)
        swp = krnl->mswp[typ];
      if (swp != NULL)
        MEMPHY_put_freefp(swp, PAGING_SWP(ent));
      (*nr_swp)++;
    }
  }

  free(tbl);
}

/*
 * free_pcb_pgtbl - tear down the address space of a finished process:
 *   print the final page-table statistics, give every frame back to its
 *   device, free the page tables and the mm structures.
 */
int free_pcb_pgtbl(struct pcb_t *caller)
{
  struct mm_struct *mm;
  struct vm_area_struct *vma, *vnext;
  struct vm_rg_struct *rg, *rnext;
  struct pgn_t *pg, *pnext;
  uint64_t nr_ram = 0, nr_swp = 0;

  if (caller == NULL || caller->mm == NULL)
    return -1;

  mm = caller->mm;

  pthread_mutex_lock(&pgtbl_lock);

  print_pgtbl_stat(mm, caller->pid);

  /* never free the shared kernel tree from a process */
  if (mm->pgd != NULL && (caller->krnl == NULL || mm->pgd != caller->krnl->krnl_pgd))
    pgtbl_free_tree(mm->pgd, PAGING64_LVL_PGD, caller->krnl, &nr_ram, &nr_swp);

  printf(" [PGTBL-FREE] PID %u: released %" PRIu64 " RAM frame(s), %" PRIu64
         " swap frame(s), %" PRIu64 "B of page tables\n",
         caller->pid, nr_ram, nr_swp, pgtbl_size_bytes(mm));

  mm->pgd = mm->p4d = mm->pud = mm->pmd = mm->pt = NULL;
  mm->pgstat.pwc_valid = 0;

  for (pg = mm->fifo_pgn; pg != NULL; pg = pnext)
  {
    pnext = pg->pg_next;
    free(pg);
  }
  mm->fifo_pgn = NULL;

  for (vma = mm->mmap; vma != NULL; vma = vnext)
  {
    vnext = vma->vm_next;
    for (rg = vma->vm_freerg_list; rg != NULL; rg = rnext)
    {
      rnext = rg->rg_next;
      free(rg);
    }
    free(vma);
  }
  mm->mmap = NULL;

  pthread_mutex_unlock(&pgtbl_lock);

  free(mm);
  caller->mm = NULL;

  return 0;
}

#endif  //def MM64
