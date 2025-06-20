/*
   Copyright (c) 2005, 2018, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include "sql_plugin.h"                         // SHOW_MY_BOOL
#include "sql_priv.h"
#include "unireg.h"
#include "sql_class.h"                          // set_var.h: THD
#include "sys_vars_shared.h"
#include "sql_locale.h"
#include "sql_plugin.h"
#include "sql_parse.h"          // check_table_access
#include "sql_base.h"                           // close_mysql_tables
#include "key.h"                                // key_copy
#include "sql_table.h"
#include "sql_show.h"           // remove_status_vars, add_status_vars
#include "strfunc.h"            // find_set
#include "records.h"          // init_read_record, end_read_record
#include <my_pthread.h>
#include <my_getopt.h>
#include "sql_audit.h"
#include <mysql/plugin_auth.h>
#include "lock.h"                               // MYSQL_LOCK_IGNORE_TIMEOUT
#include <mysql/plugin_auth.h>
#include <mysql/plugin_password_validation.h>
#include <mysql/plugin_encryption.h>
#include <mysql/plugin_data_type.h>
#include <mysql/plugin_function.h>
#include "sql_plugin_compat.h"
#include "wsrep_mysqld.h"

static PSI_memory_key key_memory_plugin_mem_root;
static PSI_memory_key key_memory_plugin_int_mem_root;
static PSI_memory_key key_memory_mysql_plugin;
static PSI_memory_key key_memory_mysql_plugin_dl;
static PSI_memory_key key_memory_plugin_bookmark;

#ifdef HAVE_LINK_H
#include <link.h>
#endif

extern struct st_maria_plugin *mysql_optional_plugins[];
extern struct st_maria_plugin *mysql_mandatory_plugins[];

/**
  @note The order of the enumeration is critical.
  @see construct_options
*/
const char *global_plugin_typelib_names[]=
  { "OFF", "ON", "FORCE", "FORCE_PLUS_PERMANENT", NULL };
static TYPELIB global_plugin_typelib=
  CREATE_TYPELIB_FOR(global_plugin_typelib_names);

static I_List<i_string> opt_plugin_load_list;
I_List<i_string> *opt_plugin_load_list_ptr= &opt_plugin_load_list;
char *opt_plugin_dir_ptr;
char opt_plugin_dir[FN_REFLEN];
ulong plugin_maturity;

static LEX_CSTRING MYSQL_PLUGIN_NAME= {STRING_WITH_LEN("plugin") };

/*
  not really needed now, this map will become essential when we add more
  maturity levels. We cannot change existing maturity constants,
  so the next value - even if it will be MariaDB_PLUGIN_MATURITY_VERY_BUGGY -
  will inevitably be larger than MariaDB_PLUGIN_MATURITY_STABLE.
  To be able to compare them we use this mapping array
*/
uint plugin_maturity_map[]=
{ 0, 1, 2, 3, 4, 5, 6 };

/*
  When you add a new plugin type, add both a string and make sure that the
  init and deinit array are correctly updated.
*/
const LEX_CSTRING plugin_type_names[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  { STRING_WITH_LEN("UDF") },
  { STRING_WITH_LEN("STORAGE ENGINE") },
  { STRING_WITH_LEN("FTPARSER") },
  { STRING_WITH_LEN("DAEMON") },
  { STRING_WITH_LEN("INFORMATION SCHEMA") },
  { STRING_WITH_LEN("AUDIT") },
  { STRING_WITH_LEN("REPLICATION") },
  { STRING_WITH_LEN("AUTHENTICATION") },
  { STRING_WITH_LEN("PASSWORD VALIDATION") },
  { STRING_WITH_LEN("ENCRYPTION") },
  { STRING_WITH_LEN("DATA TYPE") },
  { STRING_WITH_LEN("FUNCTION") }
};

extern int initialize_schema_table(void *plugin);
extern int finalize_schema_table(void *plugin);

extern int initialize_audit_plugin(void *plugin);
extern int finalize_audit_plugin(void *plugin);

extern int initialize_encryption_plugin(void *plugin);
extern int finalize_encryption_plugin(void *plugin);

extern int initialize_data_type_plugin(void *plugin);

/*
  The number of elements in both plugin_type_initialize and
  plugin_type_deinitialize should equal to the number of plugins
  defined.
*/
plugin_type_init plugin_type_initialize[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0, ha_initialize_handlerton, 0, 0,initialize_schema_table,
  initialize_audit_plugin, 0, 0, 0, initialize_encryption_plugin,
  initialize_data_type_plugin, 0
};

plugin_type_init plugin_type_deinitialize[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0, ha_finalize_handlerton, 0, 0, finalize_schema_table,
  finalize_audit_plugin, 0, 0, 0, finalize_encryption_plugin, 0,
  0 // FUNCTION
};

/*
  Defines in which order plugin types have to be initialized.
  Essentially, we want to initialize MYSQL_KEY_MANAGEMENT_PLUGIN before
  MYSQL_STORAGE_ENGINE_PLUGIN, and that before MYSQL_INFORMATION_SCHEMA_PLUGIN
*/
static int plugin_type_initialization_order[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  MYSQL_DAEMON_PLUGIN,
  MariaDB_ENCRYPTION_PLUGIN,
  MariaDB_DATA_TYPE_PLUGIN,
  MariaDB_FUNCTION_PLUGIN,
  MYSQL_STORAGE_ENGINE_PLUGIN,
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  MYSQL_FTPARSER_PLUGIN,
  MYSQL_AUTHENTICATION_PLUGIN,
  MariaDB_PASSWORD_VALIDATION_PLUGIN,
  MYSQL_AUDIT_PLUGIN,
  MYSQL_REPLICATION_PLUGIN,
  MYSQL_UDF_PLUGIN
};

#ifdef HAVE_DLOPEN
static const char *plugin_interface_version_sym=
                   "_mysql_plugin_interface_version_";
static const char *sizeof_st_plugin_sym=
                   "_mysql_sizeof_struct_st_plugin_";
static const char *plugin_declarations_sym= "_mysql_plugin_declarations_";
static int min_plugin_interface_version= MYSQL_PLUGIN_INTERFACE_VERSION & ~0xFF;
static const char *maria_plugin_interface_version_sym=
                   "_maria_plugin_interface_version_";
static const char *maria_sizeof_st_plugin_sym=
                   "_maria_sizeof_struct_st_plugin_";
static const char *maria_plugin_declarations_sym=
                   "_maria_plugin_declarations_";
static int min_maria_plugin_interface_version=
                   MARIA_PLUGIN_INTERFACE_VERSION & ~0xFF;
#endif

/* Note that 'int version' must be the first field of every plugin
   sub-structure (plugin->info).
*/
static int min_plugin_info_interface_version[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0x0000,
  MYSQL_HANDLERTON_INTERFACE_VERSION,
  MYSQL_FTPARSER_INTERFACE_VERSION,
  MYSQL_DAEMON_INTERFACE_VERSION,
  MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION,
  MYSQL_AUDIT_INTERFACE_VERSION,
  MYSQL_REPLICATION_INTERFACE_VERSION,
  MIN_AUTHENTICATION_INTERFACE_VERSION,
  MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION,
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  MariaDB_FUNCTION_INTERFACE_VERSION
};
static int cur_plugin_info_interface_version[MYSQL_MAX_PLUGIN_TYPE_NUM]=
{
  0x0000, /* UDF: not implemented */
  MYSQL_HANDLERTON_INTERFACE_VERSION,
  MYSQL_FTPARSER_INTERFACE_VERSION,
  MYSQL_DAEMON_INTERFACE_VERSION,
  MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION,
  MYSQL_AUDIT_INTERFACE_VERSION,
  MYSQL_REPLICATION_INTERFACE_VERSION,
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  MariaDB_PASSWORD_VALIDATION_INTERFACE_VERSION,
  MariaDB_ENCRYPTION_INTERFACE_VERSION,
  MariaDB_DATA_TYPE_INTERFACE_VERSION,
  MariaDB_FUNCTION_INTERFACE_VERSION
};

static struct
{
  Lex_ident_plugin plugin_name;
  enum enum_plugin_load_option override;
} override_plugin_load_policy[]={
  /*
    If the performance schema is compiled in,
    treat the storage engine plugin as 'mandatory',
    to suppress any plugin-level options such as '--performance-schema'.
    This is specific to the performance schema, and is done on purpose:
    the server-level option '--performance-schema' controls the overall
    performance schema initialization, which consists of much more that
    the underlying storage engine initialization.
    See mysqld.cc, set_vars.cc.
    Suppressing ways to interfere directly with the storage engine alone
    prevents awkward situations where:
    - the user wants the performance schema functionality, by using
      '--enable-performance-schema' (the server option),
    - yet disable explicitly a component needed for the functionality
      to work, by using '--skip-performance-schema' (the plugin)
  */
  { "performance_schema"_Lex_ident_plugin, PLUGIN_FORCE }

  /* we disable few other plugins by default */
  ,{ "feedback"_Lex_ident_plugin, PLUGIN_OFF }
};

/* support for Services */

#include "sql_plugin_services.inl"

/*
  A mutex LOCK_plugin must be acquired before accessing the
  following variables/structures.
  We are always manipulating ref count, so a rwlock here is unnecessary.
*/
mysql_mutex_t LOCK_plugin;
static DYNAMIC_ARRAY plugin_dl_array;
static DYNAMIC_ARRAY plugin_array;
static HASH plugin_hash[MYSQL_MAX_PLUGIN_TYPE_NUM];
static MEM_ROOT plugin_mem_root;
static bool reap_needed= false;
volatile int global_plugin_version= 1;

static bool initialized= 0;
ulong dlopen_count;


/*
  write-lock on LOCK_system_variables_hash is required before modifying
  the following variables/structures
*/
static MEM_ROOT plugin_vars_mem_root;
static size_t global_variables_dynamic_size= 0;
static HASH bookmark_hash;


/*
  hidden part of opaque value passed to variable check functions.
  Used to provide a object-like structure to non C++ consumers.
*/
struct st_item_value_holder : public st_mysql_value
{
  Item *item;
};


/*
  stored in bookmark_hash, this structure is never removed from the
  hash and is used to mark a single offset for a thd local variable
  even if plugins have been uninstalled and reinstalled, repeatedly.
  This structure is allocated from plugin_mem_root.

  The key format is as follows:
    1 byte         - variable type code
    name_len bytes - variable name
    '\0'           - end of key
*/
struct st_bookmark
{
  uint name_len;
  int offset;
  uint version;
  bool loaded;
  char key[1];
};


/*
  skeleton of a plugin variable - portion of structure common to all.
*/
struct st_mysql_sys_var
{
  MYSQL_PLUGIN_VAR_HEADER;
};

enum install_status { INSTALL_GOOD, INSTALL_FAIL_WARN_OK, INSTALL_FAIL_NOT_OK };
/*
  sys_var class for access to all plugin variables visible to the user
*/
class sys_var_pluginvar: public sys_var, public Sql_alloc
{
public:
  struct st_plugin_int *plugin;
  struct st_mysql_sys_var *plugin_var;

  sys_var_pluginvar(sys_var_chain *chain, const char *name_arg,
                    st_plugin_int *p, st_mysql_sys_var *plugin_var_arg,
                    const char *substitute);
  sys_var_pluginvar *cast_pluginvar() override { return this; }
  uchar* real_value_ptr(THD *thd, enum_var_type type) const;
  TYPELIB* plugin_var_typelib(void) const;
  const uchar* do_value_ptr(THD *thd, enum_var_type type, const LEX_CSTRING *base) const;
  const uchar* session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return do_value_ptr(thd, OPT_SESSION, base); }
  const uchar* global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return do_value_ptr(thd, OPT_GLOBAL, base); }
  const uchar *default_value_ptr(THD *thd) const override
  { return do_value_ptr(thd, OPT_DEFAULT, 0); }
  bool do_check(THD *thd, set_var *var) override;
  void session_save_default(THD *thd, set_var *var) override {}
  void global_save_default(THD *thd, set_var *var) override {}
  bool session_update(THD *thd, set_var *var) override;
  bool global_update(THD *thd, set_var *var) override;
  bool session_is_default(THD *thd) override;
};


/* prototypes */
static void plugin_load(MEM_ROOT *tmp_root);
static bool plugin_load_list(MEM_ROOT *, const char *);
static int test_plugin_options(MEM_ROOT *, struct st_plugin_int *,
                               int *, char **);
static bool register_builtin(struct st_maria_plugin *, struct st_plugin_int *,
                             struct st_plugin_int **);
static void unlock_variables(THD *thd, struct system_variables *vars);
static void cleanup_variables(struct system_variables *vars);
static void plugin_vars_free_values(st_mysql_sys_var **vars);
static void restore_ptr_backup(uint n, st_ptr_backup *backup);
static void intern_plugin_unlock(LEX *lex, plugin_ref plugin);
static void reap_plugins(void);

bool plugin_is_forced(struct st_plugin_int *p)
{
  return p->load_option == PLUGIN_FORCE ||
         p->load_option == PLUGIN_FORCE_PLUS_PERMANENT;
}

/**
   Check if the provided path is valid in the sense that it does cause
   a relative reference outside the directory.

   @note Currently, this function only check if there are any
   characters in FN_DIRSEP in the string, but it might change in the
   future.

   @code
   check_valid_path("../foo.so") -> true
   check_valid_path("foo.so") -> false
   @endcode
 */
bool check_valid_path(const char *path, size_t len)
{
  size_t prefix= my_strcspn(files_charset_info, path, path + len, FN_DIRSEP);
  return  prefix < len;
}

static void fix_dl_name(MEM_ROOT *root, LEX_CSTRING *dl)
{
  const Lex_ident_plugin so_ext(STRING_WITH_LEN(SO_EXT));
  if (dl->length < so_ext.length ||
      !so_ext.streq(Lex_cstring(dl->str + dl->length - so_ext.length,
                                so_ext.length)))
  {
    size_t s_size= dl->length + so_ext.length + 1;
    char *s= (char*)alloc_root(root, s_size);
    memcpy(s, dl->str, dl->length);
    safe_strcpy(s + dl->length, s_size - dl->length, SO_EXT);
    dl->str= s;
    dl->length+= so_ext.length;
  }
}


/****************************************************************************
  Value type thunks, allows the C world to play in the C++ world
****************************************************************************/

static int item_value_type(struct st_mysql_value *value)
{
  switch (((st_item_value_holder*)value)->item->result_type()) {
  case INT_RESULT:
    return MYSQL_VALUE_TYPE_INT;
  case REAL_RESULT:
    return MYSQL_VALUE_TYPE_REAL;
  default:
    return MYSQL_VALUE_TYPE_STRING;
  }
}

static const char *item_val_str(struct st_mysql_value *value,
                                char *buffer, int *length)
{
  size_t org_length= *length;
  String str(buffer, org_length, system_charset_info), *res;
  if (!(res= ((st_item_value_holder*)value)->item->val_str(&str)))
    return NULL;
  *length= res->length();
  if (res->ptr() == buffer && res->length() < org_length)
  {
    buffer[res->length()]= 0;
    return buffer;
  }

  /*
    Lets be nice and create a temporary string since the
    buffer was too small
  */
  return current_thd->strmake(res->ptr(), res->length());
}


static int item_val_int(struct st_mysql_value *value, long long *buf)
{
  Item *item= ((st_item_value_holder*)value)->item;
  *buf= item->val_int();
  if (item->is_null())
    return 1;
  return 0;
}

static int item_is_unsigned(struct st_mysql_value *value)
{
  Item *item= ((st_item_value_holder*)value)->item;
  return item->unsigned_flag;
}

static int item_val_real(struct st_mysql_value *value, double *buf)
{
  Item *item= ((st_item_value_holder*)value)->item;
  *buf= item->val_real();
  if (item->is_null())
    return 1;
  return 0;
}


/****************************************************************************
  Plugin support code
****************************************************************************/

#ifdef HAVE_DLOPEN

static struct st_plugin_dl *plugin_dl_find(const LEX_CSTRING *dl)
{
  size_t i;
  struct st_plugin_dl *tmp;
  DBUG_ENTER("plugin_dl_find");
  for (i= 0; i < plugin_dl_array.elements; i++)
  {
    tmp= *dynamic_element(&plugin_dl_array, i, struct st_plugin_dl **);
    if (tmp->ref_count &&
        ! files_charset_info->strnncoll(dl->str, dl->length,
                                        tmp->dl.str, tmp->dl.length))
      DBUG_RETURN(tmp);
  }
  DBUG_RETURN(0);
}


static st_plugin_dl *plugin_dl_insert_or_reuse(struct st_plugin_dl *plugin_dl)
{
  size_t i;
  struct st_plugin_dl *tmp;
  DBUG_ENTER("plugin_dl_insert_or_reuse");
  for (i= 0; i < plugin_dl_array.elements; i++)
  {
    tmp= *dynamic_element(&plugin_dl_array, i, struct st_plugin_dl **);
    if (! tmp->ref_count)
    {
      memcpy(tmp, plugin_dl, sizeof(struct st_plugin_dl));
      DBUG_RETURN(tmp);
    }
  }
  if (insert_dynamic(&plugin_dl_array, (uchar*)&plugin_dl))
    DBUG_RETURN(0);
  tmp= *dynamic_element(&plugin_dl_array, plugin_dl_array.elements - 1,
                        struct st_plugin_dl **)=
      (struct st_plugin_dl *) memdup_root(&plugin_mem_root, (uchar*)plugin_dl,
                                           sizeof(struct st_plugin_dl));
  DBUG_RETURN(tmp);
}
#else
static struct st_plugin_dl *plugin_dl_find(const LEX_STRING *)
{
  return 0;
}
#endif /* HAVE_DLOPEN */


static void free_plugin_mem(struct st_plugin_dl *p)
{
#ifdef HAVE_DLOPEN
  if (p->ptr_backup)
  {
    DBUG_ASSERT(p->nbackups);
    DBUG_ASSERT(p->handle);
    restore_ptr_backup(p->nbackups, p->ptr_backup);
    my_free(p->ptr_backup);
  }
  if (p->handle)
    dlclose(p->handle);
#endif
  my_free(const_cast<char*>(p->dl.str));
  if (p->allocated)
    my_free(p->plugins);
}


/**
  Reads data from mysql plugin interface

  @param plugin_dl       Structure where the data should be put
  @param sym             Reverence on version info
  @param dlpath          Path to the module
  @param MyFlags         Where errors should be reported (0 or ME_ERROR_LOG)

  @retval FALSE OK
  @retval TRUE  ERROR
*/

#ifdef HAVE_DLOPEN
static my_bool read_mysql_plugin_info(struct st_plugin_dl *plugin_dl,
                                      void *sym, char *dlpath, myf MyFlags)
{
  DBUG_ENTER("read_maria_plugin_info");
  /* Determine interface version */
  if (!sym)
  {
    my_error(ER_CANT_FIND_DL_ENTRY, MyFlags, plugin_interface_version_sym, dlpath);
    DBUG_RETURN(TRUE);
  }
  plugin_dl->mariaversion= 0;
  plugin_dl->mysqlversion= *(int *)sym;
  /* Versioning */
  if (plugin_dl->mysqlversion < min_plugin_interface_version ||
      (plugin_dl->mysqlversion >> 8) > (MYSQL_PLUGIN_INTERFACE_VERSION >> 8))
  {
    my_error(ER_CANT_OPEN_LIBRARY, MyFlags, dlpath, ENOEXEC,
                 "plugin interface version mismatch");
    DBUG_RETURN(TRUE);
  }
  /* Find plugin declarations */
  if (!(sym= dlsym(plugin_dl->handle, plugin_declarations_sym)))
  {
    my_error(ER_CANT_FIND_DL_ENTRY, MyFlags, plugin_declarations_sym, dlpath);
    DBUG_RETURN(TRUE);
  }

  /* convert mysql declaration to maria one */
  {
    int i;
    uint sizeof_st_plugin;
    struct st_mysql_plugin *old;
    struct st_maria_plugin *cur;
    char *ptr= (char *)sym;

    if ((sym= dlsym(plugin_dl->handle, sizeof_st_plugin_sym)))
      sizeof_st_plugin= *(int *)sym;
    else
    {
      DBUG_ASSERT(min_plugin_interface_version == 0);
      sizeof_st_plugin= (int)offsetof(struct st_mysql_plugin, version);
    }

    for (i= 0;
         ((struct st_mysql_plugin *)(ptr + i * sizeof_st_plugin))->info;
         i++)
      /* no op */;

    cur= (struct st_maria_plugin*)
          my_malloc(key_memory_mysql_plugin, (i + 1) * sizeof(struct st_maria_plugin),
                    MYF(MY_ZEROFILL|MY_WME));
    if (!cur)
    {
      my_error(ER_OUTOFMEMORY, MyFlags,
                   static_cast<int>(plugin_dl->dl.length));
      DBUG_RETURN(TRUE);
    }
    /*
      All st_plugin fields not initialized in the plugin explicitly, are
      set to 0. It matches C standard behaviour for struct initializers that
      have less values than the struct definition.
    */
    for (i=0;
         (old= (struct st_mysql_plugin *)(ptr + i * sizeof_st_plugin))->info;
         i++)
    {

      cur[i].type= old->type;
      cur[i].info= old->info;
      cur[i].name= old->name;
      cur[i].author= old->author;
      cur[i].descr= old->descr;
      cur[i].license= old->license;
      cur[i].init= old->init;
      cur[i].deinit= old->deinit;
      cur[i].version= old->version;
      cur[i].status_vars= old->status_vars;
      cur[i].system_vars= old->system_vars;
      /*
        Something like this should be added to process
        new mysql plugin versions:
        if (plugin_dl->mysqlversion > 0x0101)
        {
           cur[i].newfield= CONSTANT_MEANS_UNKNOWN;
        }
        else
        {
           cur[i].newfield= old->newfield;
        }
      */
      /* Maria only fields */
      cur[i].version_info= "Unknown";
      cur[i].maturity= MariaDB_PLUGIN_MATURITY_UNKNOWN;
    }
    plugin_dl->allocated= true;
    plugin_dl->plugins= (struct st_maria_plugin *)cur;
  }

  DBUG_RETURN(FALSE);
}


/**
  Reads data from maria plugin interface

  @param plugin_dl       Structure where the data should be put
  @param sym             Reverence on version info
  @param dlpath          Path to the module
  @param MyFlags         Where errors should be reported (0 or ME_ERROR_LOG)

  @retval FALSE OK
  @retval TRUE  ERROR
*/

static my_bool read_maria_plugin_info(struct st_plugin_dl *plugin_dl,
                                      void *sym, char *dlpath, myf MyFlags)
{
  DBUG_ENTER("read_maria_plugin_info");

  /* Determine interface version */
  if (!(sym))
  {
    /*
      Actually this branch impossible because in case of absence of maria
      version we try mysql version.
    */
    my_error(ER_CANT_FIND_DL_ENTRY, MyFlags,
             maria_plugin_interface_version_sym, dlpath);
    DBUG_RETURN(TRUE);
  }
  plugin_dl->mariaversion= *(int *)sym;
  plugin_dl->mysqlversion= 0;
  /* Versioning */
  if (plugin_dl->mariaversion < min_maria_plugin_interface_version ||
      (plugin_dl->mariaversion >> 8) > (MARIA_PLUGIN_INTERFACE_VERSION >> 8))
  {
    my_error(ER_CANT_OPEN_LIBRARY, MyFlags, dlpath, ENOEXEC,
                 "plugin interface version mismatch");
    DBUG_RETURN(TRUE);
  }
  /* Find plugin declarations */
  if (!(sym= dlsym(plugin_dl->handle, maria_plugin_declarations_sym)))
  {
    my_error(ER_CANT_FIND_DL_ENTRY, MyFlags, maria_plugin_declarations_sym, dlpath);
    DBUG_RETURN(TRUE);
  }
  if (plugin_dl->mariaversion != MARIA_PLUGIN_INTERFACE_VERSION)
  {
    uint sizeof_st_plugin;
    struct st_maria_plugin *old, *cur;
    char *ptr= (char *)sym;

    if ((sym= dlsym(plugin_dl->handle, maria_sizeof_st_plugin_sym)))
      sizeof_st_plugin= *(int *)sym;
    else
    {
      my_error(ER_CANT_FIND_DL_ENTRY, MyFlags, maria_sizeof_st_plugin_sym,
               dlpath);
      DBUG_RETURN(TRUE);
    }

    if (sizeof_st_plugin != sizeof(st_mysql_plugin))
    {
      int i;
      for (i= 0;
           ((struct st_maria_plugin *)(ptr + i * sizeof_st_plugin))->info;
           i++)
        /* no op */;

      cur= (struct st_maria_plugin*)
        my_malloc(key_memory_mysql_plugin, (i + 1) * sizeof(struct st_maria_plugin),
                  MYF(MY_ZEROFILL|MY_WME));
      if (!cur)
      {
        my_error(ER_OUTOFMEMORY, MyFlags,
                     static_cast<int>(plugin_dl->dl.length));
        DBUG_RETURN(TRUE);
      }
      /*
        All st_plugin fields not initialized in the plugin explicitly, are
        set to 0. It matches C standard behaviour for struct initializers that
        have less values than the struct definition.
      */
      for (i=0;
           (old= (struct st_maria_plugin *)(ptr + i * sizeof_st_plugin))->info;
           i++)
        memcpy(cur + i, old, MY_MIN(sizeof(cur[i]), sizeof_st_plugin));

      sym= cur;
      plugin_dl->allocated= true;
    }
    else
      sym= ptr;
  }
  plugin_dl->plugins= (struct st_maria_plugin *)sym;

  DBUG_RETURN(FALSE);
}
#endif /* HAVE_DLOPEN */

static st_plugin_dl *plugin_dl_add(const LEX_CSTRING *dl, myf MyFlags)
{
#ifdef HAVE_DLOPEN
  char dlpath[FN_REFLEN];
  size_t plugin_dir_len,i;
  uint dummy_errors;
  struct st_plugin_dl *tmp= 0, plugin_dl;
  void *sym;
  st_ptr_backup tmp_backup[array_elements(list_of_services)];
  DBUG_ENTER("plugin_dl_add");
  DBUG_PRINT("enter", ("dl->str: '%s', dl->length: %d",
                       dl->str, (int) dl->length));
  mysql_mutex_assert_owner(&LOCK_plugin);
  plugin_dir_len= strlen(opt_plugin_dir);
  /*
    Ensure that the dll doesn't have a path.
    This is done to ensure that only approved libraries from the
    plugin directory are used (to make this even remotely secure).
  */
  if (check_string_char_length((LEX_CSTRING *) dl, 0, NAME_CHAR_LEN,
                               system_charset_info, 1) ||
      check_valid_path(dl->str, dl->length) ||
      plugin_dir_len + dl->length + 1 >= FN_REFLEN)
  {
    my_error(ER_UDF_NO_PATHS, MyFlags);
    DBUG_RETURN(0);
  }
  /* If this dll is already loaded just increase ref_count. */
  if ((tmp= plugin_dl_find(dl)))
  {
    tmp->ref_count++;
    DBUG_RETURN(tmp);
  }
  bzero(&plugin_dl, sizeof(plugin_dl));
  /* Compile dll path */
  strxnmov(dlpath, sizeof(dlpath) - 1, opt_plugin_dir, "/", dl->str, NullS);
  (void) unpack_filename(dlpath, dlpath);
  plugin_dl.ref_count= 1;
  /* Open new dll handle */
  if (!(plugin_dl.handle= dlopen(dlpath, RTLD_NOW)))
  {
    my_error(ER_CANT_OPEN_LIBRARY, MyFlags, dlpath, errno, my_dlerror(dlpath));
    goto ret;
  }
  dlopen_count++;

#ifdef HAVE_LINK_H
  if (global_system_variables.log_warnings > 2)
  {
    struct link_map *lm = (struct link_map*) plugin_dl.handle;
    sql_print_information("Loaded '%s' with offset 0x%zx", dl->str, (size_t)lm->l_addr);
  }
#endif

  /* Checks which plugin interface present and reads info */
  if (!(sym= dlsym(plugin_dl.handle, maria_plugin_interface_version_sym)))
  {
    if (read_mysql_plugin_info(&plugin_dl,
                               dlsym(plugin_dl.handle,
                                     plugin_interface_version_sym),
                               dlpath,
                               MyFlags))
      goto ret;
  }
  else
  {
    if (read_maria_plugin_info(&plugin_dl, sym, dlpath, MyFlags))
      goto ret;
  }

  /* link the services in */
  for (i= 0; i < array_elements(list_of_services); i++)
  {
    if ((sym= dlsym(plugin_dl.handle, list_of_services[i].name)))
    {
      void **ptr= (void **)sym;
      uint ver= (uint)(intptr)*ptr;
      if (ver > list_of_services[i].version ||
        (ver >> 8) < (list_of_services[i].version >> 8))
      {
        char buf[MYSQL_ERRMSG_SIZE];
        my_snprintf(buf, sizeof(buf),
                    "service '%s' interface version mismatch",
                    list_of_services[i].name);
        my_error(ER_CANT_OPEN_LIBRARY, MyFlags, dlpath, ENOEXEC, buf);
        goto ret;
      }
      tmp_backup[plugin_dl.nbackups++].save(ptr);
      *ptr= list_of_services[i].service;
    }
  }

  if (plugin_dl.nbackups)
  {
    size_t bytes= plugin_dl.nbackups * sizeof(plugin_dl.ptr_backup[0]);
    plugin_dl.ptr_backup= (st_ptr_backup *)my_malloc(key_memory_mysql_plugin_dl,
                                                     bytes, MYF(0));
    if (!plugin_dl.ptr_backup)
    {
      restore_ptr_backup(plugin_dl.nbackups, tmp_backup);
      my_error(ER_OUTOFMEMORY, MyFlags, bytes);
      goto ret;
    }
    memcpy(plugin_dl.ptr_backup, tmp_backup, bytes);
  }

  /* Duplicate and convert dll name */
  plugin_dl.dl.length= dl->length * files_charset_info->mbmaxlen + 1;
  if (! (plugin_dl.dl.str= (char*) my_malloc(key_memory_mysql_plugin_dl,
                                             plugin_dl.dl.length, MYF(0))))
  {
    my_error(ER_OUTOFMEMORY, MyFlags,
                 static_cast<int>(plugin_dl.dl.length));
    goto ret;
  }
  plugin_dl.dl.length= copy_and_convert((char*) plugin_dl.dl.str,
                                        plugin_dl.dl.length,
                                        files_charset_info, dl->str,
                                        dl->length, system_charset_info,
                                        &dummy_errors);
  ((char*) plugin_dl.dl.str)[plugin_dl.dl.length]= 0;
  /* Add this dll to array */
  if (! (tmp= plugin_dl_insert_or_reuse(&plugin_dl)))
  {
    my_error(ER_OUTOFMEMORY, MyFlags,
                 static_cast<int>(sizeof(struct st_plugin_dl)));
    goto ret;
  }

ret:
  if (!tmp)
    free_plugin_mem(&plugin_dl);

  DBUG_RETURN(tmp);

#else
  DBUG_ENTER("plugin_dl_add");
  my_error(ER_FEATURE_DISABLED, MyFlags, "plugin", "HAVE_DLOPEN");
  DBUG_RETURN(0);
#endif
}


static void plugin_dl_del(struct st_plugin_dl *plugin_dl)
{
  DBUG_ENTER("plugin_dl_del");

  if (!plugin_dl)
    DBUG_VOID_RETURN;

  mysql_mutex_assert_owner(&LOCK_plugin);

  /* Do not remove this element, unless no other plugin uses this dll. */
  if (! --plugin_dl->ref_count)
  {
    free_plugin_mem(plugin_dl);
    bzero(plugin_dl, sizeof(struct st_plugin_dl));
  }

  DBUG_VOID_RETURN;
}


static struct st_plugin_int *plugin_find_internal(const LEX_CSTRING *name,
                                                  int type)
{
  uint i;
  DBUG_ENTER("plugin_find_internal");
  if (! initialized)
    DBUG_RETURN(0);

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (type == MYSQL_ANY_PLUGIN)
  {
    for (i= 0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
    {
      struct st_plugin_int *plugin= (st_plugin_int *)
        my_hash_search(&plugin_hash[i], (const uchar *)name->str, name->length);
      if (plugin)
        DBUG_RETURN(plugin);
    }
  }
  else
    DBUG_RETURN((st_plugin_int *)
        my_hash_search(&plugin_hash[type], (const uchar *)name->str,
                       name->length));
  DBUG_RETURN(0);
}


static SHOW_COMP_OPTION plugin_status(const LEX_CSTRING *name, int type)
{
  SHOW_COMP_OPTION rc= SHOW_OPTION_NO;
  struct st_plugin_int *plugin;
  DBUG_ENTER("plugin_is_ready");
  mysql_mutex_lock(&LOCK_plugin);
  if ((plugin= plugin_find_internal(name, type)))
  {
    rc= SHOW_OPTION_DISABLED;
    if (plugin->state == PLUGIN_IS_READY)
      rc= SHOW_OPTION_YES;
  }
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(rc);
}


bool plugin_is_ready(const LEX_CSTRING *name, int type)
{
  bool rc= FALSE;
  if (plugin_status(name, type) == SHOW_OPTION_YES)
    rc= TRUE;
  return rc;
}


SHOW_COMP_OPTION plugin_status(const char *name, size_t len, int type)
{
  LEX_CSTRING plugin_name= { name, len };
  return plugin_status(&plugin_name, type);
}


/*
  If LEX is passed non-NULL, an automatic unlock of the plugin will happen
  in the LEX destructor.
*/
static plugin_ref intern_plugin_lock(LEX *lex, plugin_ref rc,
                                     uint state_mask= PLUGIN_IS_READY |
                                                      PLUGIN_IS_UNINITIALIZED |
                                                      PLUGIN_IS_DELETED)
{
  st_plugin_int *pi= plugin_ref_to_int(rc);
  DBUG_ENTER("intern_plugin_lock");

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (pi->state & state_mask)
  {
    plugin_ref plugin;
#ifdef DBUG_OFF
    /*
      In optimized builds we don't do reference counting for built-in
      (plugin->plugin_dl == 0) plugins.
    */
    if (!pi->plugin_dl)
      DBUG_RETURN(pi);

    plugin= pi;
#else
    /*
      For debugging, we do an additional malloc which allows the
      memory manager and/or valgrind to track locked references and
      double unlocks to aid resolving reference counting problems.
    */
    if (!(plugin= (plugin_ref) my_malloc(PSI_NOT_INSTRUMENTED, sizeof(pi),
                                         MYF(MY_WME))))
      DBUG_RETURN(NULL);

    *plugin= pi;
#endif
    pi->ref_count++;
    DBUG_PRINT("lock",("thd: %p  plugin: \"%s\" LOCK ref_count: %d",
                       current_thd, pi->name.str, pi->ref_count));

    if (lex)
      insert_dynamic(&lex->plugins, (uchar*)&plugin);
    DBUG_RETURN(plugin);
  }
  DBUG_RETURN(NULL);
}


/*
  Notes on lifetime:

  If THD is passed as non-NULL (and with a non-NULL thd->lex), an entry is made
  in the thd->lex which will cause an automatic unlock of the plugin in the LEX
  destructor. In this case, no manual unlock must be done.

  Otherwise, when passing a NULL THD, the caller must arrange that plugin
  unlock happens later.
*/
plugin_ref plugin_lock(THD *thd, plugin_ref ptr)
{
  LEX *lex= thd ? thd->lex : 0;
  plugin_ref rc;
  DBUG_ENTER("plugin_lock");

#ifdef DBUG_OFF
  /*
    In optimized builds we don't do reference counting for built-in
    (plugin->plugin_dl == 0) plugins.

    Note that we access plugin->plugin_dl outside of LOCK_plugin, and for
    dynamic plugins a 'plugin' could correspond to plugin that was unloaded
    meanwhile!  But because st_plugin_int is always allocated on
    plugin_mem_root, the pointer can never be invalid - the memory is never
    freed.
    Of course, the memory that 'plugin' points to can be overwritten by
    another plugin being loaded, but plugin->plugin_dl can never change
    from zero to non-zero or vice versa.
    That is, it's always safe to check for plugin->plugin_dl==0 even
    without a mutex.
  */
  if (! plugin_dlib(ptr))
  {
    plugin_ref_to_int(ptr)->locks_total++;
    DBUG_RETURN(ptr);
  }
#endif
  mysql_mutex_lock(&LOCK_plugin);
  plugin_ref_to_int(ptr)->locks_total++;
  rc= intern_plugin_lock(lex, ptr);
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(rc);
}


/*
  Notes on lifetime:

  If THD is passed as non-NULL (and with a non-NULL thd->lex), an entry is made
  in the thd->lex which will cause an automatic unlock of the plugin in the LEX
  destructor. In this case, no manual unlock must be done.

  Otherwise, when passing a NULL THD, the caller must arrange that plugin
  unlock happens later.
*/
plugin_ref plugin_lock_by_name(THD *thd, const LEX_CSTRING *name, int type)
{
  LEX *lex= thd ? thd->lex : 0;
  plugin_ref rc= NULL;
  st_plugin_int *plugin;
  DBUG_ENTER("plugin_lock_by_name");
  if (!name->length)
    DBUG_RETURN(NULL);
  mysql_mutex_lock(&LOCK_plugin);
  if ((plugin= plugin_find_internal(name, type)))
    rc= intern_plugin_lock(lex, plugin_int_to_ref(plugin));
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(rc);
}


static st_plugin_int *plugin_insert_or_reuse(struct st_plugin_int *plugin)
{
  size_t i;
  struct st_plugin_int *tmp;
  DBUG_ENTER("plugin_insert_or_reuse");
  for (i= 0; i < plugin_array.elements; i++)
  {
    tmp= *dynamic_element(&plugin_array, i, struct st_plugin_int **);
    if (tmp->state == PLUGIN_IS_FREED)
    {
      memcpy(tmp, plugin, sizeof(struct st_plugin_int));
      DBUG_RETURN(tmp);
    }
  }
  if (insert_dynamic(&plugin_array, (uchar*)&plugin))
    DBUG_RETURN(0);
  tmp= *dynamic_element(&plugin_array, plugin_array.elements - 1,
                        struct st_plugin_int **)=
       (struct st_plugin_int *) memdup_root(&plugin_mem_root, (uchar*)plugin,
                                            sizeof(struct st_plugin_int));
  DBUG_RETURN(tmp);
}


/*
  NOTE
    Requires that a write-lock is held on LOCK_system_variables_hash
*/
static enum install_status plugin_add(MEM_ROOT *tmp_root, bool if_not_exists,
                       const LEX_CSTRING *name, LEX_CSTRING *dl, myf MyFlags)
{
  struct st_plugin_int tmp, *maybe_dupe;
  struct st_maria_plugin *plugin;
  uint oks= 0, errs= 0, dupes= 0;
  DBUG_ENTER("plugin_add");
  DBUG_PRINT("enter", ("name: %s  dl: %s", name->str, dl->str));

  if (name->str && plugin_find_internal(name, MYSQL_ANY_PLUGIN))
  {
    if (if_not_exists)
      MyFlags|= ME_NOTE;
    my_error(ER_PLUGIN_INSTALLED, MyFlags, name->str);
    DBUG_RETURN(if_not_exists ? INSTALL_FAIL_WARN_OK : INSTALL_FAIL_NOT_OK);
  }
  /* Clear the whole struct to catch future extensions. */
  bzero((char*) &tmp, sizeof(tmp));
  fix_dl_name(tmp_root, dl);
  if (! (tmp.plugin_dl= plugin_dl_add(dl, MyFlags)))
    DBUG_RETURN(INSTALL_FAIL_NOT_OK);
  /* Find plugin by name */
  for (plugin= tmp.plugin_dl->plugins; plugin->info; plugin++)
  {
    tmp.name.str= (char *)plugin->name;
    tmp.name.length= strlen(plugin->name);

    if (plugin->type < 0 || plugin->type >= MYSQL_MAX_PLUGIN_TYPE_NUM)
      continue; // invalid plugin type

    if (plugin->type == MYSQL_UDF_PLUGIN ||
        (plugin->type == MariaDB_PASSWORD_VALIDATION_PLUGIN &&
         tmp.plugin_dl->mariaversion == 0))
      continue; // unsupported plugin type

    if (name->str && !Lex_ident_plugin(*name).streq(tmp.name))
      continue; // plugin name doesn't match

    if (!name->str &&
        (maybe_dupe= plugin_find_internal(&tmp.name, MYSQL_ANY_PLUGIN)))
    {
      if (plugin->name != maybe_dupe->plugin->name)
      {
        my_error(ER_UDF_EXISTS, MyFlags, plugin->name);
        DBUG_RETURN(INSTALL_FAIL_NOT_OK);
      }
      dupes++;
      continue; // already installed
    }
    struct st_plugin_int *tmp_plugin_ptr;
    if (*(int*)plugin->info <
        min_plugin_info_interface_version[plugin->type] ||
        ((*(int*)plugin->info) >> 8) >
        (cur_plugin_info_interface_version[plugin->type] >> 8))
    {
      char buf[256];
      strxnmov(buf, sizeof(buf) - 1, "API version for ",
               plugin_type_names[plugin->type].str,
               " plugin ", tmp.name.str,
               " not supported by this version of the server", NullS);
      my_error(ER_CANT_OPEN_LIBRARY, MyFlags, dl->str, ENOEXEC, buf);
      goto err;
    }

    if (plugin_maturity_map[plugin->maturity] < plugin_maturity)
    {
      char buf[256];
      strxnmov(buf, sizeof(buf) - 1, "Loading of ",
               plugin_maturity_names[plugin->maturity],
               " plugin ", tmp.name.str,
               " is prohibited by --plugin-maturity=",
               plugin_maturity_names[plugin_maturity],
               NullS);
      my_error(ER_CANT_OPEN_LIBRARY, MyFlags, dl->str, EPERM, buf);
      goto err;
    }
    else if (plugin_maturity_map[plugin->maturity] < SERVER_MATURITY_LEVEL)
    {
      sql_print_warning("Plugin '%s' is of maturity level %s while the server is %s",
        tmp.name.str,
        plugin_maturity_names[plugin->maturity],
        plugin_maturity_names[SERVER_MATURITY_LEVEL]);
    }

    tmp.plugin= plugin;
    tmp.ref_count= 0;
    tmp.state= PLUGIN_IS_UNINITIALIZED;
    tmp.load_option= PLUGIN_ON;

    if (!(tmp_plugin_ptr= plugin_insert_or_reuse(&tmp)))
      goto err;
    if (my_hash_insert(&plugin_hash[plugin->type], (uchar*)tmp_plugin_ptr))
      tmp_plugin_ptr->state= PLUGIN_IS_FREED;
    init_alloc_root(key_memory_plugin_int_mem_root, &tmp_plugin_ptr->mem_root,
                    4096, 4096, MYF(0));

    if (name->str)
      DBUG_RETURN(INSTALL_GOOD); // all done

    oks++;
    tmp.plugin_dl->ref_count++;
    continue; // otherwise - go on

err:
    errs++;
    if (name->str)
      break;
  }

  DBUG_ASSERT(!name->str || !dupes); // dupes is ONLY for name->str == 0

  if (errs == 0 && oks == 0 && !dupes) // no plugin was found
    my_error(ER_CANT_FIND_DL_ENTRY, MyFlags, name->str, tmp.plugin_dl->dl.str);


  plugin_dl_del(tmp.plugin_dl);
  if (errs > 0 || oks + dupes == 0)
    DBUG_RETURN(INSTALL_FAIL_NOT_OK);
  DBUG_RETURN(INSTALL_GOOD);
}

static void plugin_variables_deinit(struct st_plugin_int *plugin)
{

  for (sys_var *var= plugin->system_vars; var; var= var->next)
    (*var->test_load)= FALSE;
  mysql_del_sys_var_chain(plugin->system_vars);
}

static void plugin_deinitialize(struct st_plugin_int *plugin, bool ref_check)
{
  /*
    we don't want to hold the LOCK_plugin mutex as it may cause
    deinitialization to deadlock if plugins have worker threads
    with plugin locks
  */
  mysql_mutex_assert_not_owner(&LOCK_plugin);

  if (plugin->plugin->status_vars)
  {
    /*
      historical ndb behavior caused MySQL plugins to specify
      status var names in full, with the plugin name prefix.
      this was never fixed in MySQL.
      MariaDB fixes that but supports MySQL style too.
    */
    SHOW_VAR *show_vars= plugin->plugin->status_vars;
    SHOW_VAR tmp_array[2]= {
      {plugin->plugin->name, (char*)plugin->plugin->status_vars, SHOW_ARRAY},
      {0, 0, SHOW_UNDEF}
    };
    if (strncasecmp(show_vars->name, plugin->name.str, plugin->name.length))
      show_vars= tmp_array;

    remove_status_vars(show_vars);
  }

  plugin_type_init deinit= plugin_type_deinitialize[plugin->plugin->type];
  if (!deinit)
    deinit= (plugin_type_init)(plugin->plugin->deinit);

  if (deinit && deinit(plugin))
  {
    if (THD *thd= current_thd)
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                   WARN_PLUGIN_BUSY, ER_THD(thd, WARN_PLUGIN_BUSY));
  }
  else
    plugin->state= PLUGIN_IS_UNINITIALIZED; // free to unload

  if (ref_check && plugin->ref_count)
    sql_print_error("Plugin '%s' has ref_count=%d after deinitialization.",
                    plugin->name.str, plugin->ref_count);
  plugin_variables_deinit(plugin);
}

static void plugin_del(struct st_plugin_int *plugin, uint del_mask)
{
  DBUG_ENTER("plugin_del");
  mysql_mutex_assert_owner(&LOCK_plugin);
  del_mask|= PLUGIN_IS_UNINITIALIZED | PLUGIN_IS_DISABLED; // always use these
  if (!(plugin->state & del_mask))
    DBUG_VOID_RETURN;
  /* Free allocated strings before deleting the plugin. */
  plugin_vars_free_values(plugin->plugin->system_vars);
  restore_ptr_backup(plugin->nbackups, plugin->ptr_backup);
  if (plugin->plugin_dl)
  {
    my_hash_delete(&plugin_hash[plugin->plugin->type], (uchar*)plugin);
    plugin_dl_del(plugin->plugin_dl);
    plugin->state= PLUGIN_IS_FREED;
    free_root(&plugin->mem_root, MYF(0));
  }
  else
    plugin->state= PLUGIN_IS_UNINITIALIZED;
  DBUG_VOID_RETURN;
}

static void reap_plugins(void)
{
  size_t count;
  struct st_plugin_int *plugin, **reap, **list;

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (!reap_needed)
    return;

  reap_needed= false;
  count= plugin_array.elements;
  reap= (struct st_plugin_int **)my_alloca(sizeof(plugin)*(count+1));
  *(reap++)= NULL;

  for (uint i=0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
  {
    HASH *hash= plugin_hash + plugin_type_initialization_order[i];
    for (uint j= 0; j < hash->records; j++)
    {
      plugin= (struct st_plugin_int *) my_hash_element(hash, j);
      if (plugin->state == PLUGIN_IS_DELETED && !plugin->ref_count)
      {
        /* change the status flag to prevent reaping by another thread */
        plugin->state= PLUGIN_IS_DYING;
        *(reap++)= plugin;
      }
    }
  }

  mysql_mutex_unlock(&LOCK_plugin);

  list= reap;
  while ((plugin= *(--list)))
    plugin_deinitialize(plugin, true);

  mysql_mutex_lock(&LOCK_plugin);

  while ((plugin= *(--reap)))
    plugin_del(plugin, 0);

  my_afree(reap);
}

static void intern_plugin_unlock(LEX *lex, plugin_ref plugin)
{
  ssize_t i;
  st_plugin_int *pi;
  DBUG_ENTER("intern_plugin_unlock");

  mysql_mutex_assert_owner(&LOCK_plugin);

  if (!plugin)
    DBUG_VOID_RETURN;

  pi= plugin_ref_to_int(plugin);

#ifdef DBUG_OFF
  if (!pi->plugin_dl)
    DBUG_VOID_RETURN;
#else
  my_free(plugin);
#endif

  if (lex)
  {
    /*
      Remove one instance of this plugin from the use list.
      We are searching backwards so that plugins locked last
      could be unlocked faster - optimizing for LIFO semantics.
    */
    for (i= lex->plugins.elements - 1; i >= 0; i--)
      if (plugin == *dynamic_element(&lex->plugins, i, plugin_ref*))
      {
        delete_dynamic_element(&lex->plugins, i);
        break;
      }
    DBUG_ASSERT(i >= 0);
  }

  DBUG_ASSERT(pi->ref_count);
  pi->ref_count--;

  DBUG_PRINT("lock",("thd: %p  plugin: \"%s\" UNLOCK ref_count: %d",
                     current_thd, pi->name.str, pi->ref_count));

  if (pi->state == PLUGIN_IS_DELETED && !pi->ref_count)
    reap_needed= true;

  DBUG_VOID_RETURN;
}


void plugin_unlock(THD *thd, plugin_ref plugin)
{
  LEX *lex= thd ? thd->lex : 0;
  DBUG_ENTER("plugin_unlock");
  if (!plugin)
    DBUG_VOID_RETURN;
#ifdef DBUG_OFF
  /* built-in plugins don't need ref counting */
  if (!plugin_dlib(plugin))
    DBUG_VOID_RETURN;
#endif
  mysql_mutex_lock(&LOCK_plugin);
  intern_plugin_unlock(lex, plugin);
  reap_plugins();
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_VOID_RETURN;
}


void plugin_unlock_list(THD *thd, plugin_ref *list, size_t count)
{
  LEX *lex= thd ? thd->lex : 0;
  DBUG_ENTER("plugin_unlock_list");
  if (count == 0)
    DBUG_VOID_RETURN;

  DBUG_ASSERT(list);
  mysql_mutex_lock(&LOCK_plugin);
  while (count--)
    intern_plugin_unlock(lex, *list++);
  reap_plugins();
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_VOID_RETURN;
}

static void print_init_failed_error(st_plugin_int *p)
{
  sql_print_error("Plugin '%s' registration as a %s failed.",
                  p->name.str,
                  plugin_type_names[p->plugin->type].str);
}

static int plugin_do_initialize(struct st_plugin_int *plugin, uint &state)
{
  DBUG_ENTER("plugin_do_initialize");
  mysql_mutex_assert_not_owner(&LOCK_plugin);
  plugin_type_init init= plugin_type_initialize[plugin->plugin->type];
  if (!init)
    init= plugin->plugin->init;
  if (init)
  {
    if (int ret= init(plugin))
    {
      /* Plugin init failed and did not requested a retry */
      if (ret != HA_ERR_RETRY_INIT)
        print_init_failed_error(plugin);
      DBUG_RETURN(ret);
    }
  }
  state= PLUGIN_IS_READY; // plugin->init() succeeded

  if (plugin->plugin->status_vars)
  {
    /*
      historical ndb behavior caused MySQL plugins to specify
      status var names in full, with the plugin name prefix.
      this was never fixed in MySQL.
      MariaDB fixes that but supports MySQL style too.
    */
    SHOW_VAR *show_vars= plugin->plugin->status_vars;
    SHOW_VAR tmp_array[2]= {{plugin->plugin->name,
                             (char *) plugin->plugin->status_vars, SHOW_ARRAY},
                            {0, 0, SHOW_UNDEF}};
    if (strncasecmp(show_vars->name, plugin->name.str, plugin->name.length))
      show_vars= tmp_array;

    if (add_status_vars(show_vars))
      DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

static int plugin_initialize(MEM_ROOT *tmp_root, struct st_plugin_int *plugin,
                             int *argc, char **argv, bool options_only)
{
  int ret= 1;
  DBUG_ENTER("plugin_initialize");

  mysql_mutex_assert_owner(&LOCK_plugin);
  uint state= plugin->state;
  DBUG_ASSERT(state == PLUGIN_IS_UNINITIALIZED);

  mysql_mutex_unlock(&LOCK_plugin);

  mysql_prlock_wrlock(&LOCK_system_variables_hash);
  if (test_plugin_options(tmp_root, plugin, argc, argv))
    state= PLUGIN_IS_DISABLED;
  mysql_prlock_unlock(&LOCK_system_variables_hash);

  if (options_only || state == PLUGIN_IS_DISABLED)
  {
    ret= !options_only && plugin_is_forced(plugin);
    state= PLUGIN_IS_DISABLED;
  }
  else
    ret= plugin_do_initialize(plugin, state);

  if (ret && ret != HA_ERR_RETRY_INIT)
    plugin_variables_deinit(plugin);

  mysql_mutex_lock(&LOCK_plugin);
  plugin->state= state;

  DBUG_RETURN(ret);
}


extern "C" const uchar *get_plugin_hash_key(const void *, size_t *, my_bool);
extern "C" const uchar *get_bookmark_hash_key(const void *, size_t *, my_bool);


const uchar *get_plugin_hash_key(const void *buff, size_t *length, my_bool)
{
  auto plugin= static_cast<const st_plugin_int *>(buff);
  *length= plugin->name.length;
  return reinterpret_cast<const uchar *>(plugin->name.str);
}


const uchar *get_bookmark_hash_key(const void *buff, size_t *length, my_bool)
{
  auto var= static_cast<const st_bookmark *>(buff);
  *length= var->name_len + 1;
  return reinterpret_cast<const uchar *>(var->key);
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_plugin;

static PSI_mutex_info all_plugin_mutexes[]=
{
  { &key_LOCK_plugin, "LOCK_plugin", PSI_FLAG_GLOBAL}
};

static PSI_memory_info all_plugin_memory[]=
{
  { &key_memory_plugin_mem_root, "plugin_mem_root", PSI_FLAG_GLOBAL},
  { &key_memory_plugin_int_mem_root, "plugin_int_mem_root", 0},
  { &key_memory_mysql_plugin_dl, "mysql_plugin_dl", 0},
  { &key_memory_mysql_plugin, "mysql_plugin", 0},
  { &key_memory_plugin_bookmark, "plugin_bookmark", PSI_FLAG_GLOBAL}
};

static void init_plugin_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_plugin_mutexes);
  PSI_server->register_mutex(category, all_plugin_mutexes, count);

  count= array_elements(all_plugin_memory);
  mysql_memory_register(category, all_plugin_memory, count);
}
#else
static void init_plugin_psi_keys(void) {}
#endif /* HAVE_PSI_INTERFACE */

/*
  The logic is that we first load and initialize all compiled in plugins.
  From there we load up the dynamic types (assuming we have not been told to
  skip this part).

  Finally we initialize everything, aka the dynamic that have yet to initialize.
*/
int plugin_init(int *argc, char **argv, int flags)
{
  size_t i;
  struct st_maria_plugin **builtins;
  struct st_maria_plugin *plugin;
  struct st_plugin_int tmp, *plugin_ptr, **reap, **retry_end, **retry_start;
  MEM_ROOT tmp_root;
  bool reaped_mandatory_plugin= false;
  bool mandatory= true;
  I_List_iterator<i_string> opt_plugin_load_list_iter(opt_plugin_load_list);
  char plugin_table_engine_name_buf[NAME_CHAR_LEN + 1];
  LEX_CSTRING plugin_table_engine_name= { plugin_table_engine_name_buf, 0 };
  LEX_CSTRING MyISAM= { STRING_WITH_LEN("MyISAM") };
  DBUG_ENTER("plugin_init");

  if (initialized)
    DBUG_RETURN(0);

  dlopen_count =0;

  init_plugin_psi_keys();

  init_alloc_root(key_memory_plugin_mem_root, &plugin_mem_root, 4096, 16384, MYF(0));
  init_alloc_root(key_memory_plugin_mem_root, &plugin_vars_mem_root, 4096, 32768, MYF(0));
  init_alloc_root(PSI_NOT_INSTRUMENTED, &tmp_root, 16384, 32768, MYF(0));

  if (my_hash_init(key_memory_plugin_bookmark, &bookmark_hash, &my_charset_bin, 32, 0, 0,
                   get_bookmark_hash_key, NULL, HASH_UNIQUE))
      goto err;

  /*
    The 80 is from 2016-04-27 when we had 71 default plugins
    Big enough to avoid many mallocs even in future
  */
  if (my_init_dynamic_array(key_memory_mysql_plugin_dl, &plugin_dl_array,
                            sizeof(struct st_plugin_dl *), 16, 16, MYF(0)) ||
      my_init_dynamic_array(key_memory_mysql_plugin, &plugin_array,
                            sizeof(struct st_plugin_int *), 80, 32, MYF(0)))
    goto err;

  for (i= 0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
  {
    if (my_hash_init(key_memory_plugin_mem_root, &plugin_hash[i],
                     Lex_ident_plugin::charset_info(),
                     32, 0, 0,
                     get_plugin_hash_key, NULL, HASH_UNIQUE))
      goto err;
  }

  /* prepare debug_sync service */
  DBUG_ASSERT(strcmp(list_of_services[1].name, "debug_sync_service") == 0);
  list_of_services[1].service= *(void**)&debug_sync_C_callback_ptr;

  /* prepare encryption_keys service */
  finalize_encryption_plugin(0);

  mysql_mutex_lock(&LOCK_plugin);

  initialized= 1;

  /*
    First we register builtin plugins
  */
  if (global_system_variables.log_warnings >= 9)
    sql_print_information("Initializing built-in plugins");

  for (builtins= mysql_mandatory_plugins; *builtins || mandatory; builtins++)
  {
    if (!*builtins)
    {
      builtins= mysql_optional_plugins;
      mandatory= false;
      if (!*builtins)
        break;
    }
    for (plugin= *builtins; plugin->info; plugin++)
    {
      Lex_ident_plugin tmp_plugin_name(Lex_cstring_strlen(plugin->name));
      if (opt_ignore_builtin_innodb &&
          tmp_plugin_name.streq("InnoDB"_Lex_ident_plugin))
        continue;

      bzero(&tmp, sizeof(tmp));
      tmp.plugin= plugin;
      tmp.name= tmp_plugin_name;
      tmp.state= 0;
      tmp.load_option= mandatory ? PLUGIN_FORCE : PLUGIN_ON;

      for (i=0; i < array_elements(override_plugin_load_policy); i++)
      {
        if (tmp_plugin_name.streq(override_plugin_load_policy[i].plugin_name))
        {
          tmp.load_option= override_plugin_load_policy[i].override;
          break;
        }
      }

      tmp.state= PLUGIN_IS_UNINITIALIZED;
      if (register_builtin(plugin, &tmp, &plugin_ptr))
        goto err_unlock;
    }
  }

  /*
    First, we initialize only MyISAM - that should almost always succeed
    (almost always, because plugins can be loaded outside of the server, too).
  */
  plugin_ptr= plugin_find_internal(&MyISAM, MYSQL_STORAGE_ENGINE_PLUGIN);
  DBUG_ASSERT(plugin_ptr || !mysql_mandatory_plugins[0]);
  if (plugin_ptr)
  {
    DBUG_ASSERT(plugin_ptr->load_option == PLUGIN_FORCE);

    if (plugin_initialize(&tmp_root, plugin_ptr, argc, argv, false))
      goto err_unlock;

    /*
      set the global default storage engine variable so that it will
      not be null in any child thread.
    */
    global_system_variables.table_plugin =
      intern_plugin_lock(NULL, plugin_int_to_ref(plugin_ptr));
    DBUG_SLOW_ASSERT(plugin_ptr->ref_count == 1);
  }
  mysql_mutex_unlock(&LOCK_plugin);

  /* Register (not initialize!) all dynamic plugins */
  if (global_system_variables.log_warnings >= 9)
    sql_print_information("Initializing plugins specified on the command line");
  while (i_string *item= opt_plugin_load_list_iter++)
    plugin_load_list(&tmp_root, item->ptr);

  if (!(flags & PLUGIN_INIT_SKIP_PLUGIN_TABLE))
  {
    char path[FN_REFLEN + 1];
    build_table_filename(path, sizeof(path) - 1, "mysql", "plugin", reg_ext, 0);
    Table_type ttype= dd_frm_type(0, path, &plugin_table_engine_name, NULL);
    if (ttype != TABLE_TYPE_NORMAL)
      plugin_table_engine_name=empty_clex_str;
  }

  /*
    Now we initialize all remaining plugins
  */

  mysql_mutex_lock(&LOCK_plugin);
  /* List of plugins to reap */
  reap= (st_plugin_int **) my_alloca((plugin_array.elements+1) * sizeof(void*));
  *(reap++)= NULL;
  /* List of plugins to retry */
  retry_start= retry_end=
    (st_plugin_int **) my_alloca((plugin_array.elements+1) * sizeof(void*));

  for(;;)
  {
    int error;
    for (i=0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
    {
      HASH *hash= plugin_hash + plugin_type_initialization_order[i];
      for (uint idx= 0; idx < hash->records; idx++)
      {
        plugin_ptr= (struct st_plugin_int *) my_hash_element(hash, idx);
        if (plugin_ptr->state == PLUGIN_IS_UNINITIALIZED)
        {
          bool plugin_table_engine= lex_string_eq(&plugin_table_engine_name,
                                                  &plugin_ptr->name);
          bool opts_only= flags & PLUGIN_INIT_SKIP_INITIALIZATION &&
                         (flags & PLUGIN_INIT_SKIP_PLUGIN_TABLE ||
                          !plugin_table_engine);
          error= plugin_initialize(&tmp_root, plugin_ptr, argc, argv,
                                   opts_only);
          if (error)
          {
            plugin_ptr->state= PLUGIN_IS_DYING;
            /* The plugin wants a retry of the initialisation,
               possibly due to dependency on other plugins */
            if (unlikely(error == HA_ERR_RETRY_INIT))
              *(retry_end++)= plugin_ptr;
            else
              *(reap++)= plugin_ptr;
          }
        }
      }
    }
    /* Retry plugins that asked for it */
    while (retry_start < retry_end)
    {
      st_plugin_int **to_re_retry, **retrying;
      for (to_re_retry= retrying= retry_start; retrying < retry_end; retrying++)
      {
        plugin_ptr= *retrying;
        uint state= plugin_ptr->state;
        mysql_mutex_unlock(&LOCK_plugin);
        error= plugin_do_initialize(plugin_ptr, state);
        DBUG_EXECUTE_IF("fail_spider_init_retry", error= 1;);
        mysql_mutex_lock(&LOCK_plugin);
        plugin_ptr->state= state;
        if (error == HA_ERR_RETRY_INIT)
          *(to_re_retry++)= plugin_ptr;
        else if (error)
          *(reap++)= plugin_ptr;
      }
      /* If the retry list has not changed, i.e. if all retry attempts
      result in another retry request, empty the retry list */
      if (to_re_retry == retry_end)
        while (to_re_retry > retry_start)
        {
          plugin_ptr= *(--to_re_retry);
          *(reap++)= plugin_ptr;
          /** `plugin_do_initialize()' did not print any error in this
          case, so we do it here. */
          print_init_failed_error(plugin_ptr);
        }
      retry_end= to_re_retry;
    }

    /* load and init plugins from the plugin table (unless done already) */
    if (flags & PLUGIN_INIT_SKIP_PLUGIN_TABLE)
      break;

    mysql_mutex_unlock(&LOCK_plugin);
    plugin_load(&tmp_root);
    flags|= PLUGIN_INIT_SKIP_PLUGIN_TABLE;
    mysql_mutex_lock(&LOCK_plugin);
  }

  /*
    Check if any plugins have to be reaped
  */
  while ((plugin_ptr= *(--reap)))
  {
    mysql_mutex_unlock(&LOCK_plugin);
    if (plugin_is_forced(plugin_ptr))
      reaped_mandatory_plugin= TRUE;
    plugin_deinitialize(plugin_ptr, true);
    mysql_mutex_lock(&LOCK_plugin);
    plugin_del(plugin_ptr, 0);
  }

  mysql_mutex_unlock(&LOCK_plugin);
  my_afree(retry_start);
  my_afree(reap);
  if (reaped_mandatory_plugin && !opt_help)
    goto err;

  free_root(&tmp_root, MYF(0));

  DBUG_RETURN(0);

err_unlock:
  mysql_mutex_unlock(&LOCK_plugin);
err:
  free_root(&tmp_root, MYF(0));
  DBUG_RETURN(1);
}


static bool register_builtin(struct st_maria_plugin *plugin,
                             struct st_plugin_int *tmp,
                             struct st_plugin_int **ptr)
{
  DBUG_ENTER("register_builtin");
  tmp->ref_count= 0;
  tmp->plugin_dl= 0;

  if (insert_dynamic(&plugin_array, (uchar*)&tmp))
    DBUG_RETURN(1);

  *ptr= *dynamic_element(&plugin_array, plugin_array.elements - 1,
                         struct st_plugin_int **)=
        (struct st_plugin_int *) memdup_root(&plugin_mem_root, (uchar*)tmp,
                                             sizeof(struct st_plugin_int));

  if (my_hash_insert(&plugin_hash[plugin->type],(uchar*) *ptr))
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/*
  called only by plugin_init()
*/

static void plugin_load(MEM_ROOT *tmp_root)
{
  TABLE_LIST tables;
  TABLE *table;
  READ_RECORD read_record_info;
  int error;
  THD *new_thd= new THD(0);
  bool result;
  unsigned long event_class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE] =
  { MYSQL_AUDIT_GENERAL_CLASSMASK };
  DBUG_ENTER("plugin_load");

  if (global_system_variables.log_warnings >= 9)
    sql_print_information("Initializing installed plugins");

  new_thd->thread_stack= (void*) &tables;       // Big stack
  new_thd->store_globals();
  new_thd->set_query_inner((char*) STRING_WITH_LEN("intern:plugin_load"),
                           default_charset_info);
  new_thd->db= MYSQL_SCHEMA_NAME;
  bzero((char*) &new_thd->net, sizeof(new_thd->net));
  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_PLUGIN_NAME, 0, TL_READ);
  tables.open_strategy= TABLE_LIST::OPEN_NORMAL;

  result= open_and_lock_tables(new_thd, &tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT);

  table= tables.table;
  if (result)
  {
    DBUG_PRINT("error",("Can't open plugin table"));
    if (!opt_help)
      sql_print_error("Could not open mysql.plugin table: \"%s\". "
                      "Some plugins may be not loaded",
                      new_thd->get_stmt_da()->message());
    else
      sql_print_warning("Could not open mysql.plugin table: \"%s\". "
                        "Some options may be missing from the help text",
                        new_thd->get_stmt_da()->message());
    goto end;
  }

  if (init_read_record(&read_record_info, new_thd, table, NULL, NULL, 1, 0,
                       FALSE))
  {
    sql_print_error("Could not initialize init_read_record; Plugins not "
                    "loaded");
    goto end;
  }
  table->use_all_columns();
  while (!(error= read_record_info.read_record()))
  {
    DBUG_PRINT("info", ("init plugin record"));
    DBUG_ASSERT(new_thd == table->field[0]->get_thd());
    DBUG_ASSERT(new_thd == table->field[1]->get_thd());
    DBUG_ASSERT(!(new_thd->variables.sql_mode & MODE_PAD_CHAR_TO_FULL_LENGTH));
    LEX_CSTRING name= table->field[0]->val_lex_string_strmake(tmp_root);
    LEX_CSTRING dl= table->field[1]->val_lex_string_strmake(tmp_root);

    if (!name.length || !dl.length)
      continue;

    /*
      Pre-acquire audit plugins for events that may potentially occur
      during [UN]INSTALL PLUGIN.

      When audit event is triggered, audit subsystem acquires interested
      plugins by walking through plugin list. Evidently plugin list
      iterator protects plugin list by acquiring LOCK_plugin, see
      plugin_foreach_with_mask().

      On the other hand plugin_load is acquiring LOCK_plugin
      rather for a long time.

      When audit event is triggered during plugin_load plugin
      list iterator acquires the same lock (within the same thread)
      second time.

      This hack should be removed when LOCK_plugin is fixed so it
      protects only what it supposed to protect.

      See also mysql_install_plugin(), mysql_uninstall_plugin() and
        initialize_audit_plugin()
    */
    if (mysql_audit_general_enabled())
      mysql_audit_acquire_plugins(new_thd, event_class_mask);

    /*
      there're no other threads running yet, so we don't need a mutex.
      but plugin_add() before is designed to work in multi-threaded
      environment, and it uses mysql_mutex_assert_owner(), so we lock
      the mutex here to satisfy the assert
    */
    mysql_mutex_lock(&LOCK_plugin);
    plugin_add(tmp_root, true, &name, &dl, MYF(ME_ERROR_LOG));
    free_root(tmp_root, MYF(MY_MARK_BLOCKS_FREE));
    mysql_mutex_unlock(&LOCK_plugin);
  }
  if (unlikely(error > 0))
    sql_print_error(ER_DEFAULT(ER_GET_ERRNO), my_errno,
                           table->file->table_type());
  end_read_record(&read_record_info);
  table->mark_table_for_reopen();
  close_mysql_tables(new_thd);
end:
  new_thd->db= null_clex_str;                 // Avoid free on thd->db
  delete new_thd;
  DBUG_VOID_RETURN;
}


/*
  called only by plugin_init()
*/
static bool plugin_load_list(MEM_ROOT *tmp_root, const char *list)
{
  char buffer[FN_REFLEN];
  LEX_CSTRING name= {buffer, 0}, dl= {NULL, 0}, *str= &name;
  char *p= buffer;
  DBUG_ENTER("plugin_load_list");
  while (list)
  {
    if (p == buffer + sizeof(buffer) - 1)
    {
      sql_print_error("plugin-load parameter too long");
      DBUG_RETURN(TRUE);
    }

    switch ((*(p++)= *(list++))) {
    case '\0':
      list= NULL; /* terminate the loop */
      /* fall through */
    case ';':
#ifndef _WIN32
    case ':':     /* can't use this as delimiter as it may be drive letter */
#endif
      p[-1]= 0;
      if (str == &name)  // load all plugins in named module
      {
        if (!name.length)
        {
          p--;    /* reset pointer */
          continue;
        }

        dl= name;
        mysql_mutex_lock(&LOCK_plugin);
        free_root(tmp_root, MYF(MY_MARK_BLOCKS_FREE));
        name.str= 0; // load everything
        if (plugin_add(tmp_root, false, &name, &dl,
                       MYF(ME_ERROR_LOG)) != INSTALL_GOOD)
          goto error;
      }
      else
      {
        free_root(tmp_root, MYF(MY_MARK_BLOCKS_FREE));
        mysql_mutex_lock(&LOCK_plugin);
        if (plugin_add(tmp_root, false, &name, &dl,
                       MYF(ME_ERROR_LOG)) != INSTALL_GOOD)
          goto error;
      }
      mysql_mutex_unlock(&LOCK_plugin);
      name.length= dl.length= 0;
      dl.str= NULL; name.str= p= buffer;
      str= &name;
      continue;
    case '=':
    case '#':
      if (str == &name)
      {
        p[-1]= 0;
        str= &dl;
        str->str= p;
        continue;
      }
      /* fall through */
    default:
      str->length++;
      continue;
    }
  }
  DBUG_RETURN(FALSE);
error:
  mysql_mutex_unlock(&LOCK_plugin);
  if (name.str)
    sql_print_error("Couldn't load plugin '%s' from '%s'.",
                    name.str, dl.str);
  else
    sql_print_error("Couldn't load plugins from '%s'.", dl.str);
  DBUG_RETURN(TRUE);
}


void plugin_shutdown(void)
{
  size_t i, count= plugin_array.elements;
  struct st_plugin_int **plugins, *plugin;
  struct st_plugin_dl **dl;
  DBUG_ENTER("plugin_shutdown");

  if (initialized)
  {
    if (opt_gtid_pos_auto_plugins)
    {
      free_engine_list(opt_gtid_pos_auto_plugins);
      opt_gtid_pos_auto_plugins= NULL;
    }

    mysql_mutex_lock(&LOCK_plugin);

    reap_needed= true;

    /*
      We want to shut down plugins in a reasonable order, this will
      become important when we have plugins which depend upon each other.
      Circular references cannot be reaped so they are forced afterwards.
      TODO: Have an additional step here to notify all active plugins that
      shutdown is requested to allow plugins to deinitialize in parallel.
    */
    while (reap_needed && (count= plugin_array.elements))
    {
      reap_plugins();
      for (i= 0; i < count; i++)
      {
        plugin= *dynamic_element(&plugin_array, i, struct st_plugin_int **);
        if (plugin->state == PLUGIN_IS_READY)
        {
          plugin->state= PLUGIN_IS_DELETED;
          reap_needed= true;
        }
      }
      if (!reap_needed)
      {
        /*
          release any plugin references held.
        */
        unlock_variables(NULL, &global_system_variables);
        unlock_variables(NULL, &max_system_variables);
      }
    }

    plugins= (struct st_plugin_int **) my_alloca(sizeof(void*) * (count+1));

    /*
      If we have any plugins which did not die cleanly, we force shutdown.
      Don't re-deinit() plugins that failed deinit() earlier (already dying)
    */
    for (i= 0; i < count; i++)
    {
      plugins[i]= *dynamic_element(&plugin_array, i, struct st_plugin_int **);
      if (plugins[i]->state == PLUGIN_IS_DYING)
        plugins[i]->state= PLUGIN_IS_UNINITIALIZED;
      if (plugins[i]->state == PLUGIN_IS_DELETED)
        plugins[i]->state= PLUGIN_IS_DYING;
    }
    mysql_mutex_unlock(&LOCK_plugin);

    /*
      We loop through all plugins and call deinit() if they have one.
    */
    for (i= 0; i < count; i++)
      if (!(plugins[i]->state & (PLUGIN_IS_UNINITIALIZED | PLUGIN_IS_FREED |
                                 PLUGIN_IS_DISABLED)))
      {
        /*
          We are forcing deinit on plugins so we don't want to do a ref_count
          check until we have processed all the plugins.
        */
        plugin_deinitialize(plugins[i], false);
      }

    /*
      It's perfectly safe not to lock LOCK_plugin, as there're no
      concurrent threads anymore. But some functions called from here
      use mysql_mutex_assert_owner(), so we lock the mutex to satisfy it
    */
    mysql_mutex_lock(&LOCK_plugin);

    /*
      We defer checking ref_counts until after all plugins are deinitialized
      as some may have worker threads holding on to plugin references.
    */
    for (i= 0; i < count; i++)
    {
      if (plugins[i]->ref_count)
        sql_print_error("Plugin '%s' has ref_count=%d after shutdown.",
                        plugins[i]->name.str, plugins[i]->ref_count);
      plugin_del(plugins[i], PLUGIN_IS_DYING);
    }

    /*
      Now we can deallocate all memory.
    */

    cleanup_variables(&global_system_variables);
    cleanup_variables(&max_system_variables);
    mysql_mutex_unlock(&LOCK_plugin);

    initialized= 0;
    mysql_mutex_destroy(&LOCK_plugin);

    my_afree(plugins);
  }

  /* Dispose of the memory */

  for (i= 0; i < MYSQL_MAX_PLUGIN_TYPE_NUM; i++)
    my_hash_free(&plugin_hash[i]);
  delete_dynamic(&plugin_array);

  count= plugin_dl_array.elements;
  dl= (struct st_plugin_dl **)my_alloca(sizeof(void*) * count);
  for (i= 0; i < count; i++)
    dl[i]= *dynamic_element(&plugin_dl_array, i, struct st_plugin_dl **);
  for (i= 0; i < plugin_dl_array.elements; i++)
    free_plugin_mem(dl[i]);
  my_afree(dl);
  delete_dynamic(&plugin_dl_array);

  my_hash_free(&bookmark_hash);
  free_root(&plugin_mem_root, MYF(0));
  free_root(&plugin_vars_mem_root, MYF(0));

  global_variables_dynamic_size= 0;

  DBUG_VOID_RETURN;
}

/**
  complete plugin installation (after plugin_add).

  That is, initialize it, and update mysql.plugin table
*/
static bool finalize_install(THD *thd, TABLE *table, const LEX_CSTRING *name,
                             int *argc, char **argv)
{
  struct st_plugin_int *tmp= plugin_find_internal(name, MYSQL_ANY_PLUGIN);
  int error;
  DBUG_ASSERT(tmp);
  mysql_mutex_assert_owner(&LOCK_plugin); // because of tmp->state

  if (tmp->state != PLUGIN_IS_UNINITIALIZED)
  {
    /* already installed */
    return 0;
  }
  else
  {
    if (plugin_initialize(thd->mem_root, tmp, argc, argv, false))
    {
      my_error(ER_CANT_INITIALIZE_UDF, MYF(0), name->str,
                   "Plugin initialization function failed.");
      tmp->state= PLUGIN_IS_DELETED;
      return 1;
    }
  }
  if (tmp->state == PLUGIN_IS_DISABLED)
  {
    if (global_system_variables.log_warnings)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_CANT_INITIALIZE_UDF,
                          ER_THD(thd, ER_CANT_INITIALIZE_UDF),
                          name->str, "Plugin is disabled");
  }

  /*
    We do not replicate the INSTALL PLUGIN statement. Disable binlogging
    of the insert into the plugin table, so that it is not replicated in
    row based mode.
  */
  DBUG_ASSERT(!table->file->row_logging);
  table->use_all_columns();
  restore_record(table, s->default_values);
  table->field[0]->store(name->str, name->length, system_charset_info);
  table->field[1]->store(tmp->plugin_dl->dl.str, tmp->plugin_dl->dl.length,
                         files_charset_info);
  error= table->file->ha_write_row(table->record[0]);
  if (unlikely(error))
  {
    table->file->print_error(error, MYF(0));
    tmp->state= PLUGIN_IS_DELETED;
    return 1;
  }
  return 0;
}

bool mysql_install_plugin(THD *thd, const LEX_CSTRING *name,
                          const LEX_CSTRING *dl_arg)
{
  TABLE_LIST tables;
  TABLE *table;
  LEX_CSTRING dl= *dl_arg;
  enum install_status error;
  int argc=orig_argc;
  char **argv=orig_argv;
  unsigned long event_class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE] =
  { MYSQL_AUDIT_GENERAL_CLASSMASK };
  DBUG_ENTER("mysql_install_plugin");

  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_PLUGIN_NAME, 0, TL_WRITE);
  if (!opt_noacl && check_table_access(thd, INSERT_ACL, &tables, FALSE, 1, FALSE))
    DBUG_RETURN(TRUE);
  WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

  /* need to open before acquiring LOCK_plugin or it will deadlock */
  if (! (table = open_ltable(thd, &tables, TL_WRITE,
                             MYSQL_LOCK_IGNORE_TIMEOUT)))
    DBUG_RETURN(TRUE);

  if (my_load_defaults(MYSQL_CONFIG_NAME, load_default_groups, &argc, &argv, NULL))
  {
    my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), name->str);
    DBUG_RETURN(TRUE);
  }

  /*
    Pre-acquire audit plugins for events that may potentially occur
    during [UN]INSTALL PLUGIN.

    When audit event is triggered, audit subsystem acquires interested
    plugins by walking through plugin list. Evidently plugin list
    iterator protects plugin list by acquiring LOCK_plugin, see
    plugin_foreach_with_mask().

    On the other hand [UN]INSTALL PLUGIN is acquiring LOCK_plugin
    rather for a long time.

    When audit event is triggered during [UN]INSTALL PLUGIN, plugin
    list iterator acquires the same lock (within the same thread)
    second time.

    This hack should be removed when LOCK_plugin is fixed so it
    protects only what it supposed to protect.

    See also mysql_uninstall_plugin() and initialize_audit_plugin()
  */
  if (mysql_audit_general_enabled())
    mysql_audit_acquire_plugins(thd, event_class_mask);

  mysql_mutex_lock(&LOCK_plugin);
  DEBUG_SYNC(thd, "acquired_LOCK_plugin");
  error= plugin_add(thd->mem_root, thd->lex->create_info.if_not_exists(),
                    name, &dl, MYF(0));
  if (unlikely(error != INSTALL_GOOD))
    goto err;

  if (name->str)
    error= finalize_install(thd, table, name, &argc, argv)
             ? INSTALL_FAIL_NOT_OK : INSTALL_GOOD;
  else
  {
    st_plugin_dl *plugin_dl= plugin_dl_find(&dl);
    struct st_maria_plugin *plugin;
    for (plugin= plugin_dl->plugins; plugin->info; plugin++)
    {
      LEX_CSTRING str= { plugin->name, strlen(plugin->name) };
      if (finalize_install(thd, table, &str, &argc, argv))
        error= INSTALL_FAIL_NOT_OK;
    }
  }

  if (unlikely(error != INSTALL_GOOD))
  {
    reap_needed= true;
    reap_plugins();
  }
err:
  global_plugin_version++;
  mysql_mutex_unlock(&LOCK_plugin);
  if (argv)
    free_defaults(argv);
  DBUG_RETURN(error == INSTALL_FAIL_NOT_OK);
#ifdef WITH_WSREP
wsrep_error_label:
  DBUG_RETURN(true);
#endif
}


static bool do_uninstall(THD *thd, TABLE *table, const LEX_CSTRING *name)
{
  struct st_plugin_int *plugin;
  mysql_mutex_assert_owner(&LOCK_plugin);

  if (!(plugin= plugin_find_internal(name, MYSQL_ANY_PLUGIN)) ||
      plugin->state & (PLUGIN_IS_UNINITIALIZED | PLUGIN_IS_DYING))
  {
    // maybe plugin is present in mysql.plugin; postpone the error
    plugin= nullptr;
  }

  if (plugin)
  {
    if (!plugin->plugin_dl)
    {
      my_error(ER_PLUGIN_DELETE_BUILTIN, MYF(0));
      return 1;
    }
    if (plugin->load_option == PLUGIN_FORCE_PLUS_PERMANENT)
    {
      my_error(ER_PLUGIN_IS_PERMANENT, MYF(0), name->str);
      return 1;
    }

    plugin->state= PLUGIN_IS_DELETED;
    if (plugin->ref_count)
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
          WARN_PLUGIN_BUSY, ER_THD(thd, WARN_PLUGIN_BUSY));
    else
      reap_needed= true;
  }

  uchar user_key[MAX_KEY_LENGTH];
  table->use_all_columns();
  table->field[0]->store(name->str, name->length, system_charset_info);
  key_copy(user_key, table->record[0], table->key_info,
           table->key_info->key_length);
  if (! table->file->ha_index_read_idx_map(table->record[0], 0, user_key,
                                           HA_WHOLE_KEY, HA_READ_KEY_EXACT))
  {
    int error;
    /*
      We do not replicate the UNINSTALL PLUGIN statement. Disable binlogging
      of the delete from the plugin table, so that it is not replicated in
      row based mode.
    */
    table->file->row_logging= 0;                // No logging
    error= table->file->ha_delete_row(table->record[0]);
    if (unlikely(error))
    {
      table->file->print_error(error, MYF(0));
      return 1;
    }
  }
  else if (!plugin)
  {
    const myf MyFlags= thd->lex->if_exists() ? ME_NOTE : 0;
    my_error(ER_SP_DOES_NOT_EXIST, MyFlags, "PLUGIN", name->str);
    return !MyFlags;
  }
  return 0;
}


bool mysql_uninstall_plugin(THD *thd, const LEX_CSTRING *name,
                            const LEX_CSTRING *dl_arg)
{
  TABLE *table;
  TABLE_LIST tables;
  LEX_CSTRING dl= *dl_arg;
  bool error= false;
  unsigned long event_class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE] =
  { MYSQL_AUDIT_GENERAL_CLASSMASK };
  DBUG_ENTER("mysql_uninstall_plugin");

  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_PLUGIN_NAME, 0, TL_WRITE);

  if (!opt_noacl && check_table_access(thd, DELETE_ACL, &tables, FALSE, 1, FALSE))
    DBUG_RETURN(TRUE);

  WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

  /* need to open before acquiring LOCK_plugin or it will deadlock */
  if (! (table= open_ltable(thd, &tables, TL_WRITE, MYSQL_LOCK_IGNORE_TIMEOUT)))
    DBUG_RETURN(TRUE);

  if (!table->key_info)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "The table %s.%s has no primary key. "
                    "Please check the table definition and "
                    "create the primary key accordingly.", MYF(0),
                    table->s->db.str, table->s->table_name.str);
    DBUG_RETURN(TRUE);
  }

  /*
    Pre-acquire audit plugins for events that may potentially occur
    during [UN]INSTALL PLUGIN.

    When audit event is triggered, audit subsystem acquires interested
    plugins by walking through plugin list. Evidently plugin list
    iterator protects plugin list by acquiring LOCK_plugin, see
    plugin_foreach_with_mask().

    On the other hand [UN]INSTALL PLUGIN is acquiring LOCK_plugin
    rather for a long time.

    When audit event is triggered during [UN]INSTALL PLUGIN, plugin
    list iterator acquires the same lock (within the same thread)
    second time.

    This hack should be removed when LOCK_plugin is fixed so it
    protects only what it supposed to protect.

    See also mysql_install_plugin() and initialize_audit_plugin()
  */
  if (mysql_audit_general_enabled())
    mysql_audit_acquire_plugins(thd, event_class_mask);

  mysql_mutex_lock(&LOCK_plugin);

  if (name->str)
    error= do_uninstall(thd, table, name);
  else
  {
    fix_dl_name(thd->mem_root, &dl);
    st_plugin_dl *plugin_dl= plugin_dl_find(&dl);
    if (plugin_dl)
    {
      for (struct st_maria_plugin *plugin= plugin_dl->plugins;
           plugin->info; plugin++)
      {
        LEX_CSTRING str= { plugin->name, strlen(plugin->name) };
        error|= do_uninstall(thd, table, &str);
      }
    }
    else
    {
      myf MyFlags= thd->lex->if_exists() ? ME_NOTE : 0;
      my_error(ER_SP_DOES_NOT_EXIST, MyFlags, "SONAME", dl.str);
      error|= !MyFlags;
    }
  }
  reap_plugins();

  global_plugin_version++;
  mysql_mutex_unlock(&LOCK_plugin);
  DBUG_RETURN(error);
#ifdef WITH_WSREP
wsrep_error_label:
  DBUG_RETURN(true);
#endif
}


bool plugin_foreach_with_mask(THD *thd, plugin_foreach_func *func,
                       int type, uint state_mask, void *arg)
{
  size_t idx, total= 0;
  struct st_plugin_int *plugin;
  plugin_ref *plugins;
  my_bool res= FALSE;
  DBUG_ENTER("plugin_foreach_with_mask");

  if (!initialized)
    DBUG_RETURN(FALSE);

  mysql_mutex_lock(&LOCK_plugin);
  /*
    Do the alloca out here in case we do have a working alloca:
    leaving the nested stack frame invalidates alloca allocation.
  */
  if (type == MYSQL_ANY_PLUGIN)
  {
    plugins= (plugin_ref*) my_alloca(plugin_array.elements * sizeof(plugin_ref));
    for (idx= 0; idx < plugin_array.elements; idx++)
    {
      plugin= *dynamic_element(&plugin_array, idx, struct st_plugin_int **);
      if ((plugins[total]= intern_plugin_lock(0, plugin_int_to_ref(plugin),
                                              state_mask)))
        total++;
    }
  }
  else
  {
    HASH *hash= plugin_hash + type;
    plugins= (plugin_ref*) my_alloca(hash->records * sizeof(plugin_ref));
    for (idx= 0; idx < hash->records; idx++)
    {
      plugin= (struct st_plugin_int *) my_hash_element(hash, idx);
      if ((plugins[total]= intern_plugin_lock(0, plugin_int_to_ref(plugin),
                                              state_mask)))
        total++;
    }
  }
  mysql_mutex_unlock(&LOCK_plugin);

  for (idx= 0; idx < total; idx++)
  {
    /* It will stop iterating on first engine error when "func" returns TRUE */
    if ((res= func(thd, plugins[idx], arg)))
        break;
  }

  plugin_unlock_list(0, plugins, total);
  my_afree(plugins);
  DBUG_RETURN(res);
}


static bool plugin_dl_foreach_internal(THD *thd, st_plugin_dl *plugin_dl,
                                       st_maria_plugin *plug,
                                       plugin_foreach_func *func, void *arg)
{
  for (; plug->name; plug++)
  {
    st_plugin_int tmp, *plugin;

    tmp.name.str= const_cast<char*>(plug->name);
    tmp.name.length= strlen(plug->name);
    tmp.plugin= plug;
    tmp.plugin_dl= plugin_dl;

    mysql_mutex_lock(&LOCK_plugin);
    if ((plugin= plugin_find_internal(&tmp.name, plug->type)) &&
        plugin->plugin == plug)

    {
      tmp.state= plugin->state;
      tmp.load_option= plugin->load_option;
    }
    else
    {
      tmp.state= PLUGIN_IS_FREED;
      tmp.load_option= PLUGIN_OFF;
    }
    mysql_mutex_unlock(&LOCK_plugin);

    plugin= &tmp;
    if (func(thd, plugin_int_to_ref(plugin), arg))
      return 1;
  }
  return 0;
}

bool plugin_dl_foreach(THD *thd, const LEX_CSTRING *dl,
                       plugin_foreach_func *func, void *arg)
{
  bool err= 0;

  if (dl)
  {
    mysql_mutex_lock(&LOCK_plugin);
    st_plugin_dl *plugin_dl= plugin_dl_add(dl, MYF(0));
    mysql_mutex_unlock(&LOCK_plugin);

    if (!plugin_dl)
      return 1;

    err= plugin_dl_foreach_internal(thd, plugin_dl, plugin_dl->plugins,
                                    func, arg);

    mysql_mutex_lock(&LOCK_plugin);
    plugin_dl_del(plugin_dl);
    mysql_mutex_unlock(&LOCK_plugin);
  }
  else
  {
    struct st_maria_plugin **builtins;
    for (builtins= mysql_mandatory_plugins; !err && *builtins; builtins++)
      err= plugin_dl_foreach_internal(thd, 0, *builtins, func, arg);
    for (builtins= mysql_optional_plugins; !err && *builtins; builtins++)
      err= plugin_dl_foreach_internal(thd, 0, *builtins, func, arg);
  }
  return err;
}


/****************************************************************************
  Internal type declarations for variables support
****************************************************************************/

#undef MYSQL_SYSVAR_NAME
#define MYSQL_SYSVAR_NAME(name) name
#define PLUGIN_VAR_TYPEMASK 0x7f
#define BOOKMARK_MEMALLOC   0x80

static inline char plugin_var_bookmark_key(uint flags)
{
  return (flags & PLUGIN_VAR_TYPEMASK) |
         (flags & PLUGIN_VAR_MEMALLOC ? BOOKMARK_MEMALLOC : 0);
}

#define EXTRA_OPTIONS 3 /* options for: 'foo', 'plugin-foo' and NULL */

typedef DECLARE_MYSQL_SYSVAR_BASIC(sysvar_bool_t, my_bool);
typedef DECLARE_MYSQL_THDVAR_BASIC(thdvar_bool_t, my_bool);
typedef DECLARE_MYSQL_SYSVAR_BASIC(sysvar_str_t, char *);
typedef DECLARE_MYSQL_THDVAR_BASIC(thdvar_str_t, char *);

typedef DECLARE_MYSQL_SYSVAR_TYPELIB(sysvar_enum_t, unsigned long);
typedef DECLARE_MYSQL_THDVAR_TYPELIB(thdvar_enum_t, unsigned long);
typedef DECLARE_MYSQL_SYSVAR_TYPELIB(sysvar_set_t, ulonglong);
typedef DECLARE_MYSQL_THDVAR_TYPELIB(thdvar_set_t, ulonglong);

typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_int_t, int);
typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_long_t, long);
typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_longlong_t, longlong);
typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_uint_t, uint);
typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_ulong_t, ulong);
typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_ulonglong_t, ulonglong);
typedef DECLARE_MYSQL_SYSVAR_SIMPLE(sysvar_double_t, double);

typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_int_t, int);
typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_long_t, long);
typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_longlong_t, longlong);
typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_uint_t, uint);
typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_ulong_t, ulong);
typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_ulonglong_t, ulonglong);
typedef DECLARE_MYSQL_THDVAR_SIMPLE(thdvar_double_t, double);


/****************************************************************************
  default variable data check and update functions
****************************************************************************/

static int check_func_bool(THD *thd, struct st_mysql_sys_var *var,
                           void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  int result, length;
  long long tmp;

  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING)
  {
    length= sizeof(buff);
    if (!(str= value->val_str(value, buff, &length)) ||
        (result= find_type(&bool_typelib, str, length, 1)-1) < 0)
      goto err;
  }
  else
  {
    if (value->val_int(value, &tmp) < 0)
      goto err;
    if (tmp != 0 && tmp != 1)
      goto err;
    result= (int) tmp;
  }
  *(my_bool *) save= result ? 1 : 0;
  return 0;
err:
  return 1;
}


static int check_func_int(THD *thd, struct st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  my_bool fixed1, fixed2;
  long long orig, val;
  struct my_option options;
  value->val_int(value, &orig);
  val= orig;
  plugin_opt_set_limits(&options, var);

  if (var->flags & PLUGIN_VAR_UNSIGNED)
  {
    if ((fixed1= (!value->is_unsigned(value) && val < 0)))
      val=0;
    *(uint *)save= (uint) getopt_ull_limit_value((ulonglong) val, &options,
                                                   &fixed2);
  }
  else
  {
    if ((fixed1= (value->is_unsigned(value) && val < 0)))
      val=LONGLONG_MAX;
    *(int *)save= (int) getopt_ll_limit_value(val, &options, &fixed2);
  }

  return throw_bounds_warning(thd, var->name, fixed1 || fixed2,
                              value->is_unsigned(value), (longlong) orig);
}


static int check_func_long(THD *thd, struct st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  my_bool fixed1, fixed2;
  long long orig, val;
  struct my_option options;
  value->val_int(value, &orig);
  val= orig;
  plugin_opt_set_limits(&options, var);

  if (var->flags & PLUGIN_VAR_UNSIGNED)
  {
    if ((fixed1= (!value->is_unsigned(value) && val < 0)))
      val=0;
    *(ulong *)save= (ulong) getopt_ull_limit_value((ulonglong) val, &options,
                                                   &fixed2);
  }
  else
  {
    if ((fixed1= (value->is_unsigned(value) && val < 0)))
      val=LONGLONG_MAX;
    *(long *)save= (long) getopt_ll_limit_value(val, &options, &fixed2);
  }

  return throw_bounds_warning(thd, var->name, fixed1 || fixed2,
                              value->is_unsigned(value), (longlong) orig);
}


static int check_func_longlong(THD *thd, struct st_mysql_sys_var *var,
                               void *save, st_mysql_value *value)
{
  my_bool fixed1, fixed2;
  long long orig, val;
  struct my_option options;
  value->val_int(value, &orig);
  val= orig;
  plugin_opt_set_limits(&options, var);

  if (var->flags & PLUGIN_VAR_UNSIGNED)
  {
    if ((fixed1= (!value->is_unsigned(value) && val < 0)))
      val=0;
    *(ulonglong *)save= getopt_ull_limit_value((ulonglong) val, &options,
                                               &fixed2);
  }
  else
  {
    if ((fixed1= (value->is_unsigned(value) && val < 0)))
      val=LONGLONG_MAX;
    *(longlong *)save= getopt_ll_limit_value(val, &options, &fixed2);
  }

  return throw_bounds_warning(thd, var->name, fixed1 || fixed2,
                              value->is_unsigned(value), (longlong) orig);
}

static int check_func_str(THD *thd, struct st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  int length;

  length= sizeof(buff);
  if ((str= value->val_str(value, buff, &length)))
    str= thd->strmake(str, length);
  *(const char**)save= str;
  return 0;
}


static int check_func_enum(THD *thd, struct st_mysql_sys_var *var,
                           void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  TYPELIB *typelib;
  long long tmp;
  long result;
  int length;

  if (var->flags & PLUGIN_VAR_THDLOCAL)
    typelib= ((thdvar_enum_t*) var)->typelib;
  else
    typelib= ((sysvar_enum_t*) var)->typelib;

  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING)
  {
    length= sizeof(buff);
    if (!(str= value->val_str(value, buff, &length)))
      goto err;
    if ((result= (long)find_type(typelib, str, length, 0) - 1) < 0)
      goto err;
  }
  else
  {
    if (value->val_int(value, &tmp))
      goto err;
    if (tmp < 0 || tmp >= typelib->count)
      goto err;
    result= (long) tmp;
  }
  *(long*)save= result;
  return 0;
err:
  return 1;
}


static int check_func_set(THD *thd, struct st_mysql_sys_var *var,
                          void *save, st_mysql_value *value)
{
  char buff[STRING_BUFFER_USUAL_SIZE], *error= 0;
  const char *str;
  TYPELIB *typelib;
  ulonglong result;
  uint error_len= 0;                            // init as only set on error
  bool not_used;
  int length;

  if (var->flags & PLUGIN_VAR_THDLOCAL)
    typelib= ((thdvar_set_t*) var)->typelib;
  else
    typelib= ((sysvar_set_t*)var)->typelib;

  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING)
  {
    length= sizeof(buff);
    if (!(str= value->val_str(value, buff, &length)))
      goto err;
    result= find_set(typelib, str, length, NULL,
                     &error, &error_len, &not_used);
    if (unlikely(error_len))
      goto err;
  }
  else
  {
    if (value->val_int(value, (long long *)&result))
      goto err;
    if (unlikely((result >= (1ULL << typelib->count)) &&
                 (typelib->count < sizeof(long)*8)))
      goto err;
  }
  *(ulonglong*)save= result;
  return 0;
err:
  return 1;
}

static int check_func_double(THD *thd, struct st_mysql_sys_var *var,
                             void *save, st_mysql_value *value)
{
  double v;
  my_bool fixed;
  struct my_option option;

  value->val_real(value, &v);
  plugin_opt_set_limits(&option, var);
  *(double *) save= getopt_double_limit_value(v, &option, &fixed);

  return throw_bounds_warning(thd, var->name, fixed, v);
}


static void update_func_bool(THD *thd, struct st_mysql_sys_var *var,
                             void *tgt, const void *save)
{
  *(my_bool *) tgt= *(my_bool *) save ? 1 : 0;
}


static void update_func_int(THD *thd, struct st_mysql_sys_var *var,
                             void *tgt, const void *save)
{
  *(int *)tgt= *(int *) save;
}


static void update_func_long(THD *thd, struct st_mysql_sys_var *var,
                             void *tgt, const void *save)
{
  *(long *)tgt= *(long *) save;
}


static void update_func_longlong(THD *thd, struct st_mysql_sys_var *var,
                             void *tgt, const void *save)
{
  *(longlong *)tgt= *(ulonglong *) save;
}


static void update_func_str(THD *thd, struct st_mysql_sys_var *var,
                             void *tgt, const void *save)
{
  char *value= *(char**) save;
  if (var->flags & PLUGIN_VAR_MEMALLOC)
  {
    char *old= *(char**) tgt;
    if (value)
      *(char**) tgt= my_strdup(key_memory_global_system_variables,
                               value, MYF(0));
    else
      *(char**) tgt= 0;
    my_free(old);
  }
  else
    *(char**) tgt= value;
}

static void update_func_double(THD *thd, struct st_mysql_sys_var *var,
                               void *tgt, const void *save)
{
  *(double *) tgt= *(double *) save;
}

/****************************************************************************
  System Variables support
****************************************************************************/

sys_var *find_sys_var(THD *thd, const char *str, size_t length,
                      bool throw_error)
{
  sys_var *var;
  sys_var_pluginvar *pi;
  DBUG_ENTER("find_sys_var");
  DBUG_PRINT("enter", ("var '%.*s'", (int)length, str));

  mysql_prlock_rdlock(&LOCK_system_variables_hash);
  if ((var= intern_find_sys_var(str, length)) &&
      (pi= var->cast_pluginvar()))
  {
    mysql_mutex_lock(&LOCK_plugin);
    if (!intern_plugin_lock(thd ? thd->lex : 0, plugin_int_to_ref(pi->plugin),
                            PLUGIN_IS_READY))
      var= NULL; /* failed to lock it, it must be uninstalling */
    mysql_mutex_unlock(&LOCK_plugin);
  }
  mysql_prlock_unlock(&LOCK_system_variables_hash);

  if (unlikely(!throw_error && !var))
    my_error(ER_UNKNOWN_SYSTEM_VARIABLE, MYF(0),
             (int) (length ? length : strlen(str)), (char*) str);
  DBUG_RETURN(var);
}


/*
  called by register_var, construct_options and test_plugin_options.
  Returns the 'bookmark' for the named variable.
  returns null for non thd-local variables.
  LOCK_system_variables_hash should be at least read locked
*/
static st_bookmark *find_bookmark(const char *plugin, const char *name,
                                  int flags)
{
  st_bookmark *result= NULL;
  size_t namelen, length, pluginlen= 0;
  char *varname, *p;

  if (!(flags & PLUGIN_VAR_THDLOCAL))
    return NULL;

  namelen= strlen(name);
  if (plugin)
    pluginlen= strlen(plugin) + 1;
  length= namelen + pluginlen + 2;
  varname= (char*) my_alloca(length);

  if (plugin)
  {
    strxmov(varname + 1, plugin, "_", name, NullS);
    for (p= varname + 1; *p; p++)
      if (*p == '-')
        *p= '_';
  }
  else
    memcpy(varname + 1, name, namelen + 1);

  varname[0]= plugin_var_bookmark_key(flags);

  result= (st_bookmark*) my_hash_search(&bookmark_hash,
                                        (const uchar*) varname, length - 1);

  my_afree(varname);
  return result;
}


static size_t var_storage_size(int flags)
{
  switch (flags & PLUGIN_VAR_TYPEMASK) {
  case PLUGIN_VAR_BOOL:         return sizeof(my_bool);
  case PLUGIN_VAR_INT:          return sizeof(int);
  case PLUGIN_VAR_LONG:         return sizeof(long);
  case PLUGIN_VAR_ENUM:         return sizeof(long);
  case PLUGIN_VAR_LONGLONG:     return sizeof(ulonglong);
  case PLUGIN_VAR_SET:          return sizeof(ulonglong);
  case PLUGIN_VAR_STR:          return sizeof(char*);
  case PLUGIN_VAR_DOUBLE:       return sizeof(double);
  default: DBUG_ASSERT_NO_ASSUME(0);      return 0;
  }
}


/*
  returns a bookmark for thd-local variables, creating if neccessary.
  Requires that a write lock is obtained on LOCK_system_variables_hash
*/
static st_bookmark *register_var(const char *plugin, const char *name,
                                 int flags)
{
  size_t length= strlen(plugin) + strlen(name) + 3, size, offset, new_size;
  st_bookmark *result;
  char *varname, *p;

  DBUG_ASSERT(flags & PLUGIN_VAR_THDLOCAL);

  size= var_storage_size(flags);
  varname= ((char*) my_alloca(length));
  strxmov(varname + 1, plugin, "_", name, NullS);
  for (p= varname + 1; *p; p++)
    if (*p == '-')
      *p= '_';

  if (!(result= find_bookmark(NULL, varname + 1, flags)))
  {
    result= (st_bookmark*) alloc_root(&plugin_vars_mem_root,
                                      sizeof(struct st_bookmark) + length-1);
    varname[0]= plugin_var_bookmark_key(flags);
    memcpy(result->key, varname, length);
    result->name_len= (uint)(length - 2);
    result->offset= -1;

    DBUG_ASSERT(size && !(size & (size-1))); /* must be power of 2 */

    offset= global_system_variables.dynamic_variables_size;
    offset= (offset + size - 1) & ~(size - 1);
    result->offset= (int) offset;

    new_size= (offset + size + 63) & ~63;

    if (new_size > global_variables_dynamic_size)
    {
      global_system_variables.dynamic_variables_ptr= (char*)
        my_realloc(key_memory_global_system_variables,
                   global_system_variables.dynamic_variables_ptr, new_size,
                   MYF(MY_WME | MY_FAE | MY_ALLOW_ZERO_PTR));
      max_system_variables.dynamic_variables_ptr= (char*)
        my_realloc(key_memory_global_system_variables,
                   max_system_variables.dynamic_variables_ptr, new_size,
                   MYF(MY_WME | MY_FAE | MY_ALLOW_ZERO_PTR));
      /*
        Clear the new variable value space. This is required for string
        variables. If their value is non-NULL, it must point to a valid
        string.
      */
      bzero(global_system_variables.dynamic_variables_ptr +
            global_variables_dynamic_size,
            new_size - global_variables_dynamic_size);
      bzero(max_system_variables.dynamic_variables_ptr +
            global_variables_dynamic_size,
            new_size - global_variables_dynamic_size);
      global_variables_dynamic_size= new_size;
    }

    global_system_variables.dynamic_variables_head= (uint)offset;
    max_system_variables.dynamic_variables_head= (uint)offset;
    global_system_variables.dynamic_variables_size= (uint)(offset + size);
    max_system_variables.dynamic_variables_size= (uint)(offset + size);
    global_system_variables.dynamic_variables_version++;
    max_system_variables.dynamic_variables_version++;

    result->version= global_system_variables.dynamic_variables_version;

    /* this should succeed because we have already checked if a dup exists */
    if (my_hash_insert(&bookmark_hash, (uchar*) result))
    {
      fprintf(stderr, "failed to add placeholder to hash");
      DBUG_ASSERT_NO_ASSUME(0);
    }
  }
  my_afree(varname);
  return result;
}


void sync_dynamic_session_variables(THD* thd, bool global_lock)
{
  uint idx;

  thd->variables.dynamic_variables_ptr= (char*)
    my_realloc(key_memory_THD_variables,
               thd->variables.dynamic_variables_ptr,
               global_variables_dynamic_size,
               MYF(MY_WME | MY_FAE | MY_ALLOW_ZERO_PTR));

  if (global_lock)
    mysql_mutex_lock(&LOCK_global_system_variables);

  mysql_mutex_assert_owner(&LOCK_global_system_variables);

  memcpy(thd->variables.dynamic_variables_ptr +
           thd->variables.dynamic_variables_size,
         global_system_variables.dynamic_variables_ptr +
           thd->variables.dynamic_variables_size,
         global_system_variables.dynamic_variables_size -
           thd->variables.dynamic_variables_size);

  /*
    now we need to iterate through any newly copied 'defaults'
    and if it is a string type with MEMALLOC flag, we need to strdup
  */
  for (idx= 0; idx < bookmark_hash.records; idx++)
  {
    st_bookmark *v= (st_bookmark*) my_hash_element(&bookmark_hash,idx);

    if (v->version <= thd->variables.dynamic_variables_version)
      continue; /* already in thd->variables */

    /* Here we do anything special that may be required of the data types */

    if ((v->key[0] & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR &&
         v->key[0] & BOOKMARK_MEMALLOC)
    {
      char **pp= (char**) (thd->variables.dynamic_variables_ptr + v->offset);
      if (*pp)
        *pp= my_strdup(key_memory_THD_variables, *pp, MYF(MY_WME|MY_FAE));
    }
  }

  if (global_lock)
    mysql_mutex_unlock(&LOCK_global_system_variables);

  thd->variables.dynamic_variables_version=
         global_system_variables.dynamic_variables_version;
  thd->variables.dynamic_variables_head=
         global_system_variables.dynamic_variables_head;
  thd->variables.dynamic_variables_size=
         global_system_variables.dynamic_variables_size;
}


/*
  returns a pointer to the memory which holds the thd-local variable or
  a pointer to the global variable if thd==null.
  If required, will sync with global variables if the requested variable
  has not yet been allocated in the current thread.
*/
static void *intern_sys_var_ptr(THD* thd, int offset, bool global_lock)
{
  DBUG_ENTER("intern_sys_var_ptr");
  DBUG_ASSERT(offset >= 0);
  DBUG_ASSERT((uint)offset <= global_system_variables.dynamic_variables_head);

  if (!thd)
    DBUG_RETURN(global_system_variables.dynamic_variables_ptr + offset);

  /*
    dynamic_variables_head points to the largest valid offset
  */
  if (!thd->variables.dynamic_variables_ptr ||
      (uint)offset > thd->variables.dynamic_variables_head)
  {
    mysql_prlock_rdlock(&LOCK_system_variables_hash);
    sync_dynamic_session_variables(thd, global_lock);
    mysql_prlock_unlock(&LOCK_system_variables_hash);
  }
  DBUG_RETURN(thd->variables.dynamic_variables_ptr + offset);
}


/**
  For correctness and simplicity's sake, a pointer to a function
  must be compatible with pointed-to type, that is, the return and
  parameters types must be the same. Thus, a callback function is
  defined for each scalar type. The functions are assigned in
  construct_options to their respective types.
*/

static char *mysql_sys_var_char(THD* thd, int offset)
{
  return static_cast<char*>(intern_sys_var_ptr(thd, offset, true));
}

static int *mysql_sys_var_int(THD* thd, int offset)
{
  return static_cast<int*>(intern_sys_var_ptr(thd, offset, true));
}

static unsigned int *mysql_sys_var_uint(THD* thd, int offset)
{
  return static_cast<unsigned int*>(intern_sys_var_ptr(thd, offset, true));
}

static long *mysql_sys_var_long(THD* thd, int offset)
{
  return static_cast<long*>(intern_sys_var_ptr(thd, offset, true));
}

static unsigned long *mysql_sys_var_ulong(THD* thd, int offset)
{
  return static_cast<unsigned long*>(intern_sys_var_ptr(thd, offset, true));
}

static long long *mysql_sys_var_longlong(THD* thd, int offset)
{
  return static_cast<long long*>(intern_sys_var_ptr(thd, offset, true));
}

static unsigned long long *mysql_sys_var_ulonglong(THD* thd, int offset)
{
  return static_cast<unsigned long long*>(intern_sys_var_ptr(thd, offset, true));
}

static char **mysql_sys_var_str(THD* thd, int offset)
{
  return static_cast<char**>(intern_sys_var_ptr(thd, offset, true));
}

static double *mysql_sys_var_double(THD* thd, int offset)
{
  return static_cast<double*>(intern_sys_var_ptr(thd, offset, true));
}

void plugin_thdvar_init(THD *thd)
{
  plugin_ref old_table_plugin= thd->variables.table_plugin;
  plugin_ref old_tmp_table_plugin= thd->variables.tmp_table_plugin;
  plugin_ref old_enforced_table_plugin= thd->variables.enforced_table_plugin;
  DBUG_ENTER("plugin_thdvar_init");

  // This function may be called many times per THD (e.g. on COM_CHANGE_USER)
  thd->variables.table_plugin= NULL;
  thd->variables.tmp_table_plugin= NULL;
  thd->variables.enforced_table_plugin= NULL;
  cleanup_variables(&thd->variables);

  /* This and all other variable cleanups are here for COM_CHANGE_USER :( */
#ifndef EMBEDDED_LIBRARY
  thd->session_tracker.sysvars.deinit(thd);
  my_free(thd->variables.redirect_url);
  thd->variables.redirect_url= 0;
#endif
  my_free((char*) thd->variables.default_master_connection.str);
  thd->variables.default_master_connection.str= 0;
  thd->variables.default_master_connection.length= 0;

  thd->variables= global_system_variables;

  /* we are going to allocate these lazily */
  thd->variables.dynamic_variables_version= 0;
  thd->variables.dynamic_variables_size= 0;
  thd->variables.dynamic_variables_ptr= 0;

  mysql_mutex_lock(&LOCK_plugin);
  thd->variables.table_plugin=
      intern_plugin_lock(NULL, global_system_variables.table_plugin);
  if (global_system_variables.tmp_table_plugin)
    thd->variables.tmp_table_plugin=
          intern_plugin_lock(NULL, global_system_variables.tmp_table_plugin);
  if (global_system_variables.enforced_table_plugin)
    thd->variables.enforced_table_plugin=
          intern_plugin_lock(NULL, global_system_variables.enforced_table_plugin);
  intern_plugin_unlock(NULL, old_table_plugin);
  intern_plugin_unlock(NULL, old_tmp_table_plugin);
  intern_plugin_unlock(NULL, old_enforced_table_plugin);
  mysql_mutex_unlock(&LOCK_plugin);

  thd->variables.default_master_connection.str=
    my_strndup(key_memory_Sys_var_charptr_value,
               global_system_variables.default_master_connection.str,
               global_system_variables.default_master_connection.length,
               MYF(MY_WME | MY_THREAD_SPECIFIC));
#ifndef EMBEDDED_LIBRARY
  thd->session_tracker.sysvars.init(thd);
  thd->variables.redirect_url=
    my_strdup(key_memory_Sys_var_charptr_value,
              global_system_variables.redirect_url,
              MYF(MY_WME | MY_THREAD_SPECIFIC));
#endif

  DBUG_VOID_RETURN;
}


/*
  Unlocks all system variables which hold a reference
*/
static void unlock_variables(THD *thd, struct system_variables *vars)
{
  intern_plugin_unlock(NULL, vars->table_plugin);
  intern_plugin_unlock(NULL, vars->tmp_table_plugin);
  intern_plugin_unlock(NULL, vars->enforced_table_plugin);
  vars->table_plugin= vars->tmp_table_plugin= vars->enforced_table_plugin= NULL;
}


/*
  Frees memory used by system variables

  Unlike plugin_vars_free_values() it frees all variables of all plugins,
  it's used on shutdown.
*/
static void cleanup_variables(struct system_variables *vars)
{
  st_bookmark *v;
  uint idx;

  mysql_prlock_rdlock(&LOCK_system_variables_hash);
  for (idx= 0; idx < bookmark_hash.records; idx++)
  {
    v= (st_bookmark*) my_hash_element(&bookmark_hash, idx);

    if (v->version > vars->dynamic_variables_version)
      continue; /* not in vars */

    DBUG_ASSERT((uint)v->offset <= vars->dynamic_variables_head);

    /* free allocated strings (PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC) */
    if ((v->key[0] & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR &&
         v->key[0] & BOOKMARK_MEMALLOC)
    {
      char **ptr= (char**)(vars->dynamic_variables_ptr + v->offset);
      my_free(*ptr);
      *ptr= NULL;
    }
  }
  mysql_prlock_unlock(&LOCK_system_variables_hash);

  DBUG_ASSERT(vars->table_plugin == NULL);
  DBUG_ASSERT(vars->tmp_table_plugin == NULL);
  DBUG_ASSERT(vars->enforced_table_plugin == NULL);

  my_free(vars->dynamic_variables_ptr);
  vars->dynamic_variables_ptr= NULL;
  vars->dynamic_variables_size= 0;
  vars->dynamic_variables_version= 0;
}


void plugin_thdvar_cleanup(THD *thd)
{
  size_t idx;
  plugin_ref *list;
  DBUG_ENTER("plugin_thdvar_cleanup");

#ifndef EMBEDDED_LIBRARY
  thd->session_tracker.sysvars.deinit(thd);
  my_free(thd->variables.redirect_url);
  thd->variables.redirect_url= 0;
#endif
  my_free((char*) thd->variables.default_master_connection.str);
  thd->variables.default_master_connection.str= 0;
  thd->variables.default_master_connection.length= 0;

  mysql_mutex_lock(&LOCK_plugin);

  unlock_variables(thd, &thd->variables);
  cleanup_variables(&thd->variables);

  if ((idx= thd->lex->plugins.elements))
  {
    list= ((plugin_ref*) thd->lex->plugins.buffer) + idx - 1;
    DBUG_PRINT("info",("unlocking %zu plugins", idx));
    while ((uchar*) list >= thd->lex->plugins.buffer)
      intern_plugin_unlock(NULL, *list--);
  }

  reap_plugins();
  mysql_mutex_unlock(&LOCK_plugin);

  reset_dynamic(&thd->lex->plugins);

  DBUG_VOID_RETURN;
}


/**
  @brief Free values of thread variables of a plugin.

  This must be called before a plugin is deleted. Otherwise its
  variables are no longer accessible and the value space is lost. Note
  that only string values with PLUGIN_VAR_MEMALLOC are allocated and
  must be freed.
*/

static void plugin_vars_free_values(st_mysql_sys_var **vars)
{
  DBUG_ENTER("plugin_vars_free_values");

  if (!vars)
    DBUG_VOID_RETURN;

  while(st_mysql_sys_var *var= *vars++)
  {
    if ((var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR &&
         var->flags & PLUGIN_VAR_MEMALLOC)
    {
      char **val;
      if (var->flags & PLUGIN_VAR_THDLOCAL)
      {
        st_bookmark *v= find_bookmark(0, var->name, var->flags);
        if (!v)
          continue;
        val= (char**)(global_system_variables.dynamic_variables_ptr + v->offset);
      }
      else
        val= *(char***) (var + 1);

      DBUG_PRINT("plugin", ("freeing value for: '%s'  addr: %p",
                            var->name, val));
      my_free(*val);
      *val= NULL;
    }
  }
  DBUG_VOID_RETURN;
}

static SHOW_TYPE pluginvar_show_type(const st_mysql_sys_var *plugin_var)
{
  switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_UNSIGNED)) {
  case PLUGIN_VAR_BOOL:
    return SHOW_MY_BOOL;
  case PLUGIN_VAR_INT:
    return SHOW_SINT;
  case PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED:
    return SHOW_UINT;
  case PLUGIN_VAR_LONG:
    return SHOW_SLONG;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED:
    return SHOW_ULONG;
  case PLUGIN_VAR_LONGLONG:
    return SHOW_SLONGLONG;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED:
    return SHOW_ULONGLONG;
  case PLUGIN_VAR_STR:
    return SHOW_CHAR_PTR;
  case PLUGIN_VAR_ENUM:
  case PLUGIN_VAR_SET:
    return SHOW_CHAR;
  case PLUGIN_VAR_DOUBLE:
    return SHOW_DOUBLE;
  default:
    DBUG_ASSERT_NO_ASSUME(0);
    return SHOW_UNDEF;
  }
}


static int pluginvar_sysvar_flags(const st_mysql_sys_var *p)
{
  return (p->flags & PLUGIN_VAR_THDLOCAL ? sys_var::SESSION : sys_var::GLOBAL)
       | (p->flags & PLUGIN_VAR_READONLY ? sys_var::READONLY : 0);
}

sys_var_pluginvar::sys_var_pluginvar(sys_var_chain *chain, const char *name_arg,
        st_plugin_int *p, st_mysql_sys_var *pv, const char *substitute)
    : sys_var(chain, name_arg, pv->comment, pluginvar_sysvar_flags(pv),
              0, pv->flags & PLUGIN_VAR_NOCMDOPT ? -1 : 0, NO_ARG,
              pluginvar_show_type(pv), 0,
              NULL, VARIABLE_NOT_IN_BINLOG, NULL, NULL, substitute),
    plugin(p), plugin_var(pv)
{
  plugin_var->name= name_arg;
  plugin_opt_set_limits(&option, pv);
}

uchar* sys_var_pluginvar::real_value_ptr(THD *thd, enum_var_type type) const
{
  if (type == OPT_DEFAULT)
  {
    switch (plugin_var->flags & PLUGIN_VAR_TYPEMASK) {
    case PLUGIN_VAR_BOOL:
      thd->sys_var_tmp.my_bool_value= (my_bool)option.def_value;
      return (uchar*) &thd->sys_var_tmp.my_bool_value;
    case PLUGIN_VAR_INT:
      thd->sys_var_tmp.int_value= (int)option.def_value;
      return (uchar*) &thd->sys_var_tmp.int_value;
    case PLUGIN_VAR_LONG:
    case PLUGIN_VAR_ENUM:
      thd->sys_var_tmp.long_value= (long)option.def_value;
      return (uchar*) &thd->sys_var_tmp.long_value;
    case PLUGIN_VAR_LONGLONG:
    case PLUGIN_VAR_SET:
      return (uchar*) &option.def_value;
    case PLUGIN_VAR_STR:
      thd->sys_var_tmp.ptr_value= (void*) option.def_value;
      return (uchar*) &thd->sys_var_tmp.ptr_value;
    case PLUGIN_VAR_DOUBLE:
      thd->sys_var_tmp.double_value= getopt_ulonglong2double(option.def_value);
      return (uchar*) &thd->sys_var_tmp.double_value;
    default:
      DBUG_ASSERT(0);
    }
  }

  DBUG_ASSERT(thd || (type == OPT_GLOBAL));
  if (plugin_var->flags & PLUGIN_VAR_THDLOCAL)
  {
    if (type == OPT_GLOBAL)
      thd= NULL;

    return (uchar*) intern_sys_var_ptr(thd, *(int*) (plugin_var+1), false);
  }
  return *(uchar**) (plugin_var+1);
}


bool sys_var_pluginvar::session_is_default(THD *thd)
{
  uchar *value= plugin_var->flags & PLUGIN_VAR_THDLOCAL
                ? static_cast<uchar*>(intern_sys_var_ptr(thd, *(int*) (plugin_var+1), true))
                : *reinterpret_cast<uchar**>(plugin_var+1);

    real_value_ptr(thd, OPT_SESSION);

  switch (plugin_var->flags & PLUGIN_VAR_TYPEMASK) {
  case PLUGIN_VAR_BOOL:
    return option.def_value == *(my_bool*)value;
  case PLUGIN_VAR_INT:
    return option.def_value == *(int*)value;
  case PLUGIN_VAR_LONG:
  case PLUGIN_VAR_ENUM:
    return option.def_value == *(long*)value;
  case PLUGIN_VAR_LONGLONG:
  case PLUGIN_VAR_SET:
    return option.def_value == *(longlong*)value;
  case PLUGIN_VAR_STR:
    {
      const char *a=(char*)option.def_value;
      const char *b=(char*)value;
      return (!a && !b) || (a && b && strcmp(a,b));
    }
  case PLUGIN_VAR_DOUBLE:
    return getopt_ulonglong2double(option.def_value) == *(double*)value;
  }
  DBUG_ASSERT_NO_ASSUME(0);
  return 0;
}


TYPELIB* sys_var_pluginvar::plugin_var_typelib(void) const
{
  switch (plugin_var->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL)) {
  case PLUGIN_VAR_ENUM:
    return ((sysvar_enum_t *)plugin_var)->typelib;
  case PLUGIN_VAR_SET:
    return ((sysvar_set_t *)plugin_var)->typelib;
  case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
    return ((thdvar_enum_t *)plugin_var)->typelib;
  case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
    return ((thdvar_set_t *)plugin_var)->typelib;
  default:
    return NULL;
  }
  return NULL;	/* Keep compiler happy */
}


const uchar* sys_var_pluginvar::do_value_ptr(THD *thd, enum_var_type type,
                                             const LEX_CSTRING *base) const
{
  const uchar* result= real_value_ptr(thd, type);

  if ((plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_ENUM)
    result= (uchar*) get_type(plugin_var_typelib(), *(ulong*)result);
  else if ((plugin_var->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_SET)
    result= (uchar*) set_to_string(thd, 0, *(ulonglong*) result,
                                   plugin_var_typelib()->type_names);
  return result;
}

bool sys_var_pluginvar::do_check(THD *thd, set_var *var)
{
  st_item_value_holder value;
  DBUG_ASSERT(!is_readonly());
  DBUG_ASSERT(plugin_var->check);

  value.value_type= item_value_type;
  value.val_str= item_val_str;
  value.val_int= item_val_int;
  value.val_real= item_val_real;
  value.is_unsigned= item_is_unsigned;
  value.item= var->value;

  return plugin_var->check(thd, plugin_var, &var->save_result, &value);
}

bool sys_var_pluginvar::session_update(THD *thd, set_var *var)
{
  DBUG_ASSERT(!is_readonly());
  DBUG_ASSERT(plugin_var->flags & PLUGIN_VAR_THDLOCAL);
  DBUG_ASSERT(thd == current_thd);

  mysql_mutex_lock(&LOCK_global_system_variables);
  void *tgt= real_value_ptr(thd, OPT_SESSION);
  const void *src= var->value ? (void*)&var->save_result
                              : (void*)real_value_ptr(thd, OPT_GLOBAL);
  mysql_mutex_unlock(&LOCK_global_system_variables);

  plugin_var->update(thd, plugin_var, tgt, src);

  return false;
}

static const void *var_def_ptr(st_mysql_sys_var *pv)
{
    switch (pv->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_THDLOCAL)) {
    case PLUGIN_VAR_INT:
      return &((sysvar_uint_t*) pv)->def_val;
    case PLUGIN_VAR_LONG:
      return &((sysvar_ulong_t*) pv)->def_val;
    case PLUGIN_VAR_LONGLONG:
      return &((sysvar_ulonglong_t*) pv)->def_val;
    case PLUGIN_VAR_ENUM:
      return &((sysvar_enum_t*) pv)->def_val;
    case PLUGIN_VAR_SET:
      return &((sysvar_set_t*) pv)->def_val;
    case PLUGIN_VAR_BOOL:
      return &((sysvar_bool_t*) pv)->def_val;
    case PLUGIN_VAR_STR:
      return &((sysvar_str_t*) pv)->def_val;
    case PLUGIN_VAR_DOUBLE:
      return &((sysvar_double_t*) pv)->def_val;
    case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_uint_t*) pv)->def_val;
    case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_ulong_t*) pv)->def_val;
    case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_ulonglong_t*) pv)->def_val;
    case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_enum_t*) pv)->def_val;
    case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_set_t*) pv)->def_val;
    case PLUGIN_VAR_BOOL | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_bool_t*) pv)->def_val;
    case PLUGIN_VAR_STR | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_str_t*) pv)->def_val;
    case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
      return &((thdvar_double_t*) pv)->def_val;
    default:
      DBUG_ASSERT_NO_ASSUME(0);
      return NULL;
    }
}


bool sys_var_pluginvar::global_update(THD *thd, set_var *var)
{
  DBUG_ASSERT(!is_readonly());
  mysql_mutex_assert_owner(&LOCK_global_system_variables);

  void *tgt= real_value_ptr(thd, OPT_GLOBAL);
  const void *src= &var->save_result;

  if (!var->value)
    src= var_def_ptr(plugin_var);

  plugin_var->update(thd, plugin_var, tgt, src);
  return false;
}


#define OPTION_SET_LIMITS(type, options, opt) \
  options->var_type= type; \
  options->def_value= (opt)->def_val; \
  options->min_value= (opt)->min_val; \
  options->max_value= (opt)->max_val; \
  options->block_size= (long) (opt)->blk_sz

#define OPTION_SET_LIMITS_DOUBLE(options, opt) \
  options->var_type= GET_DOUBLE; \
  options->def_value= (longlong) getopt_double2ulonglong((opt)->def_val); \
  options->min_value= (longlong) getopt_double2ulonglong((opt)->min_val); \
  options->max_value= getopt_double2ulonglong((opt)->max_val); \
  options->block_size= (long) (opt)->blk_sz;

void plugin_opt_set_limits(struct my_option *options,
                           const struct st_mysql_sys_var *opt)
{
  switch (opt->flags & (PLUGIN_VAR_TYPEMASK |
                        PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL)) {
  /* global system variables */
  case PLUGIN_VAR_INT:
    OPTION_SET_LIMITS(GET_INT, options, (sysvar_int_t*) opt);
    break;
  case PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED:
    OPTION_SET_LIMITS(GET_UINT, options, (sysvar_uint_t*) opt);
    break;
  case PLUGIN_VAR_LONG:
    OPTION_SET_LIMITS(GET_LONG, options, (sysvar_long_t*) opt);
    break;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED:
    OPTION_SET_LIMITS(GET_ULONG, options, (sysvar_ulong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG:
    OPTION_SET_LIMITS(GET_LL, options, (sysvar_longlong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED:
    OPTION_SET_LIMITS(GET_ULL, options, (sysvar_ulonglong_t*) opt);
    break;
  case PLUGIN_VAR_ENUM:
    options->var_type= GET_ENUM;
    options->typelib= ((sysvar_enum_t*) opt)->typelib;
    options->def_value= ((sysvar_enum_t*) opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= options->typelib->count - 1;
    break;
  case PLUGIN_VAR_SET:
    options->var_type= GET_SET;
    options->typelib= ((sysvar_set_t*) opt)->typelib;
    options->def_value= ((sysvar_set_t*) opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= (1ULL << options->typelib->count) - 1;
    break;
  case PLUGIN_VAR_BOOL:
    options->var_type= GET_BOOL;
    options->def_value= ((sysvar_bool_t*) opt)->def_val;
    options->typelib= &bool_typelib;
    break;
  case PLUGIN_VAR_STR:
    options->var_type= ((opt->flags & PLUGIN_VAR_MEMALLOC) ?
                        GET_STR_ALLOC : GET_STR);
    options->def_value= (intptr) ((sysvar_str_t*) opt)->def_val;
    break;
  case PLUGIN_VAR_DOUBLE:
    OPTION_SET_LIMITS_DOUBLE(options, (sysvar_double_t*) opt);
    break;
  /* threadlocal variables */
  case PLUGIN_VAR_INT | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_INT, options, (thdvar_int_t*) opt);
    break;
  case PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_UINT, options, (thdvar_uint_t*) opt);
    break;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_LONG, options, (thdvar_long_t*) opt);
    break;
  case PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_ULONG, options, (thdvar_ulong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_LL, options, (thdvar_longlong_t*) opt);
    break;
  case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS(GET_ULL, options, (thdvar_ulonglong_t*) opt);
    break;
  case PLUGIN_VAR_DOUBLE | PLUGIN_VAR_THDLOCAL:
    OPTION_SET_LIMITS_DOUBLE(options, (thdvar_double_t*) opt);
    break;
  case PLUGIN_VAR_ENUM | PLUGIN_VAR_THDLOCAL:
    options->var_type= GET_ENUM;
    options->typelib= reinterpret_cast<const thdvar_enum_t*>(opt)->typelib;
    options->def_value= reinterpret_cast<const thdvar_enum_t*>(opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= options->typelib->count - 1;
    break;
  case PLUGIN_VAR_SET | PLUGIN_VAR_THDLOCAL:
    options->var_type= GET_SET;
    options->typelib= reinterpret_cast<const thdvar_set_t*>(opt)->typelib;
    options->def_value= reinterpret_cast<const thdvar_set_t*>(opt)->def_val;
    options->min_value= options->block_size= 0;
    options->max_value= (1ULL << options->typelib->count) - 1;
    break;
  case PLUGIN_VAR_BOOL | PLUGIN_VAR_THDLOCAL:
    options->var_type= GET_BOOL;
    options->def_value= reinterpret_cast<const thdvar_bool_t*>(opt)->def_val;
    options->typelib= &bool_typelib;
    break;
  case PLUGIN_VAR_STR | PLUGIN_VAR_THDLOCAL:
    options->var_type= ((opt->flags & PLUGIN_VAR_MEMALLOC) ?
                        GET_STR_ALLOC : GET_STR);
    options->def_value= reinterpret_cast<intptr_t>(reinterpret_cast<const thdvar_str_t*>(opt)->def_val);
    break;
  default:
    DBUG_ASSERT(0);
  }
  options->arg_type= REQUIRED_ARG;
  if (opt->flags & PLUGIN_VAR_NOCMDARG)
    options->arg_type= NO_ARG;
  if (opt->flags & PLUGIN_VAR_OPCMDARG)
    options->arg_type= OPT_ARG;
}

/**
  Creates a set of my_option objects associated with a specified plugin-
  handle.

  @param mem_root Memory allocator to be used.
  @param tmp A pointer to a plugin handle
  @param[out] options A pointer to a pre-allocated static array

  The set is stored in the pre-allocated static array supplied to the function.
  The size of the array is calculated as (number_of_plugin_varaibles*2+3). The
  reason is that each option can have a prefix '--plugin-' in addition to the
  shorter form '--&lt;plugin-name&gt;'. There is also space allocated for
  terminating NULL pointers.

  @return
    @retval -1 An error occurred
    @retval 0 Success
*/

static int construct_options(MEM_ROOT *mem_root, struct st_plugin_int *tmp,
                             my_option *options)
{
  const char *plugin_name= tmp->plugin->name;
  const LEX_CSTRING plugin_dash = { STRING_WITH_LEN("plugin-") };
  size_t plugin_name_len= strlen(plugin_name);
  size_t optnamelen;
  const int max_comment_len= 255;
  char *comment= static_cast<char*>(alloc_root(mem_root, max_comment_len + 1));
  char *optname;

  int index= 0, UNINIT_VAR(offset);
  st_mysql_sys_var *opt, **plugin_option;
  st_bookmark *v;

  /** Used to circumvent the const attribute on my_option::name */
  char *plugin_name_ptr, *plugin_name_with_prefix_ptr;

  DBUG_ENTER("construct_options");

  plugin_name_ptr= static_cast<char*>(alloc_root(mem_root, plugin_name_len + 1));
  safe_strcpy(plugin_name_ptr, plugin_name_len + 1, plugin_name);
  my_casedn_str_latin1(plugin_name_ptr); // Plugin names are pure ASCII
  convert_underscore_to_dash(plugin_name_ptr, plugin_name_len);
  plugin_name_with_prefix_ptr= (char*) alloc_root(mem_root,
                                                  plugin_name_len +
                                                  plugin_dash.length + 1);
  strxmov(plugin_name_with_prefix_ptr, plugin_dash.str, plugin_name_ptr, NullS);

  if (!plugin_is_forced(tmp))
  {
    /* support --skip-plugin-foo syntax */
    options[0].name= plugin_name_ptr;
    options[1].name= plugin_name_with_prefix_ptr;
    options[0].id= options[1].id= 0;
    options[0].var_type= options[1].var_type= GET_ENUM;
    options[0].arg_type= options[1].arg_type= OPT_ARG;
    options[0].def_value= options[1].def_value= 1; /* ON */
    options[0].typelib= options[1].typelib= &global_plugin_typelib;

    strxnmov(comment, max_comment_len, "Enable or disable ", plugin_name,
            " plugin. One of: ON, OFF, FORCE (don't start if the plugin"
            " fails to load), FORCE_PLUS_PERMANENT (like FORCE, but the"
            " plugin can not be uninstalled).", NullS);
    options[0].comment= comment;
    /*
      Allocate temporary space for the value of the tristate.
      This option will have a limited lifetime and is not used beyond
      server initialization.
      GET_ENUM value is an unsigned long integer.
    */
    options[0].value= options[1].value=
                      (uchar **)alloc_root(mem_root, sizeof(ulong));
    *((ulong*) options[0].value)= (ulong) options[0].def_value;

    options+= 2;
  }

  /*
    Two passes as the 2nd pass will take pointer addresses for use
    by my_getopt and register_var() in the first pass uses realloc
  */

  for (plugin_option= tmp->plugin->system_vars;
       plugin_option && *plugin_option; plugin_option++, index++)
  {
    opt= *plugin_option;

    if (!opt->name)
    {
      sql_print_error("Missing variable name in plugin '%s'.",
                      plugin_name);
      DBUG_RETURN(-1);
    }

    if (!(opt->flags & PLUGIN_VAR_THDLOCAL))
      continue;
    if (!(register_var(plugin_name_ptr, opt->name, opt->flags)))
      continue;
    switch (opt->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_UNSIGNED)) {
    case PLUGIN_VAR_BOOL:
      ((thdvar_bool_t *) opt)->resolve= mysql_sys_var_char;
      break;
    case PLUGIN_VAR_INT:
      ((thdvar_int_t *) opt)->resolve= mysql_sys_var_int;
      break;
    case PLUGIN_VAR_INT | PLUGIN_VAR_UNSIGNED:
      ((thdvar_uint_t *) opt)->resolve= mysql_sys_var_uint;
      break;
    case PLUGIN_VAR_LONG:
      ((thdvar_long_t *) opt)->resolve= mysql_sys_var_long;
      break;
    case PLUGIN_VAR_LONG | PLUGIN_VAR_UNSIGNED:
      ((thdvar_ulong_t *) opt)->resolve= mysql_sys_var_ulong;
      break;
    case PLUGIN_VAR_LONGLONG:
      ((thdvar_longlong_t *) opt)->resolve= mysql_sys_var_longlong;
      break;
    case PLUGIN_VAR_LONGLONG | PLUGIN_VAR_UNSIGNED:
      ((thdvar_ulonglong_t *) opt)->resolve= mysql_sys_var_ulonglong;
      break;
    case PLUGIN_VAR_STR:
      ((thdvar_str_t *) opt)->resolve= mysql_sys_var_str;
      break;
    case PLUGIN_VAR_ENUM:
      ((thdvar_enum_t *) opt)->resolve= mysql_sys_var_ulong;
      break;
    case PLUGIN_VAR_SET:
      ((thdvar_set_t *) opt)->resolve= mysql_sys_var_ulonglong;
      break;
    case PLUGIN_VAR_DOUBLE:
      ((thdvar_double_t *) opt)->resolve= mysql_sys_var_double;
      break;
    default:
      sql_print_error("Unknown variable type code 0x%x in plugin '%s'.",
                      opt->flags, plugin_name);
      DBUG_RETURN(-1);
    };
  }

  for (plugin_option= tmp->plugin->system_vars;
       plugin_option && *plugin_option; plugin_option++, index++)
  {
    switch ((opt= *plugin_option)->flags & PLUGIN_VAR_TYPEMASK) {
    case PLUGIN_VAR_BOOL:
      if (!opt->check)
        opt->check= check_func_bool;
      if (!opt->update)
        opt->update= update_func_bool;
      break;
    case PLUGIN_VAR_INT:
      if (!opt->check)
        opt->check= check_func_int;
      if (!opt->update)
        opt->update= update_func_int;
      break;
    case PLUGIN_VAR_LONG:
      if (!opt->check)
        opt->check= check_func_long;
      if (!opt->update)
        opt->update= update_func_long;
      break;
    case PLUGIN_VAR_LONGLONG:
      if (!opt->check)
        opt->check= check_func_longlong;
      if (!opt->update)
        opt->update= update_func_longlong;
      break;
    case PLUGIN_VAR_STR:
      if (!opt->check)
        opt->check= check_func_str;
      if (!opt->update)
      {
        opt->update= update_func_str;
        if (!(opt->flags & (PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_READONLY)))
        {
          opt->flags|= PLUGIN_VAR_READONLY;
          sql_print_warning("Server variable %s of plugin %s was forced "
                            "to be read-only: string variable without "
                            "update_func and PLUGIN_VAR_MEMALLOC flag",
                            opt->name, plugin_name);
        }
      }
      break;
    case PLUGIN_VAR_ENUM:
      if (!opt->check)
        opt->check= check_func_enum;
      if (!opt->update)
        opt->update= update_func_long;
      break;
    case PLUGIN_VAR_SET:
      if (!opt->check)
        opt->check= check_func_set;
      if (!opt->update)
        opt->update= update_func_longlong;
      break;
    case PLUGIN_VAR_DOUBLE:
      if (!opt->check)
        opt->check= check_func_double;
      if (!opt->update)
        opt->update= update_func_double;
      break;
    default:
      sql_print_error("Unknown variable type code 0x%x in plugin '%s'.",
                      opt->flags, plugin_name);
      DBUG_RETURN(-1);
    }

    if ((opt->flags & (PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_THDLOCAL))
                    == PLUGIN_VAR_NOCMDOPT)
      continue;

    if (!(opt->flags & PLUGIN_VAR_THDLOCAL))
    {
      optnamelen= strlen(opt->name);
      optname= (char*) alloc_root(mem_root, plugin_name_len + optnamelen + 2);
      strxmov(optname, plugin_name_ptr, "-", opt->name, NullS);
      optnamelen= plugin_name_len + optnamelen + 1;
    }
    else
    {
      /* this should not fail because register_var should create entry */
      if (!(v= find_bookmark(plugin_name_ptr, opt->name, opt->flags)))
      {
        sql_print_error("Thread local variable '%s' not allocated "
                        "in plugin '%s'.", opt->name, plugin_name);
        DBUG_RETURN(-1);
      }

      *(int*)(opt + 1)= offset= v->offset;

      if (opt->flags & PLUGIN_VAR_NOCMDOPT)
      {
        char *val= global_system_variables.dynamic_variables_ptr + offset;
        if (((opt->flags & PLUGIN_VAR_TYPEMASK) == PLUGIN_VAR_STR) &&
             (opt->flags & PLUGIN_VAR_MEMALLOC))
        {
          char *def_val= *(char**)var_def_ptr(opt);
          *(char**)val= def_val ? my_strdup(PSI_INSTRUMENT_ME, def_val, MYF(0)) : NULL;
        }
        else
          memcpy(val, var_def_ptr(opt), var_storage_size(opt->flags));
        continue;
      }

      optname= (char*) memdup_root(mem_root, v->key + 1,
                                   (optnamelen= v->name_len) + 1);
    }

    convert_underscore_to_dash(optname, optnamelen);

    options->name= optname;
    options->comment= opt->comment;
    options->app_type= (opt->flags & PLUGIN_VAR_NOSYSVAR) ? NULL : opt;
    options->id= 0;

    plugin_opt_set_limits(options, opt);

    if (opt->flags & PLUGIN_VAR_THDLOCAL)
      options->value= options->u_max_value= (uchar**)
        (global_system_variables.dynamic_variables_ptr + offset);
    else
      options->value= options->u_max_value= *(uchar***) (opt + 1);

    if (opt->flags & PLUGIN_VAR_DEPRECATED)
      options->deprecation_substitute= "";

    char *option_name_ptr;
    options[1]= options[0];
    options[1].name= option_name_ptr= (char*) alloc_root(mem_root,
                                                        plugin_dash.length +
                                                        optnamelen + 1);
    options[1].comment= 0; /* Hidden from the help text */
    strxmov(option_name_ptr, plugin_dash.str, optname, NullS);

    options+= 2;
  }

  DBUG_RETURN(0);
}


static my_option *construct_help_options(MEM_ROOT *mem_root,
                                         struct st_plugin_int *p)
{
  st_mysql_sys_var **opt;
  my_option *opts;
  uint count= EXTRA_OPTIONS;
  DBUG_ENTER("construct_help_options");

  for (opt= p->plugin->system_vars; opt && *opt; opt++, count+= 2)
    ;

  if (!(opts= (my_option*) alloc_root(mem_root, sizeof(my_option) * count)))
    DBUG_RETURN(NULL);

  bzero(opts, sizeof(my_option) * count);

  /**
    some plugin variables
    have their names prefixed with the plugin name. Restore the names here
    to get the correct (not double-prefixed) help text.
    We won't need @@sysvars anymore and don't care about their proper names.
  */
  restore_ptr_backup(p->nbackups, p->ptr_backup);

  if (construct_options(mem_root, p, opts))
    DBUG_RETURN(NULL);

  DBUG_RETURN(opts);
}

extern "C" my_bool mark_changed(const struct my_option *, const char *,
                                const char *);
my_bool mark_changed(const struct my_option *opt, const char *,
                     const char *filename)
{
  if (opt->app_type)
  {
    sys_var *var= (sys_var*) opt->app_type;
    if (*filename)
    {
      var->origin_filename= filename;
      var->value_origin= sys_var::CONFIG;
    }
    else
      var->value_origin= sys_var::COMMAND_LINE;
  }
  return 0;
}

/**
  It is always false to mark global plugin variable unloaded just to be
  safe because we have no way now to know truth about them.

  TODO: make correct mechanism for global plugin variables
*/
static bool static_unload= FALSE;

/**
  Create and register system variables supplied from the plugin and
  assigns initial values from corresponding command line arguments.

  @param tmp_root Temporary scratch space
  @param[out] plugin Internal plugin structure
  @param argc Number of command line arguments
  @param argv Command line argument vector

  The plugin will be updated with a policy on how to handle errors during
  initialization.

  @note Requires that a write-lock is held on LOCK_system_variables_hash

  @return How initialization of the plugin should be handled.
    @retval  0 Initialization should proceed.
    @retval  1 Plugin is disabled.
    @retval -1 An error has occurred.
*/

static int test_plugin_options(MEM_ROOT *tmp_root, struct st_plugin_int *tmp,
                               int *argc, char **argv)
{
  struct sys_var_chain chain= { NULL, NULL };
  bool disable_plugin;
  enum_plugin_load_option plugin_load_option= tmp->load_option;

  MEM_ROOT *mem_root= alloc_root_inited(&tmp->mem_root) ?
                      &tmp->mem_root : &plugin_vars_mem_root;
  st_mysql_sys_var **opt;
  my_option *opts= NULL;
  int error= 1;
  struct st_bookmark *var;
  size_t len=0, count= EXTRA_OPTIONS;
  st_ptr_backup *tmp_backup= 0;
  DBUG_ENTER("test_plugin_options");
  DBUG_ASSERT(tmp->plugin && tmp->name.str);

  if (tmp->plugin->system_vars || (*argc > 1))
  {
    for (opt= tmp->plugin->system_vars; opt && *opt; opt++)
    {
      len++;
      if (!((*opt)->flags & PLUGIN_VAR_NOCMDOPT))
        count+= 2; /* --{plugin}-{optname} and --plugin-{plugin}-{optname} */
    }

    if (!(opts= (my_option*) alloc_root(tmp_root, sizeof(my_option) * count)))
    {
      sql_print_error("Out of memory for plugin '%s'.", tmp->name.str);
      DBUG_RETURN(-1);
    }
    bzero(opts, sizeof(my_option) * count);

    if (construct_options(tmp_root, tmp, opts))
    {
      sql_print_error("Bad options for plugin '%s'.", tmp->name.str);
      DBUG_RETURN(-1);
    }

    if (tmp->plugin->system_vars)
    {
      tmp_backup= (st_ptr_backup *)my_alloca(len * sizeof(tmp_backup[0]));
      DBUG_ASSERT(tmp->nbackups == 0);
      DBUG_ASSERT(tmp->ptr_backup == 0);

      for (opt= tmp->plugin->system_vars; *opt; opt++)
      {
        st_mysql_sys_var *o= *opt;
        char *varname;
        sys_var *v;

        tmp_backup[tmp->nbackups++].save(&o->name);
        if ((var= find_bookmark(tmp->name.str, o->name, o->flags)))
        {
          varname= var->key + 1;
          var->loaded= TRUE;
        }
        else
        {
          var= NULL;
          len= tmp->name.length + strlen(o->name) + 2;
          varname= (char*) alloc_root(mem_root, len);
          strxmov(varname, tmp->name.str, "_", o->name, NullS);
          // Ok to use latin1, as the variable name is pure ASCII
          my_casedn_str_latin1(varname);
        }
        if (o->flags & PLUGIN_VAR_NOSYSVAR)
        {
          o->name= varname;
          continue;
        }

        const char *s= o->flags & PLUGIN_VAR_DEPRECATED ? "" : NULL;
        v= new (mem_root) sys_var_pluginvar(&chain, varname, tmp, o, s);
        v->test_load= (var ? &var->loaded : &static_unload);
        DBUG_ASSERT(static_unload == FALSE);

        if (!(o->flags & PLUGIN_VAR_NOCMDOPT))
        {
          // update app_type, used for I_S.SYSTEM_VARIABLES
          for (my_option *mo=opts; mo->name; mo++)
            if (mo->app_type == o)
              mo->app_type= v;
        }
      }

      if (tmp->nbackups)
      {
        size_t bytes= tmp->nbackups * sizeof(tmp->ptr_backup[0]);
        tmp->ptr_backup= (st_ptr_backup *)alloc_root(mem_root, bytes);
        if (!tmp->ptr_backup)
        {
          restore_ptr_backup(tmp->nbackups, tmp_backup);
          my_afree(tmp_backup);
          goto err;
        }
        memcpy(tmp->ptr_backup, tmp_backup, bytes);
      }
      my_afree(tmp_backup);
    }

    /*
      We adjust the default value to account for the hardcoded exceptions
      we have set for the federated and ndbcluster storage engines.
    */
    if (!plugin_is_forced(tmp))
      opts[0].def_value= opts[1].def_value= plugin_load_option;

    error= handle_options(argc, &argv, opts, mark_changed);
    (*argc)++; /* add back one for the program name */

    if (unlikely(error))
    {
       sql_print_error("Parsing options for plugin '%s' failed. Disabling plugin",
                       tmp->name.str);
       goto err;
    }
    /*
     Set plugin loading policy from option value. First element in the option
     list is always the <plugin name> option value.
    */
    if (!plugin_is_forced(tmp))
      plugin_load_option= (enum_plugin_load_option) *(ulong*) opts[0].value;
  }

  disable_plugin= (plugin_load_option == PLUGIN_OFF);
  tmp->load_option= plugin_load_option;

  error= 1;

  /*
    If the plugin is disabled it should not be initialized.
  */
  if (disable_plugin)
  {
    if (global_system_variables.log_warnings && !opt_help)
      sql_print_information("Plugin '%s' is disabled.",
                            tmp->name.str);
    goto err;
  }

  if (tmp->plugin->system_vars)
  {
    if (mysqld_server_started)
    {
      /*
        PLUGIN_VAR_STR command-line options without PLUGIN_VAR_MEMALLOC, point
        directly to values in the argv[] array. For plugins started at the
        server startup, argv[] array is allocated with load_defaults(), and
        freed when the server is shut down.  But for plugins loaded with
        INSTALL PLUGIN, the memory allocated with load_defaults() is freed with
        free() at the end of mysql_install_plugin(). Which means we cannot
        allow any pointers into that area.
        Thus, for all plugins loaded after the server was started,
        we copy string values to a plugin's memroot.
      */
      for (opt= tmp->plugin->system_vars; *opt; opt++)
      {
        if ((((*opt)->flags & (PLUGIN_VAR_TYPEMASK | PLUGIN_VAR_NOCMDOPT |
                               PLUGIN_VAR_MEMALLOC)) == PLUGIN_VAR_STR))
        {
          sysvar_str_t* str= (sysvar_str_t *)*opt;
          if (*str->value)
            *str->value= strdup_root(mem_root, *str->value);
        }
      }
      /* same issue with config file names */
      for (my_option *mo=opts; mo->name; mo++)
      {
        sys_var *var= (sys_var*) mo->app_type;
        if (var && var->value_origin == sys_var::CONFIG)
          var->origin_filename= strdup_root(mem_root, var->origin_filename);
      }
    }

    if (chain.first)
    {
      chain.last->next = NULL;
      if (mysql_add_sys_var_chain(chain.first))
      {
        sql_print_error("Plugin '%s' has conflicting system variables",
                        tmp->name.str);
        goto err;
      }
      tmp->system_vars= chain.first;
    }
  }

  DBUG_RETURN(0);

err:
  if (opts)
    my_cleanup_options(opts);
  DBUG_RETURN(error);
}


/****************************************************************************
  Help Verbose text with Plugin System Variables
****************************************************************************/


void add_plugin_options(DYNAMIC_ARRAY *options, MEM_ROOT *mem_root)
{
  struct st_plugin_int *p;
  my_option *opt;

  if (!initialized)
    return;

  for (size_t idx= 0; idx < plugin_array.elements; idx++)
  {
    p= *dynamic_element(&plugin_array, idx, struct st_plugin_int **);

    if (!(opt= construct_help_options(mem_root, p)))
      continue;

    /* Only options with a non-NULL comment are displayed in help text */
    for (;opt->name; opt++)
      if (opt->comment)
        insert_dynamic(options, (uchar*) opt);
  }
}


/**
  Returns a sys_var corresponding to a particular MYSQL_SYSVAR(...)
*/
sys_var *find_plugin_sysvar(st_plugin_int *plugin, st_mysql_sys_var *plugin_var)
{
  for (sys_var *var= plugin->system_vars; var; var= var->next)
  {
    sys_var_pluginvar *pvar=var->cast_pluginvar();
    if (pvar->plugin_var == plugin_var)
      return var;
  }
  return 0;
}

/*
  On dlclose() we need to restore values of all symbols that we've modified in
  the DSO. The reason is - the DSO might not actually be unloaded, so on the
  next dlopen() these symbols will have old values, they won't be
  reinitialized.

  Perhaps, there can be many reason, why a DSO won't be unloaded. Strictly
  speaking, it's implementation defined whether to unload an unused DSO or to
  keep it in memory.

  In particular, this happens for some plugins: In 2009 a new ELF stub was
  introduced, see Ulrich Drepper's email "Unique symbols for C++"
  http://www.redhat.com/archives/posix-c++-wg/2009-August/msg00002.html

  DSO that has objects with this stub (STB_GNU_UNIQUE) cannot be unloaded
  (this is mentioned in the email, see the url above).

  These "unique" objects are, for example, static variables in templates,
  in inline functions, in classes. So any DSO that uses them can
  only be loaded once. And because Boost has them, any DSO that uses Boost
  almost certainly cannot be unloaded.

  To know whether a particular DSO has these objects, one can use

    readelf -s /path/to/plugin.so|grep UNIQUE

  There's nothing we can do about it, but to reset the DSO to its initial
  state before dlclose().
*/
static void restore_ptr_backup(uint n, st_ptr_backup *backup)
{
  while (n--)
    (backup++)->restore();
}

/****************************************************************************
  thd specifics service, see include/mysql/service_thd_specifics.h
****************************************************************************/
static const int INVALID_THD_KEY= -1;
static uint thd_key_no = 42;

int thd_key_create(MYSQL_THD_KEY_T *key)
{
  int flags= PLUGIN_VAR_THDLOCAL | PLUGIN_VAR_STR |
             PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT;
  char namebuf[256];
  snprintf(namebuf, sizeof(namebuf), "%u", thd_key_no++);
  mysql_prlock_wrlock(&LOCK_system_variables_hash);
  // non-letters in the name as an extra safety
  st_bookmark *bookmark= register_var("\a\v\a\t\a\r", namebuf, flags);
  mysql_prlock_unlock(&LOCK_system_variables_hash);
  if (bookmark)
  {
    *key= bookmark->offset;
    return 0;
  }
  return ENOMEM;
}

void thd_key_delete(MYSQL_THD_KEY_T *key)
{
  *key= INVALID_THD_KEY;
}

void* thd_getspecific(MYSQL_THD thd, MYSQL_THD_KEY_T key)
{
  DBUG_ASSERT(key != INVALID_THD_KEY);
  if (key == INVALID_THD_KEY || (!thd && !(thd= current_thd)))
    return 0;

  return *(void**)(intern_sys_var_ptr(thd, key, true));
}

int thd_setspecific(MYSQL_THD thd, MYSQL_THD_KEY_T key, void *value)
{
  DBUG_ASSERT(key != INVALID_THD_KEY);
  if (key == INVALID_THD_KEY || (!thd && !(thd= current_thd)))
    return EINVAL;

  memcpy(intern_sys_var_ptr(thd, key, true), &value, sizeof(void*));
  return 0;
}

void plugin_mutex_init()
{
  init_plugin_psi_keys();
  mysql_mutex_init(key_LOCK_plugin, &LOCK_plugin, MY_MUTEX_INIT_FAST);
}

#ifdef WITH_WSREP

/*
  Placeholder for global_system_variables.table_plugin required during
  initialization of startup wsrep threads.
*/
static st_plugin_int wsrep_dummy_plugin;
static st_plugin_int *wsrep_dummy_plugin_ptr;

/*
  Initialize wsrep_dummy_plugin and assign it to
  global_system_variables.table_plugin.
*/
void wsrep_plugins_pre_init()
{
  wsrep_dummy_plugin_ptr= &wsrep_dummy_plugin;
  wsrep_dummy_plugin.state= PLUGIN_IS_DISABLED;
  global_system_variables.table_plugin=
    plugin_int_to_ref(wsrep_dummy_plugin_ptr);
}

/*
  This function is intended to be called after the plugins and related
  global system variables are initialized. It re-initializes some data
  members of wsrep startup threads with correct values, as these value
  were not available at the time these threads were created.
*/

my_bool post_init_callback(THD *thd, void *)
{
  DBUG_ASSERT(!current_thd);
  if (thd->wsrep_applier)
  {
    // Save options_bits as it will get overwritten in
    // plugin_thdvar_init() (verified)
    ulonglong option_bits_saved= thd->variables.option_bits;

    set_current_thd(thd);
    plugin_thdvar_init(thd);

    // Restore option_bits
    thd->variables.option_bits= option_bits_saved;
  }
  set_current_thd(0);
  return 0;
}


void wsrep_plugins_post_init()
{
  mysql_mutex_lock(&LOCK_global_system_variables);
  server_threads.iterate(post_init_callback);
  mysql_mutex_unlock(&LOCK_global_system_variables);
}
#endif /* WITH_WSREP */
