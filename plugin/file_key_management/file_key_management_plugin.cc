/* Copyright (c) 2002, 2012, eperi GmbH.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_global.h>
#include <typelib.h>
#include "parser.h"
#include <mysql/plugin_encryption.h>
#include <string.h>

static char* filename;
static char* filekey;
static unsigned long encryption_algorithm;

static const char *encryption_algorithm_names[]=
{
  "aes_cbc",
#ifdef HAVE_EncryptAes128Ctr
  "aes_ctr",
#endif
  0
};

static TYPELIB encryption_algorithm_typelib=CREATE_TYPELIB_FOR(encryption_algorithm_names);

static MYSQL_SYSVAR_STR(filename, filename,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Path and name of the key file",
  NULL, NULL, "");

static MYSQL_SYSVAR_STR(filekey, filekey,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Key to encrypt / decrypt the keyfile",
  NULL, NULL, "");

#ifdef HAVE_EncryptAes128Ctr
#define recommendation  ", aes_ctr is the recommended one"
#else
#define recommendation  ""
#endif
static MYSQL_SYSVAR_ENUM(encryption_algorithm, encryption_algorithm,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "Encryption algorithm to use" recommendation,
  NULL, NULL, 0, &encryption_algorithm_typelib);

static struct st_mysql_sys_var* settings[] = {
  MYSQL_SYSVAR(filename),
  MYSQL_SYSVAR(filekey),
  MYSQL_SYSVAR(encryption_algorithm),
  NULL
};

std::map<unsigned int,keyentry> keys;

static keyentry *get_key(unsigned int key_id)
{
  keyentry &key= keys[key_id];
  if (key.id == 0)
    return 0;
  return &key;
}

/* the version is always the same, no automatic key rotation */
static unsigned int get_latest_version(uint key_id)
{
  return  get_key(key_id) ? 1 : ENCRYPTION_KEY_VERSION_INVALID;
}

static unsigned int get_key_from_key_file(unsigned int key_id,
       unsigned int key_version, unsigned char* dstbuf, unsigned *buflen)
{
  if (key_version != 1)
    return ENCRYPTION_KEY_VERSION_INVALID;

  keyentry* entry = get_key(key_id);

  if (entry == NULL)
    return ENCRYPTION_KEY_VERSION_INVALID;

  if (*buflen < entry->length)
  {
    *buflen= entry->length;
    return ENCRYPTION_KEY_BUFFER_TOO_SMALL;
  }

  *buflen= entry->length;
  if (dstbuf)
    memcpy(dstbuf, entry->key, entry->length);

  return 0;
}

// let's simplify the condition below
#ifndef HAVE_EncryptAes128Gcm
#define MY_AES_GCM MY_AES_CTR
#ifndef HAVE_EncryptAes128Ctr
#define MY_AES_CTR MY_AES_CBC
#endif
#endif

static inline enum my_aes_mode mode(int flags)
{
  /*
    If encryption_algorithm is AES_CTR then
      if no-padding, use AES_CTR
      else use AES_GCM (like CTR but appends a "checksum" block)
    else
      use AES_CBC
  */
  if (encryption_algorithm)
    if (flags & ENCRYPTION_FLAG_NOPAD)
      return MY_AES_CTR;
    else
      return MY_AES_GCM;
  else
    return MY_AES_CBC;
}

static int ctx_init(void *ctx, const unsigned char* key, unsigned int klen,
                    const unsigned char* iv, unsigned int ivlen, int flags,
                    unsigned int key_id, unsigned int key_version)
{
  return my_aes_crypt_init(ctx, mode(flags), flags, key, klen, iv, ivlen);
}

static int ctx_update(void *ctx, const unsigned char *src, unsigned int slen,
  unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_update(ctx, src, slen, dst, dlen);
}


static int ctx_finish(void *ctx, unsigned char *dst, unsigned int *dlen)
{
  return my_aes_crypt_finish(ctx, dst, dlen);
}

static unsigned int get_length(unsigned int slen, unsigned int key_id,
                               unsigned int key_version)
{
  return my_aes_get_size(mode(0), slen);
}

static uint ctx_size(uint, uint)
{
  return my_aes_ctx_size(mode(0));
}

struct st_mariadb_encryption file_key_management_plugin= {
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  get_latest_version,
  get_key_from_key_file,
  ctx_size,
  ctx_init,
  ctx_update,
  ctx_finish,
  get_length
};

static int file_key_management_plugin_init(void *p)
{
  Parser parser(filename, filekey);
  return parser.parse(&keys);
}

static int file_key_management_plugin_deinit(void *p)
{
  keys.clear();
  return 0;
}

/*
  Plugin library descriptor
*/
maria_declare_plugin(file_key_management)
{
  MariaDB_ENCRYPTION_PLUGIN,
  &file_key_management_plugin,
  "file_key_management",
  "Denis Endro eperi GmbH",
  "File-based key management plugin",
  PLUGIN_LICENSE_GPL,
  file_key_management_plugin_init,
  file_key_management_plugin_deinit,
  0x0100 /* 1.0 */,
  NULL,	/* status variables */
  settings,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
