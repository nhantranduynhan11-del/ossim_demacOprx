/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * CPU and co-processor module
 */
#include "cpu.h"
#include "mem.h"
#include "mm.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <string.h>

int calc(struct pcb_t *proc)
{
	return ((unsigned long)proc & 0UL);
}

int alloc(struct pcb_t *proc, uint32_t size, uint32_t reg_index)
{
	addr_t addr = alloc_mem(size, proc);
	if (addr == 0)
	{
		return 1;
	}
	else
	{
		proc->regs[reg_index] = addr;
		return 0;
	}
}

int free_data(struct pcb_t *proc, uint32_t reg_index)
{
	return free_mem(proc->regs[reg_index], proc);
}

int cpu_read(
	struct pcb_t *proc, // Process executing the instruction
	uint32_t source,	// Index of source register
	uint32_t offset,	// Source address = [source] + [offset]
	uint32_t destination)
{ // Index of destination register

	BYTE data;
	if (read_mem(proc->regs[source] + offset, proc, &data))
	{
		proc->regs[destination] = data;
		return 0;
	}
	else
	{
		return 1;
	}
}

int cpu_write(
	struct pcb_t *proc,	// Process executing the instruction
	BYTE data,		// Data to be wrttien into memory
	uint32_t destination, // Index of destination register
	uint32_t offset)
{ // Destination address =
	// [destination] + [offset]
	return write_mem(proc->regs[destination] + offset, proc, data);
}

int run(struct pcb_t *proc)
{
	/* Check if Program Counter point to the proper instruction */
	if (proc->pc >= proc->code->size)
	{
		return 1;
	}

	struct inst_t ins = proc->code->text[proc->pc];
	proc->pc++;
	int stat = 1;
switch (ins.opcode)
	{
case CALC:
{
    const char *async_env = getenv("OSSIM_DMA_ASYNC_TEST");

    if (async_env != NULL &&
        strcmp(async_env, "1") == 0) {

        int pending = dma_process_has_pending(proc->pid);

        printf("[DMA ASYNC TEST] CPU reached CALC "
               "pid=%u dma_pending=%d\n",
               proc->pid, pending);
    }

    stat = calc(proc);
    break;
}
	case ALLOC:
#ifdef MM_PAGING
		stat = liballoc(proc, ins.arg_0, ins.arg_1);
#else
		stat = alloc(proc, ins.arg_0, ins.arg_1);
#endif
		break;
#ifdef MM_PAGING
	case KMALLOC:
		stat = libkmem_malloc(proc, ins.arg_0, ins.arg_1);
		break;

case COPY_FROM_USER:
{
    stat = libkmem_copy_from_user(
        proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);

    if (getenv("OSSIM_KERNEL_COPY_TEST") != NULL) {
        printf("[KERNEL COPY TEST] FROM_USER pid=%u status=%d\n",
               proc->pid, stat);
    }
    break;
}

case COPY_TO_USER:
{
    stat = libkmem_copy_to_user(
        proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);

    if (getenv("OSSIM_KERNEL_COPY_TEST") != NULL) {
        printf("[KERNEL COPY TEST] TO_USER pid=%u status=%d\n",
               proc->pid, stat);

        /*
         * Dedicated ABCD regression:
         *   user region 1 -> kernel region 2 -> user region 3
         */
        if (ins.arg_0 == 2 && ins.arg_1 == 3 &&
            ins.arg_2 == 0 && ins.arg_3 == 4) {

            const BYTE expected[4] = {65, 66, 67, 68};
            int verified = (stat == 0);

            for (uint32_t i = 0; i < 4 && verified; i++) {
                uint32_t actual = 0;

                if (libread(proc, 3, i, &actual) != 0 ||
                    actual != expected[i]) {
                    verified = 0;
                }
            }

            if (verified) {
                printf("[KERNEL COPY TEST] PASS: "
                       "user->kernel->user data=ABCD\n");
            } else {
                fprintf(stderr,
                        "[KERNEL COPY TEST] FAIL: "
                        "copy status or destination data mismatch\n");
            }
        }
    }
    break;
}

	case DMA_MEMCPY:
		stat = libmem_dma_memcpy(proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);
		break;
#endif
	case FREE:
#ifdef MM_PAGING
		stat = libfree(proc, ins.arg_0);
#else
		stat = free_data(proc, ins.arg_0);
#endif
		break;
	case READ:
#ifdef MM_PAGING
		stat = libread(proc, ins.arg_0, ins.arg_1, (uint32_t*) &ins.arg_2);
#else
stat = cpu_read(proc, ins.arg_0, ins.arg_1, ins.arg_2);
#endif
		break;
	case WRITE:
#ifdef MM_PAGING
		stat = libwrite(proc, ins.arg_0, ins.arg_1, ins.arg_2);
#else
stat = cpu_write(proc, ins.arg_0, ins.arg_1, ins.arg_2);
#endif
		break;
	case SYSCALL:
		stat = libsyscall(proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);
		break;
	default:
		stat = 1;
	}
	return stat;
}
