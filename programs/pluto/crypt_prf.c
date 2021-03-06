/*
 * PRF helper functions, for libreswan
 *
 * Copyright (C) 2007-2008 Michael C. Richardson <mcr@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2009-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2009-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012 Wes Hardaker <opensource@hardakers.net>
 * Copyright (C) 2013 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2015 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2015 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stdlib.h>

//#include "libreswan.h"
#include "lswalloc.h"
#include "lswlog.h"
#include "ike_alg.h"
#include "crypt_prf.h"
#include "crypt_symkey.h"
#include "crypt_dbg.h"
#include "crypto.h"

struct crypt_prf {
	struct prf_context *context;
	const struct prf_ops *ops;
};

static struct crypt_prf *wrap(const struct prf_desc *prf_desc, const char *name,
			      struct prf_context *context)
{
	struct crypt_prf *prf = alloc_thing(struct crypt_prf, name);
	*prf = (struct crypt_prf) {
		.context = context,
		.ops = prf_desc->prf_ops,
	};
	return prf;
}

struct crypt_prf *crypt_prf_init_chunk(const char *name, lset_t debug,
				       const struct prf_desc *prf_desc,
				       const char *key_name, chunk_t key)
{
	return wrap(prf_desc, name,
		    prf_desc->prf_ops->init_bytes(prf_desc, name, debug,
						  key_name, key.ptr, key.len));
}

struct crypt_prf *crypt_prf_init_symkey(const char *name, lset_t debug,
					const struct prf_desc *prf_desc,
					const char *key_name, PK11SymKey *key)
{
	return wrap(prf_desc, name,
		    prf_desc->prf_ops->init_symkey(prf_desc, name, debug,
						   key_name, key));
}

/*
 * Accumulate data.
 */

void crypt_prf_update_chunk(const char *name, struct crypt_prf *prf,
			    chunk_t update)
{
	prf->ops->digest_bytes(prf->context, name, update.ptr, update.len);
}

void crypt_prf_update_symkey(const char *name, struct crypt_prf *prf,
			     PK11SymKey *update)
{
	prf->ops->digest_symkey(prf->context, name, update);
}

void crypt_prf_update_byte(const char *name, struct crypt_prf *prf,
			   uint8_t update)
{
	prf->ops->digest_bytes(prf->context, name, &update, 1);
}

void crypt_prf_update_bytes(const char *name, struct crypt_prf *prf,
			    const void *update, size_t sizeof_update)
{
	prf->ops->digest_bytes(prf->context, name, update, sizeof_update);
}

PK11SymKey *crypt_prf_final_symkey(struct crypt_prf **prfp)
{
	PK11SymKey *tmp = (*prfp)->ops->final_symkey(&(*prfp)->context);
	pfree(*prfp);
	*prfp = NULL;
	return tmp;
}

void crypt_prf_final_bytes(struct crypt_prf **prfp,
			   void *bytes, size_t sizeof_bytes)
{
	(*prfp)->ops->final_bytes(&(*prfp)->context, bytes, sizeof_bytes);
	pfree(*prfp);
	*prfp = NULL;
}
