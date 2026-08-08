#pragma once

#include <stdbool.h>
#include <stdint.h>

/* An interactive BusyBox boot is a normal OS boot, not the bring-up test
 * harness. Keep the diagnostic tests available for explicit test targets, but
 * suppress them when TWILIGHT_BUSYBOX_SELF_TEST selects the shell image. The
 * initialization itself still runs and remains fail-closed on real errors. */
#if defined(TWILIGHT_BUSYBOX_SELF_TEST) && TWILIGHT_BUSYBOX_SELF_TEST
#ifdef TWILIGHT_PMM_SELF_TEST
#undef TWILIGHT_PMM_SELF_TEST
#endif
#define TWILIGHT_PMM_SELF_TEST 0

#ifdef TWILIGHT_VMM_SELF_TEST
#undef TWILIGHT_VMM_SELF_TEST
#endif
#define TWILIGHT_VMM_SELF_TEST 0

#ifdef TWILIGHT_HEAP_SELF_TEST
#undef TWILIGHT_HEAP_SELF_TEST
#endif
#define TWILIGHT_HEAP_SELF_TEST 0

#ifdef TWILIGHT_USERMODE_SELF_TEST
#undef TWILIGHT_USERMODE_SELF_TEST
#endif
#define TWILIGHT_USERMODE_SELF_TEST 0

#ifdef TWILIGHT_LINUX_USER_SELF_TEST
#undef TWILIGHT_LINUX_USER_SELF_TEST
#endif
#define TWILIGHT_LINUX_USER_SELF_TEST 0

#ifdef TWILIGHT_LINUX_COMPAT_SELF_TEST
#undef TWILIGHT_LINUX_COMPAT_SELF_TEST
#endif
#define TWILIGHT_LINUX_COMPAT_SELF_TEST 0

#ifdef TWILIGHT_SCROLL_SELF_TEST
#undef TWILIGHT_SCROLL_SELF_TEST
#endif
#define TWILIGHT_SCROLL_SELF_TEST 0
#endif

bool user_mode_is_available(void);
bool user_mode_self_test(void);
bool linux_user_self_test(void);

/* Entry point used by the DPL3 int 0x80 assembly stub. */
int user_syscall_dispatch(uint64_t syscall_number);

/* Linux x86-64 syscall dispatcher used by the SYSCALL entry stub. */
int64_t linux_syscall_dispatch(uint64_t syscall_number,
                               uint64_t arg1,
                               uint64_t arg2,
                               uint64_t arg3,
                               uint64_t arg4,
                               uint64_t arg5,
                               uint64_t arg6);
