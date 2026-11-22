/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA Interface Header
 */

#ifndef __DMA_H__
#define __DMA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
struct pcb_t;

/*
 * do_dma - perform DMA function
 * caller: process control block
 * source: source memory address
 * destination: destination memory address
 * length: number of bytes to copy
 */
int do_dma(struct pcb_t *caller, uint64_t source, uint64_t destination, uint64_t length);

/*
 * Submit an asynchronous DMA request using physical bus addresses.
 * Returns 0 when the request is queued, -1 on failure.
 *
 * Caller must guarantee that source/destination frames remain valid
 * until DMA completion. This API does not translate virtual addresses.
 */
enum dma_request_state {
    DMA_REQ_QUEUED = 0,
    DMA_REQ_RUNNING = 1,
    DMA_REQ_DONE = 2,
    DMA_REQ_FAILED = 3
};

int dma_submit_physical(uint64_t source,
                        uint64_t destination,
                        uint64_t length);
/*
 * Submit DMA asynchronously and return its request ID.
 * Returns 0 if queued, -1 on failure.
 */
int dma_submit_physical_with_id(uint64_t source,
                                uint64_t destination,
                                uint64_t length,
                                uint64_t *request_id);
/*
 * MM64 only: submit one physical DMA segment.
 *
 * Caller must already hold ONE pin reference for each
 * source/destination frame (two pins even if same frame).
 *
 * Success: DMA request owns both pins.
 * Failure: caller still owns both pins and must unpin.
 *
 * A segment must not cross either physical page boundary.
 */
#ifdef MM64
int dma_submit_pinned_with_id(uint32_t owner_pid,
                              uint64_t source,
                              uint64_t destination,
                              uint64_t length,
                              uint64_t *request_id);
#endif

/*
 * Return request state:
 * 0 = QUEUED
 * 1 = RUNNING
 * 2 = DONE
 * 3 = FAILED
 * -1 = unknown request ID
 */
int dma_get_request_status(uint64_t id);
/*
 * Wait for all DMA requests owned by this process.
 * Used before releasing PCB and page tables.
 */
int dma_wait_for_process(uint32_t pid);

/*
 * Return 1 if the process has pending DMA requests,
 * 0 if no requests are pending.
 */
int dma_process_has_pending(uint32_t pid);

/*
 * DMA frame pin tracking.
 *
 * A pinned physical RAM frame must not be selected
 * for page eviction until its DMA transfer finishes.
 *
 * Frame number (FPN), not physical byte address.
 */
int dma_pin_frame(uint64_t fpn);
int dma_unpin_frame(uint64_t fpn);
int dma_frame_is_pinned(uint64_t fpn);

#ifdef __cplusplus
}
#endif

#endif /* __DMA_H__ */
