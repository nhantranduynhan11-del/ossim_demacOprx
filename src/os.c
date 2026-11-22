/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System OS module
 */
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "loader.h"
#include "mm.h"
#ifdef MM64
#include "mm64.h"
#endif

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "sysbus.h"
#include "dpi_rtl.h"
#include "dma.h"
#include <time.h>

static int time_slot;
static int num_cpus;
static int done = 0;
static struct krnl_t os;

static pthread_mutex_t dma_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dma_cond = PTHREAD_COND_INITIALIZER;

static int dma_init_done = 0;
static int dma_init_ok = 0;
static int dma_stop_requested = 0;

#ifdef MM_PAGING
static unsigned long memramsz;
static unsigned long memswpsz[PAGING_MAX_MMSWP];

struct mmpaging_ld_args {
	/* A dispatched argument struct to compact many-fields passing to loader */
	int vmemsz;
	struct memphy_struct *mram;
	struct memphy_struct **mswp;
	struct memphy_struct *active_mswp;
	int active_mswp_id;
	struct timer_id_t  *timer_id;
};
#endif

static struct ld_args{
	char ** path;
	unsigned long * start_time;
#ifdef MLQ_SCHED
	unsigned long * prio;
#endif
} ld_processes;
int num_processes;

struct cpu_args {
	struct timer_id_t * timer_id;
	int id;
};


/*
 * DMA request queue.
 *
 * Only physical bus addresses are stored here.
 * The co-processor thread is the sole RTL executor.
 */

struct dma_request {
    uint32_t owner_pid;
    int owns_frame_pins;
    uint64_t source_fpn;
    uint64_t destination_fpn;
    uint64_t id;
    uint64_t source;
    uint64_t destination;
    uint64_t length;

    enum dma_request_state state;

    /* Link in pending request queue. */
    struct dma_request *next;

    /* Link in all submitted requests, for status queries. */
    struct dma_request *all_next;
};

static struct dma_request *dma_head = NULL;
static struct dma_request *dma_tail = NULL;
static struct dma_request *dma_all_head = NULL;
static uint64_t dma_next_id = 1;
static uint64_t dma_ram_bound = 0;
static BYTE *dma_verify_ram = NULL;

static uint64_t dma_success_count = 0;
static uint64_t dma_failure_count = 0;


/*
 * Physical RAM frame pin reference counts.
 *
 * Multiple DMA requests may use the same frame.
 * Protect this list with dma_mutex.
 */
struct dma_frame_pin {
    uint64_t fpn;
    uint32_t refs;
    struct dma_frame_pin *next;
};

static struct dma_frame_pin *dma_pin_head = NULL;

int dma_pin_frame(uint64_t fpn)
{
    struct dma_frame_pin *node;

    pthread_mutex_lock(&dma_mutex);

    for (node = dma_pin_head; node != NULL;
         node = node->next) {
        if (node->fpn == fpn) {
            if (node->refs == UINT32_MAX) {
                pthread_mutex_unlock(&dma_mutex);
                return -1;
            }

            node->refs++;
            pthread_mutex_unlock(&dma_mutex);
            return 0;
        }
    }

    node = malloc(sizeof(*node));
    if (node == NULL) {
        pthread_mutex_unlock(&dma_mutex);
        return -1;
    }

    node->fpn = fpn;
    node->refs = 1;
    node->next = dma_pin_head;
    dma_pin_head = node;

    pthread_mutex_unlock(&dma_mutex);
    return 0;
}

int dma_unpin_frame(uint64_t fpn)
{
    struct dma_frame_pin *node;
    struct dma_frame_pin *prev = NULL;

    pthread_mutex_lock(&dma_mutex);

    for (node = dma_pin_head; node != NULL;
         prev = node, node = node->next) {
        if (node->fpn == fpn) {
            if (--node->refs == 0) {
                if (prev == NULL)
                    dma_pin_head = node->next;
                else
                    prev->next = node->next;

                free(node);
            }

            pthread_mutex_unlock(&dma_mutex);
            return 0;
        }
    }

    pthread_mutex_unlock(&dma_mutex);
    return -1;
}

int dma_frame_is_pinned(uint64_t fpn)
{
    struct dma_frame_pin *node;
    int pinned = 0;

    pthread_mutex_lock(&dma_mutex);

    for (node = dma_pin_head; node != NULL;
         node = node->next) {
        if (node->fpn == fpn) {
            pinned = 1;
            break;
        }
    }

    pthread_mutex_unlock(&dma_mutex);
    return pinned;
}

static int dma_submit_internal(uint64_t source,
                               uint64_t destination,
                               uint64_t length,
                               uint32_t owner_pid,
                               int owns_frame_pins,
                               uint64_t *request_id)
{
    struct dma_request *req;

    /*
     * Temporary restriction: only aligned, 4-byte-multiple
     * physical transfers. Byte-sized transfers require
     * additional RTL/bus handling.
     */
    if (length == 0 || (source & 3U) != 0 ||
        (destination & 3U) != 0 || (length & 3U) != 0)
        return -1;

    if (source >= dma_ram_bound ||
        destination >= dma_ram_bound ||
        length > dma_ram_bound - source ||
        length > dma_ram_bound - destination)
        return -1;

    req = malloc(sizeof(*req));
    if (req == NULL)
        return -1;

req->source = source;
req->destination = destination;
req->length = length;
req->next = NULL;
req->all_next = NULL;
req->state = DMA_REQ_QUEUED;

req->owner_pid = owner_pid;
req->owns_frame_pins = owns_frame_pins;
req->source_fpn = source / PAGING64_PAGESZ;
req->destination_fpn = destination / PAGING64_PAGESZ;

    pthread_mutex_lock(&dma_mutex);

    if (dma_stop_requested || !dma_init_done ||
        !dma_init_ok) {
        pthread_mutex_unlock(&dma_mutex);
        free(req);
        return -1;
    }

    req->id = dma_next_id++;
   if (request_id != NULL)
        *request_id = req->id;
    req->all_next = dma_all_head;
    dma_all_head = req;

    if (dma_tail == NULL) {
        dma_head = req;
        dma_tail = req;
    } else {
        dma_tail->next = req;
        dma_tail = req;
    }

    printf("[DMA] SUBMIT id=%llu len=%llu\n",
           (unsigned long long)req->id,
           (unsigned long long)req->length);

    pthread_cond_signal(&dma_cond);
    pthread_mutex_unlock(&dma_mutex);

    return 0;
}


int dma_submit_physical_with_id(uint64_t source,
                                uint64_t destination,
                                uint64_t length,
                                uint64_t *request_id)
{
    return dma_submit_internal(source, destination, length,
                               0, 0, request_id);
}

#ifdef MM64
int dma_submit_pinned_with_id(uint32_t owner_pid,
                              uint64_t source,
                              uint64_t destination,
                              uint64_t length,
                              uint64_t *request_id)
{
if (request_id == NULL ||
    length == 0 || length > PAGING64_PAGESZ)
        return -1;

    /* One request must remain inside both pinned frames. */
    if (source % PAGING64_PAGESZ >
            PAGING64_PAGESZ - length ||
        destination % PAGING64_PAGESZ >
            PAGING64_PAGESZ - length)
        return -1;

    /*
     * Caller has pinned both frames. Transfer ownership
     * to the worker only after successful queue insertion.
     */
    return dma_submit_internal(source, destination, length,
                               owner_pid, 1, request_id);
}
#endif


int dma_submit_physical(uint64_t source,
                        uint64_t destination,
                        uint64_t length)
{
    return dma_submit_physical_with_id(
        source, destination, length, NULL);
}

int dma_get_request_status(uint64_t id)
{
    struct dma_request *req;
    int status = -1;

    pthread_mutex_lock(&dma_mutex);

    for (req = dma_all_head; req != NULL;
         req = req->all_next) {
        if (req->id == id) {
            status = (int)req->state;
            break;
        }
    }

    pthread_mutex_unlock(&dma_mutex);

    return status;
}

int dma_wait_for_process(uint32_t pid)
{
    struct dma_request *req;
    int pending;

    pthread_mutex_lock(&dma_mutex);

    for (;;) {
        pending = 0;

        for (req = dma_all_head; req != NULL;
             req = req->all_next) {
            if (req->owner_pid == pid &&
                (req->state == DMA_REQ_QUEUED ||
                 req->state == DMA_REQ_RUNNING)) {
                pending = 1;
                break;
            }
        }

        if (!pending)
            break;

        pthread_cond_wait(&dma_cond, &dma_mutex);
    }

    pthread_mutex_unlock(&dma_mutex);

    return 0;
}


int dma_process_has_pending(uint32_t pid)
{
    struct dma_request *req;
    int pending = 0;

    pthread_mutex_lock(&dma_mutex);

    for (req = dma_all_head; req != NULL;
         req = req->all_next) {
        if (req->owner_pid == pid &&
            (req->state == DMA_REQ_QUEUED ||
             req->state == DMA_REQ_RUNNING)) {
            pending = 1;
            break;
        }
    }

    pthread_mutex_unlock(&dma_mutex);

    return pending;
}

static void * cpu_routine(void * args) {
	struct timer_id_t * timer_id = ((struct cpu_args*)args)->timer_id;
	int id = ((struct cpu_args*)args)->id;
	/* Check for new process in ready queue */
	int time_left = 0;
	struct pcb_t * proc = NULL;
	while (1) {
		/* Check the status of current process */
if (proc == NULL) {
    proc = get_proc();

    if (proc == NULL) {
        /*
         * No ready process remains and the loader has
         * finished creating all processes.
         */
        if (done) {
            printf("\tCPU %d stopped\n", id);
            break;
        }

        next_slot(timer_id);
        continue;
    }
}
		else if (proc->pc == proc->code->size) {
			/* The porcess has finish it job */
			printf("\tCPU %d: Processed %2d has finished\n",
				id ,proc->pid);
#ifdef MM64
    /*
     * Wait for outstanding DMA before releasing
     * process memory and page tables.
     */
    dma_wait_for_process(proc->pid);
    free_pcb_pgtbl(proc);
#endif

    /*
     * Remove PCB from the scheduler registry
     * before releasing the PCB itself.
     */
    finish_proc(proc);

    /*
     * Release objects allocated by load().
     */
    if (proc->code != NULL) {
        free(proc->code->text);
        free(proc->code);
    }

    free(proc->page_table);
    free(proc);
			proc = get_proc();
			time_left = 0;
		}else if (time_left == 0) {
			/* The process has done its job in current time slot */
			printf("\tCPU %d: Put process %2d to run queue\n",
				id, proc->pid);
			put_proc(proc);
			proc = get_proc();
		}

		/* Recheck process status after loading new process */
		if (proc == NULL && done) {
			/* No process to run, exit */
			printf("\tCPU %d stopped\n", id);
			break;
		}else if (proc == NULL) {
			/* There may be new processes to run in
			 * next time slots, just skip current slot */
			next_slot(timer_id);
			continue;
		}else if (time_left == 0) {
			printf("\tCPU %d: Dispatched process %2d\n",
				id, proc->pid);
			time_left = time_slot;
		}

		/* Run current process */
		run(proc);
		time_left--;
		next_slot(timer_id);
	}
	detach_event(timer_id);
	pthread_exit(NULL);
}

/*
 * Handle a DMA request after the RTL transfer has finished.
 * dpi_rtl_dma_copy() validates ch_done, IRQ, callback and errors.
 */
static void dma_completion_handler(struct dma_request *req, int result)
{
    int failed = (result != 0 || sysbus_get_dma_error() != 0);

#ifdef MM64
    /*
     * The DMA transfer has stopped using these physical frames.
     */
    if (req->owns_frame_pins) {
        int src_rc = dma_unpin_frame(req->source_fpn);
        int dst_rc = dma_unpin_frame(req->destination_fpn);

        if (src_rc != 0 || dst_rc != 0) {
            fprintf(stderr,
                    "[DMA] Pin release failed id=%llu\n",
                    (unsigned long long)req->id);
            failed = 1;
        }

        req->owns_frame_pins = 0;
    }
#endif

    pthread_mutex_lock(&dma_mutex);

    if (failed) {
        dma_failure_count++;
        req->state = DMA_REQ_FAILED;
    } else {
        dma_success_count++;
        req->state = DMA_REQ_DONE;
    }

    pthread_cond_broadcast(&dma_cond);
    pthread_mutex_unlock(&dma_mutex);

    if (failed) {
        fprintf(stderr,
                "[DMA] FAILED id=%llu\n",
                (unsigned long long)req->id);
    } else {
        printf("[DMA] IRQ HANDLED id=%llu status=DONE\n",
               (unsigned long long)req->id);
        printf("[DMA] TRANSFER_DONE id=%llu\n",
               (unsigned long long)req->id);
    }
}

static void *co_processor(void *arg)
{
    (void)arg;
    int rc = rtl_dma_init_simulation();

    pthread_mutex_lock(&dma_mutex);

    dma_init_ok = (rc == 0);
    dma_init_done = 1;

    pthread_cond_broadcast(&dma_cond);
    pthread_mutex_unlock(&dma_mutex);

    if (rc != 0) {
        fprintf(stderr, "[DMA] RTL initialization failed\n");
        rtl_dma_cleanup_simulation();
        return NULL;
    }

    printf("[DMA] co-processor ready\n");


    for (;;) {
        struct dma_request *req;
        int result;

        pthread_mutex_lock(&dma_mutex);

        while (dma_head == NULL && !dma_stop_requested) {
            pthread_cond_wait(&dma_cond, &dma_mutex);
        }

        /*
         * Shutdown only after processing queued requests.
         */
        if (dma_head == NULL && dma_stop_requested) {
            pthread_mutex_unlock(&dma_mutex);
            break;
        }

        req = dma_head;
        dma_head = req->next;

        if (dma_head == NULL)
            dma_tail = NULL;

        req->state = DMA_REQ_RUNNING;

        pthread_mutex_unlock(&dma_mutex);

        /*
         * IMPORTANT: Do not hold dma_mutex while executing
         * the RTL transfer. The CPU can submit other jobs.
         */
        printf("[DMA] START id=%llu\n",
               (unsigned long long)req->id);

        /*
         * Test-only: delay the worker to verify that the CPU
         * can continue after enqueueing a DMA request.
         */
        const char *async_env = getenv("OSSIM_DMA_ASYNC_TEST");

        if (async_env != NULL &&
            strcmp(async_env, "1") == 0 &&
            req->owner_pid == 1 &&
            req->length == 4) {

            printf("[DMA ASYNC TEST] Worker delaying request=%llu\n",
                   (unsigned long long)req->id);

            struct timespec delay = {
                .tv_sec = 1,
                .tv_nsec = 0
            };

            nanosleep(&delay, NULL);
        }

        sysbus_reset_dma_error();

        /*
         * Test-only: verify the known ABCD process DMA request.
         * Inspect RAM while both DMA frames remain pinned.
         */
        const char *abcd_env = getenv("OSSIM_DMA_ABCD_TEST");
        int abcd_check =
            abcd_env != NULL &&
            strcmp(abcd_env, "1") == 0 &&
            req->owner_pid == 1 &&
            req->length == 4 &&
            dma_verify_ram != NULL;

        const BYTE abcd_expected[4] = {65, 66, 67, 68};
        BYTE abcd_destination_before[4] = {0};

        if (abcd_check) {
            memcpy(abcd_destination_before,
                   dma_verify_ram + req->destination, 4);
        }

        result = dpi_rtl_dma_copy(req->source,
                                  req->destination,
                                  req->length);

        if (abcd_check) {
            int source_ok =
                memcmp(dma_verify_ram + req->source,
                       abcd_expected, 4) == 0;

            int destination_was_different =
                memcmp(abcd_destination_before,
                       abcd_expected, 4) != 0;

            int destination_ok =
                memcmp(dma_verify_ram + req->destination,
                       abcd_expected, 4) == 0;

            if (result == 0 && source_ok &&
                destination_was_different && destination_ok) {
                printf("[DMA ABCD TEST] PASS: "
                       "source=ABCD destination=ABCD "
                       "id=%llu\n",
                       (unsigned long long)req->id);
            } else {
                fprintf(stderr,
                        "[DMA ABCD TEST] FAIL: "
                        "source_ok=%d dst_changed=%d "
                        "destination_ok=%d id=%llu\n",
                        source_ok,
                        destination_was_different,
                        destination_ok,
                        (unsigned long long)req->id);
                result = -1;
            }
        }

        dma_completion_handler(req, result);
    }

    rtl_dma_cleanup_simulation();

    printf("[DMA] co-processor stopped\n");

    return NULL;
}

static void * ld_routine(void * args) {
#ifdef MM_PAGING
	struct memphy_struct* mram = ((struct mmpaging_ld_args *)args)->mram;
	struct memphy_struct** mswp = ((struct mmpaging_ld_args *)args)->mswp;
	struct memphy_struct* active_mswp = ((struct mmpaging_ld_args *)args)->active_mswp;
	struct timer_id_t * timer_id = ((struct mmpaging_ld_args *)args)->timer_id;
#else
	struct timer_id_t * timer_id = (struct timer_id_t*)args;
#endif
	int i = 0;
	printf("ld_routine\n");
	os.mram = mram;
	os.mswp = mswp;
	os.active_mswp = active_mswp;
#ifdef MM_PAGING
	os.active_mswp_id = ((struct mmpaging_ld_args *)args)->active_mswp_id;
#endif
	/* Kernel memory map: init_mm(.., NULL) = kernel mm (area at KERNEL_BASE_ADDR) */
	os.mm = (struct mm_struct*)malloc(sizeof(struct mm_struct));
	init_mm(os.mm, NULL);

	/* @Role B: init kernel page table directory.
	 * MM64: krnl_pgd is the root of the kernel half (VA >= KERNEL_BASE_ADDR),
	 *       shared by all processes; krnl_p4d..krnl_pt is the path of the
	 *       first kernel page, built at boot. */
#ifdef MM64
	if (init_krnl_pgtbl(&os) != 0)
		printf("[mm64] cannot init kernel page table\n");
#else
	os.krnl_pgd = (uint32_t*)calloc(PAGING_MAX_PGN, sizeof(uint32_t));
#endif
	while (i < num_processes) {
		struct pcb_t * proc = load(ld_processes.path[i]);
		proc->krnl = &os;

#ifdef MLQ_SCHED
	proc->prio = ld_processes.prio[i];
#endif
	while (current_time() < ld_processes.start_time[i]) {
		next_slot(timer_id);
	}
#ifdef MM_PAGING
	proc->mm = (struct mm_struct*)malloc(sizeof(struct mm_struct));
	init_mm(proc->mm, proc);
#endif
		printf("\tLoaded a process at %s, PID: %d PRIO: %ld\n",
			ld_processes.path[i], proc->pid, ld_processes.prio[i]);
		add_proc(proc);
		free(ld_processes.path[i]);
		i++;
		next_slot(timer_id);
	}
	free(ld_processes.path);
	free(ld_processes.start_time);
	done = 1;
	detach_event(timer_id);
	pthread_exit(NULL);
}

static void read_config(const char * path) {
	FILE * file;
	if ((file = fopen(path, "r")) == NULL) {
		printf("Cannot find configure file at %s\n", path);
		exit(1);
	}
	fscanf(file, "%d %d %d\n", &time_slot, &num_cpus, &num_processes);
	ld_processes.path = (char**)malloc(sizeof(char*) * num_processes);
	ld_processes.start_time = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#ifdef MM_PAGING
	int sit;
#ifdef MM_FIXED_MEMSZ
	/* We provide here a back compatible with legacy OS simulatiom config file
         * In which, it have no addition config line for Mema, keep only one line
	 * for legacy info
         *  [time slice] [N = Number of CPU] [M = Number of Processes to be run]
         */
        memramsz  =  0x100000000;
        memswpsz[0] = 0x1000000;
	for(sit = 1; sit < PAGING_MAX_MMSWP; sit++)
		memswpsz[sit] = 0;

#else
    /*
     * Support both paging configs and legacy scheduler configs.
     *
     * Paging format:
     *   RAM_SIZE SWAP0_SIZE SWAP1_SIZE SWAP2_SIZE SWAP3_SIZE
     *
     * Legacy format:
     *   The process list begins immediately after the header.
     */
    {
        long config_pos = ftell(file);
        char config_line[256];
        unsigned long sizes[5];
        char extra;

        int fields = 0;

        if (config_pos < 0) {
            fprintf(stderr, "[CONFIG] Cannot inspect input position\n");
            exit(1);
        }

        if (fgets(config_line, sizeof(config_line), file) != NULL) {
            fields = sscanf(config_line,
                            "%lu %lu %lu %lu %lu %c",
                            &sizes[0],
                            &sizes[1],
                            &sizes[2],
                            &sizes[3],
                            &sizes[4],
                            &extra);
        }

        if (fields == 5 && sizes[0] > 0 && sizes[1] > 0) {
            /* Explicit paging memory configuration. */
            memramsz = sizes[0];

            for (sit = 0; sit < PAGING_MAX_MMSWP; sit++)
                memswpsz[sit] = sizes[sit + 1];

            printf("[CONFIG] Paging RAM=%lu SWAP0=%lu\n",
                   memramsz, memswpsz[0]);
        } else {
            /*
             * Legacy input has no memory-size line.
             * Use the sizes from the corresponding paging_compat
             * test configurations.
             */
            memramsz = 1048576UL;
            memswpsz[0] = 16777216UL;

            for (sit = 1; sit < PAGING_MAX_MMSWP; sit++)
                memswpsz[sit] = 0;

            if (fseek(file, config_pos, SEEK_SET) != 0) {
                fprintf(stderr, "[CONFIG] Cannot restore input position\n");
                exit(1);
            }

            printf("[CONFIG] Legacy input: using default RAM=%lu SWAP0=%lu\n",
                   memramsz, memswpsz[0]);
        }
    }
#endif

#endif

#ifdef MLQ_SCHED
	ld_processes.prio = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#endif
	int i;
	for (i = 0; i < num_processes; i++) {
		ld_processes.path[i] = (char*)malloc(sizeof(char) * 100);
		ld_processes.path[i][0] = '\0';
		strcat(ld_processes.path[i], "input/proc/");
		char proc[100];
#ifdef MLQ_SCHED
		fscanf(file, "%lu %s %lu\n", &ld_processes.start_time[i], proc, &ld_processes.prio[i]);
#else
		fscanf(file, "%lu %s\n", &ld_processes.start_time[i], proc);
#endif
		strcat(ld_processes.path[i], proc);
	}
}

int main(int argc, char * argv[]) {
	/* Read config */
	if (argc != 2) {
		printf("Usage: os [path to configure file]\n");
		return 1;
	}
	char path[100];
	path[0] = '\0';
	strcat(path, "input/");
	strcat(path, argv[1]);
	read_config(path);

	pthread_t * cpu = (pthread_t*)malloc(num_cpus * sizeof(pthread_t));
	pthread_t * dma = (pthread_t*)malloc(sizeof(pthread_t));
	struct cpu_args * args =
		(struct cpu_args*)malloc(sizeof(struct cpu_args) * num_cpus);
	pthread_t ld;

#ifdef MM_PAGING
    int dma_test_enabled = (getenv("OSSIM_DMA_SELFTEST") != NULL);
    int dma_test_failed = 0;
    int dma_test_submitted = 0;
    uint64_t dma_test_request_id = 0;
    int dma_test_src_allocated = 0;
    int dma_test_dst_allocated = 0;
    addr_t dma_test_src_fpn = 0;
    addr_t dma_test_dst_fpn = 0;
    uint64_t dma_test_src_pa = 0;
    uint64_t dma_test_dst_pa = 0;
    const size_t dma_test_len = 512;
#endif

	/* Init timer */
	int i;
	for (i = 0; i < num_cpus; i++) {
		args[i].timer_id = attach_event();
		args[i].id = i;
	}
	struct timer_id_t * ld_event = attach_event();
	start_timer();

#ifdef MM_PAGING
	/* Init all MEMPHY include 1 MEMRAM and n of MEMSWP */
	int rdmflag = 1; /* By default memphy is RANDOM ACCESS MEMORY */

	struct memphy_struct mram;
	struct memphy_struct mswp[PAGING_MAX_MMSWP];
	/* krnl->mswp is a "struct memphy_struct **": give it a real array of
	 * pointers (casting &mswp would make mswp[i] read garbage pointers) */
	struct memphy_struct *mswp_ptr[PAGING_MAX_MMSWP];

	/* Create MEM RAM */
	init_memphy(&mram, memramsz, rdmflag);

        /* Expose physical RAM to the DMA system bus. */
        if (mram.storage == NULL || mram.maxsz <= 0 ||
            sysbus_bind_os_ram(mram.storage, (size_t)mram.maxsz) != 0) {
            fprintf(stderr, "[DMA BUS] Cannot bind OS RAM\n");
            stop_timer();
            return 1;
        }

        dma_ram_bound = (uint64_t)mram.maxsz;
        dma_verify_ram = mram.storage;

        /* Create all MEM SWAP */
	int sit;
	for(sit = 0; sit < PAGING_MAX_MMSWP; sit++) {
	       init_memphy(&mswp[sit], memswpsz[sit], rdmflag);
	       mswp_ptr[sit] = &mswp[sit];
	}

	/* In Paging mode, it needs passing the system mem to each PCB through loader*/
	struct mmpaging_ld_args *mm_ld_args = (struct mmpaging_ld_args*)malloc(sizeof(struct mmpaging_ld_args));

	mm_ld_args->timer_id = ld_event;
	mm_ld_args->mram = (struct memphy_struct *) &mram;
	mm_ld_args->mswp = (struct memphy_struct**) mswp_ptr;
	mm_ld_args->active_mswp = (struct memphy_struct *) &mswp[0];
        mm_ld_args->active_mswp_id = 0;


#endif

	/* Init scheduler */
	init_scheduler();

    /* Start co-processor before CPU and loader */
    if (pthread_create(dma, NULL, co_processor, NULL) != 0) {
        fprintf(stderr, "[DMA] Cannot create co-processor thread\n");
        stop_timer();
        return 1;
    }

    /* Wait until RTL initialization succeeds or fails */
    pthread_mutex_lock(&dma_mutex);

    while (!dma_init_done) {
        pthread_cond_wait(&dma_cond, &dma_mutex);
    }

    int dma_ready = dma_init_ok;

    pthread_mutex_unlock(&dma_mutex);

    if (!dma_ready) {
        pthread_join(*dma, NULL);
        fprintf(stderr, "[DMA] OS startup aborted\n");
        stop_timer();
        return 1;
    }

	/* Run CPU and loader */

#ifdef MM_PAGING
	pthread_create(&ld, NULL, ld_routine, (void*)mm_ld_args);
#else
	pthread_create(&ld, NULL, ld_routine, (void*)ld_event);
#endif
	for (i = 0; i < num_cpus; i++) {
		pthread_create(&cpu[i], NULL,
			cpu_routine, (void*)&args[i]);
	}

    /* Wait for CPU and loader finishing */
    for (i = 0; i < num_cpus; i++) {
        pthread_join(cpu[i], NULL);
    }

    pthread_join(ld, NULL);


#ifdef MM_PAGING
    if (dma_test_enabled) {
        printf("[DMA SELFTEST] Preparing two reserved RAM frames\n");

        if (MEMPHY_get_freefp(&mram, &dma_test_src_fpn) != 0) {
            fprintf(stderr, "[DMA SELFTEST] Cannot allocate source frame\n");
            dma_test_failed = 1;
        } else {
            dma_test_src_allocated = 1;
        }

        if (!dma_test_failed) {
            if (MEMPHY_get_freefp(&mram, &dma_test_dst_fpn) != 0) {
                fprintf(stderr, "[DMA SELFTEST] Cannot allocate destination frame\n");
                dma_test_failed = 1;
            } else {
                dma_test_dst_allocated = 1;
            }
        }

        if (!dma_test_failed) {
            dma_test_src_pa =
                (uint64_t)dma_test_src_fpn * PAGING64_PAGESZ;
            dma_test_dst_pa =
                (uint64_t)dma_test_dst_fpn * PAGING64_PAGESZ;

            if (dma_test_src_pa >= (uint64_t)mram.maxsz ||
                dma_test_dst_pa >= (uint64_t)mram.maxsz ||
                dma_test_len > (uint64_t)mram.maxsz - dma_test_src_pa ||
                dma_test_len > (uint64_t)mram.maxsz - dma_test_dst_pa) {
                fprintf(stderr, "[DMA SELFTEST] Invalid frame addresses\n");
                dma_test_failed = 1;
            }
        }

        if (!dma_test_failed) {
            for (size_t k = 0; k < dma_test_len; ++k) {
                mram.storage[dma_test_src_pa + k] =
                    (unsigned char)((k * 17U + 3U) & 0xFFU);

                mram.storage[dma_test_dst_pa + k] = 0xA5U;
            }

            printf("[DMA SELFTEST] Source=0x%llx Destination=0x%llx\n",
                   (unsigned long long)dma_test_src_pa,
                   (unsigned long long)dma_test_dst_pa);


            int src_pinned = 0;
            int dst_pinned = 0;

            /* Pin source frame. */
            if (dma_pin_frame(dma_test_src_fpn) == 0) {
                src_pinned = 1;
            } else {
                fprintf(stderr,
                        "[DMA SELFTEST] Cannot pin source frame\n");
                dma_test_failed = 1;
            }

            /* Pin destination frame. */
            if (!dma_test_failed) {
                if (dma_pin_frame(dma_test_dst_fpn) == 0) {
                    dst_pinned = 1;
                } else {
                    fprintf(stderr,
                            "[DMA SELFTEST] Cannot pin destination frame\n");
                    dma_test_failed = 1;
                }
            }

            /* Submit only if both frames were pinned. */
            if (!dma_test_failed) {
                if (dma_submit_pinned_with_id(
                        9999,
                        dma_test_src_pa,
                        dma_test_dst_pa,
                        dma_test_len,
                        &dma_test_request_id) != 0) {
                    fprintf(stderr,
                            "[DMA SELFTEST] Pinned submit failed\n");
                    dma_test_failed = 1;
                } else {
                    /* Worker owns both pins after submit. */
                    src_pinned = 0;
                    dst_pinned = 0;
                    dma_test_submitted = 1;

                    printf("[DMA SELFTEST] Pinned request queued\n");
                }
            }

            /* If submit failed, release pins here. */
            if (src_pinned)
                dma_unpin_frame(dma_test_src_fpn);

            if (dst_pinned)
                dma_unpin_frame(dma_test_dst_fpn);
        }
    }
#endif

    /* Request co-processor shutdown */
    pthread_mutex_lock(&dma_mutex);

    dma_stop_requested = 1;
    pthread_cond_broadcast(&dma_cond);

    pthread_mutex_unlock(&dma_mutex);

    /* Wait until RTL cleanup finishes */
    pthread_join(*dma, NULL);

#ifdef MM_PAGING
    if (dma_test_enabled) {
        if (dma_test_submitted &&
            (dma_frame_is_pinned(dma_test_src_fpn) ||
             dma_frame_is_pinned(dma_test_dst_fpn))) {
            fprintf(stderr,
                    "[DMA SELFTEST] FAIL: DMA frame pins leaked\n");
            dma_test_failed = 1;
        }
        if (dma_test_submitted) {
            int state = dma_get_request_status(dma_test_request_id);

            if (state != DMA_REQ_DONE) {
                fprintf(stderr,
                        "[DMA SELFTEST] FAIL: Request %llu status=%d\n",
                        (unsigned long long)dma_test_request_id,
                        state);
                dma_test_failed = 1;
            } else {
                printf("[DMA SELFTEST] PASS: Request %llu status=DONE\n",
                       (unsigned long long)dma_test_request_id);
            }
        }

        if (dma_test_submitted) {
            if (memcmp(
                    mram.storage + dma_test_src_pa,
                    mram.storage + dma_test_dst_pa,
                    dma_test_len) == 0) {
                printf("[DMA SELFTEST] PASS: All 512 bytes match\n");
            } else {
                fprintf(stderr,
                        "[DMA SELFTEST] FAIL: Destination data mismatch\n");
                dma_test_failed = 1;
            }
        }

        if (dma_test_dst_allocated) {
            MEMPHY_put_freefp(&mram, dma_test_dst_fpn);
        }

        if (dma_test_src_allocated) {
            MEMPHY_put_freefp(&mram, dma_test_src_fpn);
        }

        if (dma_test_failed) {
            fprintf(stderr, "[DMA SELFTEST] FAILED\n");
        }
    }
#endif

    /*
     * All DMA requests have completed.
     * Self-test has already queried request status.
     * It is now safe to release completion records.
     */
    while (dma_all_head != NULL) {
        struct dma_request *req = dma_all_head;
        dma_all_head = req->all_next;
        free(req);
    }

    dma_head = NULL;
    dma_tail = NULL;

    sysbus_unbind_os_ram();
    /* Stop timer */
    stop_timer();

    free(cpu);
    free(dma);
    free(args);

#ifdef MM_PAGING
    free(mm_ld_args);
#endif

#ifdef MM_PAGING
    if (dma_test_enabled && dma_test_failed)
        return 1;
#endif
    return 0;
}



