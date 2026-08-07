#pragma once

#include <linux/types.h>

/*
 * Twilight's current heap never sleeps, so GFP_KERNEL and GFP_ATOMIC share
 * the same allocator today. Keep the distinction in driver source now so the
 * scheduler-aware implementation can enforce it later without driver rewrites.
 */
#define __GFP_ZERO  (1u << 0)
#define GFP_KERNEL  ((gfp_t)0u)
#define GFP_ATOMIC  ((gfp_t)(1u << 1))
#define GFP_DMA     ((gfp_t)(1u << 2))
#define GFP_DMA32   ((gfp_t)(1u << 3))
