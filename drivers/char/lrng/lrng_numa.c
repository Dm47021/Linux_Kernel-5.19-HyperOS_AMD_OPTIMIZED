// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG NUMA support
 *
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/lrng.h>
#include <linux/slab.h>

#include "lrng_drng_mgr.h"
#include "lrng_es_irq.h"
#include "lrng_es_mgr.h"
#include "lrng_numa.h"
#include "lrng_proc.h"

static struct lrng_drng **lrng_drng __read_mostly = NULL;

struct lrng_drng **lrng_drng_instances(void)
{
	/* counterpart to cmpxchg_release in _lrng_drngs_numa_alloc */
	return READ_ONCE(lrng_drng);
}

/* Allocate the data structures for the per-NUMA node DRNGs */
static void _lrng_drngs_numa_alloc(struct work_struct *work)
{
	struct lrng_drng **drngs;
	struct lrng_drng *lrng_drng_init = lrng_drng_init_instance();
	u32 node;
	bool init_drng_used = false;

	mutex_lock(&lrng_crypto_cb_update);

	/* per-NUMA-node DRNGs are already present */
	if (lrng_drng)
		goto unlock;

	/* Make sure the initial DRNG is initialized and its drng_cb is set */
	if (lrng_drng_initalize())
		goto err;

	drngs = kcalloc(nr_node_ids, sizeof(void *), GFP_KERNEL|__GFP_NOFAIL);
	for_each_online_node(node) {
		struct lrng_drng *drng;

		if (!init_drng_used) {
			drngs[node] = lrng_drng_init;
			init_drng_used = true;
			continue;
		}

		drng = kmalloc_node(sizeof(struct lrng_drng),
				    GFP_KERNEL|__GFP_NOFAIL, node);
		memset(drng, 0, sizeof(lrng_drng));

		if (lrng_drng_alloc_common(drng, lrng_drng_init->drng_cb)) {
			kfree(drng);
			goto err;
		}

		drng->hash_cb = lrng_drng_init->hash_cb;
		drng->hash = lrng_drng_init->hash_cb->hash_alloc();
		if (IS_ERR(drng->hash)) {
			lrng_drng_init->drng_cb->drng_dealloc(drng->drng);
			kfree(drng);
			goto err;
		}

		mutex_init(&drng->lock);
		rwlock_init(&drng->hash_lock);

		/*
		 * No reseeding of NUMA DRNGs from previous DRNGs as this
		 * would complicate the code. Let it simply reseed.
		 */
		drngs[node] = drng;

		lrng_pool_inc_numa_node();
		pr_info("DRNG and entropy pool read hash for NUMA node %d allocated\n",
			node);
	}

	/* counterpart to READ_ONCE in lrng_drng_instances */
	if (!cmpxchg_release(&lrng_drng, NULL, drngs)) {
		lrng_pool_all_numa_nodes_seeded(false);
		goto unlock;
	}

err:
	for_each_online_node(node) {
		struct lrng_drng *drng = drngs[node];

		if (drng == lrng_drng_init)
			continue;

		if (drng) {
			drng->hash_cb->hash_dealloc(drng->hash);
			drng->drng_cb->drng_dealloc(drng->drng);
			kfree(drng);
		}
	}
	kfree(drngs);

unlock:
	mutex_unlock(&lrng_crypto_cb_update);
}

static DECLARE_WORK(lrng_drngs_numa_alloc_work, _lrng_drngs_numa_alloc);

static void lrng_drngs_numa_alloc(void)
{
	schedule_work(&lrng_drngs_numa_alloc_work);
}

static int __init lrng_numa_init(void)
{
	lrng_drngs_numa_alloc();
	return 0;
}

late_initcall(lrng_numa_init);
