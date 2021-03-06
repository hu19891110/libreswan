/*
 * SYMKEY manipulation functions, for libreswan
 *
 * Copyright (C) 2015, 2016 Andrew Cagney <cagney@gnu.org>
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

#include "libreswan.h"
#include "lswalloc.h"
#include "lswlog.h"
#include "ike_alg.h"
#include "crypt_symkey.h"
#ifndef FIPS_CHECK
#include "crypt_dbg.h"
#endif
#include "crypto.h"
#include "lswfips.h"
#include "lswnss.h"


static CK_MECHANISM_TYPE nss_key_derivation_mech(const struct hash_desc *hasher)
{
	CK_MECHANISM_TYPE mechanism = 0x80000000;

	switch (hasher->common.ikev1_oakley_id) {
	case OAKLEY_MD5:
		mechanism = CKM_MD5_KEY_DERIVATION;
		break;
	case OAKLEY_SHA1:
		mechanism = CKM_SHA1_KEY_DERIVATION;
		break;
	case OAKLEY_SHA2_256:
		mechanism = CKM_SHA256_KEY_DERIVATION;
		break;
	case OAKLEY_SHA2_384:
		mechanism = CKM_SHA384_KEY_DERIVATION;
		break;
	case OAKLEY_SHA2_512:
		mechanism = CKM_SHA512_KEY_DERIVATION;
		break;
	default:
		DBG(DBG_CRYPT,
		    DBG_log("NSS: key derivation mechanism not supported"));
		break;
	}
	return mechanism;
}

struct nss_alg {
	CK_FLAGS flags;
	CK_MECHANISM_TYPE mechanism;
};

static struct nss_alg nss_alg(const char *verb, const char *name, lset_t debug,
			      const struct ike_alg *alg)
{
	/*
	 * NSS expects a key's mechanism to match the NSS algorithm
	 * the key is intended for.  If this is wrong then the
	 * operation fails.
	 *
	 * Unfortunately, some algorithms are not implemented by NSS,
	 * so the correct key type can't always be specified.  For
	 * those specify CKM_VENDOR_DEFINED.
	 */
	CK_FLAGS flags;
	CK_MECHANISM_TYPE mechanism;
	if (alg == NULL) {
		/*
		 * Something of an old code hack.  Keys fed to the
		 * hasher get this type.
		 */
		mechanism = CKM_EXTRACT_KEY_FROM_KEY;
		flags = 0;
		if (DBGP(debug)) {
			DBG_log("%s %s for non-NSS algorithm: NULL (legacy hack), mechanism: %s(%lu), flags: %lx",
				verb, name,
				lsw_nss_ckm_to_string(mechanism), mechanism,
				flags);
		}
	} else if (alg->nss_mechanism == 0) {
		/*
		 * A non-NSS algorithm.  The values shouldn't matter.
		 */
		mechanism = CKM_VENDOR_DEFINED;
		flags = 0;
		if (DBGP(debug)) {
			DBG_log("%s %s for non-NSS algorithm: %s, mechanism: %s(%lu), flags: %lx",
				verb, name, alg->name,
				lsw_nss_ckm_to_string(mechanism), mechanism,
				flags);
		}
	} else {
		mechanism = alg->nss_mechanism;
		switch (alg->algo_type) {
		case IKE_ALG_ENCRYPT:
			flags = CKF_ENCRYPT | CKF_DECRYPT;
			break;
		case IKE_ALG_PRF:
		case IKE_ALG_INTEG:
			flags = CKF_SIGN;
			break;
		case IKE_ALG_HASH:
			flags = CKF_DIGEST;
			break;
		default:
			flags = 0;
			/* crypto will likely fail */
			PEXPECT_LOG("algorithm type %d unknown", alg->algo_type);
		}
		if (DBGP(debug)) {
			DBG_log("%s %s for NSS algorithm: %s, mechanism: %s(%lu), flags: %lx",
				verb, name, alg->name,
				lsw_nss_ckm_to_string(mechanism), mechanism,
				flags);
		}
	}
	return (struct nss_alg) {
		.mechanism = mechanism,
		.flags = flags,
	};
}

static PK11SymKey *ephemeral_symkey(int debug)
{
	static int tried;
	static PK11SymKey *ephemeral_key;
	if (!tried) {
		tried = 1;
		/* get a secret key */
		PK11SlotInfo *slot = PK11_GetBestSlot(CKM_AES_KEY_GEN,
						      lsw_return_nss_password_file_info());
		if (slot == NULL) {
			loglog(RC_LOG_SERIOUS, "NSS: ephemeral slot error");
			return NULL;
		}
		ephemeral_key = PK11_KeyGen(slot, CKM_AES_KEY_GEN,
					    NULL, 128/8, NULL);
		PK11_FreeSlot(slot); /* reference counted */
	}
	DBG(debug, DBG_symkey("ephemeral_key", ephemeral_key));
	return ephemeral_key;
}

void free_any_symkey(const char *prefix, PK11SymKey **key)
{
	if (*key != NULL) {
		DBG(DBG_CRYPT, DBG_log("%s: free key %p", prefix, *key));
		PK11_FreeSymKey(*key);
	} else {
		DBG(DBG_CRYPT, DBG_log("%s: free key NULL", prefix));
	}
	*key = NULL;
}

size_t sizeof_symkey(PK11SymKey *key)
{
	if (key == NULL) {
		return 0;
	} else {
		return PK11_GetKeyLength(key);
	}
}

void DBG_symkey(const char *prefix, PK11SymKey *key)
{
	if (key == NULL) {
		/*
		 * For instance, when a zero-length key gets extracted
		 * from an existing key.
		 */
		DBG_log("%s: key=NULL", prefix);
	} else {
		DBG_log("%s: key@%p, size: %zd bytes, type/mechanism: %s (0x%08x)",
			prefix, key, sizeof_symkey(key),
			lsw_nss_ckm_to_string(PK11_GetMechanism(key)),
			(int)PK11_GetMechanism(key));
	}
}

/*
 * Merge a symkey and an array of bytes into a new SYMKEY.
 *
 * derive: the operation that is to be performed; target: the
 * mechanism/type of the resulting symkey.
 */
static PK11SymKey *merge_symkey_bytes(const char *prefix,
				      PK11SymKey *base_key,
				      const void *bytes, size_t sizeof_bytes,
				      CK_MECHANISM_TYPE derive,
				      CK_MECHANISM_TYPE target)
{
	passert(sizeof_bytes > 0);
	CK_KEY_DERIVATION_STRING_DATA string = {
		.pData = (void *)bytes,
		.ulLen = sizeof_bytes,
	};
	SECItem data_param = {
		.data = (unsigned char*)&string,
		.len = sizeof(string),
	};
	CK_ATTRIBUTE_TYPE operation = CKA_DERIVE;
	int key_size = 0;

	DBG(DBG_CRYPT,
	    DBG_log("%s merge symkey(%p) bytes(%p/%zd) - derive(%s) target(%s)",
		    prefix,
		    base_key, bytes, sizeof_bytes,
		    lsw_nss_ckm_to_string(derive),
		    lsw_nss_ckm_to_string(target));
	    DBG_symkey("symkey", base_key);
	    DBG_dump("bytes:", bytes, sizeof_bytes));
	PK11SymKey *result = PK11_Derive(base_key, derive, &data_param, target,
					 operation, key_size);
	DBG(DBG_CRYPT, DBG_symkey(prefix, result))
	return result;
}

/*
 * Merge two SYMKEYs into a new SYMKEY.
 *
 * derive: the operation to be performed; target: the mechanism/type
 * of the resulting symkey.
 */

static PK11SymKey *merge_symkey_symkey(const char *prefix,
				       PK11SymKey *base_key, PK11SymKey *key,
				       CK_MECHANISM_TYPE derive,
				       CK_MECHANISM_TYPE target)
{
	CK_OBJECT_HANDLE key_handle = PK11_GetSymKeyHandle(key);
	SECItem key_param = {
		.data = (unsigned char*)&key_handle,
		.len = sizeof(key_handle)
	};
	CK_ATTRIBUTE_TYPE operation = CKA_DERIVE;
	int key_size = 0;
	DBG(DBG_CRYPT,
	    DBG_log("%s merge symkey(1: %p) symkey(2: %p) - derive(%s) target(%s)",
		    prefix, base_key, key,
		    lsw_nss_ckm_to_string(derive),
		    lsw_nss_ckm_to_string(target));
	    DBG_symkey("symkey 1", base_key);
	    DBG_symkey("symkey 2", key));
	PK11SymKey *result = PK11_Derive(base_key, derive, &key_param, target,
					 operation, key_size);
	DBG(DBG_CRYPT, DBG_symkey(prefix, result));
	return result;
}

/*
 * Extract a SYMKEY from an existing SYMKEY.
 */
static PK11SymKey *symkey_from_symkey(const char *prefix,
				      PK11SymKey *base_key,
				      CK_MECHANISM_TYPE target,
				      CK_FLAGS flags,
				      size_t next_byte, size_t key_size)
{
	/* spell out all the parameters */
	CK_EXTRACT_PARAMS bs = next_byte * BITS_PER_BYTE;
	SECItem param = {
		.data = (unsigned char*)&bs,
		.len = sizeof(bs),
	};
	CK_MECHANISM_TYPE derive = CKM_EXTRACT_KEY_FROM_KEY;
	CK_ATTRIBUTE_TYPE operation = CKA_FLAGS_ONLY;

	DBG(DBG_CRYPT,
	    DBG_log("%s symkey from symkey(%p) - next-byte(%zd) key-size(%zd) flags(0x%lx) derive(%s) target(%s)",
		    prefix, base_key, next_byte, key_size, (long)flags,
		    lsw_nss_ckm_to_string(derive), lsw_nss_ckm_to_string(target));
	    DBG_symkey("symkey", base_key));
	PK11SymKey *result = PK11_DeriveWithFlags(base_key, derive, &param,
						  target, operation,
						  key_size, flags);
	DBG(DBG_CRYPT, DBG_symkey(prefix, result));
	return result;
}


/*
 * For on-wire algorithms.
 */
chunk_t chunk_from_symkey(const char *name, lset_t debug,
			  PK11SymKey *symkey)
{
	SECStatus status;
	if (symkey == NULL) {
		DBG(debug, DBG_log("%s NULL key has no bytes", name));
		return empty_chunk;
	}

	size_t sizeof_bytes = sizeof_symkey(symkey);
	DBG(debug, DBG_log("%s extracting all %zd bytes of symkey %p",
			     name, sizeof_bytes, symkey));
	DBG(debug, DBG_symkey("symkey", symkey));

	/* get a secret key */
	PK11SymKey *ephemeral_key = ephemeral_symkey(debug);
	if (ephemeral_key == NULL) {
		loglog(RC_LOG_SERIOUS, "%s NSS: ephemeral error", name);
		return empty_chunk;
	}

	/* copy the source key to the secret slot */
	PK11SymKey *slot_key;
	{
		PK11SlotInfo *slot = PK11_GetSlotFromKey(ephemeral_key);
		slot_key = PK11_MoveSymKey(slot, CKA_UNWRAP, 0, 0, symkey);
		PK11_FreeSlot(slot); /* reference counted */
		if (slot_key == NULL) {
			loglog(RC_LOG_SERIOUS, "%s NSS: slot error", name);
			return empty_chunk;
		}
	}
	DBG(debug, DBG_symkey("slot_key", slot_key));

	SECItem wrapped_key;
	/* Round up the wrapped key length to a 16-byte boundary.  */
	wrapped_key.len = (sizeof_bytes + 15) & ~15;
	wrapped_key.data = alloc_bytes(wrapped_key.len, name);
	DBG(debug, DBG_log("sizeof bytes %d", wrapped_key.len));
	status = PK11_WrapSymKey(CKM_AES_ECB, NULL, ephemeral_key, slot_key,
				 &wrapped_key);
	if (status != SECSuccess) {
		loglog(RC_LOG_SERIOUS, "%s NSS: containment error (%d)",
		       name, status);
		pfreeany(wrapped_key.data);
		free_any_symkey("slot_key:", &slot_key);
		return empty_chunk;
	}
	DBG(debug, DBG_dump("wrapper:", wrapped_key.data, wrapped_key.len));

	void *bytes = alloc_bytes(wrapped_key.len, name);
	unsigned int out_len = 0;
	status = PK11_Decrypt(ephemeral_key, CKM_AES_ECB, NULL,
			      bytes, &out_len, wrapped_key.len,
			      wrapped_key.data, wrapped_key.len);
	pfreeany(wrapped_key.data);
	free_any_symkey("slot_key:", &slot_key);
	if (status != SECSuccess) {
		loglog(RC_LOG_SERIOUS, "%s NSS: error calculating contents (%d)",
		       name, status);
		return empty_chunk;
	}
	passert(out_len >= sizeof_bytes);

	DBG(debug, DBG_log("%s extracted len %d bytes at %p", name, out_len, bytes));
	DBG(debug, DBG_dump("unwrapped:", bytes, out_len));

	return (chunk_t) {
		.ptr = bytes,
		.len = sizeof_bytes,
	};
}

/*
 * SYMKEY I/O operations.
 */

PK11SymKey *symkey_from_bytes(const char *name, lset_t debug,
			      const struct ike_alg *alg,
			      const u_int8_t *bytes, size_t sizeof_bytes)
{
	PK11SymKey *scratch = ephemeral_symkey(debug);
	PK11SymKey *tmp = merge_symkey_bytes("symkey_from_bytes",
					     scratch, bytes, sizeof_bytes,
					     CKM_CONCATENATE_DATA_AND_BASE,
					     CKM_EXTRACT_KEY_FROM_KEY);
	passert(tmp != NULL);
	PK11SymKey *key = symkey_from_symkey_bytes(name, debug, alg,
						   0, sizeof_bytes, tmp);
	passert(key != NULL);
	free_any_symkey(__func__, &tmp);
	return key;
}

PK11SymKey *symkey_from_chunk(const char *name, lset_t debug,
			      const struct ike_alg *alg,
			      chunk_t chunk)
{
	return symkey_from_bytes(name, debug, alg,
				 chunk.ptr, chunk.len);
}

/*
 * Concatenate two pieces of keying material creating a
 * new SYMKEY object.
 */

PK11SymKey *concat_symkey_symkey(const struct hash_desc *hasher,
				 PK11SymKey *lhs, PK11SymKey *rhs)
{
	return merge_symkey_symkey("concat:", lhs, rhs,
				   CKM_CONCATENATE_BASE_AND_KEY,
				   nss_key_derivation_mech(hasher));
}

PK11SymKey *concat_symkey_bytes(const struct hash_desc *hasher,
				PK11SymKey *lhs, const void *rhs,
				size_t sizeof_rhs)
{
	CK_MECHANISM_TYPE mechanism = nss_key_derivation_mech(hasher);
	return merge_symkey_bytes("concat_symkey_bytes",
				  lhs, rhs, sizeof_rhs,
				  CKM_CONCATENATE_BASE_AND_DATA,
				  mechanism);
}

PK11SymKey *concat_symkey_chunk(const struct hash_desc *hasher,
				PK11SymKey *lhs, chunk_t rhs)
{
	return concat_symkey_bytes(hasher, lhs, rhs.ptr, rhs.len);
}

PK11SymKey *concat_symkey_byte(const struct hash_desc *hasher,
			       PK11SymKey *lhs, uint8_t rhs)
{
	return concat_symkey_bytes(hasher, lhs, &rhs, sizeof(rhs));
}

chunk_t concat_chunk_chunk(const char *name, chunk_t lhs, chunk_t rhs)
{
	size_t len = lhs.len + rhs.len;
	chunk_t cat = {
		.len = len,
		.ptr = alloc_things(u_int8_t, len, name),
	};
	memcpy(cat.ptr, lhs.ptr, lhs.len);
	memcpy(cat.ptr + lhs.len, rhs.ptr, rhs.len);
	return cat;
}

/*
 * Append new keying material to an existing key; replace the existing
 * key with the result.
 *
 * Use this to chain a series of concat operations.
 */

void append_symkey_symkey(const struct hash_desc *hasher,
			  PK11SymKey **lhs, PK11SymKey *rhs)
{
	PK11SymKey *newkey = concat_symkey_symkey(hasher, *lhs, rhs);
	free_any_symkey(__func__, lhs);
	*lhs = newkey;
}

void append_symkey_bytes(const struct hash_desc *hasher,
			 PK11SymKey **lhs, const void *rhs,
			 size_t sizeof_rhs)
{
	PK11SymKey *newkey = concat_symkey_bytes(hasher, *lhs,
						 rhs, sizeof_rhs);
	free_any_symkey(__func__, lhs);
	*lhs = newkey;
}

void append_symkey_chunk(const struct hash_desc *hasher,
			 PK11SymKey **lhs, chunk_t rhs)
{
	append_symkey_bytes(hasher, lhs, rhs.ptr, rhs.len);
}

void append_symkey_byte(const struct hash_desc *hasher,
			PK11SymKey **lhs, uint8_t rhs)
{
	append_symkey_bytes(hasher, lhs, &rhs, sizeof(rhs));
}

void append_chunk_chunk(const char *name, chunk_t *lhs, chunk_t rhs)
{
	chunk_t new = concat_chunk_chunk(name, *lhs, rhs);
	freeanychunk(*lhs);
	*lhs = new;
}

/*
 * Extract SIZEOF_SYMKEY bytes of keying material as an ENCRYPTER key
 * (i.e., can be used to encrypt/decrypt data using ENCRYPTER).
 *
 * Offset into the SYMKEY is in BYTES.
 */

PK11SymKey *symkey_from_symkey_bytes(const char *name, lset_t debug,
				     const struct ike_alg *symkey_alg,
				     size_t symkey_start_byte, size_t sizeof_symkey,
				     PK11SymKey *source_key)
{
	/*
	 * NSS expects a key's mechanism to match the NSS algorithm
	 * the key is intended for.  If this is wrong then the
	 * operation fails.
	 *
	 * Unfortunately, some algorithms are not implemented by NSS,
	 * so the correct key type can't always be specified.  For
	 * those specify CKM_VENDOR_DEFINED.
	 */
	struct nss_alg nss = nss_alg("extract symkey", name, debug, symkey_alg);
	return symkey_from_symkey(name, source_key,
				  nss.mechanism, nss.flags,
				  symkey_start_byte, sizeof_symkey);
}

PK11SymKey *key_from_symkey_bytes(PK11SymKey *source_key,
				  size_t next_byte, size_t sizeof_key)
{
	return symkey_from_symkey("key:", source_key, CKM_EXTRACT_KEY_FROM_KEY,
				  0, next_byte, sizeof_key);
}

/*
 * XOR a symkey with a chunk.
 *
 * XXX: hmac.c had very similar code, only, instead of
 * target=CKM_CONCATENATE_BASE_AND_DATA it used
 * target=hasher-to-ckm(hasher).
 *
 * hasher-to-ckm maped hasher->common.alg_id to CMK vis: OAKLEY_MD5 ->
 * CKM_MD5; OAKLEY_SHA1 -> CKM_SHA_1; OAKLEY_SHA2_256 -> CKM_SHA256;
 * OAKLEY_SHA2_384 -> CKM_SHA384; OAKLEY_SHA2_512 -> CKM_SHA512; only
 * in the default case it would set target to 0x80000000????
 */
PK11SymKey *xor_symkey_chunk(PK11SymKey *lhs, chunk_t rhs)
{
	return merge_symkey_bytes("xor_symkey_chunk", lhs, rhs.ptr, rhs.len,
				  CKM_XOR_BASE_AND_DATA,
				  CKM_CONCATENATE_BASE_AND_DATA);
}
