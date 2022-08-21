// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Backend for the LRNG providing the cryptographic primitives using the
 * kernel crypto API.
 *
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/lrng.h>
#include <crypto/hash.h>
#include <crypto/rng.h>
#include <linux/init.h>
#include <linux/module.h>

#include "lrng_drng_kcapi.h"

static char *drng_name = NULL;
module_param(drng_name, charp, 0444);
MODULE_PARM_DESC(drng_name, "Kernel crypto API name of DRNG");

static char *seed_hash = NULL;
module_param(seed_hash, charp, 0444);
MODULE_PARM_DESC(seed_hash,
		 "Kernel crypto API name of hash with output size equal to seedsize of DRNG to bring seed string to the size required by the DRNG");

struct lrng_drng_info {
	struct crypto_rng *kcapi_rng;
	struct crypto_shash *hash_tfm;
};

static int lrng_kcapi_drng_seed_helper(void *drng, const u8 *inbuf,
				       u32 inbuflen)
{
	struct lrng_drng_info *lrng_drng_info = (struct lrng_drng_info *)drng;
	struct crypto_rng *kcapi_rng = lrng_drng_info->kcapi_rng;
	struct crypto_shash *hash_tfm = lrng_drng_info->hash_tfm;
	SHASH_DESC_ON_STACK(shash, hash_tfm);
	u32 digestsize;
	u8 digest[HASH_MAX_DIGESTSIZE] __aligned(8);
	int ret;

	if (!hash_tfm)
		return crypto_rng_reset(kcapi_rng, inbuf, inbuflen);

	shash->tfm = hash_tfm;
	digestsize = crypto_shash_digestsize(hash_tfm);

	ret = crypto_shash_digest(shash, inbuf, inbuflen, digest);
	shash_desc_zero(shash);
	if (ret)
		return ret;

	ret = crypto_rng_reset(kcapi_rng, digest, digestsize);
	if (ret)
		return ret;

	memzero_explicit(digest, digestsize);
	return 0;
}

static int lrng_kcapi_drng_generate_helper(void *drng, u8 *outbuf,
					   u32 outbuflen)
{
	struct lrng_drng_info *lrng_drng_info = (struct lrng_drng_info *)drng;
	struct crypto_rng *kcapi_rng = lrng_drng_info->kcapi_rng;
	int ret = crypto_rng_get_bytes(kcapi_rng, outbuf, outbuflen);

	if (ret < 0)
		return ret;

	return outbuflen;
}

static void *lrng_kcapi_drng_alloc(u32 sec_strength)
{
	struct lrng_drng_info *lrng_drng_info;
	struct crypto_rng *kcapi_rng;
	u32 time = random_get_entropy();
	int seedsize, rv;
	void *ret =  ERR_PTR(-ENOMEM);

	if (!drng_name) {
		pr_err("DRNG name missing\n");
		return ERR_PTR(-EINVAL);
	}

	if (!memcmp(drng_name, "stdrng", 6) ||
	    !memcmp(drng_name, "lrng", 4) ||
	    !memcmp(drng_name, "drbg", 4) ||
	    !memcmp(drng_name, "jitterentropy_rng", 17)) {
		pr_err("Refusing to load the requested random number generator\n");
		return ERR_PTR(-EINVAL);
	}

	lrng_drng_info = kzalloc(sizeof(*lrng_drng_info), GFP_KERNEL);
	if (!lrng_drng_info)
		return ERR_PTR(-ENOMEM);

	kcapi_rng = crypto_alloc_rng(drng_name, 0, 0);
	if (IS_ERR(kcapi_rng)) {
		pr_err("DRNG %s cannot be allocated\n", drng_name);
		ret = ERR_CAST(kcapi_rng);
		goto free;
	}

	lrng_drng_info->kcapi_rng = kcapi_rng;

	seedsize = crypto_rng_seedsize(kcapi_rng);
	if (seedsize) {
		struct crypto_shash *hash_tfm;

		if (!seed_hash) {
			switch (seedsize) {
			case 32:
				seed_hash = "sha256";
				break;
			case 48:
				seed_hash = "sha384";
				break;
			case 64:
				seed_hash = "sha512";
				break;
			default:
				pr_err("Seed size %d cannot be processed\n",
				       seedsize);
				goto dealloc;
			}
		}

		hash_tfm = crypto_alloc_shash(seed_hash, 0, 0);
		if (IS_ERR(hash_tfm)) {
			ret = ERR_CAST(hash_tfm);
			goto dealloc;
		}

		if (seedsize != crypto_shash_digestsize(hash_tfm)) {
			pr_err("Seed hash output size not equal to DRNG seed size\n");
			crypto_free_shash(hash_tfm);
			ret = ERR_PTR(-EINVAL);
			goto dealloc;
		}

		lrng_drng_info->hash_tfm = hash_tfm;

		pr_info("Seed hash %s allocated\n", seed_hash);
	}

	rv = lrng_kcapi_drng_seed_helper(lrng_drng_info, (u8 *)(&time),
					 sizeof(time));
	if (rv) {
		ret = ERR_PTR(rv);
		goto dealloc;
	}

	pr_info("Kernel crypto API DRNG %s allocated\n", drng_name);

	return lrng_drng_info;

dealloc:
	crypto_free_rng(kcapi_rng);
free:
	kfree(lrng_drng_info);
	return ret;
}

static void lrng_kcapi_drng_dealloc(void *drng)
{
	struct lrng_drng_info *lrng_drng_info = (struct lrng_drng_info *)drng;
	struct crypto_rng *kcapi_rng = lrng_drng_info->kcapi_rng;

	crypto_free_rng(kcapi_rng);
	if (lrng_drng_info->hash_tfm)
		crypto_free_shash(lrng_drng_info->hash_tfm);
	kfree(lrng_drng_info);
	pr_info("DRNG %s deallocated\n", drng_name);
}

static const char *lrng_kcapi_drng_name(void)
{
	return drng_name;
}

const struct lrng_drng_cb lrng_kcapi_drng_cb = {
	.drng_name	= lrng_kcapi_drng_name,
	.drng_alloc	= lrng_kcapi_drng_alloc,
	.drng_dealloc	= lrng_kcapi_drng_dealloc,
	.drng_seed	= lrng_kcapi_drng_seed_helper,
	.drng_generate	= lrng_kcapi_drng_generate_helper,
};

#ifndef CONFIG_LRNG_DFLT_DRNG_KCAPI
static int __init lrng_kcapi_init(void)
{
	return lrng_set_drng_cb(&lrng_kcapi_drng_cb);
}
static void __exit lrng_kcapi_exit(void)
{
	lrng_set_drng_cb(NULL);
}

late_initcall(lrng_kcapi_init);
module_exit(lrng_kcapi_exit);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Entropy Source and DRNG Manager - kernel crypto API DRNG backend");
#endif /* CONFIG_LRNG_DFLT_DRNG_KCAPI */
