/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

//#ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *pvma;

  if (mm->mmap == NULL)
    return NULL;

  pvma = mm->mmap;

  int vmait = pvma->vm_id;

  while (vmait < vmaid)
  {
    if (pvma == NULL)
      return NULL;

    pvma = pvma->vm_next;
    vmait = pvma->vm_id;
  }

  return pvma;
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
    return 0;
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct * newrg;
  /* TODO retrive current vma to obtain newrg, current comment out due to compiler redundant warning*/
  struct vm_area_struct *cur_vma;

  if(caller == NULL || caller->mm == NULL || alignedsz == 0)
    return NULL;

  cur_vma = get_vma_by_num(caller->mm, vmaid);
  if(cur_vma->sbrk > cur_vma->vm_end || alignedsz > (cur_vma->vm_end - cur_vma->sbrk))
    return NULL;

  newrg = malloc(sizeof(struct vm_rg_struct));

  if(newrg == NULL)
    return NULL;

  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = cur_vma->sbrk + alignedsz;
  newrg->vmaid = vmaid;
  newrg->rg_next = NULL;

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct vm_area_struct *vma;

  /* TODO validate the planned memory area is not overlapped */
  if(caller == NULL || caller->mm == NULL)
  {
    return -1;
  }

  if (vmastart >= vmaend)
  {
    return -1;
  }

  vma = caller->mm->mmap;
  
  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);

  if(cur_area == NULL)
  {
    return -1;
  }

  while(vma != NULL)
  {
    if (vma != cur_area && OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }
  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  /* TOTO with new address scheme, the size need tobe aligned 
   *      the raw inc_sz maybe not fit pagesize
   */ 

  struct vm_area_struct *cur_area;
  struct vm_rg_struct *newrg;
  struct vm_rg_struct *rg;

  addr_t inc_atm;
  addr_t inc_pg;
  
  if(caller == NULL || caller->mm == NULL || inc_sz == 0)
  {
    return -1;
  }

  cur_area = get_vma_by_num(caller->mm, vmaid);

  if(cur_area == NULL)
  {
    return -1;
  }

#ifdef MM64
  inc_atm = ((inc_sz + PAGING64_PAGESZ - 1) / PAGING64_PAGESZ) * PAGING64_PAGESZ;
  inc_pg = inc_atm / PAGING64_PAGESZ;
#else
  inc_atm = PAGING_PAGE_ALIGNSZ(inc_sz);
  inc_pg = inc_atm / PAGING_PAGESZ;
#endif

  if(inc_atm < inc_sz)
  {
    return -1;
  }

  newrg = get_vm_area_node_at_brk(caller, vmaid, inc_atm);
  if(newrg == NULL)
  {
    return -1;
  }

  if(validate_overlap_vm_area(caller, vmaid, newrg->rg_start, newrg->rg_end) < 0)
  {
    free(newrg);
    return -1;
  }

  memset(&rg, 0, sizeof(rg));
  if(vm_map_ram(caller, newrg->rg_start, newrg->rg_end, newrg->rg_start, inc_pg, &rg) < 0)
  {
    free(newrg);
    return -1;
  }

  cur_area->sbrk = newrg->rg_end;
  free(newrg);

  return 0;
}

// #endif
