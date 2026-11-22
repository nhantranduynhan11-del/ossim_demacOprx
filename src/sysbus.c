/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus - Only for dma_testbech simulation access
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sysbus.h"

// Maximum number of memory banks supported
#define MAX_MEMORY_BANKS 8

// Memory bank structure
typedef struct {
    uint8_t* data;
    size_t size;
    int initialized;
} memory_bank_t_struct;

// Array of memory banks
static memory_bank_t_struct g_banks[MAX_MEMORY_BANKS] = {0};
static int g_bank_count = 0;

// Physical RAM exposed to the OS DMA bus.
static uint8_t *g_os_ram = NULL;
static size_t g_os_ram_size = 0;

// Accessed by the DMA simulation thread.
static int g_dma_bus_error = 0;

void sysbus_reset_dma_error(void)
{
    g_dma_bus_error = 0;
}

int sysbus_get_dma_error(void)
{
    return g_dma_bus_error;
}


/*
 * Completion state for the active RTL transfer.
 * Only the co-processor thread may drive the RTL.
 */
static int g_dma_completion_seen = 0;

void sysbus_reset_dma_completion(void)
{
    g_dma_completion_seen = 0;
}

int sysbus_get_dma_completion(void)
{
    return g_dma_completion_seen;
}

int sysbus_bind_os_ram(uint8_t *storage, size_t size)
{
    if (storage == NULL || size == 0)
        return -1;

    g_os_ram = storage;
    g_os_ram_size = size;

    return 0;
}

void sysbus_unbind_os_ram(void)
{
    g_os_ram = NULL;
    g_os_ram_size = 0;
}

// Initialize a memory bank with given size in bytes
memory_bank_t memory_bank_init(size_t size_bytes) {
    // Find first free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (!g_banks[i].initialized) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        printf("Maximum number of memory banks (%d) reached\n", MAX_MEMORY_BANKS);
        return MEMORY_BANK_INVALID;
    }

    if (size_bytes == 0) {
        printf("Invalid bank size: 0\n");
        return MEMORY_BANK_INVALID;
    }

    uint8_t* data = (uint8_t*)malloc(size_bytes);
    if (!data) {
        printf("Failed to allocate memory bank of %zu bytes\n", size_bytes);
        return MEMORY_BANK_INVALID;
    }

    memset(data, 0, size_bytes);
    g_banks[free_slot].data = data;
    g_banks[free_slot].size = size_bytes;
    g_banks[free_slot].initialized = 1;
    g_bank_count++;

    printf("Memory bank [%d] initialized: %zu bytes at address %p\n", free_slot, size_bytes, (void*)data);
    return free_slot;
}

// Cleanup memory bank
void memory_bank_cleanup(void) {
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            if (g_banks[i].data) {
                free(g_banks[i].data);
                g_banks[i].data = NULL;
            }
            g_banks[i].size = 0;
            g_banks[i].initialized = 0;
            g_bank_count--;
            printf("Memory bank [%d] cleaned up\n", i);
        }
    }
}

int memory_bank_is_available(void) {
    // Check if at least one memory bank is initialized
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            return 1;
        }
    }
    return 0;
}

// Get pointer to locate data at offset
void* memory_bank_get_ptr(uint64_t offset) {
    if (offset >= g_banks[0].size) {
        return NULL;
    }

    return g_banks[0].data + offset;
}

// Get bank size in bytes
size_t memory_bank_get_size() {
    return g_banks[0].size;
}


#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPI Callback: Called from RTL when DMA read access
 */


int dpi_bank_read32_legacy(long long offset)
{
    uint8_t *base = NULL;
    size_t size = 0;
    uint32_t value = 0;
    uint64_t addr;

    if (g_os_ram != NULL) {
        base = g_os_ram;
        size = g_os_ram_size;
    } else if (memory_bank_is_available()) {
        base = (uint8_t *)memory_bank_get_ptr(0);
        size = memory_bank_get_size();
    }

    if (offset < 0 || base == NULL) {
        fprintf(stderr, "[DMA BUS] Invalid read source\n");
        g_dma_bus_error = 1;
        return 0;
    }

    addr = (uint64_t)offset;

    if (addr >= size || size - (size_t)addr < sizeof(value)) {
        fprintf(stderr, "[DMA BUS] Read outside memory bounds\n");
        g_dma_bus_error = 1;
        return 0;
    }

    memcpy(&value, base + (size_t)addr, sizeof(value));

    return (int)value;
}

/**
 * DPI Callback: Called from RTL when DMA write access
 */


int dpi_bank_write32_legacy(long long offset, int value)
{
    uint8_t *base = NULL;
    size_t size = 0;
    uint32_t data = (uint32_t)value;
    uint64_t addr;

    if (g_os_ram != NULL) {
        base = g_os_ram;
        size = g_os_ram_size;
    } else if (memory_bank_is_available()) {
        base = (uint8_t *)memory_bank_get_ptr(0);
        size = memory_bank_get_size();
    }

    if (offset < 0 || base == NULL) {
        fprintf(stderr, "[DMA BUS] Invalid write destination\n");
        g_dma_bus_error = 1;
        return -1;
    }

    addr = (uint64_t)offset;

    if (addr >= size || size - (size_t)addr < sizeof(data)) {
        fprintf(stderr, "[DMA BUS] Write outside memory bounds\n");
        g_dma_bus_error = 1;
        return -1;
    }

    memcpy(base + (size_t)addr, &data, sizeof(data));

    return 0;
}

/**
 * DPI Callback: Called from RTL when DMA transfer completes
 */

void dma_done_callback(void)
{
    g_dma_completion_seen = 1;

    printf("[DMA] RTL completion callback received\n");
    fflush(stdout);
}

#ifdef __cplusplus
}
#endif
