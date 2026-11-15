/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
static struct queue_t ready_queue;
static struct queue_t run_queue;
static pthread_mutex_t queue_lock;

static struct queue_t running_list;
#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
static int slot[MAX_PRIO];
static int current_prio = 0;
#endif

int queue_empty(void)
{
    int result;

    pthread_mutex_lock(&queue_lock);

#ifdef MLQ_SCHED
    for (int prio = 0; prio < MAX_PRIO; prio++) {
        if (!empty(&mlq_ready_queue[prio])) {
            pthread_mutex_unlock(&queue_lock);
            return -1;
        }
    }
#endif

    result = empty(&ready_queue) && empty(&run_queue);

    pthread_mutex_unlock(&queue_lock);

    return result;
}

void init_scheduler(void) {
#ifdef MLQ_SCHED
    int i ;

	for (i = 0; i < MAX_PRIO; i ++) {
		mlq_ready_queue[i].size = 0;
		slot[i] = MAX_PRIO - i;
	}

	current_prio = 0;
#endif
	ready_queue.size = 0;
	run_queue.size = 0;
	running_list.size = 0;
	pthread_mutex_init(&queue_lock, NULL);
}

#ifdef MLQ_SCHED
/*
 *  Stateful design for routine calling
 *  based on the priority and our MLQ policy
 *  We implement stateful here using transition technique
 *  State representation   prio = 0 .. MAX_PRIO, curr_slot = 0..(MAX_PRIO - prio)
 */
struct pcb_t *get_mlq_proc(void)
{
    struct pcb_t *proc = NULL;

    pthread_mutex_lock(&queue_lock);

if (running_list.size >= MAX_QUEUE_SIZE) {
    pthread_mutex_unlock(&queue_lock);
    return NULL;
}

    for (int pass = 0; pass < 2 && proc == NULL; pass++) {

        int checked = 0;

        while (checked < MAX_PRIO) {
            int p = current_prio;

            if (slot[p] > 0 &&
                !empty(&mlq_ready_queue[p])) {

		proc = dequeue(&mlq_ready_queue[p]);

		if (proc != NULL) {
        enqueue(&running_list, proc);
        slot[p]--;

                    if (slot[p] == 0)
                        current_prio = (p + 1) % MAX_PRIO;

                    break;
                }
            }

            current_prio = (p + 1) % MAX_PRIO;
            checked++;
        }

        if (proc == NULL) {
            for (int i = 0; i < MAX_PRIO; i++)
                slot[i] = MAX_PRIO - i;
        }
    }

    pthread_mutex_unlock(&queue_lock);

    return proc;
}

void put_mlq_proc(struct pcb_t *proc)
{
    if (proc == NULL || proc->krnl == NULL)
        return;

    if (proc->prio >= MAX_PRIO)
        return;
pthread_mutex_lock(&queue_lock);

proc->krnl->ready_queue = &ready_queue;
proc->krnl->mlq_ready_queue = mlq_ready_queue;
proc->krnl->running_list = &running_list;

purgequeue(&running_list, proc);

enqueue(&mlq_ready_queue[proc->prio], proc);

pthread_mutex_unlock(&queue_lock);

}

void add_mlq_proc(struct pcb_t *proc)
{
    if (proc == NULL || proc->krnl == NULL)
        return;

    if (proc->prio >= MAX_PRIO)
        return;

    pthread_mutex_lock(&queue_lock);

    proc->krnl->ready_queue = &ready_queue;
    proc->krnl->mlq_ready_queue = mlq_ready_queue;
    proc->krnl->running_list = &running_list;

    enqueue(&mlq_ready_queue[proc->prio], proc);

    pthread_mutex_unlock(&queue_lock);
}

struct pcb_t * get_proc(void) {
	return get_mlq_proc();
}

void put_proc(struct pcb_t * proc) {
	return put_mlq_proc(proc);
}

void add_proc(struct pcb_t * proc) {
	return add_mlq_proc(proc);
}
#else
struct pcb_t * get_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	/*TODO: get a process from [ready_queue].
	 *       It worth to protect by a mechanism.
	 *
	 */

	pthread_mutex_unlock(&queue_lock);

	return proc;
}

void put_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	/* TODO: put running proc to running_list
	 *       It worth to protect by a mechanism.
	 *
	 */

	pthread_mutex_lock(&queue_lock);
	enqueue(&run_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	/* TODO: put running proc to running_list
	 *       It worth to protect by a mechanism.
	 *
	 */

	pthread_mutex_lock(&queue_lock);
	enqueue(&ready_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}
#endif

void finish_proc(struct pcb_t *proc)
{
    if (proc == NULL)
        return;

    pthread_mutex_lock(&queue_lock);

    purgequeue(&running_list, proc);

    pthread_mutex_unlock(&queue_lock);
}
/*
 * Find a currently running process by PID.
 * Intended for synchronous system calls only.
 */
struct pcb_t *find_running_proc_by_pid(uint32_t pid,
                                       struct krnl_t *krnl)
{
    struct pcb_t *found = NULL;

    if (krnl == NULL)
        return NULL;

    pthread_mutex_lock(&queue_lock);

    for (int i = 0; i < running_list.size; i++) {
        struct pcb_t *proc = running_list.proc[i];

        if (proc != NULL &&
            proc->pid == pid &&
            proc->krnl == krnl) {
            found = proc;
            break;
        }
    }

    pthread_mutex_unlock(&queue_lock);

    return found;
}
