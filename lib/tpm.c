/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2012 Free Software Foundation.
 * Copyright © 2008-2012 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 * Author: Nikos Mavrogiannopoulos
 *
 * GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

/*
 * TPM code based on client-tpm.c from
 * Carolin Latze <latze@angry-red-pla.net> and Tobias Soder
 */

#include <gnutls/gnutls.h>
#include <gnutls/abstract.h>
#include <gnutls/tpm.h>

#include <gnutls_int.h>
#include <gnutls_errors.h>
#include <pkcs11_int.h>
#include <x509/common.h>
#include <x509_b64.h>
#include <random.h>

#include <trousers/tss.h>
#include <trousers/trousers.h>

/* Signing function for TPM privkeys, set with gnutls_privkey_import_ext2() */
struct tpm_ctx_st
{
  TSS_HCONTEXT tpm_ctx;
  TSS_HKEY tpm_key;
  TSS_HPOLICY tpm_key_policy;
  TSS_HKEY srk;
  TSS_HPOLICY srk_policy;
};

static void tpm_close_session(struct tpm_ctx_st *s);
static int import_tpm_key (gnutls_privkey_t pkey,
                           const gnutls_datum_t * fdata,
                           gnutls_x509_crt_fmt_t format,
                           TSS_UUID *uuid,
                           const char *srk_password,
                           const char *key_password);

/* TPM URL format:
 *
 * tpmkey:file=/path/to/file
 * tpmkey:uuid=7f468c16-cb7f-11e1-824d-b3a4f4b20343
 *
 */


static int tss_err(TSS_RESULT err)
{
  _gnutls_debug_log("TPM error: %s (%x)\n", Trspi_Error_String(err), (unsigned int)Trspi_Error_Code(err));
  switch(Trspi_Error_Code(err))
    {
      case TSS_E_COMM_FAILURE:
      case TSS_E_NO_CONNECTION:
      case TSS_E_CONNECTION_FAILED:
      case TSS_E_CONNECTION_BROKEN:
        return GNUTLS_E_TPM_SESSION_ERROR;
      case TPM_E_AUTHFAIL:
        return GNUTLS_E_TPM_SRK_PASSWORD_ERROR;
      default:
        return GNUTLS_E_TPM_ERROR;
    }
}

static void
tpm_deinit_fn (gnutls_privkey_t key, void *_s)
{
  struct tpm_ctx_st *s = _s;

  Tspi_Context_CloseObject (s->tpm_ctx, s->tpm_key_policy);
  Tspi_Context_CloseObject (s->tpm_ctx, s->tpm_key);

  tpm_close_session(s);
  gnutls_free (s);
}

static int
tpm_sign_fn (gnutls_privkey_t key, void *_s,
	     const gnutls_datum_t * data, gnutls_datum_t * sig)
{
  struct tpm_ctx_st *s = _s;
  TSS_HHASH hash;
  int err;

  _gnutls_debug_log ("TPM sign function called for %u bytes.\n",
		     data->size);

  err =
      Tspi_Context_CreateObject (s->tpm_ctx,
				 TSS_OBJECT_TYPE_HASH, TSS_HASH_OTHER,
				 &hash);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to create TPM hash object: %s\n",
			 Trspi_Error_String (err));
      return GNUTLS_E_PK_SIGN_FAILED;
    }
  err = Tspi_Hash_SetHashValue (hash, data->size, data->data);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to set value in TPM hash object: %s\n",
			 Trspi_Error_String (err));
      Tspi_Context_CloseObject (s->tpm_ctx, hash);
      return GNUTLS_E_PK_SIGN_FAILED;
    }
  err = Tspi_Hash_Sign (hash, s->tpm_key, &sig->size, &sig->data);
  Tspi_Context_CloseObject (s->tpm_ctx, hash);
  if (err)
    {
      if (s->tpm_key_policy || err != TPM_E_AUTHFAIL)
	_gnutls_debug_log ("TPM hash signature failed: %s\n",
			   Trspi_Error_String (err));
      if (err == TPM_E_AUTHFAIL)
	return GNUTLS_E_INSUFFICIENT_CREDENTIALS;
      else
	return GNUTLS_E_PK_SIGN_FAILED;
    }
  return 0;
}

static const unsigned char nullpass[20];
const TSS_UUID srk_uuid = TSS_UUID_SRK;

static int tpm_open_session(struct tpm_ctx_st *s, const char* srk_password)
{
int err, ret;

  err = Tspi_Context_Create (&s->tpm_ctx);
  if (err)
    {
      gnutls_assert ();
      return tss_err(err);
    }

  err = Tspi_Context_Connect (s->tpm_ctx, NULL);
  if (err)
    {
      gnutls_assert ();
      ret = tss_err(err);
      goto out_tspi_ctx;
    }

  err =
      Tspi_Context_LoadKeyByUUID (s->tpm_ctx, TSS_PS_TYPE_SYSTEM,
				  srk_uuid, &s->srk);
  if (err)
    {
      gnutls_assert ();
      ret = tss_err(err);
      goto out_tspi_ctx;
    }

  err = Tspi_GetPolicyObject (s->srk, TSS_POLICY_USAGE, &s->srk_policy);
  if (err)
    {
      gnutls_assert ();
      ret = tss_err(err);
      goto out_srk;
    }

  if (srk_password)
    err = Tspi_Policy_SetSecret (s->srk_policy,
				 TSS_SECRET_MODE_PLAIN,
				 strlen (srk_password), (BYTE *) srk_password);
  else				/* Well-known NULL key */
    err = Tspi_Policy_SetSecret (s->srk_policy,
				 TSS_SECRET_MODE_SHA1,
				 sizeof (nullpass), (BYTE *) nullpass);
  if (err)
    {
      gnutls_assert ();
      _gnutls_debug_log ("Failed to set TPM PIN: %s\n",
			 Trspi_Error_String (err));
      ret = tss_err(err);
      goto out_srkpol;
    }
  
  return 0;

out_srkpol:
  Tspi_Context_CloseObject (s->tpm_ctx, s->srk_policy);
  s->srk_policy = 0;
out_srk:
  Tspi_Context_CloseObject (s->tpm_ctx, s->srk);
  s->srk = 0;
out_tspi_ctx:
  Tspi_Context_Close (s->tpm_ctx);
  s->tpm_ctx = 0;
  return ret;

}

static void tpm_close_session(struct tpm_ctx_st *s)
{
  Tspi_Context_CloseObject (s->tpm_ctx, s->srk_policy);
  s->srk_policy = 0;
  Tspi_Context_CloseObject (s->tpm_ctx, s->srk);
  s->srk = 0;
  Tspi_Context_Close (s->tpm_ctx);
  s->tpm_ctx = 0;
}

static int
import_tpm_key (gnutls_privkey_t pkey,
                const gnutls_datum_t * fdata,
                gnutls_x509_crt_fmt_t format,
                TSS_UUID *uuid,
                const char *srk_password,
                const char *key_password)
{
  gnutls_datum_t asn1 = { NULL, 0 };
  size_t slen;
  int err, ret;
  struct tpm_ctx_st *s;
  gnutls_datum_t tmp_sig;

  s = gnutls_malloc (sizeof (*s));
  if (s == NULL)
    {
      gnutls_assert ();
      return GNUTLS_E_MEMORY_ERROR;
    }

  ret = tpm_open_session(s, srk_password);
  if (ret < 0)
    {
      gnutls_assert();
      goto out_ctx;
    }

  if (fdata != NULL)
    {
      ret = gnutls_pem_base64_decode_alloc ("TSS KEY BLOB", fdata, &asn1);
      if (ret)
        {
          gnutls_assert ();
          _gnutls_debug_log ("Error decoding TSS key blob: %s\n",
                             gnutls_strerror (ret));
          goto out_session;
        }

      slen = asn1.size;
      ret = _gnutls_x509_decode_octet_string(NULL, asn1.data, asn1.size, asn1.data, &slen);
      if (ret < 0)
        {
          gnutls_assert();
          goto out_blob;
        }
      asn1.size = slen;

      /* ... we get it here instead. */
      err = Tspi_Context_LoadKeyByBlob (s->tpm_ctx, s->srk,
                                        asn1.size, asn1.data, &s->tpm_key);
      if (err != 0)
        {
          if (srk_password)
            {
              gnutls_assert ();
              _gnutls_debug_log
                  ("Failed to load TPM key blob: %s\n",
                   Trspi_Error_String (err));
            }

          if (err)
            {
              gnutls_assert ();
              ret = tss_err(err);
              goto out_blob;
            }
        }
    }
  else if (uuid)
    {
      err =
          Tspi_Context_LoadKeyByUUID (s->tpm_ctx, TSS_PS_TYPE_SYSTEM,
              *uuid, &s->tpm_key);
      if (err)
        {
          gnutls_assert ();
          ret = tss_err(err);
          goto out_session;
        }
    }
  else
    {
      gnutls_assert();
      ret = GNUTLS_E_INVALID_REQUEST;
      goto out_session;
    }

  ret =
      gnutls_privkey_import_ext2 (pkey, GNUTLS_PK_RSA, s,
				  tpm_sign_fn, NULL, tpm_deinit_fn, 0);
  if (ret < 0)
    {
      gnutls_assert ();
      goto out_session;
    }

  ret =
      gnutls_privkey_sign_data (pkey, GNUTLS_DIG_SHA1, 0, fdata, &tmp_sig);
  if (ret == GNUTLS_E_INSUFFICIENT_CREDENTIALS)
    {
      if (!s->tpm_key_policy)
	{
	  err = Tspi_Context_CreateObject (s->tpm_ctx,
					   TSS_OBJECT_TYPE_POLICY,
					   TSS_POLICY_USAGE,
					   &s->tpm_key_policy);
	  if (err)
	    {
	      gnutls_assert ();
	      _gnutls_debug_log
		  ("Failed to create key policy object: %s\n",
		   Trspi_Error_String (err));
              ret = tss_err(err);
	      goto out_key;
	    }

	  err = Tspi_Policy_AssignToObject (s->tpm_key_policy, s->tpm_key);
	  if (err)
	    {
	      gnutls_assert ();
	      _gnutls_debug_log ("Failed to assign policy to key: %s\n",
				 Trspi_Error_String (err));
              ret = tss_err(err);
	      goto out_key_policy;
	    }
	}

      err = Tspi_Policy_SetSecret (s->tpm_key_policy,
				   TSS_SECRET_MODE_PLAIN,
				   strlen (key_password), (void *) key_password);

      if (err)
	{
	  gnutls_assert ();
	  _gnutls_debug_log ("Failed to set key PIN: %s\n",
			     Trspi_Error_String (err));
          ret = GNUTLS_E_TPM_KEY_PASSWORD_ERROR;
	  goto out_key_policy;
	}
    }
  else if (ret < 0)
    {
      gnutls_assert ();
      goto out_blob;
    }

  gnutls_free (asn1.data);
  return 0;
out_key_policy:
  Tspi_Context_CloseObject (s->tpm_ctx, s->tpm_key_policy);
  s->tpm_key_policy = 0;
out_key:
  Tspi_Context_CloseObject (s->tpm_ctx, s->tpm_key);
  s->tpm_key = 0;
out_blob:
  gnutls_free (asn1.data);
out_session:
  tpm_close_session(s);
out_ctx:
  gnutls_free (s);
  return ret;
}

/**
 * gnutls_privkey_import_tpm_raw:
 * @pkey: The private key
 * @fdata: The TPM key to be imported
 * @format: The format of the private key
 * @srk_password: The password for the SRK key (optional)
 * @key_password: A password for the key (optional)
 *
 * This function will import the given private key to the abstract
 * #gnutls_privkey_t structure. If a password is needed to access
 * TPM then or the provided password is wrong, then 
 * %GNUTLS_E_TPM_SRK_PASSWORD_ERROR is returned. If the key password
 * is wrong or not provided then %GNUTLS_E_TPM_KEY_PASSWORD_ERROR
 * is returned. 
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 *
 **/
int
gnutls_privkey_import_tpm_raw (gnutls_privkey_t pkey,
			       const gnutls_datum_t * fdata,
			       gnutls_x509_crt_fmt_t format,
			       const char *srk_password,
			       const char *key_password)
{
  return import_tpm_key(pkey, fdata, format, NULL, srk_password, key_password);
}

struct tpmkey_url_st
{
  char* filename;
  TSS_UUID uuid;
  unsigned int uuid_set;
};

static void clear_tpmkey_url(struct tpmkey_url_st *s)
{
  gnutls_free(s->filename);
  memset(s, 0, sizeof(*s));
}

static int
unescape_string (char *output, const char *input, size_t * size,
                 char terminator)
{
  gnutls_buffer_st str;
  int ret = 0;
  char *p;
  int len;

  _gnutls_buffer_init (&str);

  /* find terminator */
  p = strchr (input, terminator);
  if (p != NULL)
    len = p - input;
  else
    len = strlen (input);

  ret = _gnutls_buffer_append_data (&str, input, len);
  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }

  ret = _gnutls_buffer_unescape (&str);
  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }

  ret = _gnutls_buffer_append_data (&str, "", 1);
  if (ret < 0)
    {
      gnutls_assert ();
      return ret;
    }

  _gnutls_buffer_pop_data (&str, output, size);

  _gnutls_buffer_clear (&str);

  return ret;
}

#define UUID_SIZE 16

static int randomize_uuid(TSS_UUID* uuid)
{
  uint8_t raw_uuid[16];
  int ret;

  ret = _gnutls_rnd (GNUTLS_RND_NONCE, raw_uuid, sizeof(raw_uuid));
  if (ret < 0)
    return gnutls_assert_val(ret);
    
  memcpy(&uuid->ulTimeLow, raw_uuid, 4);
  memcpy(&uuid->usTimeMid, &raw_uuid[4], 2);
  memcpy(&uuid->usTimeHigh, &raw_uuid[6], 2);
  uuid->bClockSeqHigh = raw_uuid[8];
  uuid->bClockSeqLow = raw_uuid[9];
  memcpy(&uuid->rgbNode, &raw_uuid[10], 6);

  return 0;
}

static int encode_tpmkey_url(char** url, TSS_UUID* uuid)
{
size_t size = UUID_SIZE*2+4+32;

  *url = gnutls_malloc(size);
  if (*url == NULL)
    return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);

  snprintf(*url, size, "tpmkey:uuid=%.8x-%.4x-%.4x-%.2x%.2x-%.2x%.2x%.2x%.2x%.2x%.2x", 
           (unsigned int)uuid->ulTimeLow, (unsigned int)uuid->usTimeMid, 
           (unsigned int)uuid->usTimeHigh, (unsigned int)uuid->bClockSeqHigh, 
           (unsigned int)uuid->bClockSeqLow, (unsigned int)uuid->rgbNode[0],
           (unsigned int)uuid->rgbNode[1], (unsigned int)uuid->rgbNode[2],
           (unsigned int)uuid->rgbNode[3], (unsigned int)uuid->rgbNode[4],
           (unsigned int)uuid->rgbNode[5]);

  return 0;
}

static int decode_tpmkey_url(const char* url, struct tpmkey_url_st *s)
{
  char* p;
  size_t size;
  int ret;
  unsigned int i, j;

  if (strstr (url, "tpmkey:") == NULL)
    return gnutls_assert_val(GNUTLS_E_PARSING_ERROR);
                        
  memset(s, 0, sizeof(*s));

  p = strstr(url, "file=");
  if (p != NULL)
    {
      p += sizeof ("file=") - 1;
      size = strlen(p);
      s->filename = gnutls_malloc(size);
      if (s->filename == NULL)
        return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);

      ret = unescape_string (s->filename, p, &size, ';');
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }

      
      
    }
  else if ((p = strstr(url, "uuid=")) != NULL)
   {
      char tmp_uuid[33];
      uint8_t raw_uuid[16];

      p += sizeof ("uuid=") - 1;
      size = strlen(p);

      for (j=i=0;i<size;i++)
        {
          if (j==sizeof(tmp_uuid)-1) 
            {
              break;
            }
          if (isalnum(p[i])) tmp_uuid[j++]=p[i];
        }
      tmp_uuid[j] = 0;

      size = sizeof(raw_uuid);
      ret = _gnutls_hex2bin(tmp_uuid, strlen(tmp_uuid), raw_uuid, &size);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }

      memcpy(&s->uuid.ulTimeLow, raw_uuid, 4);
      memcpy(&s->uuid.usTimeMid, &raw_uuid[4], 2);
      memcpy(&s->uuid.usTimeHigh, &raw_uuid[6], 2);
      s->uuid.bClockSeqHigh = raw_uuid[8];
      s->uuid.bClockSeqLow = raw_uuid[9];
      memcpy(&s->uuid.rgbNode, &raw_uuid[10], 6);
      s->uuid_set = 1;
    }
  else
    {
      return gnutls_assert_val(GNUTLS_E_PARSING_ERROR);
    }

  return 0;

cleanup:
  clear_tpmkey_url(s);
  return ret;
}

/**
 * gnutls_privkey_import_tpm_url:
 * @pkey: The private key
 * @url: The URL of the TPM key to be imported
 * @srk_password: The password for the SRK key (optional)
 * @key_password: A password for the key (optional)
 *
 * This function will import the given private key to the abstract
 * #gnutls_privkey_t structure. If a password is needed to access
 * TPM then or the provided password is wrong, then 
 * %GNUTLS_E_TPM_SRK_PASSWORD_ERROR is returned. If the key password
 * is wrong or not provided then %GNUTLS_E_TPM_KEY_PASSWORD_ERROR
 * is returned. 
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 *
 **/
int
gnutls_privkey_import_tpm_url (gnutls_privkey_t pkey,
                               const char* url,
                               const char *srk_password,
                               const char *key_password)
{
struct tpmkey_url_st durl;
gnutls_datum_t fdata = { NULL, 0 };
int ret;

  ret = decode_tpmkey_url(url, &durl);
  if (ret < 0)
    return gnutls_assert_val(ret);

  if (durl.filename)
    {

      ret = gnutls_load_file(durl.filename, &fdata);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }

      ret = gnutls_privkey_import_tpm_raw (pkey, &fdata, GNUTLS_X509_FMT_PEM,
			       		                           srk_password, key_password);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }
    }
  else if (durl.uuid_set)
    {
      ret = import_tpm_key (pkey, NULL, 0, &durl.uuid, srk_password, key_password);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }
    }

  ret = 0;
cleanup:
  gnutls_free(fdata.data);
  clear_tpmkey_url(&durl);
  return ret;
}


/* reads the RSA public key from the given TSS key.
 * If psize is non-null it contains the total size of the parameters
 * in bytes */
static int read_pubkey(gnutls_pubkey_t pub, TSS_HKEY key_ctx, size_t *psize)
{
void* tdata;
UINT32 tint;
TSS_RESULT tssret;
gnutls_datum_t m, e;
int ret;

  /* read the public key */

  tssret = Tspi_GetAttribData(key_ctx, TSS_TSPATTRIB_RSAKEY_INFO,
                                TSS_TSPATTRIB_KEYINFO_RSA_MODULUS, &tint, (void*)&tdata);
  if (tssret != 0)
    {
      gnutls_assert();
      return tss_err(tssret);
    }
    
  m.data = tdata;
  m.size = tint;

  tssret = Tspi_GetAttribData(key_ctx, TSS_TSPATTRIB_RSAKEY_INFO,
                                TSS_TSPATTRIB_KEYINFO_RSA_EXPONENT, &tint, (void*)&tdata);
  if (tssret != 0)
    {
      gnutls_assert();
      Tspi_Context_FreeMemory(key_ctx, m.data);
      return tss_err(tssret);
    }
    
  e.data = tdata;
  e.size = tint;
    
  ret = gnutls_pubkey_import_rsa_raw(pub, &m, &e);

  Tspi_Context_FreeMemory(key_ctx, m.data);
  Tspi_Context_FreeMemory(key_ctx, e.data);

  if (ret < 0)
    return gnutls_assert_val(ret);
  
  if (psize)
    *psize = e.size + m.size;

  return 0;
}


static int
import_tpm_pubkey (gnutls_pubkey_t pkey,
                   const gnutls_datum_t * fdata,
                   gnutls_x509_crt_fmt_t format,
                   TSS_UUID *uuid,
                   const char *srk_password)
{
gnutls_datum_t asn1 = {NULL, 0};
size_t slen;
int err, ret;
struct tpm_ctx_st s;

  ret = tpm_open_session(&s, srk_password);
  if (ret < 0)
    return gnutls_assert_val(ret);

  if (fdata != NULL)
    {
      ret = gnutls_pem_base64_decode_alloc ("TSS KEY BLOB", fdata, &asn1);
      if (ret)
        {
          gnutls_assert ();
          _gnutls_debug_log ("Error decoding TSS key blob: %s\n",
           gnutls_strerror (ret));
          goto out_session;
        }

      slen = asn1.size;
      ret = _gnutls_x509_decode_octet_string(NULL, asn1.data, asn1.size, asn1.data, &slen);
      if (ret < 0)
        {
          gnutls_assert();
          goto out_blob;
        }
      asn1.size = slen;

      err = Tspi_Context_LoadKeyByBlob (s.tpm_ctx, s.srk,
                                        asn1.size, asn1.data, &s.tpm_key);
      if (err != 0)
        {
          if (srk_password)
            {
              gnutls_assert ();
              _gnutls_debug_log
                  ("Failed to load TPM key blob: %s\n",
                   Trspi_Error_String (err));
            }

          if (err)
            {
              gnutls_assert ();
              ret = tss_err(err);
              goto out_blob;
            }
        }
    }
  else if (uuid)
    {
      err =
          Tspi_Context_LoadKeyByUUID (s.tpm_ctx, TSS_PS_TYPE_SYSTEM,
              *uuid, &s.tpm_key);
      if (err)
        {
          gnutls_assert ();
          ret = tss_err(err);
          goto out_session;
        }
    }
  else
    {
      gnutls_assert();
      ret = GNUTLS_E_INVALID_REQUEST;
      goto out_session;
    }

  ret = read_pubkey(pkey, s.tpm_key, NULL);
  if (ret < 0)
    {
      gnutls_assert();
      goto out_blob;
    }

  ret = 0;
out_blob:
  gnutls_free (asn1.data);
out_session:
  tpm_close_session(&s);
  return ret;
}


/**
 * gnutls_pubkey_import_tpm_raw:
 * @pkey: The public key
 * @fdata: The TPM key to be imported
 * @format: The format of the private key
 * @srk_password: The password for the SRK key (optional)
 * @key_password: A password for the key (optional)
 *
 * This function will import the public key from the provided
 * TPM key structure. If a password is needed to decrypt
 * the provided key or the provided password is wrong, then 
 * %GNUTLS_E_TPM_SRK_PASSWORD_ERROR is returned. 
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 *
 **/
int
gnutls_pubkey_import_tpm_raw (gnutls_pubkey_t pkey,
                              const gnutls_datum_t * fdata,
                              gnutls_x509_crt_fmt_t format,
                              const char *srk_password)
{
  return import_tpm_pubkey(pkey, fdata, format, NULL, srk_password);
}

/**
 * gnutls_pubkey_import_tpm_url:
 * @pkey: The public key
 * @url: The URL of the TPM key to be imported
 * @srk_password: The password for the SRK key (optional)
 *
 * This function will import the given private key to the abstract
 * #gnutls_privkey_t structure. If a password is needed to access
 * TPM then or the provided password is wrong, then 
 * %GNUTLS_E_TPM_SRK_PASSWORD_ERROR is returned. 
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 *
 **/
int
gnutls_pubkey_import_tpm_url (gnutls_pubkey_t pkey,
                              const char* url,
                              const char *srk_password)
{
struct tpmkey_url_st durl;
gnutls_datum_t fdata = { NULL, 0 };
int ret;

  ret = decode_tpmkey_url(url, &durl);
  if (ret < 0)
    return gnutls_assert_val(ret);

  if (durl.filename)
    {

      ret = gnutls_load_file(durl.filename, &fdata);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }

      ret = gnutls_pubkey_import_tpm_raw (pkey, &fdata, GNUTLS_X509_FMT_PEM,
			       		                          srk_password);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }
    }
  else if (durl.uuid_set)
    {
      ret = import_tpm_pubkey (pkey, NULL, 0, &durl.uuid, srk_password);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }
    }

  ret = 0;
cleanup:
  gnutls_free(fdata.data);
  clear_tpmkey_url(&durl);
  return ret;
}


/**
 * gnutls_tpm_privkey_generate:
 * @pk: the public key algorithm
 * @bits: the security bits
 * @srk_password: a password to protect the exported key (optional)
 * @key_password: the password for the TPM (optional)
 * @privkey: the generated key
 * @pubkey: the corresponding public key
 * @flags: should be a list of %GNUTLS_TPM flags
 *
 * This function will generate a private key in the TPM
 * chip. The private key will be generated within the chip
 * and will be exported in a wrapped with TPM's master key
 * form. Furthermore the wrapped key can be protected with
 * the provided @password.
 *
 * Note that bits in TPM is quantized value. If the input value
 * is not one of the allowed values, then it will be quantized to
 * one of 512, 1024, 2048, 4096, 8192 and 16384.
 *
 * Allowed flags are:
 *
 * %GNUTLS_TPM_KEY_SIGNING: Generate a signing key instead of a legacy,

 * %GNUTLS_TPM_REGISTER_KEY: Register the generate key in TPM. In that
 * case @privkey would contain a URL with the UUID.
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 *
 * Since: 3.1.0
 **/
int
gnutls_tpm_privkey_generate (gnutls_pk_algorithm_t pk, unsigned int bits, 
                             const char* srk_password,
                             const char* key_password,
                             gnutls_x509_crt_fmt_t format,
                             gnutls_datum_t* privkey, 
                             gnutls_datum_t* pubkey,
                             unsigned int flags)
{
TSS_FLAG tpm_flags = TSS_KEY_VOLATILE;
TSS_HKEY key_ctx; 
TSS_RESULT tssret;
int ret;
void* tdata;
UINT32 tint;
gnutls_datum_t tmpkey;
TSS_HPOLICY key_policy;
gnutls_pubkey_t pub;
struct tpm_ctx_st s;

  if (flags & GNUTLS_TPM_KEY_SIGNING)
    tpm_flags |= TSS_KEY_TYPE_SIGNING;
  else
    tpm_flags |= TSS_KEY_TYPE_LEGACY;

  if (bits <= 512)
      tpm_flags |= TSS_KEY_SIZE_512;
  else if (bits <= 1024)
      tpm_flags |= TSS_KEY_SIZE_1024;
  else if (bits <= 2048)
      tpm_flags |= TSS_KEY_SIZE_2048;
  else if (bits <= 4096)
      tpm_flags |= TSS_KEY_SIZE_4096;
  else if (bits <= 8192)
      tpm_flags |= TSS_KEY_SIZE_8192;
  else
      tpm_flags |= TSS_KEY_SIZE_16384;

  ret = tpm_open_session(&s, srk_password);
  if (ret < 0)
    return gnutls_assert_val(ret);

  tssret = Tspi_Context_CreateObject(s.tpm_ctx, TSS_OBJECT_TYPE_RSAKEY, tpm_flags, &key_ctx);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = tss_err(tssret);
      goto err_cc;
    }
    
  tssret = Tspi_SetAttribUint32(key_ctx, TSS_TSPATTRIB_KEY_INFO, TSS_TSPATTRIB_KEYINFO_SIGSCHEME,
                                TSS_SS_RSASSAPKCS1V15_DER);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = tss_err(tssret);
      goto err_sa;
    }

  /* set the key of the actual key */
  if (key_password)
    {
      tssret = Tspi_GetPolicyObject(key_ctx, TSS_POLICY_USAGE, &key_policy);
      if (tssret != 0)
        {
          gnutls_assert();
          ret = tss_err(tssret);
          goto err_sa;
        }

      tssret = Tspi_Policy_SetSecret(key_policy, TSS_SECRET_MODE_PLAIN, 
                                     strlen(key_password), (void*)key_password);
      if (tssret != 0)
        {
          gnutls_assert();
          ret = tss_err(tssret);
          goto err_sa;
        }
    }

  tssret = Tspi_Key_CreateKey(key_ctx, s.srk, 0);
  if (tssret != 0)
    {
      gnutls_assert();
      ret = tss_err(tssret);
      goto err_sa;
    }

  if (flags & GNUTLS_TPM_REGISTER_KEY)
    {
      TSS_UUID key_uuid;

      ret = randomize_uuid(&key_uuid);
      if (ret < 0)
        {
          gnutls_assert();
          goto err_sa;
        }

      tssret = Tspi_Context_RegisterKey(s.tpm_ctx, key_ctx, TSS_PS_TYPE_SYSTEM,
                                        key_uuid, TSS_PS_TYPE_SYSTEM, srk_uuid);
      if (tssret != 0)
        {
          gnutls_assert();
          ret = tss_err(tssret);
          goto err_sa;
        }

      ret = encode_tpmkey_url((char**)&privkey->data, &key_uuid);
      if (ret < 0)
        {
          TSS_HKEY tkey;

          Tspi_Context_UnregisterKey(s.tpm_ctx, TSS_PS_TYPE_SYSTEM, key_uuid, &tkey);
          gnutls_assert();
          goto err_sa;
        }
      privkey->size = strlen((char*)privkey->data);

    }
  else /* get the key as blob */
    {

      tssret = Tspi_GetAttribData(key_ctx, TSS_TSPATTRIB_KEY_BLOB,
                                  TSS_TSPATTRIB_KEYBLOB_BLOB, &tint, (void*)&tdata);
      if (tssret != 0)
        {
          gnutls_assert();
          ret = tss_err(tssret);
          goto err_sa;
        }

      ret = _gnutls_x509_encode_octet_string(tdata, tint, &tmpkey);
      if (ret < 0)
        {
          gnutls_assert();
          goto cleanup;
        }
      
      if (format == GNUTLS_X509_FMT_PEM)
        {
          ret = _gnutls_fbase64_encode ("TSS KEY BLOB", tmpkey.data, tmpkey.size, privkey);
          if (ret < 0)
            {
              gnutls_assert();
              goto cleanup;
            }
        }
      else
        {
          privkey->data = tmpkey.data;
          privkey->size = tmpkey.size;
          tmpkey.data = NULL;
        }
    }

  /* read the public key */
  {
    size_t psize;

    ret = gnutls_pubkey_init(&pub);
    if (ret < 0)
      {
        gnutls_assert();
        goto privkey_cleanup;
      }

    ret = read_pubkey(pub, key_ctx, &psize);
    if (ret < 0)
      {
        gnutls_assert();
        goto privkey_cleanup;
      }
    psize+=512;
    
    pubkey->data = gnutls_malloc(psize);
    if (pubkey->data == NULL)
      {
        gnutls_assert();
        ret = GNUTLS_E_MEMORY_ERROR;
        goto pubkey_cleanup;
      }
    
    ret = gnutls_pubkey_export(pub, format, pubkey->data, &psize);
    if (ret < 0)
      {
        gnutls_assert();
        goto pubkey_cleanup;
      }
    pubkey->size = psize;

    gnutls_pubkey_deinit(pub);
  }

  ret = 0;
  goto cleanup;
  
pubkey_cleanup:
  gnutls_pubkey_deinit(pub);
privkey_cleanup:
  gnutls_free(privkey->data);
  privkey->data = NULL;
cleanup:  
  gnutls_free(tmpkey.data);
  tmpkey.data = NULL;
err_sa:
  Tspi_Context_CloseObject(s.tpm_ctx, key_ctx);
err_cc:
  tpm_close_session(&s); 
  return ret;
}

