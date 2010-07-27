/* -*- c-basic-offset: 8 -*-
   FreeRDP: A Remote Desktop Protocol client.
   OpenSSL Cryptographic Abstraction Layer

   Copyright (C) Marc-Andre Moreau <marcandre.moreau@gmail.com> 2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "frdp.h"
#include "crypto.h"
#include "mem.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rc4.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/bn.h>
#include <openssl/x509v3.h>

#if defined(OPENSSL_VERSION_NUMBER) && (OPENSSL_VERSION_NUMBER >= 0x0090800f)
#define D2I_X509_CONST const
#else
#define D2I_X509_CONST
#endif

struct crypto_sha1_struct
{
	SHA_CTX sha_ctx;
};

CryptoSha1
crypto_sha1_init(void)
{
	CryptoSha1 sha1 = xmalloc(sizeof(*sha1));
	SHA1_Init(&sha1->sha_ctx);
	return sha1;
}

void
crypto_sha1_update(CryptoSha1 sha1, uint8 * data, uint32 len)
{
	SHA1_Update(&sha1->sha_ctx, data, len);
}

void
crypto_sha1_final(CryptoSha1 sha1, uint8 * out_data)
{
	SHA1_Final(out_data, &sha1->sha_ctx);
	xfree(sha1);
}

struct crypto_md5_struct
{
	MD5_CTX md5_ctx;
};

CryptoMd5
crypto_md5_init(void)
{
	CryptoMd5 md5 = xmalloc(sizeof(*md5));
	MD5_Init(&md5->md5_ctx);
	return md5;
}

void
crypto_md5_update(CryptoMd5 md5, uint8 * data, uint32 len)
{
	MD5_Update(&md5->md5_ctx, data, len);
}

void
crypto_md5_final(CryptoMd5 md5, uint8 * out_data)
{
	MD5_Final(out_data, &md5->md5_ctx);
	xfree(md5);
}

struct crypto_rc4_struct
{
	RC4_KEY rc4_key;
};

CryptoRc4
crypto_rc4_init(uint8 * key, uint32 len)
{
	CryptoRc4 rc4 = xmalloc(sizeof(*rc4));
	RC4_set_key(&rc4->rc4_key, len, key);
	return rc4;
}

void
crypto_rc4(CryptoRc4 rc4, uint32 len, uint8 * in_data, uint8 * out_data)
{
	RC4(&rc4->rc4_key, len, in_data, out_data);
}

void
crypto_rc4_free(CryptoRc4 rc4)
{
	xfree(rc4);
}

static void
reverse(uint8 * p, int len)
{
	int i, j;
	uint8 temp;

	for (i = 0, j = len - 1; i < j; i++, j--)
	{
		temp = p[i];
		p[i] = p[j];
		p[j] = temp;
	}
}

void
crypto_rsa_encrypt(int len, uint8 * in, uint8 * out, uint32 modulus_size, uint8 * modulus, uint8 * exponent)
{
	BN_CTX *ctx;
	BIGNUM mod, exp, x, y;
	uint8 inr[SEC_MAX_MODULUS_SIZE];
	int outlen;

	reverse(modulus, modulus_size);
	reverse(exponent, SEC_EXPONENT_SIZE);
	memcpy(inr, in, len);
	reverse(inr, len);

	ctx = BN_CTX_new();
	BN_init(&mod);
	BN_init(&exp);
	BN_init(&x);
	BN_init(&y);

	BN_bin2bn(modulus, modulus_size, &mod);
	BN_bin2bn(exponent, SEC_EXPONENT_SIZE, &exp);
	BN_bin2bn(inr, len, &x);
	BN_mod_exp(&y, &x, &exp, &mod, ctx);
	outlen = BN_bn2bin(&y, out);
	reverse(out, outlen);
	if (outlen < (int) modulus_size)
		memset(out + outlen, 0, modulus_size - outlen);

	BN_free(&y);
	BN_clear_free(&x);
	BN_free(&exp);
	BN_free(&mod);
	BN_CTX_free(ctx);
}

struct crypto_cert_struct
{
	X509 * px509;
};

CryptoCert
crypto_cert_read(uint8 * data, uint32 len)
{
	CryptoCert cert = xmalloc(sizeof(*cert));
	/* this will move the data pointer but we don't care, we don't use it again */
	cert->px509 = d2i_X509(NULL, (D2I_X509_CONST unsigned char **) &data, len);
	return cert;
}

void
crypto_cert_free(CryptoCert cert)
{
	X509_free(cert->px509);
    xfree(cert);
}

RD_BOOL
crypto_cert_verify(CryptoCert server_cert, CryptoCert cacert)
{
	/* FIXME: do the actual verification */
	return True;
}

int
crypto_cert_print_fp(FILE * fp, CryptoCert cert)
{
	return X509_print_fp(fp, cert->px509);
}

struct crypto_public_key_struct
{
	RSA * prsa;
};

CryptoPublicKey
crypto_cert_get_public_key(CryptoCert cert, uint32 * key_len)
{
	CryptoPublicKey public_key = xmalloc(sizeof(*public_key));
	int nid;
	EVP_PKEY *epk = NULL;

	/* For some reason, Microsoft sets the OID of the Public RSA key to
	   the oid for "MD5 with RSA Encryption" instead of "RSA Encryption"

	   Kudos to Richard Levitte for the following (intuitive)
	   lines of code that resets the OID and lets us extract the key. */
	
	nid = OBJ_obj2nid(cert->px509->cert_info->key->algor->algorithm);
	
	if ((nid == NID_md5WithRSAEncryption) || (nid == NID_shaWithRSAEncryption))
	{
		ASN1_OBJECT_free(cert->px509->cert_info->key->algor->algorithm);
		cert->px509->cert_info->key->algor->algorithm = OBJ_nid2obj(NID_rsaEncryption);
	}
	
	epk = X509_get_pubkey(cert->px509);
	
	if (NULL == epk)
		return NULL;

	public_key->prsa = RSAPublicKey_dup((RSA *) epk->pkey.ptr);
	*key_len = RSA_size(public_key->prsa);
	EVP_PKEY_free(epk);
	return public_key;
}

void
crypto_public_key_free(CryptoPublicKey public_key)
{
	RSA_free(public_key->prsa);
	xfree(public_key);
}

int
crypto_public_key_get_exp_mod(CryptoPublicKey public_key, uint8 * exponent, uint32 max_exp_len, uint8 * modulus, uint32 max_mod_len)
{
	int len;

	if ((BN_num_bytes(public_key->prsa->e) > (int) max_exp_len) || (BN_num_bytes(public_key->prsa->n) > (int) max_mod_len))
		return 1;
	
	len = BN_bn2bin(public_key->prsa->e, exponent);
	reverse(exponent, len);
	
	len = BN_bn2bin(public_key->prsa->n, modulus);
	reverse(modulus, len);
	
	return 0;
}