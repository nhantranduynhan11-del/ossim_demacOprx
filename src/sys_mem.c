/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "os-mm.h"
#include "syscall.h"
#include "libmem.h"
#include "queue.h"
#include <stdlib.h>
#include "sched.h"

#ifdef MM64
#include "mm64.h"
#else
#include "mm.h"
#endif

//typedef char BYTE;


int __sys_memmap(struct krnl_t *krnl,
                 uint32_t pid,
                 struct sc_regs *regs)
{
    struct pcb_t *caller;
    BYTE value;

    if (krnl == NULL || regs == NULL)
        return -1;

    switch (regs->a1)
    {
    case SYSMEM_MAP_OP:
    case SYSMEM_INC_OP:
    case SYSMEM_SWP_OP:
        /*
         * PID is passed through the syscall interface.
         * PCB lookup happens inside kernel code.
         */
        caller = find_running_proc_by_pid(pid, krnl);

        if (caller == NULL || caller->mm == NULL)
            return -1;

        if (regs->a1 == SYSMEM_MAP_OP)
            return vmap_pgd_memset(caller,
                                   regs->a2,
                                   regs->a3);

        if (regs->a1 == SYSMEM_INC_OP)
            return inc_vma_limit(caller,
                                 regs->a2,
                                 regs->a3);

        return __mm_swap_page(caller,
                              regs->a2,
                              regs->a3);


    case SYSMEM_IO_READ:
        if (krnl->mram == NULL ||
            krnl->mram->storage == NULL ||
            krnl->mram->maxsz <= 0 ||
            regs->a2 >= (addr_t)krnl->mram->maxsz)
            return -1;

        if (MEMPHY_read(krnl->mram,
                        regs->a2,
                        &value) != 0)
            return -1;

        regs->a3 = value;
        return 0;

    case SYSMEM_IO_WRITE:
        if (krnl->mram == NULL ||
            krnl->mram->storage == NULL ||
            krnl->mram->maxsz <= 0 ||
            regs->a2 >= (addr_t)krnl->mram->maxsz)
            return -1;

        return MEMPHY_write(krnl->mram,
                            regs->a2,
                            (BYTE)regs->a3);

    default:
        return -1;
    }
}
