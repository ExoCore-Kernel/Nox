#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct heap_stats {
    uint64_t active_allocations;
    uint64_t allocated_bytes;
    uint64_t mapped_pages;
    uint64_t slab_pages;
    uint64_t large_allocations;
    uint64_t large_pages;
    uint64_t invalid_frees;
    uint64_t arena_bytes;
};

bool heap_init(void);
bool heap_is_initialized(void);

void *kmalloc(size_t size);
void kfree(void *pointer);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *pointer, size_t new_size);
size_t ksize(const void *pointer);

void heap_get_stats(struct heap_stats *out);
bool heap_self_test(void);
