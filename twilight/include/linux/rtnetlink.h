#pragma once

/* Twilight is currently single-CPU during driver bring-up. These preserve the
 * Linux locking boundary and can become a real RTNL mutex with SMP/scheduler. */
static inline void rtnl_lock(void) {}
static inline void rtnl_unlock(void) {}
static inline int rtnl_lock_interruptible(void) { return 0; }
