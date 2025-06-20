/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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


/*
  The servers are saved in the system table "servers"
  
  Currently, when the user performs an ALTER SERVER or a DROP SERVER
  operation, it will cause all open tables which refer to the named
  server connection to be flushed. This may cause some undesirable
  behaviour with regard to currently running transactions. It is 
  expected that the DBA knows what s/he is doing when s/he performs
  the ALTER SERVER or DROP SERVER operation.
  
  TODO:
  It is desirable for us to implement a callback mechanism instead where
  callbacks can be registered for specific server protocols. The callback
  will be fired when such a server name has been created/altered/dropped
  or when statistics are to be gathered such as how many actual connections.
  Storage engines etc will be able to make use of the callback so that
  currently running transactions etc will not be disrupted.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_servers.h"
#include "unireg.h"
#include "sql_base.h"                           // close_mysql_tables
#include "records.h"          // init_read_record, end_read_record
#include <m_ctype.h>
#include <stdarg.h>
#include "sp_head.h"
#include "sp.h"
#include "transaction.h"
#include "lock.h"                               // MYSQL_LOCK_IGNORE_TIMEOUT
#include "create_options.h"

/*
  We only use 1 mutex to guard the data structures - THR_LOCK_servers.
  Read locked when only reading data and write-locked for all other access.
*/

static HASH servers_cache;
static MEM_ROOT mem;
static mysql_rwlock_t THR_LOCK_servers;
static LEX_CSTRING MYSQL_SERVERS_NAME= {STRING_WITH_LEN("servers") };


static bool get_server_from_table_to_cache(TABLE *table);

/* insert functions */
static int insert_server(THD *thd, FOREIGN_SERVER *server_options);
static int insert_server_record(TABLE *table, FOREIGN_SERVER *server);
static int insert_server_record_into_cache(FOREIGN_SERVER *server);
static FOREIGN_SERVER *
prepare_server_struct_for_insert(LEX_SERVER_OPTIONS *server_options);
/* drop functions */ 
static int delete_server_record(TABLE *table, LEX_CSTRING *name);
static int delete_server_record_in_cache(LEX_SERVER_OPTIONS *server_options);

/* update functions */
static void prepare_server_struct_for_update(LEX_SERVER_OPTIONS *server_options,
                                             FOREIGN_SERVER *existing,
                                             FOREIGN_SERVER *altered);
static int update_server(THD *thd, FOREIGN_SERVER *existing, 
					     FOREIGN_SERVER *altered);
static int update_server_record(TABLE *table, FOREIGN_SERVER *server);
static int update_server_record_in_cache(FOREIGN_SERVER *existing,
                                         FOREIGN_SERVER *altered);
/* utility functions */
static void merge_server_struct(FOREIGN_SERVER *from, FOREIGN_SERVER *to);

static const uchar *servers_cache_get_key(const void *server_, size_t *length,
                                          my_bool)
{
  auto server= static_cast<const FOREIGN_SERVER *>(server_);
  DBUG_ENTER("servers_cache_get_key");
  DBUG_PRINT("info", ("server_name_length %zd server_name %s",
                      server->server_name_length,
                      server->server_name));

  *length= server->server_name_length;
  DBUG_RETURN(reinterpret_cast<const uchar *>(server->server_name));
}

static PSI_memory_key key_memory_servers;

#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_key key_rwlock_THR_LOCK_servers;

static PSI_rwlock_info all_servers_cache_rwlocks[]=
{
  { &key_rwlock_THR_LOCK_servers, "THR_LOCK_servers", PSI_FLAG_GLOBAL}
};

static PSI_memory_info all_servers_cache_memory[]=
{
  { &key_memory_servers, "servers_cache", PSI_FLAG_GLOBAL}
};

static void init_servers_cache_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_servers_cache_rwlocks);
  PSI_server->register_rwlock(category, all_servers_cache_rwlocks, count);

  count= array_elements(all_servers_cache_memory);
  mysql_memory_register(category, all_servers_cache_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */


struct close_cached_connection_tables_arg
{
  THD *thd;
  LEX_CSTRING *connection;
  TABLE_LIST *tables;
};


static my_bool close_cached_connection_tables_callback(void *el, void *a)
{
  TDC_element *element= static_cast<TDC_element*>(el);
  auto arg= static_cast<close_cached_connection_tables_arg*>(a);
  TABLE_LIST *tmp;

  mysql_mutex_lock(&element->LOCK_table_share);
  /* Ignore if table is not open or does not have a connect_string */
  if (!element->share || !element->share->connect_string.length ||
      !element->ref_count)
    goto end;

  /* Compare the connection string */
  if (arg->connection &&
      (arg->connection->length > element->share->connect_string.length ||
       (arg->connection->length < element->share->connect_string.length &&
        (element->share->connect_string.str[arg->connection->length] != '/' &&
         element->share->connect_string.str[arg->connection->length] != '\\')) ||
       strncasecmp(arg->connection->str, element->share->connect_string.str,
                   arg->connection->length)))
    goto end;

  /* close_cached_tables() only uses these elements */
  if (!(tmp= (TABLE_LIST*) alloc_root(arg->thd->mem_root, sizeof(TABLE_LIST))) ||
      !(arg->thd->make_lex_string(&tmp->db, element->share->db.str, element->share->db.length)) ||
      !(arg->thd->make_lex_string(&tmp->table_name, element->share->table_name.str,
                                      element->share->table_name.length)))
  {
    mysql_mutex_unlock(&element->LOCK_table_share);
    return TRUE;
  }

  tmp->next_global= tmp->next_local= arg->tables;
  MDL_REQUEST_INIT(&tmp->mdl_request, MDL_key::TABLE, tmp->db.str,
                   tmp->table_name.str, MDL_EXCLUSIVE, MDL_TRANSACTION);
  arg->tables= tmp;

end:
  mysql_mutex_unlock(&element->LOCK_table_share);
  return FALSE;
}


/**
  Close all tables which match specified connection string or
  if specified string is NULL, then any table with a connection string.

  @return false  ok
  @return true   error, some tables may keep using old server info
*/

static bool close_cached_connection_tables(THD *thd, LEX_CSTRING *connection)
{
  close_cached_connection_tables_arg argument= { thd, connection, 0 };
  DBUG_ENTER("close_cached_connections");

  if (tdc_iterate(thd, close_cached_connection_tables_callback, &argument))
    DBUG_RETURN(true);

  DBUG_RETURN(argument.tables ?
              close_cached_tables(thd, argument.tables, true,
                                  thd->variables.lock_wait_timeout) : false);
}


/*
  Initialize structures responsible for servers used in federated
  server scheme information for them from the server
  table in the 'mysql' database.

  SYNOPSIS
    servers_init()
      dont_read_server_table  TRUE if we want to skip loading data from
                            server table and disable privilege checking.

  NOTES
    This function is mostly responsible for preparatory steps, main work
    on initialization and grants loading is done in servers_reload().

  RETURN VALUES
    0	ok
    1	Could not initialize servers
*/

bool servers_init(bool dont_read_servers_table)
{
  THD  *thd;
  bool return_val= FALSE;
  DBUG_ENTER("servers_init");

#ifdef HAVE_PSI_INTERFACE
  init_servers_cache_psi_keys();
#endif

  /* init the mutex */
  if (mysql_rwlock_init(key_rwlock_THR_LOCK_servers, &THR_LOCK_servers))
    DBUG_RETURN(TRUE);

  /* initialise our servers cache */
  if (my_hash_init(key_memory_servers, &servers_cache,
                   Lex_ident_server::charset_info(),
                   32, 0, 0, servers_cache_get_key, 0, 0))
  {
    return_val= TRUE; /* we failed, out of memory? */
    goto end;
  }

  /* Initialize the mem root for data */
  init_sql_alloc(key_memory_servers, &mem, ACL_ALLOC_BLOCK_SIZE, 0,
                 MYF(MY_THREAD_SPECIFIC));

  if (dont_read_servers_table)
    goto end;

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD(0)))
    DBUG_RETURN(TRUE);
  thd->store_globals();
  thd->set_query_inner((char*) STRING_WITH_LEN("intern:servers_init"),
                       default_charset_info);
  /*
    It is safe to call servers_reload() since servers_* arrays and hashes which
    will be freed there are global static objects and thus are initialized
    by zeros at startup.
  */
  return_val= servers_reload(thd);
  delete thd;

end:
  DBUG_RETURN(return_val);
}

/*
  Initialize server structures

  SYNOPSIS
    servers_load()
      thd     Current thread
      tables  List containing open "mysql.servers"

  RETURN VALUES
    FALSE  Success
    TRUE   Error

  TODO
    Revert back to old list if we failed to load new one.
*/

static bool servers_load(THD *thd, TABLE_LIST *tables)
{
  TABLE *table= tables[0].table;
  READ_RECORD read_record_info;
  bool return_val= TRUE;
  DBUG_ENTER("servers_load");

  my_hash_reset(&servers_cache);
  free_root(&mem, MYF(0));
  init_sql_alloc(key_memory_servers, &mem, ACL_ALLOC_BLOCK_SIZE, 0, MYF(0));

  table->use_all_columns();
  if (init_read_record(&read_record_info,thd,table, NULL, NULL,
                       1,0, FALSE))
    DBUG_RETURN(1);
  while (!(read_record_info.read_record()))
  {
    /* return_val is already TRUE, so no need to set */
    if ((get_server_from_table_to_cache(table)))
      goto end;
  }

  return_val= FALSE;

end:
  end_read_record(&read_record_info);
  DBUG_RETURN(return_val);
}


/*
  Forget current servers cache and read new servers 
  from the connection table.

  SYNOPSIS
    servers_reload()
      thd  Current thread

  NOTE
    All tables of calling thread which were open and locked by LOCK TABLES
    statement will be unlocked and closed.
    This function is also used for initialization of structures responsible
    for user/db-level privilege checking.

  RETURN VALUE
    FALSE  Success
    TRUE   Failure
*/

bool servers_reload(THD *thd)
{
  TABLE_LIST tables[1];
  bool return_val= TRUE;
  DBUG_ENTER("servers_reload");

  DBUG_PRINT("info", ("locking servers_cache"));
  mysql_rwlock_wrlock(&THR_LOCK_servers);

  tables[0].init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_SERVERS_NAME, 0, TL_READ);

  if (unlikely(open_and_lock_tables(thd, tables, FALSE,
                                    MYSQL_LOCK_IGNORE_TIMEOUT)))
  {
    /*
      Execution might have been interrupted; only print the error message
      if an error condition has been raised.
    */
    if (thd->get_stmt_da()->is_error())
      sql_print_error("Can't open and lock privilege tables: %s",
                      thd->get_stmt_da()->message());
    return_val= FALSE;
    goto end;
  }

  if ((return_val= servers_load(thd, tables)))
  {					// Error. Revert to old list
    /* blast, for now, we have no servers, discuss later way to preserve */

    DBUG_PRINT("error",("Reverting to old privileges"));
    servers_free();
  }

end:
  close_mysql_tables(thd);
  DBUG_PRINT("info", ("unlocking servers_cache"));
  mysql_rwlock_unlock(&THR_LOCK_servers);
  DBUG_RETURN(return_val);
}

static bool parse_server_options_json(FOREIGN_SERVER *server, char *ptr)
{
  enum json_types vt;
  const char *keyname, *keyname_end, *v;
  int v_len, nkey= 0;
  engine_option_value *option_list_last;
  DBUG_ENTER("parse_server_options_json");
  while ((vt= json_get_object_nkey(ptr, ptr+strlen(ptr), nkey++,
                    &keyname, &keyname_end, &v, &v_len)) != JSV_NOTHING)
  {
    if (vt != JSV_STRING)
      DBUG_RETURN(TRUE);
    /*
      We have to make copies here to create "clean" strings and
      avoid mutating ptr.
    */
    Lex_cstring name= {keyname, keyname_end}, value= {v, v + v_len},
      name_copy= safe_lexcstrdup_root(&mem, name),
      value_copy= safe_lexcstrdup_root(&mem, value);
    engine_option_value *option= new (&mem) engine_option_value(
      engine_option_value::Name(name_copy),
      engine_option_value::Value(value_copy), true);
    option->link(&server->option_list, &option_list_last);
    if (option->value.length)
    {
      LEX_CSTRING *optval= &option->value;
      char *unescaped= (char *) alloca(optval->length);
      int len= json_unescape_json(optval->str, optval->str + optval->length,
                                  unescaped, unescaped + optval->length);
      if (len < 0)
        DBUG_RETURN(TRUE);
      DBUG_ASSERT(len <= (int) optval->length);
      if (len < (int) optval->length)
        strncpy((char *) optval->str, unescaped, len);
      optval->length= len;
    }
  }
  DBUG_RETURN(FALSE);
}

/*
  Initialize structures responsible for servers used in federated
  server scheme information for them from the server
  table in the 'mysql' database.

  SYNOPSIS
    get_server_from_table_to_cache()
      TABLE *table         open table pointer


  NOTES
    This function takes a TABLE pointer (pointing to an opened
    table). With this open table, a FOREIGN_SERVER struct pointer
    is allocated into root memory, then each member of the FOREIGN_SERVER
    struct is populated. A char pointer takes the return value of get_field
    for each column we're interested in obtaining, and if that pointer
    isn't 0x0, the FOREIGN_SERVER member is set to that value, otherwise,
    is set to the value of an empty string, since get_field would set it to
    0x0 if the column's value is empty, even if the default value for that
    column is NOT NULL.

  RETURN VALUES
    0	ok
    1	could not insert server struct into global servers cache
*/

static bool 
get_server_from_table_to_cache(TABLE *table)
{
  /* alloc a server struct */
  char *ptr;
  char * const blank= (char*)"";
  FOREIGN_SERVER *server= (FOREIGN_SERVER *)alloc_root(&mem,
                                                       sizeof(FOREIGN_SERVER));
  DBUG_ENTER("get_server_from_table_to_cache");

  /* get each field into the server struct ptr */
  ptr= get_field(&mem, table->field[0]);
  server->server_name= ptr ? ptr : blank;
  server->server_name_length= (uint) strlen(server->server_name);
  ptr= get_field(&mem, table->field[1]);
  server->host= ptr ? ptr : blank;
  ptr= get_field(&mem, table->field[2]);
  server->db= ptr ? ptr : blank;
  ptr= get_field(&mem, table->field[3]);
  server->username= ptr ? ptr : blank;
  ptr= get_field(&mem, table->field[4]);
  server->password= ptr ? ptr : blank;
  ptr= get_field(&mem, table->field[5]);
  server->sport= ptr ? ptr : blank;

  server->port= server->sport ? atoi(server->sport) : 0;

  ptr= get_field(&mem, table->field[6]);
  server->socket= ptr && strlen(ptr) ? ptr : blank;
  ptr= get_field(&mem, table->field[7]);
  server->scheme= ptr ? ptr : blank;
  ptr= get_field(&mem, table->field[8]);
  server->owner= ptr ? ptr : blank;
  ptr= table->field[9] ? get_field(&mem, table->field[9]) : NULL;
  server->option_list= NULL;
  if (ptr && parse_server_options_json(server, ptr))
    DBUG_RETURN(TRUE);
  DBUG_PRINT("info", ("server->server_name %s", server->server_name));
  DBUG_PRINT("info", ("server->host %s", server->host));
  DBUG_PRINT("info", ("server->db %s", server->db));
  DBUG_PRINT("info", ("server->username %s", server->username));
  DBUG_PRINT("info", ("server->password %s", server->password));
  DBUG_PRINT("info", ("server->socket %s", server->socket));
  if (my_hash_insert(&servers_cache, (uchar*) server))
  {
    DBUG_PRINT("info", ("had a problem inserting server %s at %p",
                        server->server_name, server));
    // error handling needed here
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/*
  SYNOPSIS
    insert_server()
      THD   *thd     - thread pointer
      FOREIGN_SERVER *server - pointer to prepared FOREIGN_SERVER struct

  NOTES
    This function takes a server object that is has all members properly
    prepared, ready to be inserted both into the mysql.servers table and
    the servers cache.
	
    THR_LOCK_servers must be write locked.

  RETURN VALUES
    0  - no error
    other - ER_ error code
*/

static int 
insert_server(THD *thd, FOREIGN_SERVER *server)
{
  int error= -1;
  TABLE_LIST tables;
  TABLE *table;
  DBUG_ENTER("insert_server");

  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_SERVERS_NAME, 0, TL_WRITE);

  /* need to open before acquiring THR_LOCK_plugin or it will deadlock */
  if (! (table= open_ltable(thd, &tables, TL_WRITE, MYSQL_LOCK_IGNORE_TIMEOUT)))
    goto end;
  table->file->row_logging= 0;                  // Don't log to binary log

  /* insert the server into the table */
  if (unlikely(error= insert_server_record(table, server)))
    goto end;

  /* insert the server into the cache */
  if (unlikely((error= insert_server_record_into_cache(server))))
    goto end;

end:
  DBUG_RETURN(error);
}


/*
  SYNOPSIS
    int insert_server_record_into_cache()
      FOREIGN_SERVER *server

  NOTES
    This function takes a FOREIGN_SERVER pointer to an allocated (root mem)
    and inserts it into the global servers cache

    THR_LOCK_servers must be write locked.

  RETURN VALUE
    0   - no error
    >0  - error code

*/

static int 
insert_server_record_into_cache(FOREIGN_SERVER *server)
{
  int error=0;
  DBUG_ENTER("insert_server_record_into_cache");
  /*
    We succeeded in insertion of the server to the table, now insert
    the server to the cache
  */
  DBUG_PRINT("info", ("inserting server %s at %p, length %zd",
                        server->server_name, server,
                        server->server_name_length));
  if (my_hash_insert(&servers_cache, (uchar*) server))
  {
    DBUG_PRINT("info", ("had a problem inserting server %s at %p",
                        server->server_name, server));
    // error handling needed here
    error= 1;
  }
  DBUG_RETURN(error);
}


/*
  SYNOPSIS
    store_server_fields()
      TABLE *table
      FOREIGN_SERVER *server

  NOTES
    This function takes an opened table object, and a pointer to an 
    allocated FOREIGN_SERVER struct, and then stores each member of
    the FOREIGN_SERVER to the appropriate fields in the table, in 
    advance of insertion into the mysql.servers table

  RETURN VALUE
    0 - no errors
    >0 - ER_ error code
*/

static int
store_server_fields(TABLE *table, FOREIGN_SERVER *server)
{

  table->use_all_columns();

  if (table->s->fields < 9)
    return ER_CANT_FIND_SYSTEM_REC;

  /*
    "server" has already been prepped by prepare_server_struct_for_<>
    so, all we need to do is check if the value is set (> -1 for port)

    If this happens to be an update, only the server members that 
    have changed will be set. If an insert, then all will be set,
    even if with empty strings
  */
  if (server->host &&
    table->field[1]->store(server->host,
                           (uint) strlen(server->host), system_charset_info))
    goto err;
  if (server->db &&
    table->field[2]->store(server->db,
                           (uint) strlen(server->db), system_charset_info))
    goto err;
  if (server->username &&
    table->field[3]->store(server->username,
                           (uint) strlen(server->username), system_charset_info))
    goto err;
  if (server->password &&
    table->field[4]->store(server->password,
                           (uint) strlen(server->password), system_charset_info))
    goto err;
  if (server->port > -1 &&
    table->field[5]->store(server->port))
    goto err;
  if (server->socket &&
    table->field[6]->store(server->socket,
                           (uint) strlen(server->socket), system_charset_info))
    goto err;
  if (server->scheme &&
    table->field[7]->store(server->scheme,
                           (uint) strlen(server->scheme), system_charset_info))
    goto err;
  if (server->owner &&
    table->field[8]->store(server->owner,
                           (uint) strlen(server->owner), system_charset_info))
    goto err;
  {
    engine_option_value *option= server->option_list;
    StringBuffer<1024> json(table->field[9]->charset());
    json.append('{');
    while (option)
    {
      if (option->value.str)
      {
        json.append('"');
        json.append(option->name.str, option->name.length);
        json.append('"');
        json.append({STRING_WITH_LEN(": \"")});
        int len= json_escape_string(
          option->value.str, option->value.str + option->value.length,
          json.c_ptr() + json.length(), json.c_ptr() + json.alloced_length());
        json.length(json.length() + len);
        json.append('"');
        json.append({STRING_WITH_LEN(", ")});
      }
      option= option->next;
    }
    if (server->option_list)
      json.length(json.length() - 2);
    json.append('}');
    if (!table->field[9]->store(json.ptr(), json.length(), system_charset_info))
      return 0;
  }

err:
  THD *thd= table->in_use;
  DBUG_ASSERT(thd->is_error());
  return thd->get_stmt_da()->get_sql_errno();
}

/*
  SYNOPSIS
    insert_server_record()
      TABLE *table
      FOREIGN_SERVER *server

  NOTES
    This function takes the arguments of an open table object and a pointer
    to an allocated FOREIGN_SERVER struct. It stores the server_name into
    the first field of the table (the primary key, server_name column). With
    this, index_read_idx is called, if the record is found, an error is set
    to ER_FOREIGN_SERVER_EXISTS (the server with that server name exists in the
    table), if not, then store_server_fields stores all fields of the
    FOREIGN_SERVER to the table, then ha_write_row is inserted. If an error
    is encountered in either index_read_idx or ha_write_row, then that error
    is returned

  RETURN VALUE
    0 - no errors
    >0 - ER_ error code

  */

static
int insert_server_record(TABLE *table, FOREIGN_SERVER *server)
{
  int error;
  DBUG_ENTER("insert_server_record");
  DBUG_ASSERT(!table->file->row_logging);

  table->use_all_columns();
  empty_record(table);

  /* set the field that's the PK to the value we're looking for */
  table->field[0]->store(server->server_name,
                         server->server_name_length,
                         system_charset_info);

  /* read index until record is that specified in server_name */
  if (unlikely((error=
                table->file->ha_index_read_idx_map(table->record[0], 0,
                                                   (uchar *)table->field[0]->
                                                   ptr,
                                                   HA_WHOLE_KEY,
                                                   HA_READ_KEY_EXACT))))
  {
    /* if not found, err */
    if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
    {
      table->file->print_error(error, MYF(0));
      error= 1;
    }
    /* store each field to be inserted */
    if ((error= store_server_fields(table, server)))
      DBUG_RETURN(error);

    DBUG_PRINT("info",("record for server '%s' not found!",
                       server->server_name));
    /* write/insert the new server */
    if (unlikely(error=table->file->ha_write_row(table->record[0])))
      table->file->print_error(error, MYF(0));
  }
  else
    error= ER_FOREIGN_SERVER_EXISTS;
  DBUG_RETURN(error);
}

/*
  SYNOPSIS
    drop_server()
      THD *thd
      LEX_SERVER_OPTIONS *server_options

  NOTES
    This function takes as its arguments a THD object pointer and a pointer
    to a LEX_SERVER_OPTIONS struct from the parser. The member 'server_name'
    of this LEX_SERVER_OPTIONS struct contains the value of the server to be
    deleted. The mysql.servers table is opened via open_ltable,
    a table object returned, then delete_server_record is
    called with this table object and LEX_SERVER_OPTIONS server_name and
    server_name_length passed, containing the name of the server to be
    dropped/deleted, then delete_server_record_in_cache is called to delete
    the server from the servers cache.

  RETURN VALUE
    0 - no error
    > 0 - error code
*/

static int drop_server_internal(THD *thd, LEX_SERVER_OPTIONS *server_options)
{
  int error;
  TABLE_LIST tables;
  TABLE *table;

  DBUG_ENTER("drop_server_internal");
  DBUG_PRINT("info", ("server name server->server_name %s",
                      server_options->server_name.str));

  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_SERVERS_NAME, 0, TL_WRITE);

  /* hit the memory hit first */
  if (unlikely((error= delete_server_record_in_cache(server_options))))
    goto end;

  if (unlikely(!(table= open_ltable(thd, &tables, TL_WRITE,
                                    MYSQL_LOCK_IGNORE_TIMEOUT))))
  {
    error= my_errno;
    goto end;
  }

  error= delete_server_record(table, &server_options->server_name);

  /* close the servers table before we call closed_cached_connection_tables */
  close_mysql_tables(thd);

  if (close_cached_connection_tables(thd, &server_options->server_name))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_UNKNOWN_ERROR, "Server connection in use");
  }

end:
  DBUG_RETURN(error);
}


/**
  Drop a server with servers cache mutex lock.
*/
int drop_server(THD *thd, LEX_SERVER_OPTIONS *server_options)
{
  mysql_rwlock_wrlock(&THR_LOCK_servers);
  int rc= drop_server_internal(thd, server_options);
  mysql_rwlock_unlock(&THR_LOCK_servers);
  return rc;
}


/*

  SYNOPSIS
    delete_server_record_in_cache()
      LEX_SERVER_OPTIONS *server_options

  NOTES
    This function's  argument is a LEX_SERVER_OPTIONS struct pointer. This
    function uses the "server_name" and "server_name_length" members of the
    lex->server_options to search for the server in the servers_cache. Upon
    returned the server (pointer to a FOREIGN_SERVER struct), it then deletes
    that server from the servers_cache hash.

  RETURN VALUE
    0 - no error

*/

static int 
delete_server_record_in_cache(LEX_SERVER_OPTIONS *server_options)
{
  int error= ER_FOREIGN_SERVER_DOESNT_EXIST;
  FOREIGN_SERVER *server;
  DBUG_ENTER("delete_server_record_in_cache");

  DBUG_PRINT("info",("trying to obtain server name %s length %zu",
                     server_options->server_name.str,
                     server_options->server_name.length));


  if (!(server= (FOREIGN_SERVER *)
        my_hash_search(&servers_cache,
                       (uchar*) server_options->server_name.str,
                       server_options->server_name.length)))
  {
    DBUG_PRINT("info", ("server_name %s length %zu not found!",
                        server_options->server_name.str,
                        server_options->server_name.length));
    goto end;
  }
  /*
    We succeeded in deletion of the server to the table, now delete
    the server from the cache
  */
  DBUG_PRINT("info",("deleting server %s length %zd",
                     server->server_name,
                     server->server_name_length));

  my_hash_delete(&servers_cache, (uchar*) server);

  error= 0;

end:
  DBUG_RETURN(error);
}


/*

  SYNOPSIS
    update_server()
      THD *thd
      FOREIGN_SERVER *existing
      FOREIGN_SERVER *altered

  NOTES
    This function takes as arguments a THD object pointer, and two pointers,
    one pointing to the existing FOREIGN_SERVER struct "existing" (which is
    the current record as it is) and another pointer pointing to the
    FOREIGN_SERVER struct with the members containing the modified/altered
    values that need to be updated in both the mysql.servers table and the 
    servers_cache. It opens a table, passes the table and the altered
    FOREIGN_SERVER pointer, which will be used to update the mysql.servers 
    table for the particular server via the call to update_server_record,
    and in the servers_cache via update_server_record_in_cache. 

    THR_LOCK_servers must be write locked.

  RETURN VALUE
    0 - no error
    >0 - error code

*/

int update_server(THD *thd, FOREIGN_SERVER *existing, FOREIGN_SERVER *altered)
{
  int error;
  TABLE *table;
  TABLE_LIST tables;
  DBUG_ENTER("update_server");

  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_SERVERS_NAME, 0, TL_WRITE);

  if (!(table= open_ltable(thd, &tables, TL_WRITE, MYSQL_LOCK_IGNORE_TIMEOUT)))
  {
    error= my_errno;
    goto end;
  }

  if (unlikely((error= update_server_record(table, altered))))
    goto end;

  error= update_server_record_in_cache(existing, altered);

  /*
	Perform a reload so we don't have a 'hole' in our mem_root
  */
  servers_load(thd, &tables);

end:
  DBUG_RETURN(error);
}


/*

  SYNOPSIS
    update_server_record_in_cache()
      FOREIGN_SERVER *existing
      FOREIGN_SERVER *altered

  NOTES
    This function takes as an argument the FOREIGN_SERVER struct pointer
    for the existing server and the FOREIGN_SERVER struct populated with only 
    the members which have been updated. It then "merges" the "altered" struct
    members to the existing server, the existing server then represents an
    updated server. Then, the existing record is deleted from the servers_cache
    HASH, then the updated record inserted, in essence replacing the old
    record.

    THR_LOCK_servers must be write locked.

  RETURN VALUE
    0 - no error
    1 - error

*/

int update_server_record_in_cache(FOREIGN_SERVER *existing,
                                  FOREIGN_SERVER *altered)
{
  int error= 0;
  DBUG_ENTER("update_server_record_in_cache");

  /*
    update the members that haven't been change in the altered server struct
    with the values of the existing server struct
  */
  merge_server_struct(existing, altered);

  /*
    delete the existing server struct from the server cache
  */
  my_hash_delete(&servers_cache, (uchar*)existing);

  /*
    Insert the altered server struct into the server cache
  */
  if (my_hash_insert(&servers_cache, (uchar*)altered))
  {
    DBUG_PRINT("info", ("had a problem inserting server %s at %p",
                        altered->server_name,altered));
    error= ER_OUT_OF_RESOURCES;
  }

  DBUG_RETURN(error);
}


/*

  SYNOPSIS
    merge_server_struct()
      FOREIGN_SERVER *from
      FOREIGN_SERVER *to

  NOTES
    This function takes as its arguments two pointers each to an allocated
    FOREIGN_SERVER struct. The first FOREIGN_SERVER struct represents the struct
    that we will obtain values from (hence the name "from"), the second
    FOREIGN_SERVER struct represents which FOREIGN_SERVER struct we will be
    "copying" any members that have a value to (hence the name "to")

  RETURN VALUE
    VOID

*/

void merge_server_struct(FOREIGN_SERVER *from, FOREIGN_SERVER *to)
{
  DBUG_ENTER("merge_server_struct");
  if (!to->host)
    to->host= strdup_root(&mem, from->host);
  if (!to->db)
    to->db= strdup_root(&mem, from->db);
  if (!to->username)
    to->username= strdup_root(&mem, from->username);
  if (!to->password)
    to->password= strdup_root(&mem, from->password);
  if (to->port == -1)
    to->port= from->port;
  if (!to->socket && from->socket)
    to->socket= strdup_root(&mem, from->socket);
  if (!to->scheme && from->scheme)
    to->scheme= strdup_root(&mem, from->scheme);
  if (!to->owner)
    to->owner= strdup_root(&mem, from->owner);

  DBUG_VOID_RETURN;
}


/*

  SYNOPSIS
    update_server_record()
      TABLE *table
      FOREIGN_SERVER *server

  NOTES
    This function takes as its arguments an open TABLE pointer, and a pointer
    to an allocated FOREIGN_SERVER structure representing an updated record
    which needs to be inserted. The primary key, server_name is stored to field
    0, then index_read_idx is called to read the index to that record, the
    record then being ready to be updated, if found. If not found an error is
    set and error message printed. If the record is found, store_record is
    called, then store_server_fields stores each field from the the members of
    the updated FOREIGN_SERVER struct.

  RETURN VALUE
    0 - no error

*/


static int 
update_server_record(TABLE *table, FOREIGN_SERVER *server)
{
  int error=0;
  DBUG_ENTER("update_server_record");
  DBUG_ASSERT(!table->file->row_logging);

  table->use_all_columns();
  /* set the field that's the PK to the value we're looking for */
  if (table->field[0]->store(server->server_name,
                         server->server_name_length,
                         system_charset_info))
  {
    DBUG_ASSERT_NO_ASSUME(0); /* Protected by servers_cache */
    THD *thd= table->in_use;
    DBUG_ASSERT(thd->is_error());
    return thd->get_stmt_da()->get_sql_errno();
  }

  if (unlikely((error=
                table->file->ha_index_read_idx_map(table->record[0], 0,
                                                   (uchar *)table->field[0]->
                                                   ptr,
                                                   ~(longlong)0,
                                                   HA_READ_KEY_EXACT))))
  {
    if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      table->file->print_error(error, MYF(0));
    DBUG_PRINT("info",("server not found!"));
    error= ER_FOREIGN_SERVER_DOESNT_EXIST;
  }
  else
  {
    /* ok, so we can update since the record exists in the table */
    store_record(table,record[1]);
    if ((error= store_server_fields(table, server)))
      goto end;
    if (unlikely((error=table->file->ha_update_row(table->record[1],
                                                   table->record[0])) &&
                 error != HA_ERR_RECORD_IS_THE_SAME))
    {
      DBUG_PRINT("info",("problems with ha_update_row %d", error));
      goto end;
    }
    else
      error= 0;
  }

end:
  DBUG_RETURN(error);
}


/*

  SYNOPSIS
    delete_server_record()
      TABLE *table
      char *server_name
      int server_name_length

  NOTES

  RETURN VALUE
    0 - no error

*/

static int 
delete_server_record(TABLE *table, LEX_CSTRING *name)
{
  int error;
  DBUG_ENTER("delete_server_record");
  DBUG_ASSERT(!table->file->row_logging);

  table->use_all_columns();

  /* set the field that's the PK to the value we're looking for */
  table->field[0]->store(name->str, name->length, system_charset_info);

  if (unlikely((error=
                table->file->ha_index_read_idx_map(table->record[0], 0,
                                                   (uchar *)table->field[0]->
                                                   ptr,
                                                   HA_WHOLE_KEY,
                                                   HA_READ_KEY_EXACT))))
  {
    if (error != HA_ERR_KEY_NOT_FOUND && error != HA_ERR_END_OF_FILE)
      table->file->print_error(error, MYF(0));
    DBUG_PRINT("info",("server not found!"));
    error= ER_FOREIGN_SERVER_DOESNT_EXIST;
  }
  else
  {
    if (unlikely((error= table->file->ha_delete_row(table->record[0]))))
      table->file->print_error(error, MYF(0));
  }

  DBUG_RETURN(error);
}

/*

  SYNOPSIS
    create_server()
        THD *thd
        LEX_SERVER_OPTIONS *server_options

  NOTES

  RETURN VALUE
    0 - no error

*/

int create_server(THD *thd, LEX_SERVER_OPTIONS *server_options)
{
  int error= ER_FOREIGN_SERVER_EXISTS;
  FOREIGN_SERVER *server;

  DBUG_ENTER("create_server");
  DBUG_PRINT("info", ("server_options->server_name %s",
                      server_options->server_name.str));

  mysql_rwlock_wrlock(&THR_LOCK_servers);

  /* hit the memory first */
  if (my_hash_search(&servers_cache, (uchar*) server_options->server_name.str,
                     server_options->server_name.length))
  {
    if (thd->lex->create_info.or_replace())
    {
      if (unlikely((error= drop_server_internal(thd, server_options))))
        goto end;
    }
    else if (thd->lex->create_info.if_not_exists())
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_FOREIGN_SERVER_EXISTS,
                          ER_THD(thd, ER_FOREIGN_SERVER_EXISTS),
                          server_options->server_name.str);
      error= 0;
      goto end;
    }
    else
      goto end;
  }

  if (!(server= prepare_server_struct_for_insert(server_options)))
  {
    /* purecov: begin inspected */
    error= ER_OUT_OF_RESOURCES;
    goto end;
    /* purecov: end */
  }

  error= insert_server(thd, server);

  DBUG_PRINT("info", ("error returned %d", error));

end:
  mysql_rwlock_unlock(&THR_LOCK_servers);

  if (unlikely(error))
  {
    DBUG_PRINT("info", ("problem creating server <%s>",
                        server_options->server_name.str));
    my_error(error, MYF(0), server_options->server_name.str);
  }
  else
    my_ok(thd);

  DBUG_RETURN(error);
}


/*

  SYNOPSIS
    alter_server()
      THD *thd
      LEX_SERVER_OPTIONS *server_options

  NOTES

  RETURN VALUE
    0 - no error

*/

int alter_server(THD *thd, LEX_SERVER_OPTIONS *server_options)
{
  int error= ER_FOREIGN_SERVER_DOESNT_EXIST;
  FOREIGN_SERVER altered, *existing;
  DBUG_ENTER("alter_server");
  DBUG_PRINT("info", ("server_options->server_name %s",
                      server_options->server_name.str));

  mysql_rwlock_wrlock(&THR_LOCK_servers);

  if (!(existing= (FOREIGN_SERVER *) my_hash_search(&servers_cache,
                                     (uchar*) server_options->server_name.str,
                                     server_options->server_name.length)))
    goto end;

  prepare_server_struct_for_update(server_options, existing, &altered);

  error= update_server(thd, existing, &altered);

  /* close the servers table before we call closed_cached_connection_tables */
  close_mysql_tables(thd);

  if (close_cached_connection_tables(thd, &server_options->server_name))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_UNKNOWN_ERROR, "Server connection in use");
  }

end:
  DBUG_PRINT("info", ("error returned %d", error));
  mysql_rwlock_unlock(&THR_LOCK_servers);
  DBUG_RETURN(error);
}


static void copy_option_list(MEM_ROOT *mem, FOREIGN_SERVER *server,
                             engine_option_value *option_list)
{
  engine_option_value *option_list_last;
  server->option_list= NULL;
  for (engine_option_value *option= option_list; option;
       option= option->next)
  {
    engine_option_value *new_option= new (mem) engine_option_value(option);
    new_option->name= engine_option_value::Name(
      safe_lexcstrdup_root(mem, option->name));
    new_option->value= engine_option_value::Value(
      safe_lexcstrdup_root(mem, option->value));
    new_option->link(&server->option_list, &option_list_last);
  }
}

/*

  SYNOPSIS
    prepare_server_struct_for_insert()
      LEX_SERVER_OPTIONS *server_options

  NOTES
    As FOREIGN_SERVER members are allocated on mem_root, we do not need to
    free them in case of error.

  RETURN VALUE
    On success filled FOREIGN_SERVER, or NULL in case out of memory.

*/

static FOREIGN_SERVER *
prepare_server_struct_for_insert(LEX_SERVER_OPTIONS *server_options)
{
  FOREIGN_SERVER *server;
  ulong default_port= 0;
  DBUG_ENTER("prepare_server_struct");

  if (!(server= (FOREIGN_SERVER *)alloc_root(&mem, sizeof(FOREIGN_SERVER))))
    DBUG_RETURN(NULL); /* purecov: inspected */

#define SET_SERVER_OR_RETURN(X, DEFAULT)                        \
  do {                                                          \
    if (!(server->X= server_options->X.str ?                    \
            strmake_root(&mem, server_options->X.str,           \
                               server_options->X.length) : "")) \
      DBUG_RETURN(NULL);                                        \
  } while(0)

  /* name and scheme are always set (the parser guarantees it) */
  SET_SERVER_OR_RETURN(server_name, NULL);
  SET_SERVER_OR_RETURN(scheme, NULL);

  /* scheme-specific checks */
  if (!strcasecmp(server->scheme, "mysql"))
  {
    default_port= MYSQL_PORT;
    if (!server_options->host.str && !server_options->socket.str)
    {
      my_error(ER_CANT_CREATE_FEDERATED_TABLE, MYF(0),
               "either HOST or SOCKET must be set");
      DBUG_RETURN(NULL);
    }
  }

  SET_SERVER_OR_RETURN(host, "");
  SET_SERVER_OR_RETURN(db, "");
  SET_SERVER_OR_RETURN(username, "");
  SET_SERVER_OR_RETURN(password, "");
  SET_SERVER_OR_RETURN(socket, "");
  SET_SERVER_OR_RETURN(owner, "");
  copy_option_list(&mem, server, server_options->option_list);

  server->server_name_length= server_options->server_name.length;

  /* set to default_port if not specified */
  server->port= server_options->port > -1 ?
    server_options->port : default_port;

  DBUG_RETURN(server);
}

/*

  SYNOPSIS
    prepare_server_struct_for_update()
      LEX_SERVER_OPTIONS *server_options

  NOTES

  RETURN VALUE
    0 - no error

*/

static void
prepare_server_struct_for_update(LEX_SERVER_OPTIONS *server_options,
                                 FOREIGN_SERVER *existing,
                                 FOREIGN_SERVER *altered)
{
  DBUG_ENTER("prepare_server_struct_for_update");

  altered->server_name= existing->server_name;
  altered->server_name_length= existing->server_name_length;
  DBUG_PRINT("info", ("existing name %s altered name %s",
                      existing->server_name, altered->server_name));

  /*
    The logic here is this: is this value set AND is it different
    than the existing value?
  */
#define SET_ALTERED(X)                                                       \
  do {                                                                       \
    altered->X=                                                              \
      (server_options->X.str && strcmp(server_options->X.str, existing->X))  \
      ? strmake_root(&mem, server_options->X.str, server_options->X.length)  \
      : 0;                                                                   \
  } while(0)

  SET_ALTERED(host);
  SET_ALTERED(db);
  SET_ALTERED(username);
  SET_ALTERED(password);
  SET_ALTERED(socket);
  SET_ALTERED(scheme);
  SET_ALTERED(owner);
  merge_engine_options(existing->option_list, server_options->option_list,
                       &altered->option_list, &mem);

  /*
    port is initialised to -1, so if unset, it will be -1
  */
  altered->port= (server_options->port > -1 &&
                 server_options->port != existing->port) ?
    server_options->port : -1;

  DBUG_VOID_RETURN;
}

/*

  SYNOPSIS
    servers_free()
      bool end

  NOTES

  RETURN VALUE
    void

*/

void servers_free(bool end)
{
  DBUG_ENTER("servers_free");
  if (!my_hash_inited(&servers_cache))
    DBUG_VOID_RETURN;
  if (!end)
  {
    free_root(&mem, MYF(MY_MARK_BLOCKS_FREE));
	my_hash_reset(&servers_cache);
    DBUG_VOID_RETURN;
  }
  mysql_rwlock_destroy(&THR_LOCK_servers);
  free_root(&mem,MYF(0));
  my_hash_free(&servers_cache);
  DBUG_VOID_RETURN;
}


/*
  SYNOPSIS

  clone_server(MEM_ROOT *mem_root, FOREIGN_SERVER *orig, FOREIGN_SERVER *buff)

  Create a clone of FOREIGN_SERVER. If the supplied mem_root is of
  thd->mem_root then the copy is automatically disposed at end of statement.

  NOTES

  ARGS
   MEM_ROOT pointer (strings are copied into this mem root) 
   FOREIGN_SERVER pointer (made a copy of)
   FOREIGN_SERVER buffer (if not-NULL, this pointer is returned)

  RETURN VALUE
   FOREIGN_SEVER pointer (copy of one supplied FOREIGN_SERVER)
*/

static FOREIGN_SERVER *clone_server(MEM_ROOT *mem, FOREIGN_SERVER *server,
                                    FOREIGN_SERVER *buffer)
{
  DBUG_ENTER("sql_server.cc:clone_server");

  if (!buffer)
    buffer= (FOREIGN_SERVER *) alloc_root(mem, sizeof(FOREIGN_SERVER));

  buffer->server_name= strmake_root(mem, server->server_name,
                                    server->server_name_length);
  buffer->port= server->port;
  buffer->server_name_length= server->server_name_length;
  
  /* TODO: We need to examine which of these can really be NULL */
  buffer->db= safe_strdup_root(mem, server->db);
  buffer->scheme= safe_strdup_root(mem, server->scheme);
  buffer->username= safe_strdup_root(mem, server->username);
  buffer->password= safe_strdup_root(mem, server->password);
  buffer->socket= safe_strdup_root(mem, server->socket);
  buffer->owner= safe_strdup_root(mem, server->owner);
  buffer->host= safe_strdup_root(mem, server->host);
  copy_option_list(mem, buffer, server->option_list);

 DBUG_RETURN(buffer);
}


/*

  SYNOPSIS
    get_server_by_name()
      const char *server_name

  NOTES

  RETURN VALUE
   FOREIGN_SERVER *

*/

FOREIGN_SERVER *get_server_by_name(MEM_ROOT *mem, const char *server_name,
                                   FOREIGN_SERVER *buff)
{
  size_t server_name_length;
  FOREIGN_SERVER *server;
  DBUG_ENTER("get_server_by_name");
  DBUG_PRINT("info", ("server_name %s", server_name));

  server_name_length= strlen(server_name);

  if (! server_name || !strlen(server_name))
  {
    DBUG_PRINT("info", ("server_name not defined!"));
    DBUG_RETURN((FOREIGN_SERVER *)NULL);
  }

  DBUG_PRINT("info", ("locking servers_cache"));
  mysql_rwlock_rdlock(&THR_LOCK_servers);
  if (!(server= (FOREIGN_SERVER *) my_hash_search(&servers_cache,
                                                  (uchar*) server_name,
                                                  server_name_length)))
  {
    DBUG_PRINT("info", ("server_name %s length %u not found!",
                        server_name, (unsigned) server_name_length));
    server= (FOREIGN_SERVER *) NULL;
  }
  /* otherwise, make copy of server */
  else
    server= clone_server(mem, server, buff);

  DBUG_PRINT("info", ("unlocking servers_cache"));
  mysql_rwlock_unlock(&THR_LOCK_servers);
  DBUG_RETURN(server);

}
