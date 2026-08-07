#pragma once

#include <stddef.h>
#include <linux/gfp.h>

void *linux_compat_kmalloc(size_t size, gfp_t flags);
void *linux_compat_kzalloc(size_t size, gfp_t flags);
void *linux_compat_kcalloc(size_t count, size_t size, gfp_t flags);
void *linux_compat_krealloc(const void *pointer, size_t new_size, gfp_t flags);
void linux_compat_kfree(const void *pointer);
size_t linux_compat_ksize(const void *pointer);

#define kmalloc(size, flags) linux_compat_kmalloc((size), (flags))
#define kzalloc(size, flags) linux_compat_kzalloc((size), (flags))
#define kcalloc(count, size, flags) linux_compat_kcalloc((count), (size), (flags))
#define krealloc(pointer, size, flags) linux_compat_krealloc((pointer), (size), (flags))
#define kfree(pointer) linux_compat_kfree((pointer))
#define ksize(pointer) linux_compat_ksize((pointer))
