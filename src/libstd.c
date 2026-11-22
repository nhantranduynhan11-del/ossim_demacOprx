/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "common.h"
#include "syscall.h"
#include "libmem.h"


int libsyscall(struct pcb_t *caller,
               uint32_t syscall_idx,
               arg_t a1,
               arg_t a2,
               arg_t a3)
{
    struct sc_regs regs;

    if (caller == NULL || caller->krnl == NULL)
        return -1;

#ifdef MM_PAGING
    /*
     * A user SYSCALL instruction must not perform
     * unrestricted physical RAM I/O.
     *
     * Normal READ/WRITE operations translate and
     * validate user virtual addresses in libmem.
     */
    if (syscall_idx == 17 &&
        (a1 == SYSMEM_IO_READ ||
         a1 == SYSMEM_IO_WRITE)) {
        fprintf(stderr,
                "[SYSCALL] Denied direct user physical RAM I/O\n");
        return -1;
    }
#endif

    regs.a1 = a1;
    regs.a2 = a2;
    regs.a3 = a3;

    return _syscall(caller->krnl, caller->pid,
                    syscall_idx, &regs);
}
