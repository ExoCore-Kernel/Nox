#pragma once

struct completion { volatile unsigned int done; };

#define DECLARE_COMPLETION(name) struct completion name = { .done = 0u }
static inline void init_completion(struct completion *completion) {
    if (completion != 0) completion->done = 0u;
}
static inline void complete(struct completion *completion) {
    if (completion != 0) completion->done = 1u;
}
static inline void wait_for_completion(struct completion *completion) {
    if (completion == 0) return;
    while (completion->done == 0u) __asm__ volatile ("pause");
}
