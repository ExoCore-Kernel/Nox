#include <stddef.h>
#include <stdint.h>

#include <linux/gfp.h>
#include <twilight/heap.h>

static void zero_bytes(void *pointer, size_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

void *linux_compat_kmalloc(size_t size, gfp_t flags) {
    void *pointer = kmalloc(size);
    if (pointer != 0 && (flags & __GFP_ZERO) != 0) zero_bytes(pointer, size);
    return pointer;
}

void *linux_compat_kzalloc(size_t size, gfp_t flags) {
    (void)flags;
    return kcalloc(1, size);
}

void *linux_compat_kcalloc(size_t count, size_t size, gfp_t flags) {
    (void)flags;
    return kcalloc(count, size);
}

void *linux_compat_krealloc(const void *pointer, size_t new_size, gfp_t flags) {
    const size_t old_size = pointer != 0 ? ksize(pointer) : 0;
    void *result = krealloc((void *)pointer, new_size);
    if (result != 0 && (flags & __GFP_ZERO) != 0 && new_size > old_size) {
        zero_bytes((uint8_t *)result + old_size, new_size - old_size);
    }
    return result;
}

void linux_compat_kfree(const void *pointer) {
    kfree((void *)pointer);
}

size_t linux_compat_ksize(const void *pointer) {
    return ksize(pointer);
}
