/*
   Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2009, 2022, MariaDB

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


#include "mariadb.h"
#include "sql_priv.h"

#ifdef MYSQL_CLIENT
#error MYSQL_CLIENT must not be defined here
#endif

#ifndef MYSQL_SERVER
#error MYSQL_SERVER must be defined here
#endif

#include "unireg.h"
#include "log_event.h"
#include "log_cache.h"
#include "sql_base.h"                           // close_thread_tables
#include "sql_cache.h"                       // QUERY_CACHE_FLAGS_SIZE
#include "sql_locale.h" // MY_LOCALE, my_locale_by_number, my_locale_en_US
#include "key.h"        // key_copy
#include "lock.h"       // mysql_unlock_tables
#include "sql_parse.h"  // mysql_test_parse_for_slave
#include "tztime.h"     // struct Time_zone
#include "sql_load.h"   // mysql_load
#include "sql_db.h"     // load_db_opt_by_name
#include "slave.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "rpl_filter.h"
#include "rpl_record.h"
#include "transaction.h"
#include <my_dir.h>
#include "sql_show.h"    // append_identifier
#include "debug_sync.h"  // debug_sync
#include <mysql/psi/mysql_statement.h>
#include <strfunc.h>
#include "compat56.h"
#include "wsrep_mysqld.h"
#include "sql_insert.h"
#include "sql_table.h"
#include <mysql/service_wsrep.h>

#include <my_bitmap.h>
#include "rpl_utility.h"
#include "rpl_constants.h"
#include "sql_digest.h"
#include "zlib.h"


#define log_cs  &my_charset_latin1


#if defined(HAVE_REPLICATION)
static int rows_event_stmt_cleanup(rpl_group_info *rgi, THD* thd);

static const char *HA_ERR(int i)
{
  /* 
    This function should only be called in case of an error
    was detected 
   */
  DBUG_ASSERT(i != 0);
  switch (i) {
  case HA_ERR_KEY_NOT_FOUND: return "HA_ERR_KEY_NOT_FOUND";
  case HA_ERR_FOUND_DUPP_KEY: return "HA_ERR_FOUND_DUPP_KEY";
  case HA_ERR_RECORD_CHANGED: return "HA_ERR_RECORD_CHANGED";
  case HA_ERR_WRONG_INDEX: return "HA_ERR_WRONG_INDEX";
  case HA_ERR_CRASHED: return "HA_ERR_CRASHED";
  case HA_ERR_WRONG_IN_RECORD: return "HA_ERR_WRONG_IN_RECORD";
  case HA_ERR_OUT_OF_MEM: return "HA_ERR_OUT_OF_MEM";
  case HA_ERR_NOT_A_TABLE: return "HA_ERR_NOT_A_TABLE";
  case HA_ERR_WRONG_COMMAND: return "HA_ERR_WRONG_COMMAND";
  case HA_ERR_OLD_FILE: return "HA_ERR_OLD_FILE";
  case HA_ERR_NO_ACTIVE_RECORD: return "HA_ERR_NO_ACTIVE_RECORD";
  case HA_ERR_RECORD_DELETED: return "HA_ERR_RECORD_DELETED";
  case HA_ERR_RECORD_FILE_FULL: return "HA_ERR_RECORD_FILE_FULL";
  case HA_ERR_INDEX_FILE_FULL: return "HA_ERR_INDEX_FILE_FULL";
  case HA_ERR_END_OF_FILE: return "HA_ERR_END_OF_FILE";
  case HA_ERR_UNSUPPORTED: return "HA_ERR_UNSUPPORTED";
  case HA_ERR_TO_BIG_ROW: return "HA_ERR_TO_BIG_ROW";
  case HA_WRONG_CREATE_OPTION: return "HA_WRONG_CREATE_OPTION";
  case HA_ERR_FOUND_DUPP_UNIQUE: return "HA_ERR_FOUND_DUPP_UNIQUE";
  case HA_ERR_UNKNOWN_CHARSET: return "HA_ERR_UNKNOWN_CHARSET";
  case HA_ERR_WRONG_MRG_TABLE_DEF: return "HA_ERR_WRONG_MRG_TABLE_DEF";
  case HA_ERR_CRASHED_ON_REPAIR: return "HA_ERR_CRASHED_ON_REPAIR";
  case HA_ERR_CRASHED_ON_USAGE: return "HA_ERR_CRASHED_ON_USAGE";
  case HA_ERR_LOCK_WAIT_TIMEOUT: return "HA_ERR_LOCK_WAIT_TIMEOUT";
  case HA_ERR_LOCK_TABLE_FULL: return "HA_ERR_LOCK_TABLE_FULL";
  case HA_ERR_READ_ONLY_TRANSACTION: return "HA_ERR_READ_ONLY_TRANSACTION";
  case HA_ERR_LOCK_DEADLOCK: return "HA_ERR_LOCK_DEADLOCK";
  case HA_ERR_CANNOT_ADD_FOREIGN: return "HA_ERR_CANNOT_ADD_FOREIGN";
  case HA_ERR_NO_REFERENCED_ROW: return "HA_ERR_NO_REFERENCED_ROW";
  case HA_ERR_ROW_IS_REFERENCED: return "HA_ERR_ROW_IS_REFERENCED";
  case HA_ERR_NO_SAVEPOINT: return "HA_ERR_NO_SAVEPOINT";
  case HA_ERR_NON_UNIQUE_BLOCK_SIZE: return "HA_ERR_NON_UNIQUE_BLOCK_SIZE";
  case HA_ERR_NO_SUCH_TABLE: return "HA_ERR_NO_SUCH_TABLE";
  case HA_ERR_TABLE_EXIST: return "HA_ERR_TABLE_EXIST";
  case HA_ERR_NO_CONNECTION: return "HA_ERR_NO_CONNECTION";
  case HA_ERR_NULL_IN_SPATIAL: return "HA_ERR_NULL_IN_SPATIAL";
  case HA_ERR_TABLE_DEF_CHANGED: return "HA_ERR_TABLE_DEF_CHANGED";
  case HA_ERR_NO_PARTITION_FOUND: return "HA_ERR_NO_PARTITION_FOUND";
  case HA_ERR_RBR_LOGGING_FAILED: return "HA_ERR_RBR_LOGGING_FAILED";
  case HA_ERR_DROP_INDEX_FK: return "HA_ERR_DROP_INDEX_FK";
  case HA_ERR_FOREIGN_DUPLICATE_KEY: return "HA_ERR_FOREIGN_DUPLICATE_KEY";
  case HA_ERR_TABLE_NEEDS_UPGRADE: return "HA_ERR_TABLE_NEEDS_UPGRADE";
  case HA_ERR_TABLE_READONLY: return "HA_ERR_TABLE_READONLY";
  case HA_ERR_AUTOINC_READ_FAILED: return "HA_ERR_AUTOINC_READ_FAILED";
  case HA_ERR_AUTOINC_ERANGE: return "HA_ERR_AUTOINC_ERANGE";
  case HA_ERR_GENERIC: return "HA_ERR_GENERIC";
  case HA_ERR_RECORD_IS_THE_SAME: return "HA_ERR_RECORD_IS_THE_SAME";
  case HA_ERR_LOGGING_IMPOSSIBLE: return "HA_ERR_LOGGING_IMPOSSIBLE";
  case HA_ERR_CORRUPT_EVENT: return "HA_ERR_CORRUPT_EVENT";
  case HA_ERR_ROWS_EVENT_APPLY : return "HA_ERR_ROWS_EVENT_APPLY";
  case HA_ERR_PARTITION_LIST : return "HA_ERR_PARTITION_LIST";
  }
  return "No Error!";
}


/*
  Return true if an error caught during event execution is a temporary error
  that will cause automatic retry of the event group during parallel
  replication, false otherwise.

  In parallel replication, conflicting transactions can occasionally cause
  deadlocks; such errors are handled automatically by rolling back re-trying
  the transactions, so should not pollute the error log.
*/
bool
is_parallel_retry_error(rpl_group_info *rgi, int err)
{
  if (!rgi->is_parallel_exec)
    return false;
  if (rgi->speculation == rpl_group_info::SPECULATE_OPTIMISTIC)
    return true;
  if (rgi->killed_for_retry &&
      (err == ER_QUERY_INTERRUPTED || err == ER_CONNECTION_KILLED))
    return true;
  return has_temporary_error(rgi->thd);
}

/**
  Accumulate a Diagnostics_area's errors and warnings into an output buffer

    @param errbuf       The output buffer to write error messages
    @param errbuf_size  The size of the output buffer
    @param da           The Diagnostics_area to check for errors
*/
static void inline aggregate_da_errors(char *errbuf, size_t errbuf_size,
                                       Diagnostics_area *da)
{
  const char *errbuf_end= errbuf + errbuf_size;
  char *slider;
  Diagnostics_area::Sql_condition_iterator it= da->sql_conditions();
  const Sql_condition *err;
  size_t len;
  for (err= it++, slider= errbuf; err && slider < errbuf_end - 1;
       slider += len, err= it++)
  {
    len= my_snprintf(slider, errbuf_end - slider,
                     " %s, Error_code: %d;", err->get_message_text(),
                     err->get_sql_errno());
  }
}


/**
   Error reporting facility for Rows_log_event::do_apply_event

   @param level     error, warning or info
   @param ha_error  HA_ERR_ code
   @param rli       pointer to the active Relay_log_info instance
   @param thd       pointer to the slave thread's thd
   @param table     pointer to the event's table object
   @param type      the type of the event
   @param log_name  the master binlog file name
   @param pos       the master binlog file pos (the next after the event)

*/
static void inline slave_rows_error_report(enum loglevel level, int ha_error,
                                           rpl_group_info *rgi, THD *thd,
                                           TABLE *table, const char * type,
                                           const char *log_name, my_off_t pos)
{
  const char *handler_error= (ha_error ? HA_ERR(ha_error) : NULL);
  char buff[MAX_SLAVE_ERRMSG];
  Relay_log_info const *rli= rgi->rli;
  buff[0]= 0;
  int errcode= thd->is_error() ? thd->get_stmt_da()->sql_errno() : 0;

  /*
    In parallel replication, deadlocks or other temporary errors can happen
    occasionally in normal operation, they will be handled correctly and
    automatically by re-trying the transactions. So do not pollute the error
    log with messages about them.
  */
  if (is_parallel_retry_error(rgi, errcode))
    return;

  aggregate_da_errors(buff, sizeof(buff), thd->get_stmt_da());

  if (ha_error != 0 && !thd->killed)
    rli->report(level, errcode, rgi->gtid_info(),
                "Could not execute %s event on table %s.%s;"
                "%s handler error %s; "
                "the event's master log %s, end_log_pos %llu",
                type, table->s->db.str, table->s->table_name.str,
                buff, handler_error == NULL ? "<unknown>" : handler_error,
                log_name, pos);
  else
    rli->report(level, errcode, rgi->gtid_info(),
                "Could not execute %s event on table %s.%s;"
                "%s the event's master log %s, end_log_pos %llu",
                type, table->s->db.str, table->s->table_name.str,
                buff, log_name, pos);
}
#endif

#if defined(HAVE_REPLICATION)
static void set_thd_db(THD *thd, Rpl_filter *rpl_filter,
                       const LEX_CSTRING &db)
{
  IdentBuffer<NAME_LEN> lcase_db_buf;
  LEX_CSTRING new_db= lower_case_table_names == 1 ?
                      lcase_db_buf.copy_casedn(db).to_lex_cstring() :
                      db;
  /* TODO WARNING this makes rewrite_db respect lower_case_table_names values
   * for more info look MDEV-17446 */
  new_db.str= rpl_filter->get_rewrite_db(new_db.str, &new_db.length);
  thd->set_db(&new_db);
}
#endif


#if defined(HAVE_REPLICATION)

inline int idempotent_error_code(int err_code)
{
  int ret= 0;

  switch (err_code)
  {
    case 0:
      ret= 1;
    break;
    /*
      The following list of "idempotent" errors
      means that an error from the list might happen
      because of idempotent (more than once)
      applying of a binlog file.
      Notice, that binlog has a  ddl operation its
      second applying may cause

      case HA_ERR_TABLE_DEF_CHANGED:
      case HA_ERR_CANNOT_ADD_FOREIGN:

      which are not included into to the list.

      Note that HA_ERR_RECORD_DELETED is not in the list since
      do_exec_row() should not return that error code.
    */
    case HA_ERR_RECORD_CHANGED:
    case HA_ERR_KEY_NOT_FOUND:
    case HA_ERR_END_OF_FILE:
    case HA_ERR_FOUND_DUPP_KEY:
    case HA_ERR_FOUND_DUPP_UNIQUE:
    case HA_ERR_FOREIGN_DUPLICATE_KEY:
    case HA_ERR_NO_REFERENCED_ROW:
    case HA_ERR_ROW_IS_REFERENCED:
      ret= 1;
    break;
    default:
      ret= 0;
    break;
  }
  return (ret);
}

/**
  Ignore error code specified on command line.
*/

inline int ignored_error_code(int err_code)
{
  if (use_slave_mask && bitmap_is_set(&slave_error_mask, err_code))
  {
    statistic_increment(slave_skipped_errors, LOCK_status);
    return 1;
  }
  return err_code == ER_SLAVE_IGNORED_TABLE;
}

/*
  This function converts an engine's error to a server error.
   
  If the thread does not have an error already reported, it tries to 
  define it by calling the engine's method print_error. However, if a 
  mapping is not found, it uses the ER_UNKNOWN_ERROR and prints out a 
  warning message.
*/ 
int convert_handler_error(int error, THD* thd, TABLE *table)
{
  uint actual_error= (thd->is_error() ? thd->get_stmt_da()->sql_errno() :
                           0);

  if (actual_error == 0)
  {
    table->file->print_error(error, MYF(0));
    actual_error= (thd->is_error() ? thd->get_stmt_da()->sql_errno() :
                        ER_UNKNOWN_ERROR);
    if (actual_error == ER_UNKNOWN_ERROR)
      if (global_system_variables.log_warnings)
        sql_print_warning("Unknown error detected %d in handler", error);
  }

  return (actual_error);
}

inline bool concurrency_error_code(int error)
{
  switch (error)
  {
  case ER_LOCK_WAIT_TIMEOUT:
  case ER_LOCK_DEADLOCK:
  case ER_XA_RBDEADLOCK:
    return TRUE;
  default: 
    return (FALSE);
  }
}

inline bool unexpected_error_code(int unexpected_error)
{
  switch (unexpected_error) 
  {
  case ER_NET_READ_ERROR:
  case ER_NET_ERROR_ON_WRITE:
  case ER_QUERY_INTERRUPTED:
  case ER_STATEMENT_TIMEOUT:
  case ER_CONNECTION_KILLED:
  case ER_SERVER_SHUTDOWN:
  case ER_NEW_ABORTING_CONNECTION:
    return(TRUE);
  default:
    return(FALSE);
  }
}


/**
  Create a prefix for the temporary files that is to be used for
  load data file name for this master

  @param name	           Store prefix of name here
  @param connection_name   Connection name
 
  @return pointer to end of name

  @description
  We assume that FN_REFLEN is big enough to hold
  MAX_CONNECTION_NAME * MAX_FILENAME_MBWIDTH characters + 2 numbers +
  a short extension.

  The resulting file name has the following parts, each separated with a '-'
  - PREFIX_SQL_LOAD (SQL_LOAD-)
  - If a connection name is given (multi-master setup):
    - Add an extra '-' to mark that this is a multi-master file
    - connection name in lower case, converted to safe file characters.
    (see create_logfile_name_with_suffix()).
  - server_id
  - A last '-' (after server_id).
*/

static char *load_data_tmp_prefix(char *name,
                                  LEX_CSTRING *connection_name)
{
  name= strmov(name, PREFIX_SQL_LOAD);
  if (connection_name->length)
  {
    uint buf_length;
    uint errors;
    /* Add marker that this is a multi-master-file */
    *name++='-';
    /* Convert connection_name to a safe filename */
    buf_length= strconvert(system_charset_info, connection_name->str, FN_REFLEN,
                           &my_charset_filename, name, FN_REFLEN, &errors);
    name+= buf_length;
    *name++= '-';
  }
  name= int10_to_str(global_system_variables.server_id, name, 10);
  *name++ = '-';
  *name= '\0';                                  // For testing prefixes
  return name;
}


/**
  Creates a temporary name for LOAD DATA INFILE

  @param buf		      Store new filename here
  @param file_id	      File_id (part of file name)
  @param event_server_id      Event_id (part of file name)
  @param ext		      Extension for file name

  @return
    Pointer to start of extension
*/

static char *slave_load_file_stem(char *buf, uint file_id,
                                  int event_server_id, const char *ext,
                                  LEX_CSTRING *connection_name)
{
  char *res;
  res= buf+ unpack_dirname(buf, slave_load_tmpdir);
  to_unix_path(buf);
  buf= load_data_tmp_prefix(res, connection_name);
  buf= int10_to_str(event_server_id, buf, 10);
  *buf++ = '-';
  res= int10_to_str(file_id, buf, 10);
  strmov(res, ext);                             // Add extension last
  return res;                                   // Pointer to extension
}
#endif


#if defined(HAVE_REPLICATION)

/**
  Delete all temporary files used for SQL_LOAD.
*/

static void cleanup_load_tmpdir(LEX_CSTRING *connection_name)
{
  MY_DIR *dirp;
  FILEINFO *file;
  size_t i;
  char dir[FN_REFLEN], fname[FN_REFLEN];
  char prefbuf[31 + MAX_CONNECTION_NAME* MAX_FILENAME_MBWIDTH + 1];
  DBUG_ENTER("cleanup_load_tmpdir");

  unpack_dirname(dir, slave_load_tmpdir);
  if (!(dirp=my_dir(dir, MYF(MY_WME))))
    return;

  /* 
     When we are deleting temporary files, we should only remove
     the files associated with the server id of our server.
     We don't use event_server_id here because since we've disabled
     direct binlogging of Create_file/Append_file/Exec_load events
     we cannot meet Start_log event in the middle of events from one 
     LOAD DATA.
  */

  load_data_tmp_prefix(prefbuf, connection_name);
  DBUG_PRINT("enter", ("dir: '%s'  prefix: '%s'", dir, prefbuf));

  for (i=0 ; i < dirp->number_of_files; i++)
  {
    file=dirp->dir_entry+i;
    if (is_prefix(file->name, prefbuf))
    {
      fn_format(fname,file->name,slave_load_tmpdir,"",MY_UNPACK_FILENAME);
      mysql_file_delete(key_file_misc, fname, MYF(0));
    }
  }

  my_dirend(dirp);
  DBUG_VOID_RETURN;
}
#endif


/**
  Append a version of the 'str' string suitable for use in a query to
  the 'to' string.  To generate a correct escaping, the character set
  information in 'csinfo' is used.
*/

int append_query_string(CHARSET_INFO *csinfo, String *to,
                        const char *str, size_t len, bool no_backslash)
{
  char *beg, *ptr;
  my_bool overflow;
  uint32 const orig_len= to->length();
  if (to->reserve(orig_len + len * 2 + 4))
    return 1;

  beg= (char*) to->ptr() + to->length();
  ptr= beg;
  if (csinfo->escape_with_backslash_is_dangerous)
    ptr= str_to_hex(ptr, (uchar*)str, len);
  else
  {
    *ptr++= '\'';
    if (!no_backslash)
    {
      ptr+= escape_string_for_mysql(csinfo, ptr, 0, str, len, &overflow);
    }
    else
    {
      const char *frm_str= str;

      for (; frm_str < (str + len); frm_str++)
      {
        /* Using '' way to represent "'" */
        if (*frm_str == '\'')
          *ptr++= *frm_str;

        *ptr++= *frm_str;
      }
    }

    *ptr++= '\'';
  }
  to->length((uint32)(orig_len + ptr - beg));
  return 0;
}


/**************************************************************************
	Log_event methods (= the parent class of all events)
**************************************************************************/

Log_event::Log_event(THD* thd_arg, uint16 flags_arg, bool using_trans)
  :log_pos(0), temp_buf(0), exec_time(0),
   slave_exec_mode(SLAVE_EXEC_MODE_STRICT), thd(thd_arg)
{
  server_id=	thd->variables.server_id;
  when=         thd->start_time;
  when_sec_part=thd->start_time_sec_part;

  if (using_trans)
    cache_type= Log_event::EVENT_TRANSACTIONAL_CACHE;
  else
    cache_type= Log_event::EVENT_STMT_CACHE;
  flags= flags_arg |
    (thd->variables.option_bits & OPTION_SKIP_REPLICATION ?
     LOG_EVENT_SKIP_REPLICATION_F : 0);
}

/**
  This minimal constructor is for when you are not even sure that there
  is a valid THD. For example in the server when we are shutting down or
  flushing logs after receiving a SIGHUP (then we must write a Rotate to
  the binlog but we have no THD, so we need this minimal constructor).
*/

Log_event::Log_event()
  :temp_buf(0), exec_time(0), flags(0), cache_type(EVENT_INVALID_CACHE),
   slave_exec_mode(SLAVE_EXEC_MODE_STRICT), thd(0)
{
  server_id=	global_system_variables.server_id;
  /*
    We can't call my_time() here as this would cause a call before
    my_init() is called
  */
  when=         0;
  when_sec_part=0;
  log_pos=	0;
}



#ifdef HAVE_REPLICATION

int Log_event::do_update_pos(rpl_group_info *rgi)
{
  Relay_log_info *rli= rgi->rli;
  DBUG_ENTER("Log_event::do_update_pos");

  DBUG_ASSERT(rli);
  DBUG_ASSERT(!rli->belongs_to_client());

  /*
    In parallel execution, delay position update for the events that are
    not part of event groups (format description, rotate, and such) until
    the actual event execution reaches that point.
  */
  if (!rgi->is_parallel_exec || is_group_event(get_type_code()))
    rli->stmt_done(log_pos, thd, rgi);

  DBUG_RETURN(0);                                  // Cannot fail currently
}


Log_event::enum_skip_reason
Log_event::do_shall_skip(rpl_group_info *rgi)
{
  Relay_log_info *rli= rgi->rli;
  DBUG_PRINT("info", ("ev->server_id: %lu, ::server_id: %lu,"
                      " rli->replicate_same_server_id: %d,"
                      " rli->slave_skip_counter: %llu",
                      (ulong) server_id,
                      (ulong) global_system_variables.server_id,
                      rli->replicate_same_server_id,
                      rli->slave_skip_counter));
  if ((server_id == global_system_variables.server_id &&
       !(rli->replicate_same_server_id || (flags &  LOG_EVENT_ACCEPT_OWN_F))) ||
      (rli->slave_skip_counter == 1 && rli->is_in_group()) ||
      (flags & LOG_EVENT_SKIP_REPLICATION_F &&
       opt_replicate_events_marked_for_skip != RPL_SKIP_REPLICATE))
    return EVENT_SKIP_IGNORE;
  if (rli->slave_skip_counter > 0)
    return EVENT_SKIP_COUNT;
  return EVENT_SKIP_NOT;
}


/*
  Log_event::pack_info()
*/

void Log_event::pack_info(Protocol *protocol)
{
  protocol->store("", 0, &my_charset_bin);
}


/**
  Only called by SHOW BINLOG EVENTS
*/
int Log_event::net_send(Protocol *protocol, const char* log_name, my_off_t pos)
{
  const char *p= strrchr(log_name, FN_LIBCHAR);
  const char *event_type;
  if (p)
    log_name = p + 1;

  protocol->prepare_for_resend();
  protocol->store(log_name, strlen(log_name), &my_charset_bin);
  protocol->store((ulonglong) pos);
  event_type = get_type_str();
  protocol->store(event_type, strlen(event_type), &my_charset_bin);
  protocol->store((uint32) server_id);
  protocol->store((ulonglong) log_pos);
  pack_info(protocol);
  return protocol->write();
}
#endif /* HAVE_REPLICATION */


/**
  init_show_field_list() prepares the column names and types for the
  output of SHOW BINLOG EVENTS; it is used only by SHOW BINLOG
  EVENTS.
*/

void Log_event::init_show_field_list(THD *thd, List<Item>* field_list)
{
  MEM_ROOT *mem_root= thd->mem_root;
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Log_name", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Pos",
                                        MY_INT64_NUM_DECIMAL_DIGITS,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Event_type", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Server_id", 10,
                                        MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "End_log_pos",
                                        MY_INT64_NUM_DECIMAL_DIGITS,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root) Item_empty_string(thd, "Info", 20),
                        mem_root);
}

int Log_event_writer::write_internal(const uchar *pos, size_t len)
{
  DBUG_ASSERT(!ctx || encrypt_or_write == &Log_event_writer::encrypt_and_write);
  if (cache_data &&
#ifdef WITH_WSREP
      mysql_bin_log.is_open() &&
#endif
      cache_data->write_prepare(len))
    return 1;

  if (my_b_safe_write(file, pos, len))
  {
    DBUG_PRINT("error", ("write to log failed: %d", my_errno));
    return 1;
  }
  bytes_written+= len;
  return 0;
}

/*
  as soon as encryption produces the first output block, write event_len
  where it should be in a valid event header
*/
int Log_event_writer::maybe_write_event_len(uchar *pos, size_t len)
{
  if (len && event_len)
  {
    DBUG_ASSERT(len >= EVENT_LEN_OFFSET);
    if (write_internal(pos + EVENT_LEN_OFFSET - 4, 4))
      return 1;
    int4store(pos + EVENT_LEN_OFFSET - 4, event_len);
    event_len= 0;
  }
  return 0;
}

int Log_event_writer::encrypt_and_write(const uchar *pos, size_t len)
{
  uchar *dst;
  size_t dstsize;
  uint dstlen;
  int res;                                      // Safe as res is always set
  DBUG_ASSERT(ctx);

  if (!len)
    return 0;

  dstsize= encryption_encrypted_length((uint)len, ENCRYPTION_KEY_SYSTEM_DATA,
                                       crypto->key_version);
  if (!(dst= (uchar*)my_safe_alloca(dstsize)))
    return 1;

  if (encryption_ctx_update(ctx, pos, (uint)len, dst, &dstlen))
  {
    res= 1;
    goto err;
  }

  if (maybe_write_event_len(dst, dstlen))
  {
    res= 1;
    goto err;
  }

  res= write_internal(dst, dstlen);

err:
  my_safe_afree(dst, dstsize);
  return res;
}

int Log_event_writer::write_header(uchar *pos, size_t len)
{
  DBUG_ENTER("Log_event_writer::write_header");
  /*
    recording checksum of FD event computed with dropped
    possibly active LOG_EVENT_BINLOG_IN_USE_F flag.
    Similar step at verification: the active flag is dropped before
    checksum computing.
  */
  if (checksum_len)
  {
    uchar save=pos[FLAGS_OFFSET];
    pos[FLAGS_OFFSET]&= ~LOG_EVENT_BINLOG_IN_USE_F;
    crc= my_checksum(0, pos, len);
    pos[FLAGS_OFFSET]= save;
  }

  if (ctx)
  {
    uchar iv[BINLOG_IV_LENGTH];
    crypto->set_iv(iv, (uint32)my_b_safe_tell(file));
    if (encryption_ctx_init(ctx, crypto->key, crypto->key_length,
           iv, sizeof(iv), ENCRYPTION_FLAG_ENCRYPT | ENCRYPTION_FLAG_NOPAD,
           ENCRYPTION_KEY_SYSTEM_DATA, crypto->key_version))
      DBUG_RETURN(1);

    DBUG_ASSERT(len >= LOG_EVENT_HEADER_LEN);
    event_len= uint4korr(pos + EVENT_LEN_OFFSET);
    DBUG_ASSERT(event_len >= len);
    memcpy(pos + EVENT_LEN_OFFSET, pos, 4);
    pos+= 4;
    len-= 4;
  }
  DBUG_RETURN((this->*encrypt_or_write)(pos, len));
}

int Log_event_writer::write_data(const uchar *pos, size_t len)
{
  DBUG_ENTER("Log_event_writer::write_data");

  if (!len)
    DBUG_RETURN(0);

  if (checksum_len)
    crc= my_checksum(crc, pos, len);

  DBUG_RETURN((this->*encrypt_or_write)(pos, len));
}

int Log_event_writer::write_footer()
{
  DBUG_ENTER("Log_event_writer::write_footer");
  if (checksum_len)
  {
    uchar checksum_buf[BINLOG_CHECKSUM_LEN];
    int4store(checksum_buf, crc);
    if ((this->*encrypt_or_write)(checksum_buf, BINLOG_CHECKSUM_LEN))
      DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  if (ctx)
  {
    uint dstlen;
    uchar dst[MY_AES_BLOCK_SIZE*2];
    if (encryption_ctx_finish(ctx, dst, &dstlen))
      DBUG_RETURN(1);
    if (maybe_write_event_len(dst, dstlen) || write_internal(dst, dstlen))
      DBUG_RETURN(ER_ERROR_ON_WRITE);
  }
  DBUG_RETURN(0);
}

/*
  Log_event::write_header()
*/

bool Log_event::write_header(Log_event_writer *writer, size_t event_data_length)
{
  uchar header[LOG_EVENT_HEADER_LEN];
  my_time_t now;
  DBUG_ENTER("Log_event::write_header");
  DBUG_PRINT("enter", ("filepos: %lld  length: %zu type: %d",
                       (longlong) writer->pos(), event_data_length,
                       (int) get_type_code()));

  /* Store number of bytes that will be written by this event */
  data_written= event_data_length + sizeof(header) + writer->checksum_len;

  /*
    log_pos != 0 if this is relay-log event. In this case we should not
    change the position
  */

  if (is_artificial_event() ||
      cache_type == Log_event::EVENT_STMT_CACHE ||
      cache_type == Log_event::EVENT_TRANSACTIONAL_CACHE)
  {
    /*
      Artificial events are automatically generated and do not exist
      in master's binary log, so log_pos should be set to 0.

      Events written through transaction or statement cache have log_pos set
      to 0 so that they can be copied directly to the binlog without having
      to compute the real end_log_pos.
    */
    log_pos= 0;
  }
  else  if (!log_pos)
  {
    /*
      Calculate the position of where the next event will start
      (end of this event, that is).
    */

    log_pos= writer->pos() + data_written;
    
    DBUG_EXECUTE_IF("dbug_master_binlog_over_2GB", log_pos += (1ULL <<31););
  }

  now= get_time();                               // Query start time

  /*
    Header will be of size LOG_EVENT_HEADER_LEN for all events, except for
    FORMAT_DESCRIPTION_EVENT and ROTATE_EVENT, where it will be
    LOG_EVENT_MINIMAL_HEADER_LEN (remember these 2 have a frozen header,
    because we read them before knowing the format).
  */

  int4store(header, now);              // timestamp
  header[EVENT_TYPE_OFFSET]= get_type_code();
  int4store(header+ SERVER_ID_OFFSET, server_id);
  int4store(header+ EVENT_LEN_OFFSET, data_written);
  int4store(header+ LOG_POS_OFFSET, log_pos);
  int2store(header + FLAGS_OFFSET, flags);

  bool ret= writer->write_header(header, sizeof(header));
  DBUG_RETURN(ret);
}



#if defined(HAVE_REPLICATION)
inline Log_event::enum_skip_reason
Log_event::continue_group(rpl_group_info *rgi)
{
  if (rgi->rli->slave_skip_counter == 1)
    return Log_event::EVENT_SKIP_IGNORE;
  return Log_event::do_shall_skip(rgi);
}
#endif

/**************************************************************************
	Query_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)

/**
  This (which is used only for SHOW BINLOG EVENTS) could be updated to
  print SET @@session_var=. But this is not urgent, as SHOW BINLOG EVENTS is
  only an information, it does not produce suitable queries to replay (for
  example it does not print LOAD DATA INFILE).
  @todo
    show the catalog ??
*/

void Query_log_event::pack_info(Protocol *protocol)
{
  // TODO: show the catalog ??
  char buf_mem[1024];
  String buf(buf_mem, sizeof(buf_mem), system_charset_info);
  buf.real_alloc(9 + db_len + q_len);
  if (!(flags & LOG_EVENT_SUPPRESS_USE_F)
      && db && db_len)
  {
    buf.append(STRING_WITH_LEN("use "));
    append_identifier(protocol->thd, &buf, db, db_len);
    buf.append(STRING_WITH_LEN("; "));
  }

  DBUG_ASSERT(!flags2 || flags2_inited);

  if (flags2 & (OPTION_NO_FOREIGN_KEY_CHECKS | OPTION_AUTO_IS_NULL |
                OPTION_RELAXED_UNIQUE_CHECKS |
                OPTION_NO_CHECK_CONSTRAINT_CHECKS |
                OPTION_IF_EXISTS |
                OPTION_INSERT_HISTORY))
  {
    buf.append(STRING_WITH_LEN("set "));
    if (flags2 & OPTION_NO_FOREIGN_KEY_CHECKS)
      buf.append(STRING_WITH_LEN("foreign_key_checks=1, "));
    if (flags2 & OPTION_AUTO_IS_NULL)
      buf.append(STRING_WITH_LEN("sql_auto_is_null, "));
    if (flags2 & OPTION_RELAXED_UNIQUE_CHECKS)
      buf.append(STRING_WITH_LEN("unique_checks=1, "));
    if (flags2 & OPTION_NO_CHECK_CONSTRAINT_CHECKS)
      buf.append(STRING_WITH_LEN("check_constraint_checks=1, "));
    if (flags2 & OPTION_IF_EXISTS)
      buf.append(STRING_WITH_LEN("@@sql_if_exists=1, "));
    if (flags2 & OPTION_INSERT_HISTORY)
      buf.append(STRING_WITH_LEN("@@system_versioning_insert_history=1, "));
    buf[buf.length()-2]=';';
  }
  if (query && q_len)
    buf.append(query, q_len);
  protocol->store(&buf);
}
#endif


/**
  Utility function for the next method (Query_log_event::write()) .
*/
static void store_str_with_code_and_len(uchar **dst, const char *src,
                                        uint len, uint code)
{
  /*
    only 1 byte to store the length of catalog, so it should not
    surpass 255
  */
  DBUG_ASSERT(len <= 255);
  DBUG_ASSERT(src);
  *((*dst)++)= (uchar) code;
  *((*dst)++)= (uchar) len;
  bmove(*dst, src, len);
  (*dst)+= len;
}


/**
  Query_log_event::write().

  @note
    In this event we have to modify the header to have the correct
    EVENT_LEN_OFFSET as we don't yet know how many status variables we
    will print!
*/

bool Query_log_event::write(Log_event_writer *writer)
{
  uchar buf[QUERY_HEADER_LEN + MAX_SIZE_LOG_EVENT_STATUS];
  uchar *start, *start_of_status;
  ulong event_length;

  if (!query)
    return 1;                                   // Something wrong with event

  /*
    We want to store the thread id:
    (- as an information for the user when he reads the binlog)
    - if the query uses temporary table: for the slave SQL thread to know to
    which master connection the temp table belongs.
    Now imagine we (write()) are called by the slave SQL thread (we are
    logging a query executed by this thread; the slave runs with
    --log-slave-updates). Then this query will be logged with
    thread_id=the_thread_id_of_the_SQL_thread. Imagine that 2 temp tables of
    the same name were created simultaneously on the master (in the master
    binlog you have
    CREATE TEMPORARY TABLE t; (thread 1)
    CREATE TEMPORARY TABLE t; (thread 2)
    ...)
    then in the slave's binlog there will be
    CREATE TEMPORARY TABLE t; (thread_id_of_the_slave_SQL_thread)
    CREATE TEMPORARY TABLE t; (thread_id_of_the_slave_SQL_thread)
    which is bad (same thread id!).

    To avoid this, we log the thread's thread id EXCEPT for the SQL
    slave thread for which we log the original (master's) thread id.
    Now this moves the bug: what happens if the thread id on the
    master was 10 and when the slave replicates the query, a
    connection number 10 is opened by a normal client on the slave,
    and updates a temp table of the same name? We get a problem
    again. To avoid this, in the handling of temp tables (sql_base.cc)
    we use thread_id AND server_id.  TODO when this is merged into
    4.1: in 4.1, slave_proxy_id has been renamed to pseudo_thread_id
    and is a session variable: that's to make mysqlbinlog work with
    temp tables. We probably need to introduce

    SET PSEUDO_SERVER_ID
    for mysqlbinlog in 4.1. mysqlbinlog would print:
    SET PSEUDO_SERVER_ID=
    SET PSEUDO_THREAD_ID=
    for each query using temp tables.
  */
  int4store(buf + Q_THREAD_ID_OFFSET, slave_proxy_id);
  int4store(buf + Q_EXEC_TIME_OFFSET, exec_time);
  buf[Q_DB_LEN_OFFSET] = (char) db_len;
  int2store(buf + Q_ERR_CODE_OFFSET, error_code);

  /*
    You MUST always write status vars in increasing order of code. This
    guarantees that a slightly older slave will be able to parse those he
    knows.
  */
  start_of_status= start= buf+QUERY_HEADER_LEN;
  if (flags2_inited)
  {
    *start++= Q_FLAGS2_CODE;
    int4store(start, flags2);
    start+= 4;
  }
  if (sql_mode_inited)
  {
    *start++= Q_SQL_MODE_CODE;
    int8store(start, (ulonglong)sql_mode);
    start+= 8;
  }
  if (catalog_len) // i.e. this var is inited (false for 4.0 events)
  {
    store_str_with_code_and_len(&start,
                                catalog, catalog_len, (uint) Q_CATALOG_NZ_CODE);
    /*
      In 5.0.x where x<4 masters we used to store the end zero here. This was
      a waste of one byte so we don't do it in x>=4 masters. We change code to
      Q_CATALOG_NZ_CODE, because re-using the old code would make x<4 slaves
      of this x>=4 master segfault (expecting a zero when there is
      none). Remaining compatibility problems are: the older slave will not
      find the catalog; but it is will not crash, and it's not an issue
      that it does not find the catalog as catalogs were not used in these
      older MySQL versions (we store it in binlog and read it from relay log
      but do nothing useful with it). What is an issue is that the older slave
      will stop processing the Q_* blocks (and jumps to the db/query) as soon
      as it sees unknown Q_CATALOG_NZ_CODE; so it will not be able to read
      Q_AUTO_INCREMENT*, Q_CHARSET and so replication will fail silently in
      various ways. Documented that you should not mix alpha/beta versions if
      they are not exactly the same version, with example of 5.0.3->5.0.2 and
      5.0.4->5.0.3. If replication is from older to new, the new will
      recognize Q_CATALOG_CODE and have no problem.
    */
  }
  if (auto_increment_increment != 1 || auto_increment_offset != 1)
  {
    *start++= Q_AUTO_INCREMENT;
    int2store(start, auto_increment_increment);
    int2store(start+2, auto_increment_offset);
    start+= 4;
  }

  if (thd && (thd->used & THD::CHARACTER_SET_COLLATIONS_USED))
  {
    *start++= Q_CHARACTER_SET_COLLATIONS;
    size_t len= thd->variables.character_set_collations.to_binary((char*)start);
    start+= len;
  }

  if (charset_inited)
  {
    *start++= Q_CHARSET_CODE;
    memcpy(start, charset, 6);
    start+= 6;
  }
  if (time_zone_len)
  {
    /* In the TZ sys table, column Name is of length 64 so this should be ok */
    DBUG_ASSERT(time_zone_len <= MAX_TIME_ZONE_NAME_LENGTH);
    store_str_with_code_and_len(&start,
                                time_zone_str, time_zone_len, Q_TIME_ZONE_CODE);
  }
  if (lc_time_names_number)
  {
    DBUG_ASSERT(lc_time_names_number <= 0xFFFF);
    *start++= Q_LC_TIME_NAMES_CODE;
    int2store(start, lc_time_names_number);
    start+= 2;
  }
  if (charset_database_number)
  {
    DBUG_ASSERT(charset_database_number <= 0xFFFF);
    *start++= Q_CHARSET_DATABASE_CODE;
    int2store(start, charset_database_number);
    start+= 2;
  }
  if (table_map_for_update)
  {
    *start++= Q_TABLE_MAP_FOR_UPDATE_CODE;
    int8store(start, table_map_for_update);
    start+= 8;
  }
  if (thd && thd->need_binlog_invoker())
  {
    LEX_CSTRING user;
    LEX_CSTRING host;
    memset(&user, 0, sizeof(user));
    memset(&host, 0, sizeof(host));

    if (thd->slave_thread && thd->has_invoker())
    {
      /* user will be null, if master is older than this patch */
      user= thd->get_invoker_user();
      host= thd->get_invoker_host();
    }
    else
    {
      Security_context *ctx= thd->security_ctx;

      if (thd->need_binlog_invoker() == THD::INVOKER_USER)
      {
        user.str= ctx->priv_user;
        host.str= ctx->priv_host;
        host.length= strlen(host.str);
      }
      else
      {
        user.str= ctx->priv_role;
        host= empty_clex_str;
      }
      user.length= strlen(user.str);
    }

    if (user.length > 0)
    {
      *start++= Q_INVOKER;

      /*
        Store user length and user. The max length of use is 16, so 1 byte is
        enough to store the user's length.
       */
      *start++= (uchar)user.length;
      memcpy(start, user.str, user.length);
      start+= user.length;

      /*
        Store host length and host. The max length of host is 60, so 1 byte is
        enough to store the host's length.
       */
      *start++= (uchar)host.length;
      memcpy(start, host.str, host.length);
      start+= host.length;
    }
  }

  if (thd && (thd->used & THD::QUERY_START_SEC_PART_USED))
  {
    *start++= Q_HRNOW;
    get_time();
    int3store(start, when_sec_part);
    start+= 3;
  }

  /* xid's is used with ddl_log handling */
  if (thd && thd->binlog_xid)
  {
    *start++= Q_XID;
    int8store(start, thd->binlog_xid);
    start+= 8;
  }

  if (gtid_flags_extra)
  {
    *start++= Q_GTID_FLAGS3;
    *start++= gtid_flags_extra;
    if (gtid_flags_extra &
        (Gtid_log_event::FL_COMMIT_ALTER_E1 |
         Gtid_log_event::FL_ROLLBACK_ALTER_E1))
    {
      int8store(start, sa_seq_no);
      start+= 8;
    }
  }


  /*
    NOTE: When adding new status vars, please don't forget to update
    the MAX_SIZE_LOG_EVENT_STATUS in log_event.h and update the function
    code_name() in this file.

    Here there could be code like
    if (command-line-option-which-says-"log_this_variable" && inited)
    {
    *start++= Q_THIS_VARIABLE_CODE;
    int4store(start, this_variable);
    start+= 4;
    }
  */
  
  /* Store length of status variables */
  status_vars_len= (uint) (start-start_of_status);
  DBUG_ASSERT(status_vars_len <= MAX_SIZE_LOG_EVENT_STATUS);
  int2store(buf + Q_STATUS_VARS_LEN_OFFSET, status_vars_len);

  /*
    Calculate length of whole event
    The "1" below is the \0 in the db's length
  */
  event_length= ((uint) (start-buf) + get_post_header_size_for_derived() +
                 db_len + 1 + q_len);

  return write_header(writer, event_length) ||
         write_data(writer, buf, QUERY_HEADER_LEN) ||
         write_post_header_for_derived(writer) ||
         write_data(writer, start_of_status, (uint) status_vars_len) ||
         write_data(writer, db, db_len + 1) ||
         write_data(writer, query, q_len) ||
         write_footer(writer);
}

bool Query_compressed_log_event::write(Log_event_writer *writer)
{
  uchar *buffer;
  uint32 alloc_size, compressed_size;
  bool ret= true;

  compressed_size= alloc_size= binlog_get_compress_len(q_len);
  buffer= (uchar*) my_safe_alloca(alloc_size);
  if (buffer &&
      !binlog_buf_compress((uchar*) query, buffer, q_len, &compressed_size))
  {
    /*
      Write the compressed event. We have to temporarily store the event
      in query and q_len as Query_log_event::write() uses these.
    */
    const char *query_tmp= query;
    uint32 q_len_tmp= q_len;
    query= (char*) buffer;
    q_len= compressed_size;
    ret= Query_log_event::write(writer);
    query= query_tmp;
    q_len= q_len_tmp;
  }
  my_safe_afree(buffer, alloc_size);
  return ret;
}


/**
  The simplest constructor that could possibly work.  This is used for
  creating static objects that have a special meaning and are invisible
  to the log.  
*/
Query_log_event::Query_log_event()
  :Log_event(), data_buf(0)
{
  memset(&user, 0, sizeof(user));
  memset(&host, 0, sizeof(host));
}


/*
  SYNOPSIS
    Query_log_event::Query_log_event()
      thd_arg           - thread handle
      query_arg         - array of char representing the query
      query_length      - size of the  `query_arg' array
      using_trans       - there is a modified transactional table
      direct            - Don't cache statement
      suppress_use      - suppress the generation of 'USE' statements
      errcode           - the error code of the query
      
  DESCRIPTION
  Creates an event for binlogging
  The value for `errcode' should be supplied by caller.
*/
Query_log_event::Query_log_event(THD* thd_arg, const char* query_arg,
                                 size_t query_length, bool using_trans,
				 bool direct, bool suppress_use, int errcode)

  :Log_event(thd_arg,
             ((thd_arg->used & THD::THREAD_SPECIFIC_USED)
              ? LOG_EVENT_THREAD_SPECIFIC_F : 0) |
             (suppress_use ? LOG_EVENT_SUPPRESS_USE_F : 0),
	     using_trans),
   data_buf(0), query(query_arg), catalog(thd_arg->catalog),
   q_len((uint32) query_length),
   thread_id(thd_arg->thread_id),
   /* save the original thread id; we already know the server id */
   slave_proxy_id((ulong)thd_arg->variables.pseudo_thread_id),
   flags2_inited(1), sql_mode_inited(1), charset_inited(1), flags2(0),
   sql_mode(thd_arg->variables.sql_mode),
   auto_increment_increment(thd_arg->variables.auto_increment_increment),
   auto_increment_offset(thd_arg->variables.auto_increment_offset),
   lc_time_names_number(thd_arg->variables.lc_time_names->number),
   charset_database_number(0),
   table_map_for_update((ulonglong)thd_arg->table_map_for_update),
   gtid_flags_extra(thd_arg->get_binlog_flags_for_alter()),
   sa_seq_no(0)
{
  /* status_vars_len is set just before writing the event */

#ifdef WITH_WSREP
  /*
    If Query_log_event will contain non trans keyword (not BEGIN, COMMIT,
    SAVEPOINT or ROLLBACK) we disable PA for this transaction.
    Note that here WSREP(thd) might not be true e.g. when wsrep_shcema
    is created we create tables with thd->variables.wsrep_on=false
    to avoid replicating wsrep_schema tables to other nodes.
   */
  if (WSREP_ON && !is_trans_keyword(false))
  {
    thd->wsrep_PA_safe= false;
  }
#endif /* WITH_WSREP */

  memset(&user, 0, sizeof(user));
  memset(&host, 0, sizeof(host));
  error_code= errcode;

  /*
    For slave threads, remember the original master exec time.
    This is needed to be able to calculate the master commit time.
  */
  exec_time= ((thd->rgi_slave) ? thd->rgi_slave->orig_exec_time
                               : (my_time(0) - thd_arg->start_time));

  /**
    @todo this means that if we have no catalog, then it is replicated
    as an existing catalog of length zero. is that safe? /sven
  */
  catalog_len = (catalog) ? (uint32) strlen(catalog) : 0;

  if (!(db= thd->db.str))
    db= "";
  db_len= (uint32) strlen(db);
  if (thd_arg->variables.collation_database != thd_arg->db_charset)
    charset_database_number= thd_arg->variables.collation_database->number;
  
  /*
    We only replicate over the bits of flags2 that we need: the rest
    are masked out by "& OPTIONS_WRITTEN_TO_BINLOG".

    We also force AUTOCOMMIT=1.  Rationale (cf. BUG#29288): After
    fixing BUG#26395, we always write BEGIN and COMMIT around all
    transactions (even single statements in autocommit mode).  This is
    so that replication from non-transactional to transactional table
    and error recovery from XA to non-XA table should work as
    expected.  The BEGIN/COMMIT are added in log.cc. However, there is
    one exception: MyISAM bypasses log.cc and writes directly to the
    binlog.  So if autocommit is off, master has MyISAM, and slave has
    a transactional engine, then the slave will just see one long
    never-ending transaction.  The only way to bypass explicit
    BEGIN/COMMIT in the binlog is by using a non-transactional table.
    So setting AUTOCOMMIT=1 will make this work as expected.

    Note: explicitly replicate AUTOCOMMIT=1 from master. We do not
    assume AUTOCOMMIT=1 on slave; the slave still reads the state of
    the autocommit flag as written by the master to the binlog. This
    behavior may change after WL#4162 has been implemented.
  */
  flags2= (uint32) (thd_arg->variables.option_bits &
                    (OPTIONS_WRITTEN_TO_BIN_LOG & ~OPTION_NOT_AUTOCOMMIT));
  DBUG_ASSERT(thd_arg->variables.character_set_client->number < 256*256);
  DBUG_ASSERT(thd_arg->variables.collation_connection->number < 256*256);
  DBUG_ASSERT(thd_arg->variables.collation_server->number < 256*256);
  DBUG_ASSERT(thd_arg->variables.character_set_client->mbminlen == 1);
  int2store(charset, thd_arg->variables.character_set_client->number);
  int2store(charset+2, thd_arg->variables.collation_connection->number);
  int2store(charset+4, thd_arg->variables.collation_server->number);
  if (thd_arg->used & THD::TIME_ZONE_USED)
  {
    /*
      Note that our event becomes dependent on the Time_zone object
      representing the time zone. Fortunately such objects are never deleted
      or changed during mysqld's lifetime.
    */
    time_zone_len= thd_arg->variables.time_zone->get_name()->length();
    time_zone_str= thd_arg->variables.time_zone->get_name()->ptr();
  }
  else
    time_zone_len= 0;

  LEX *lex= thd->lex;
  /*
    Defines that the statement will be written directly to the binary log
    without being wrapped by a BEGIN...COMMIT. Otherwise, the statement
    will be written to either the trx-cache or stmt-cache.

    Note that a cache will not be used if the parameter direct is TRUE.
  */
  bool use_cache= FALSE;
  /*
    TRUE defines that the trx-cache must be used and by consequence the
    use_cache is TRUE.

    Note that a cache will not be used if the parameter direct is TRUE.
  */
  bool trx_cache= FALSE;
  cache_type= Log_event::EVENT_INVALID_CACHE;

  if (!direct)
  {
    switch (lex->sql_command)
    {
      case SQLCOM_DROP_TABLE:
      case SQLCOM_DROP_SEQUENCE:
        use_cache= (lex->tmp_table() && thd->in_multi_stmt_transaction_mode());
        break;

      case SQLCOM_CREATE_TABLE:
      case SQLCOM_CREATE_SEQUENCE:
        /*
           If we are using CREATE ... SELECT or if we are a slave
           executing BEGIN...COMMIT (generated by CREATE...SELECT) we
           have to use the transactional cache to ensure we don't
           calculate any checksum for the CREATE part.
         */
        trx_cache= (lex->first_select_lex()->item_list.elements &&
            thd->is_current_stmt_binlog_format_row()) ||
          (thd->variables.option_bits & OPTION_GTID_BEGIN);
        use_cache= (lex->tmp_table() &&
            thd->in_multi_stmt_transaction_mode()) || trx_cache;
        break;
      case SQLCOM_SET_OPTION:
        if (lex->autocommit)
          use_cache= trx_cache= FALSE;
        else
          use_cache= TRUE;
        break;
      case SQLCOM_RELEASE_SAVEPOINT:
      case SQLCOM_ROLLBACK_TO_SAVEPOINT:
      case SQLCOM_SAVEPOINT:
      case SQLCOM_XA_END:
        use_cache= trx_cache= TRUE;
        break;
      default:
        use_cache= (gtid_flags_extra) ? false : sqlcom_can_generate_row_events(thd);
        break;
    }
  }

  if (gtid_flags_extra & (Gtid_log_event::FL_COMMIT_ALTER_E1 |
                          Gtid_log_event::FL_ROLLBACK_ALTER_E1))
      sa_seq_no= thd_arg->get_binlog_start_alter_seq_no();

  if (!use_cache || direct)
  {
    cache_type= Log_event::EVENT_NO_CACHE;
  }
  else if (using_trans || trx_cache || stmt_has_updated_trans_table(thd) ||
           thd->lex->is_mixed_stmt_unsafe(thd->in_multi_stmt_transaction_mode(),
                                          thd->variables.binlog_direct_non_trans_update,
                                          trans_has_updated_trans_table(thd),
                                          thd->tx_isolation))
    cache_type= Log_event::EVENT_TRANSACTIONAL_CACHE;
  else
    cache_type= Log_event::EVENT_STMT_CACHE;
  DBUG_ASSERT(cache_type != Log_event::EVENT_INVALID_CACHE);
  DBUG_PRINT("info",("Query_log_event has flags2: %lu  sql_mode: %llu  cache_tye: %d",
                     (ulong) flags2, sql_mode, cache_type));
}

Query_compressed_log_event::Query_compressed_log_event(THD* thd_arg, const char* query_arg,
    ulong query_length, bool using_trans,
    bool direct, bool suppress_use, int errcode)
    :Query_log_event(thd_arg, query_arg, query_length, using_trans, direct,
                     suppress_use, errcode),
     query_buf(0)
{

}


#if defined(HAVE_REPLICATION)

int Query_log_event::do_apply_event(rpl_group_info *rgi)
{
  return do_apply_event(rgi, query, q_len);
}

/**
   Compare if two errors should be regarded as equal.
   This is to handle the case when you can get slightly different errors
   on master and slave for the same thing.
   @param
   expected_error	Error we got on master
   actual_error		Error we got on slave

   @return
   1 Errors are equal
   0 Errors are different
*/

bool test_if_equal_repl_errors(int expected_error, int actual_error)
{
  if (expected_error == actual_error)
    return 1;
  switch (expected_error) {
  case ER_DUP_ENTRY:
  case ER_DUP_ENTRY_WITH_KEY_NAME:
  case ER_DUP_KEY:
  case ER_AUTOINC_READ_FAILED:
    return (actual_error == ER_DUP_ENTRY ||
            actual_error == ER_DUP_ENTRY_WITH_KEY_NAME ||
            actual_error == ER_DUP_KEY ||
            actual_error == ER_AUTOINC_READ_FAILED ||
            actual_error == HA_ERR_AUTOINC_ERANGE);
  case ER_UNKNOWN_TABLE:
    return actual_error == ER_IT_IS_A_VIEW;
  default:
    break;
  }
  return 0;
}


static start_alter_info *get_new_start_alter_info(THD *thd)
{
  /*
   Why on global memory ?- So that process_commit/rollback_alter should not get
   error when spawned threads exits too early.
   */
  start_alter_info *info;
  if (!(info= (start_alter_info *)my_malloc(PSI_INSTRUMENT_ME,
                                      sizeof(start_alter_info), MYF(MY_WME))))
  {
    sql_print_error("Failed to allocate memory for ddl log free list");
    return 0;
  }
  info->sa_seq_no= 0;
  info->domain_id= 0;
  info->direct_commit_alter= false;
  info->state= start_alter_state::INVALID;
  mysql_cond_init(0, &info->start_alter_cond, NULL);
  info->error= 0;

  return info;
}


/*
  Perform necessary actions for two-phase-logged ALTER parts, to
  return

  0  when the event's query proceeds normal parsing and execution
  1  when the event skips parsing and execution
  -1 as error.
*/
int Query_log_event::handle_split_alter_query_log_event(rpl_group_info *rgi,
                                                        bool &skip_error_check)
{
  int rc= 0;

  rgi->gtid_ev_flags_extra= gtid_flags_extra;
  if (gtid_flags_extra & Gtid_log_event::FL_START_ALTER_E1)
  {
    //No Slave, Normal Slave, Start Alter under Worker 1 will simple binlog and exit
    if(!rgi->rpt || rgi->reserved_start_alter_thread || WSREP(thd))
    {
      rc= 1;
      /*
       We will just write the binlog and move to next event , because COMMIT
       Alter will take care of actual work
      */
      rgi->reserved_start_alter_thread= false;
      thd->lex->sql_command= SQLCOM_ALTER_TABLE;
      Write_log_with_flags wlwf(thd, Gtid_log_event::FL_START_ALTER_E1,
                                true /* wsrep to isolation end */);
#ifdef WITH_WSREP
      if (WSREP(thd) && wsrep_thd_is_local(thd) &&
          // no need to supply other than db in this case
          wsrep_to_isolation_begin(thd, db, NULL,NULL,NULL,NULL,NULL))
        return -1;
#endif
      if (write_bin_log(thd, false, thd->query(), thd->query_length()))
        return -1;

      my_ok(thd);
      return rc;
    }
    if (!rgi->sa_info)
      rgi->sa_info= get_new_start_alter_info(thd);
    else
    {
      /* Not send Start-Alter into query execution when it's to rollback */
      mysql_mutex_lock(&rgi->rli->mi->start_alter_lock);
      if (rgi->sa_info->state == start_alter_state::ROLLBACK_ALTER)
        mysql_cond_broadcast(&rgi->sa_info->start_alter_cond);
      mysql_mutex_unlock(&rgi->rli->mi->start_alter_lock);
    }

    return rc;
  }

  bool is_CA= (gtid_flags_extra & Gtid_log_event::FL_COMMIT_ALTER_E1) ? true : false;
  if (is_CA)
  {
    DBUG_EXECUTE_IF("rpl_slave_stop_CA_before_binlog",
    {
      // the awake comes from STOP-SLAVE running driver (sql) thread
      debug_sync_set_action(thd,
                            STRING_WITH_LEN("now WAIT_FOR proceed_CA_1"));
    });
  }
  start_alter_info *info=NULL;
  Master_info *mi= NULL;

  rgi->gtid_ev_sa_seq_no= sa_seq_no;
  // is set for both the direct execution and the write to binlog
  thd->set_binlog_start_alter_seq_no(sa_seq_no);
  mi= rgi->rli->mi;
  mysql_mutex_lock(&mi->start_alter_list_lock);
  {
    List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
    while ((info= info_iterator++))
    {
      if(info->sa_seq_no == rgi->gtid_ev_sa_seq_no &&
         info->domain_id == rgi->current_gtid.domain_id)
      {
        info_iterator.remove();
        break;
      }
    }
  }
  mysql_mutex_unlock(&mi->start_alter_list_lock);

  if (!info)
  {
    if (is_CA)
    {
      /*
        error handling, direct_commit_alter is turned on, so that we dont
        wait for master reply in mysql_alter_table (in wait_for_master)
      */
      rgi->direct_commit_alter= true;
#ifdef WITH_WSREP
      if (WSREP(thd))
        thd->set_binlog_flags_for_alter(Gtid_log_event::FL_COMMIT_ALTER_E1);
#endif
      goto cleanup;
    }
    else
    {
      //Just write the binlog because there is nothing to be done
      goto write_binlog;
    }
  }

  mysql_mutex_lock(&mi->start_alter_lock);
  if (info->state != start_alter_state::COMPLETED)
  {
    if (is_CA)
      info->state= start_alter_state::COMMIT_ALTER;
    else
      info->state= start_alter_state::ROLLBACK_ALTER;
    mysql_cond_broadcast(&info->start_alter_cond);
    mysql_mutex_unlock(&mi->start_alter_lock);
    /*
      Wait till Start Alter worker has changed the state to ::COMPLETED
      when start alter worker reaches the old code write_bin_log(), it will
      change state to COMMITTED.
      COMMITTED and `direct_commit_alter == true` at the same time indicates
      the query needs re-execution by the CA running thread.
    */
    mysql_mutex_lock(&mi->start_alter_lock);

    DBUG_ASSERT(info->state == start_alter_state::COMPLETED ||
                !info->direct_commit_alter);

    while(info->state != start_alter_state::COMPLETED)
      mysql_cond_wait(&info->start_alter_cond, &mi->start_alter_lock);
  }
  else
  {
    // SA has completed and left being kicked out by deadlock or ftwrl
    DBUG_ASSERT(info->direct_commit_alter);
  }
  mysql_mutex_unlock(&mi->start_alter_lock);

  if (info->direct_commit_alter)
  {
    rgi->direct_commit_alter= true; // execute the query as if there was no SA
    if (is_CA)
      goto cleanup;
  }

write_binlog:
  rc= 1;

  if(!is_CA)
  {
    if(((info && info->error) || error_code) &&
       global_system_variables.log_warnings > 2)
    {
      sql_print_information("Query '%s' having %d error code on master "
                            "is rolled back%s", query, error_code,
                            !(info && info->error) ? "." : ";");
      if (info && info->error)
        sql_print_information("its execution on slave %sproduced %d error.",
                              info->error == error_code ? "re":"", info->error);
    }
  }
  {
    thd->lex->sql_command= SQLCOM_ALTER_TABLE;
    Write_log_with_flags wlwf(thd, is_CA ? Gtid_log_event::FL_COMMIT_ALTER_E1 :
                              Gtid_log_event::FL_ROLLBACK_ALTER_E1,
                              true);
#ifdef WITH_WSREP
    if (WSREP(thd) && wsrep_thd_is_local(thd) &&
        wsrep_to_isolation_begin(thd, db, NULL,NULL,NULL,NULL,NULL))
      rc= -1;
#endif
    if (rc != -1 &&
        write_bin_log(thd, false, thd->query(), thd->query_length()))
      rc= -1;
  }

  if (!thd->is_error())
  {
    skip_error_check= true;
    my_ok(thd);
  }

cleanup:
  if (info)
  {
    mysql_cond_destroy(&info->start_alter_cond);
    my_free(info);
  }
  return rc;
}


/**
  @todo
  Compare the values of "affected rows" around here. Something
  like:
  @code
     if ((uint32) affected_in_event != (uint32) affected_on_slave)
     {
     sql_print_error("Slave: did not get the expected number of affected \
     rows running query from master - expected %d, got %d (this numbers \
     should have matched modulo 4294967296).", 0, ...);
     thd->query_error = 1;
     }
  @endcode
  We may also want an option to tell the slave to ignore "affected"
  mismatch. This mismatch could be implemented with a new ER_ code, and
  to ignore it you would use --slave-skip-errors...
*/
int Query_log_event::do_apply_event(rpl_group_info *rgi,
                                    const char *query_arg, uint32 q_len_arg)
{
  int expected_error,actual_error= 0;
  Schema_specification_st db_options;
  uint64 sub_id= 0;
  void *hton= NULL;
  rpl_gtid gtid;
  Relay_log_info const *rli= rgi->rli;
  Rpl_filter *rpl_filter= rli->mi->rpl_filter;
  bool current_stmt_is_commit;
  bool skip_error_check= false;
  DBUG_ENTER("Query_log_event::do_apply_event");

  /*
    Colleagues: please never free(thd->catalog) in MySQL. This would
    lead to bugs as here thd->catalog is a part of an alloced block,
    not an entire alloced block (see
    Query_log_event::do_apply_event()). Same for thd->db.  Thank
    you.
  */
  thd->catalog= catalog_len ? (char *) catalog : (char *)"";
  rgi->start_alter_ev= this;

  size_t valid_len= Well_formed_prefix(system_charset_info,
                                       db, db_len, NAME_LEN).length();

  if (valid_len != db_len)
  {
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                "Invalid database name in Query event.");
    thd->is_slave_error= true;
    goto end;
  }

  set_thd_db(thd, rpl_filter, LEX_CSTRING{db, db_len});

  /*
    Setting the character set and collation of the current database thd->db.
   */
  load_db_opt_by_name(thd, thd->db.str, &db_options);
  if (db_options.default_table_charset)
    thd->db_charset= db_options.default_table_charset;
  thd->variables.auto_increment_increment= auto_increment_increment;
  thd->variables.auto_increment_offset=    auto_increment_offset;

  DBUG_PRINT("info", ("log_pos: %lu", (ulong) log_pos));

  thd->clear_error(1);
  current_stmt_is_commit= is_commit();

  DBUG_ASSERT(!current_stmt_is_commit || !rgi->tables_to_lock);
  rgi->slave_close_thread_tables(thd);

  /*
    Note:   We do not need to execute reset_one_shot_variables() if this
            db_ok() test fails.
    Reason: The db stored in binlog events is the same for SET and for
            its companion query.  If the SET is ignored because of
            db_ok(), the companion query will also be ignored, and if
            the companion query is ignored in the db_ok() test of
            ::do_apply_event(), then the companion SET also have so
            we don't need to reset_one_shot_variables().
  */
  if (rpl_filter->is_db_empty() ||
      is_trans_keyword(
          (rgi->gtid_ev_flags2 & (Gtid_log_event::FL_PREPARED_XA |
                                  Gtid_log_event::FL_COMPLETED_XA))) ||
      rpl_filter->db_ok(thd->db.str))
  {
    bool is_rb_alter= gtid_flags_extra & Gtid_log_event::FL_ROLLBACK_ALTER_E1;

    thd->set_time(when, when_sec_part);
    thd->set_query_and_id((char*)query_arg, q_len_arg,
                          thd->charset(), next_query_id());
    thd->variables.pseudo_thread_id= thread_id;		// for temp tables
    DBUG_PRINT("query",("%s", thd->query()));

#ifdef WITH_WSREP
    if (WSREP(thd))
    {
      WSREP_DEBUG("Query_log_event thread=%llu for query=%s",
		  thd_get_thread_id(thd), wsrep_thd_query(thd));
    }
#endif

    if (unlikely(!(expected_error= !is_rb_alter ? error_code : 0)) ||
        ignored_error_code(expected_error) ||
        !unexpected_error_code(expected_error))
    {
      thd->slave_expected_error= expected_error;
      if (flags2_inited)
      {
        ulonglong mask= flags2_inited;
        thd->variables.option_bits= (flags2 & mask) |
                                    (thd->variables.option_bits & ~mask);
      }
      /*
        else, we are in a 3.23/4.0 binlog; we previously received a
        Rotate_log_event which reset thd->variables.option_bits and
        sql_mode etc, so nothing to do.
      */
      /*
        We do not replicate MODE_NO_DIR_IN_CREATE. That is, if the master is a
        slave which runs with SQL_MODE=MODE_NO_DIR_IN_CREATE, this should not
        force us to ignore the dir too. Imagine you are a ring of machines, and
        one has a disk problem so that you temporarily need
        MODE_NO_DIR_IN_CREATE on this machine; you don't want it to propagate
        elsewhere (you don't want all slaves to start ignoring the dirs).
      */
      if (sql_mode_inited)
        thd->variables.sql_mode=
          (sql_mode_t) ((thd->variables.sql_mode & MODE_NO_DIR_IN_CREATE) |
                        (sql_mode & ~(sql_mode_t) MODE_NO_DIR_IN_CREATE));

      size_t cslen= thd->variables.character_set_collations.from_binary(
                                      character_set_collations.str,
                                      character_set_collations.length);
      if (cslen != character_set_collations.length)
      {
        // Fatal: either a broken even, or an unknown collation ID
        thd->variables.character_set_collations.init();
        goto compare_errors; // QQ: report an error here?
      }

      if (charset_inited)
      {
        rpl_sql_thread_info *sql_info= thd->system_thread_info.rpl_sql_info;
        if (thd->slave_thread && sql_info->cached_charset_compare(charset))
        {
          /* Verify that we support the charsets found in the event. */
          if (!(thd->variables.character_set_client=
                get_charset(uint2korr(charset), MYF(MY_WME))) ||
              !(thd->variables.collation_connection=
                get_charset(uint2korr(charset+2), MYF(MY_WME))) ||
              !(thd->variables.collation_server=
                get_charset(uint2korr(charset+4), MYF(MY_WME))))
          {
            /*
              We updated the thd->variables with nonsensical values (0). Let's
              set them to something safe (i.e. which avoids crash), and we'll
              stop with EE_UNKNOWN_CHARSET in compare_errors (unless set to
              ignore this error).
            */
            set_slave_thread_default_charset(thd, rgi);
            goto compare_errors;
          }
          thd->update_charset(); // for the charset change to take effect
          /*
            Reset thd->query_string.cs to the newly set value.
            Note, there is a small flaw here. For a very short time frame
            if the new charset is different from the old charset and
            if another thread executes "SHOW PROCESSLIST" after
            the above thd->set_query_and_id() and before this thd->set_query(),
            and if the current query has some non-ASCII characters,
            the another thread may see some '?' marks in the PROCESSLIST
            result. This should be acceptable now. This is a reminder
            to fix this if any refactoring happens here sometime.
          */
          thd->set_query((char*) query_arg, q_len_arg, thd->charset());
        }
      }
      if (time_zone_len)
      {
        String tmp(time_zone_str, time_zone_len, &my_charset_bin);
        if (!(thd->variables.time_zone= my_tz_find(thd, &tmp)))
        {
          my_error(ER_UNKNOWN_TIME_ZONE, MYF(0), tmp.c_ptr());
          thd->variables.time_zone= global_system_variables.time_zone;
          goto compare_errors;
        }
      }
      if (lc_time_names_number)
      {
        if (!(thd->variables.lc_time_names=
              my_locale_by_number(lc_time_names_number)))
        {
          my_printf_error(ER_UNKNOWN_ERROR,
                      "Unknown locale: '%d'", MYF(0), lc_time_names_number);
          thd->variables.lc_time_names= &my_locale_en_US;
          goto compare_errors;
        }
      }
      else
        thd->variables.lc_time_names= &my_locale_en_US;
      if (charset_database_number)
      {
        CHARSET_INFO *cs;
        if (!(cs= get_charset(charset_database_number, MYF(0))))
        {
          char buf[20];
          int10_to_str((int) charset_database_number, buf, -10);
          my_error(ER_UNKNOWN_COLLATION, MYF(0), buf);
          goto compare_errors;
        }
        thd->variables.collation_database= cs;
      }
      else
        thd->variables.collation_database= thd->db_charset;

      {
        const CHARSET_INFO *cs= thd->charset();
        /*
          We cannot ask for parsing a statement using a character set
          without state_maps (parser internal data).
        */
        if (!cs->state_map)
        {
          rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                      ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                      "character_set cannot be parsed");
          thd->is_slave_error= true;
          goto end;
        }
      }

      /*
        Record any GTID in the same transaction, so slave state is
        transactionally consistent.
      */
      if (current_stmt_is_commit)
      {
        thd->variables.option_bits&= ~OPTION_GTID_BEGIN;
        if (rgi->gtid_pending)
        {
          sub_id= rgi->gtid_sub_id;
          rgi->gtid_pending= false;

          gtid= rgi->current_gtid;
          if (unlikely(rpl_global_gtid_slave_state->record_gtid(thd, &gtid,
                                                                sub_id,
                                                                true, false,
                                                                &hton)))
          {
            int errcode= thd->get_stmt_da()->sql_errno();
            if (!is_parallel_retry_error(rgi, errcode))
              rli->report(ERROR_LEVEL, ER_CANNOT_UPDATE_GTID_STATE,
                          rgi->gtid_info(),
                          "Error during COMMIT: failed to update GTID state in "
                        "%s.%s: %d: %s",
                          "mysql", rpl_gtid_slave_state_table_name.str,
                          errcode,
                          thd->get_stmt_da()->message());
            sub_id= 0;
            thd->is_slave_error= 1;
            goto end;
          }
        }
      }

      thd->table_map_for_update= (table_map)table_map_for_update;
      thd->set_invoker(&user, &host);
      /*
        Flag if we need to rollback the statement transaction on
        slave if it by chance succeeds.
        If we expected a non-zero error code and get nothing and,
        it is a concurrency issue or ignorable issue, effects
        of the statement should be rolled back.
      */
      if (unlikely(expected_error) &&
          (ignored_error_code(expected_error) ||
           concurrency_error_code(expected_error)))
      {
        thd->variables.option_bits|= OPTION_MASTER_SQL_ERROR;
        thd->variables.option_bits&= ~OPTION_GTID_BEGIN;
      }

      int sa_result= 0;
      bool is_2p_alter= gtid_flags_extra &
        (Gtid_log_event::FL_START_ALTER_E1 |
         Gtid_log_event::FL_COMMIT_ALTER_E1 |
         Gtid_log_event::FL_ROLLBACK_ALTER_E1);
      if (is_2p_alter)
        sa_result= handle_split_alter_query_log_event(rgi, skip_error_check);
      if (sa_result == 0)
      {
        /* Execute the query (note that we bypass dispatch_command()) */
        Parser_state parser_state;
        if (!parser_state.init(thd, thd->query(), thd->query_length()))
        {
          DBUG_ASSERT(thd->m_digest == NULL);
          thd->m_digest= & thd->m_digest_state;
          DBUG_ASSERT(thd->m_statement_psi == NULL);
          thd->m_statement_psi= MYSQL_START_STATEMENT(&thd->m_statement_state,
                                                      stmt_info_rpl.m_key,
                                                      thd->db.str, thd->db.length,
                                                      thd->charset(), NULL);
          THD_STAGE_INFO(thd, stage_starting);
          MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi, thd->query(), thd->query_length());
          if (thd->m_digest != NULL)
            thd->m_digest->reset(thd->m_token_array, max_digest_length);

          if (thd->slave_thread)
          {
            /*
              To be compatible with previous releases, the slave thread uses the global
              log_slow_disabled_statements value, which can be changed dynamically, so we
              have to set the sql_log_slow respectively.
            */
            thd->variables.sql_log_slow= !MY_TEST(global_system_variables.log_slow_disabled_statements & LOG_SLOW_DISABLE_SLAVE);
          }
          mysql_parse(thd, thd->query(), thd->query_length(), &parser_state);
          /* Finalize server status flags after executing a statement. */
          thd->update_server_status();
          log_slow_statement(thd);
          thd->lex->restore_set_statement_var();

          /*
            When THD::slave_expected_error gets reset inside execution stack
            that is the case of to be ignored event. In this case the expected
            error must change to the reset value as well.
          */
          expected_error= thd->slave_expected_error;
        }
      }
      else if (sa_result == -1)
      {
        rli->report(ERROR_LEVEL, expected_error, rgi->gtid_info(),
                          "TODO start alter error");
        thd->is_slave_error= 1;
        goto end;
      }
      thd->variables.option_bits&= ~OPTION_MASTER_SQL_ERROR;
      if (is_2p_alter && !rgi->is_parallel_exec)
      {
        rgi->gtid_ev_flags_extra= 0;
        rgi->direct_commit_alter= 0;
        rgi->gtid_ev_sa_seq_no= 0;
      }
    }
    else
    {
      /*
        The query got a really bad error on the master (thread killed etc),
        which could be inconsistent. Parse it to test the table names: if the
        replicate-*-do|ignore-table rules say "this query must be ignored" then
        we exit gracefully; otherwise we warn about the bad error and tell DBA
        to check/fix it.
      */
      if (mysql_test_parse_for_slave(thd, thd->query(), thd->query_length()))
        thd->clear_error(1);
      else
      {
        rli->report(ERROR_LEVEL, expected_error, rgi->gtid_info(),
                          "\
Query partially completed on the master (error on master: %d) \
and was aborted. There is a chance that your master is inconsistent at this \
point. If you are sure that your master is ok, run this query manually on the \
slave and then restart the slave with SET GLOBAL SQL_SLAVE_SKIP_COUNTER=1; \
START SLAVE; . Query: '%s'", expected_error, thd->query());
        thd->is_slave_error= 1;
      }
      goto end;
    }

    /* If the query was not ignored, it is printed to the general log */
    if (likely(!thd->is_error()) ||
        thd->get_stmt_da()->sql_errno() != ER_SLAVE_IGNORED_TABLE)
      general_log_write(thd, COM_QUERY, thd->query(), thd->query_length());
    else
    {
      /*
        Bug#54201: If we skip an INSERT query that uses auto_increment, then we
        should reset any @@INSERT_ID set by an Intvar_log_event associated with
        the query; otherwise the @@INSERT_ID will linger until the next INSERT
        that uses auto_increment and may affect extra triggers on the slave etc.

        We reset INSERT_ID unconditionally; it is probably cheaper than
        checking if it is necessary.
      */
      thd->auto_inc_intervals_forced.empty();
    }

compare_errors:
    /*
      In the slave thread, we may sometimes execute some DROP / * 40005
      TEMPORARY * / TABLE that come from parts of binlogs (likely if we
      use RESET SLAVE or CHANGE MASTER TO), while the temporary table
      has already been dropped. To ignore such irrelevant "table does
      not exist errors", we silently clear the error if TEMPORARY was used.
    */
    if ((thd->lex->sql_command == SQLCOM_DROP_TABLE ||
         thd->lex->sql_command == SQLCOM_DROP_SEQUENCE) &&
        thd->lex->tmp_table() &&
        thd->is_error() && thd->get_stmt_da()->sql_errno() == ER_BAD_TABLE_ERROR &&
        !expected_error)
      thd->get_stmt_da()->reset_diagnostics_area();
    /*
      If we expected a non-zero error code, and we don't get the same error
      code, and it should be ignored or is related to a concurrency issue.
    */
    actual_error= thd->is_error() ? thd->get_stmt_da()->sql_errno() :
                     skip_error_check? expected_error : 0;
    DBUG_PRINT("info",("expected_error: %d  sql_errno: %d",
                       expected_error, actual_error));

    if ((unlikely(expected_error) &&
         !test_if_equal_repl_errors(expected_error, actual_error) &&
         !concurrency_error_code(expected_error)) &&
        !ignored_error_code(actual_error) &&
        !ignored_error_code(expected_error))
    {
      rli->report(ERROR_LEVEL, 0, rgi->gtid_info(),
                  "Query caused different errors on master and slave.     "
                  "Error on master: message (format)='%s' error code=%d ; "
                  "Error on slave: actual message='%s', error code=%d. "
                  "Default database: '%s'. Query: '%s'",
                  ER_THD(thd, expected_error),
                  expected_error,
                  actual_error ? thd->get_stmt_da()->message() : "no error",
                  actual_error,
                  safe_str(db), query_arg);
      thd->is_slave_error= 1;
    }
    /*
      If we get the same error code as expected and it is not a concurrency
      issue, or should be ignored.
    */
    else if ((test_if_equal_repl_errors(expected_error, actual_error) &&
              !concurrency_error_code(expected_error)) ||
             ignored_error_code(actual_error))
    {
      DBUG_PRINT("info",("error ignored"));
      thd->clear_error(1);
      if (actual_error == ER_QUERY_INTERRUPTED ||
          actual_error == ER_CONNECTION_KILLED)
        thd->reset_killed();
    }
    else if (actual_error == ER_XAER_NOTA && !rpl_filter->db_ok(get_db()))
    {
      /*
        If there is an XA query whose XID cannot be found, if the replication
        filter is active and filters the target database, assume that the XID
        cache has been cleared (e.g. by server restart) since it was prepared,
        so we can just ignore this event.
      */
      thd->clear_error(1);
    }
    /*
      Other cases: mostly we expected no error and get one.
    */
    else if (unlikely(thd->is_slave_error || thd->is_fatal_error))
    {
      if (!is_parallel_retry_error(rgi, actual_error))
        rli->report(ERROR_LEVEL, actual_error, rgi->gtid_info(),
                    "Error '%s' on query. Default database: '%s'. Query: '%s'",
                    (actual_error ? thd->get_stmt_da()->message() :
                     "unexpected success or fatal error"),
                    thd->get_db(), query_arg);
      thd->is_slave_error= 1;
#ifdef WITH_WSREP
      if (wsrep_thd_is_toi(thd) && wsrep_must_ignore_error(thd))
      {
        thd->clear_error(1);
        thd->killed= NOT_KILLED;
        thd->wsrep_has_ignored_error= true;
      }
#endif /* WITH_WSREP */
    }

    /*
      TODO: compare the values of "affected rows" around here. Something
      like:
      if ((uint32) affected_in_event != (uint32) affected_on_slave)
      {
      sql_print_error("Slave: did not get the expected number of affected \
      rows running query from master - expected %d, got %d (this numbers \
      should have matched modulo 4294967296).", 0, ...);
      thd->is_slave_error = 1;
      }
      We may also want an option to tell the slave to ignore "affected"
      mismatch. This mismatch could be implemented with a new ER_ code, and
      to ignore it you would use --slave-skip-errors...

      To do the comparison we need to know the value of "affected" which the
      above mysql_parse() computed. And we need to know the value of
      "affected" in the master's binlog. Both will be implemented later. The
      important thing is that we now have the format ready to log the values
      of "affected" in the binlog. So we can release 5.0.0 before effectively
      logging "affected" and effectively comparing it.
    */
  } /* End of if (db_ok(... */

  {
    /**
      The following failure injecion works in cooperation with tests
      setting @@global.debug= 'd,stop_slave_middle_group'.
      The sql thread receives the killed status and will proceed
      to shutdown trying to finish incomplete events group.
    */
    DBUG_EXECUTE_IF("stop_slave_middle_group",
                    if (!current_stmt_is_commit && is_begin() == 0)
                    {
                      if (thd->transaction->all.modified_non_trans_table)
                        const_cast<Relay_log_info*>(rli)->abort_slave= 1;
                    };);
  }

end:
  if (unlikely(sub_id && !thd->is_slave_error))
    rpl_global_gtid_slave_state->update_state_hash(sub_id, &gtid, hton, rgi);

  /*
    Probably we have set thd->query, thd->db, thd->catalog to point to places
    in the data_buf of this event. Now the event is going to be deleted
    probably, so data_buf will be freed, so the thd->... listed above will be
    pointers to freed memory.
    So we must set them to 0, so that those bad pointers values are not later
    used. Note that "cleanup" queries like automatic DROP TEMPORARY TABLE
    don't suffer from these assignments to 0 as DROP TEMPORARY
    TABLE uses the db.table syntax.
  */
  thd->catalog= 0;
  thd->set_db(&null_clex_str);    /* will free the current database */
  thd->reset_query();
  DBUG_PRINT("info", ("end: query= 0"));

  /* Mark the statement completed. */
  MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
  thd->m_statement_psi= NULL;
  thd->m_digest= NULL;

  /*
    As a disk space optimization, future masters will not log an event for
    LAST_INSERT_ID() if that function returned 0 (and thus they will be able
    to replace the THD::stmt_depends_on_first_successful_insert_id_in_prev_stmt
    variable by (THD->first_successful_insert_id_in_prev_stmt > 0) ; with the
    resetting below we are ready to support that.
  */
  thd->first_successful_insert_id_in_prev_stmt_for_binlog= 0;
  thd->first_successful_insert_id_in_prev_stmt= 0;
  thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt= 0;
  free_root(thd->mem_root,MYF(MY_KEEP_PREALLOC));
  DBUG_RETURN(thd->is_slave_error);
}

Log_event::enum_skip_reason
Query_log_event::do_shall_skip(rpl_group_info *rgi)
{
  Relay_log_info *rli= rgi->rli;
  DBUG_ENTER("Query_log_event::do_shall_skip");
  DBUG_PRINT("debug", ("query: '%s'  q_len: %d", query, q_len));
  DBUG_ASSERT(query && q_len > 0);
  DBUG_ASSERT(thd == rgi->thd);

  /*
    An event skipped due to @@skip_replication must not be counted towards the
    number of events to be skipped due to @@sql_slave_skip_counter.
  */
  if (flags & LOG_EVENT_SKIP_REPLICATION_F &&
      opt_replicate_events_marked_for_skip != RPL_SKIP_REPLICATE)
    DBUG_RETURN(Log_event::EVENT_SKIP_IGNORE);

  if (rli->slave_skip_counter > 0)
  {
    if (is_begin())
    {
      thd->variables.option_bits|= OPTION_BEGIN | OPTION_GTID_BEGIN;
      DBUG_RETURN(Log_event::continue_group(rgi));
    }

    if (is_commit() || is_rollback())
    {
      thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_GTID_BEGIN);
      DBUG_RETURN(Log_event::EVENT_SKIP_COUNT);
    }
  }
#ifdef WITH_WSREP
  else if (WSREP(thd) && wsrep_mysql_replication_bundle &&
           opt_slave_domain_parallel_threads == 0 &&
           thd->wsrep_mysql_replicated > 0 &&
           (is_begin() || is_commit()))
  {
    if (++thd->wsrep_mysql_replicated < (int)wsrep_mysql_replication_bundle)
    {
      WSREP_DEBUG("skipping wsrep commit %d", thd->wsrep_mysql_replicated);
      DBUG_RETURN(Log_event::EVENT_SKIP_IGNORE);
    }
    else
    {
      thd->wsrep_mysql_replicated = 0;
    }
  }
#endif /* WITH_WSREP */
  DBUG_RETURN(Log_event::do_shall_skip(rgi));
}


bool
Query_log_event::peek_is_commit_rollback(const uchar *event_start,
                                         size_t event_len,
                                         enum_binlog_checksum_alg
                                         checksum_alg)
{
  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
  {
    if (event_len > BINLOG_CHECKSUM_LEN)
      event_len-= BINLOG_CHECKSUM_LEN;
    else
      event_len= 0;
  }
  else
    DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
                checksum_alg == BINLOG_CHECKSUM_ALG_OFF);

  if (event_len < LOG_EVENT_HEADER_LEN + QUERY_HEADER_LEN || event_len < 9)
    return false;
  return !memcmp(event_start + (event_len-7), "\0COMMIT", 7) ||
         !memcmp(event_start + (event_len-9), "\0ROLLBACK", 9);
}

/***************************************************************************
       Format_description_log_event methods
****************************************************************************/

void Format_description_log_event::pack_info(Protocol *protocol)
{
  char buf[12 + ST_SERVER_VER_LEN + 14 + 22], *pos;
  pos= strmov(buf, "Server ver: ");
  pos= strmov(pos, server_version);
  pos= strmov(pos, ", Binlog ver: ");
  pos= int10_to_str(binlog_version, pos, 10);
  protocol->store(buf, (uint) (pos-buf), &my_charset_bin);
}
#endif /* defined(HAVE_REPLICATION) */

bool Format_description_log_event::write(Log_event_writer *writer)
{
  bool ret;
  /*
    We don't call Start_log_event_v::write() because this would make 2
    my_b_safe_write().
  */
  uchar buff[START_V3_HEADER_LEN+1];
  size_t rec_size= sizeof(buff) + BINLOG_CHECKSUM_ALG_DESC_LEN +
                   number_of_event_types;
  int2store(buff + ST_BINLOG_VER_OFFSET,binlog_version);
  memcpy((char*) buff + ST_SERVER_VER_OFFSET,server_version,ST_SERVER_VER_LEN);
  if (!dont_set_created)
    created= get_time();
  int4store(buff + ST_CREATED_OFFSET,created);
  buff[ST_COMMON_HEADER_LEN_OFFSET]= common_header_len;
  /*
    if checksum is requested
    record the checksum-algorithm descriptor next to
    post_header_len vector which will be followed by the checksum value.
    Master is supposed to trigger checksum computing by binlog_checksum_options,
    slave does it via marking the event according to
    FD_queue checksum_alg value.
  */
  compile_time_assert(BINLOG_CHECKSUM_ALG_DESC_LEN == 1);
  uint8 checksum_byte= (uint8) (used_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF ?
                                used_checksum_alg : BINLOG_CHECKSUM_ALG_OFF);
  DBUG_ASSERT(used_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);
  /* 
     FD of checksum-aware server is always checksum-equipped, (V) is in,
     regardless of @@global.binlog_checksum policy.
     Thereby a combination of (A) == 0, (V) != 0 means
     it's the checksum-aware server's FD event that heads checksum-free binlog
     file. 
     Here 0 stands for checksumming OFF to evaluate (V) as 0 is that case.
     A combination of (A) != 0, (V) != 0 denotes FD of the checksum-aware server
     heading the checksummed binlog.
     (A), (V) presence in FD of the checksum-aware server makes the event
     1 + 4 bytes bigger comparing to the former FD.
  */

  uint orig_checksum_len= writer->checksum_len;
  writer->checksum_len= BINLOG_CHECKSUM_LEN;
  ret= write_header(writer, rec_size) ||
       write_data(writer, buff, sizeof(buff)) ||
       write_data(writer, post_header_len, number_of_event_types) ||
       write_data(writer, &checksum_byte, sizeof(checksum_byte)) ||
       write_footer(writer);
  writer->checksum_len= orig_checksum_len;
  return ret;
}

#if defined(HAVE_REPLICATION)
/*
 Auxiliary function to conduct cleanup of unfinished two-phase logged ALTERs.
*/
static void check_and_remove_stale_alter(Relay_log_info *rli)
{
  Master_info *mi= rli->mi;
  start_alter_info *info=NULL;

  mysql_mutex_lock(&mi->start_alter_list_lock);
  List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
  while ((info= info_iterator++))
  {
    DBUG_ASSERT(info->state == start_alter_state::REGISTERED);

    sql_print_warning("ALTER query started at %u-%lu-%llu could not "
                      "be completed because of unexpected master server "
                      "or its binlog change", info->domain_id,
                      mi->master_id, info->sa_seq_no);
    info_iterator.remove();
    mysql_mutex_lock(&mi->start_alter_lock);
    info->state= start_alter_state::ROLLBACK_ALTER;
    mysql_mutex_unlock(&mi->start_alter_lock);
    mysql_cond_broadcast(&info->start_alter_cond);
    mysql_mutex_lock(&mi->start_alter_lock);
    while(info->state != start_alter_state::COMPLETED)
      mysql_cond_wait(&info->start_alter_cond, &mi->start_alter_lock);
    mysql_mutex_unlock(&mi->start_alter_lock);
    mysql_cond_destroy(&info->start_alter_cond);
    my_free(info);
  }
  mysql_mutex_unlock(&mi->start_alter_list_lock);
}

int Format_description_log_event::do_apply_event(rpl_group_info *rgi)
{
  int ret= 0;
  Relay_log_info *rli= rgi->rli;
  DBUG_ENTER("Format_description_log_event::do_apply_event");

  /*
    As a transaction NEVER spans on 2 or more binlogs:
    if we have an active transaction at this point, the master died
    while writing the transaction to the binary log, i.e. while
    flushing the binlog cache to the binlog. XA guarantees that master has
    rolled back. So we roll back.
    Note: this event could be sent by the master to inform us of the
    format of its binlog; in other words maybe it is not at its
    original place when it comes to us; we'll know this by checking
    log_pos ("artificial" events have log_pos == 0).
  */
  if (!is_artificial_event() && created && !thd->rli_fake && !thd->rgi_fake)
  {
    // check_and_remove stale Start Alter:s
    if (flags & LOG_EVENT_BINLOG_IN_USE_F)
      check_and_remove_stale_alter(rli);
    if (thd->transaction->all.ha_list)
    {
      /* This is not an error (XA is safe), just an information */
      rli->report(INFORMATION_LEVEL, 0, NULL,
                  "Rolling back unfinished transaction (no COMMIT "
                  "or ROLLBACK in relay log). A probable cause is that "
                  "the master died while writing the transaction to "
                  "its binary log, thus rolled back too.");
      rgi->cleanup_context(thd, 1);
    }
  }

  /*
    If this event comes from ourselves, there is no cleaning task to perform,
    we don't do cleanup (this was just to update the log's description event).
  */
  if (server_id != (uint32) global_system_variables.server_id)
  {
    /*
      If the event was not requested by the slave i.e. the master sent
      it while the slave asked for a position >4, the event will make
      rli->group_master_log_pos advance. Say that the slave asked for
      position 1000, and the Format_desc event's end is 96. Then in
      the beginning of replication rli->group_master_log_pos will be
      0, then 96, then jump to first really asked event (which is
      >96). So this is ok.
    */
    switch (binlog_version)
    {
    case 4:
      if (created)
      {
        rli->close_temporary_tables();

        /* The following is only false if we get here with a BINLOG statement */
        if (rli->mi)
          cleanup_load_tmpdir(&rli->mi->cmp_connection_name);
      }
      break;
    default:
      rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                  ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                  "Binlog version not supported");
      ret= 1;
    }
  }

  if (!ret)
  {
    /* Save the information describing this binlog */
    copy_crypto_data(rli->relay_log.description_event_for_exec);
    delete rli->relay_log.description_event_for_exec;
    rli->relay_log.description_event_for_exec= this;
  }

  DBUG_RETURN(ret);
}

int Format_description_log_event::do_update_pos(rpl_group_info *rgi)
{
  if (server_id == (uint32) global_system_variables.server_id)
  {
    /*
      We only increase the relay log position if we are skipping
      events and do not touch any group_* variables, nor flush the
      relay log info.  If there is a crash, we will have to re-skip
      the events again, but that is a minor issue.

      If we do not skip stepping the group log position (and the
      server id was changed when restarting the server), it might well
      be that we start executing at a position that is invalid, e.g.,
      at a Rows_log_event or a Query_log_event preceeded by a
      Intvar_log_event instead of starting at a Table_map_log_event or
      the Intvar_log_event respectively.
     */
    rgi->inc_event_relay_log_pos();
    return 0;
  }
  else
  {
    return Log_event::do_update_pos(rgi);
  }
}

Log_event::enum_skip_reason
Format_description_log_event::do_shall_skip(rpl_group_info *rgi)
{
  return Log_event::EVENT_SKIP_NOT;
}

#endif


#if defined(HAVE_REPLICATION)
int Start_encryption_log_event::do_apply_event(rpl_group_info* rgi)
{
  return rgi->rli->relay_log.description_event_for_exec->start_decryption(this);
}

int Start_encryption_log_event::do_update_pos(rpl_group_info *rgi)
{
  /*
    master never sends Start_encryption_log_event, any SELE that a slave
    might see was created locally in MYSQL_BIN_LOG::open() on the slave
  */
  rgi->inc_event_relay_log_pos();
  return 0;
}

#endif


/**************************************************************************
  Rotate_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
void Rotate_log_event::pack_info(Protocol *protocol)
{
  StringBuffer<256> tmp(log_cs);
  tmp.length(0);
  tmp.append(new_log_ident, ident_len);
  tmp.append(STRING_WITH_LEN(";pos="));
  tmp.append_ulonglong(pos);
  protocol->store(tmp.ptr(), tmp.length(), &my_charset_bin);
}
#endif


Rotate_log_event::Rotate_log_event(const char* new_log_ident_arg,
                                   uint ident_len_arg, ulonglong pos_arg,
                                   uint flags_arg)
  :Log_event(), new_log_ident(new_log_ident_arg),
   pos(pos_arg),ident_len(ident_len_arg ? ident_len_arg :
                          (uint) strlen(new_log_ident_arg)), flags(flags_arg)
{
  DBUG_ENTER("Rotate_log_event::Rotate_log_event(...,flags)");
  DBUG_PRINT("enter",("new_log_ident: %s  pos: %llu  flags: %lu", new_log_ident_arg,
                      pos_arg, (ulong) flags));
  cache_type= EVENT_NO_CACHE;
  if (flags & DUP_NAME)
    new_log_ident= my_strndup(PSI_INSTRUMENT_ME, new_log_ident_arg, ident_len, MYF(MY_WME));
  if (flags & RELAY_LOG)
    set_relay_log_event();
  DBUG_VOID_RETURN;
}


bool Rotate_log_event::write(Log_event_writer *writer)
{
  char buf[ROTATE_HEADER_LEN];
  int8store(buf + R_POS_OFFSET, pos);
  return (write_header(writer, ROTATE_HEADER_LEN + ident_len) ||
          write_data(writer, buf, ROTATE_HEADER_LEN) ||
          write_data(writer, new_log_ident, (uint) ident_len) ||
          write_footer(writer));
}


#if defined(HAVE_REPLICATION)

/*
  Got a rotate log event from the master.

  This is mainly used so that we can later figure out the logname and
  position for the master.

  We can't rotate the slave's BINlog as this will cause infinitive rotations
  in a A -> B -> A setup.
  The NOTES below is a wrong comment which will disappear when 4.1 is merged.

  This must only be called from the Slave SQL thread, since it calls
  Relay_log_info::flush().

  @retval
    0	ok
    1   error
*/
int Rotate_log_event::do_update_pos(rpl_group_info *rgi)
{
  int error= 0;
  Relay_log_info *rli= rgi->rli;
  DBUG_ENTER("Rotate_log_event::do_update_pos");

  DBUG_PRINT("info", ("server_id=%lu; ::server_id=%lu",
                      (ulong) this->server_id, (ulong) global_system_variables.server_id));
  DBUG_PRINT("info", ("new_log_ident: %s", this->new_log_ident));
  DBUG_PRINT("info", ("pos: %llu", this->pos));

  /*
    If we are in a transaction or in a group: the only normal case is
    when the I/O thread was copying a big transaction, then it was
    stopped and restarted: we have this in the relay log:

    BEGIN
    ...
    ROTATE (a fake one)
    ...
    COMMIT or ROLLBACK

    In that case, we don't want to touch the coordinates which
    correspond to the beginning of the transaction.  Starting from
    5.0.0, there also are some rotates from the slave itself, in the
    relay log, which shall not change the group positions.

    In parallel replication, rotate event is executed out-of-band with normal
    events, so we cannot update group_master_log_name or _pos here, it will
    be updated with the next normal event instead.
  */
  if ((server_id != global_system_variables.server_id ||
       rli->replicate_same_server_id) &&
      !is_relay_log_event() &&
      !rli->is_in_group() &&
      !rgi->is_parallel_exec)
  {
    mysql_mutex_lock(&rli->data_lock);
    DBUG_PRINT("info", ("old group_master_log_name: '%s'  "
                        "old group_master_log_pos: %lu",
                        rli->group_master_log_name,
                        (ulong) rli->group_master_log_pos));
    memcpy(rli->group_master_log_name, new_log_ident, ident_len+1);
    rli->notify_group_master_log_name_update();
    rli->inc_group_relay_log_pos(pos, rgi, TRUE /* skip_lock */);
    DBUG_PRINT("info", ("new group_master_log_name: '%s'  "
                        "new group_master_log_pos: %lu",
                        rli->group_master_log_name,
                        (ulong) rli->group_master_log_pos));
    mysql_mutex_unlock(&rli->data_lock);
    rpl_global_gtid_slave_state->record_and_update_gtid(thd, rgi);
    error= rli->flush();
    
    /*
      Reset thd->variables.option_bits and sql_mode etc, because this could
      be the signal of a master's downgrade from 5.0 to 4.0.
      However, no need to reset description_event_for_exec: indeed, if the next
      master is 5.0 (even 5.0.1) we will soon get a Format_desc; if the next
      master is 4.0 then the events are in the slave's format (conversion).
    */
    set_slave_thread_options(thd);
    set_slave_thread_default_charset(thd, rgi);
    thd->variables.sql_mode= global_system_variables.sql_mode;
    thd->variables.auto_increment_increment=
      thd->variables.auto_increment_offset= 1;
  }
  else
    rgi->inc_event_relay_log_pos();

  DBUG_RETURN(error);
}


Log_event::enum_skip_reason
Rotate_log_event::do_shall_skip(rpl_group_info *rgi)
{
  enum_skip_reason reason= Log_event::do_shall_skip(rgi);

  switch (reason) {
  case Log_event::EVENT_SKIP_NOT:
  case Log_event::EVENT_SKIP_COUNT:
    return Log_event::EVENT_SKIP_NOT;

  case Log_event::EVENT_SKIP_IGNORE:
    return Log_event::EVENT_SKIP_IGNORE;
  }
  DBUG_ASSERT_NO_ASSUME(0);
  return Log_event::EVENT_SKIP_NOT;             // To keep compiler happy
}

#endif


/**************************************************************************
  Binlog_checkpoint_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
void Binlog_checkpoint_log_event::pack_info(Protocol *protocol)
{
  protocol->store(binlog_file_name, binlog_file_len, &my_charset_bin);
}


Log_event::enum_skip_reason
Binlog_checkpoint_log_event::do_shall_skip(rpl_group_info *rgi)
{
  enum_skip_reason reason= Log_event::do_shall_skip(rgi);
  if (reason == EVENT_SKIP_COUNT)
    reason= EVENT_SKIP_NOT;
  return reason;
}
#endif


Binlog_checkpoint_log_event::Binlog_checkpoint_log_event(
        const char *binlog_file_name_arg,
        uint binlog_file_len_arg)
  :Log_event(),
   binlog_file_name(my_strndup(PSI_INSTRUMENT_ME, binlog_file_name_arg, binlog_file_len_arg,
                               MYF(MY_WME))),
   binlog_file_len(binlog_file_len_arg)
{
  cache_type= EVENT_NO_CACHE;
}


bool Binlog_checkpoint_log_event::write(Log_event_writer *writer)
{
  uchar buf[BINLOG_CHECKPOINT_HEADER_LEN];
  int4store(buf, binlog_file_len);
  return write_header(writer, BINLOG_CHECKPOINT_HEADER_LEN + binlog_file_len) ||
         write_data(writer, buf, BINLOG_CHECKPOINT_HEADER_LEN) ||
         write_data(writer, binlog_file_name, binlog_file_len) ||
         write_footer(writer);
}


/**************************************************************************
        Global transaction ID stuff
**************************************************************************/

Gtid_log_event::Gtid_log_event(THD *thd_arg, uint64 seq_no_arg,
                               uint32 domain_id_arg, bool standalone,
                               uint16 flags_arg, bool is_transactional,
                               uint64 commit_id_arg, bool has_xid,
                               bool ro_1pc)
  : Log_event(thd_arg, flags_arg, is_transactional),
    seq_no(seq_no_arg), commit_id(commit_id_arg), domain_id(domain_id_arg),
    pad_to_size(0), flags2((standalone ? FL_STANDALONE : 0) |
           (commit_id_arg ? FL_GROUP_COMMIT_ID : 0)),
    flags_extra(0), extra_engines(0),
    thread_id(thd_arg->variables.pseudo_thread_id)
{
  cache_type= Log_event::EVENT_NO_CACHE;
  bool is_tmp_table= thd_arg->lex->stmt_accessed_temp_table();
  if (thd_arg->transaction->stmt.trans_did_wait() ||
      thd_arg->transaction->all.trans_did_wait())
    flags2|= FL_WAITED;
  if (thd_arg->transaction->stmt.trans_did_ddl() ||
      thd_arg->transaction->stmt.has_created_dropped_temp_table() ||
      thd_arg->transaction->stmt.trans_executed_admin_cmd() ||
      thd_arg->transaction->all.trans_did_ddl() ||
      thd_arg->transaction->all.has_created_dropped_temp_table() ||
      thd_arg->transaction->all.trans_executed_admin_cmd())
    flags2|= FL_DDL;
  else if (is_transactional && !is_tmp_table &&
           !(thd_arg->transaction->all.modified_non_trans_table &&
             thd->variables.binlog_direct_non_trans_update == 0 &&
             !thd->is_current_stmt_binlog_format_row()))
    flags2|= FL_TRANSACTIONAL;
  if (!(thd_arg->variables.option_bits & OPTION_RPL_SKIP_PARALLEL))
    flags2|= FL_ALLOW_PARALLEL;
  /* Preserve any DDL or WAITED flag in the slave's binlog. */
  if (thd_arg->rgi_slave)
    flags2|= (thd_arg->rgi_slave->gtid_ev_flags2 & (FL_DDL|FL_WAITED));
  if (!thd->rgi_slave ||
      thd_arg->rgi_slave->gtid_ev_flags_extra & FL_EXTRA_THREAD_ID)
    flags_extra|= FL_EXTRA_THREAD_ID;

  XID_STATE &xid_state= thd->transaction->xid_state;
  if (is_transactional)
  {
    if (xid_state.is_explicit_XA() &&
        (thd->lex->sql_command == SQLCOM_XA_PREPARE ||
         xid_state.get_state_code() == XA_PREPARED))
    {
      DBUG_ASSERT(!(thd->lex->sql_command == SQLCOM_XA_COMMIT &&
                    thd->lex->xa_opt == XA_ONE_PHASE));

      flags2|= thd->lex->sql_command == SQLCOM_XA_PREPARE ?
        FL_PREPARED_XA : FL_COMPLETED_XA;
      xid.set(xid_state.get_xid());
    }
    /* count non-zero extra recoverable engines; total = extra + 1 */
    if (has_xid)
    {
      DBUG_ASSERT(ha_count_rw_2pc(thd_arg,
                                  thd_arg->in_multi_stmt_transaction_mode()));

      extra_engines=
        ha_count_rw_2pc(thd_arg, thd_arg->in_multi_stmt_transaction_mode()) - 1;
    }
    else if (ro_1pc)
    {
      extra_engines= UCHAR_MAX;
    }
    else if (thd->lex->sql_command == SQLCOM_XA_PREPARE)
    {
      DBUG_ASSERT(thd_arg->in_multi_stmt_transaction_mode());

      uint8 count= ha_count_rw_2pc(thd_arg, true);
      extra_engines= count > 1 ? 0 : UCHAR_MAX;
    }
    if (extra_engines > 0)
      flags_extra|= FL_EXTRA_MULTI_ENGINE_E1;
  }
  if (thd->get_binlog_flags_for_alter())
  {
    flags_extra |= thd->get_binlog_flags_for_alter();
    if (flags_extra & (FL_COMMIT_ALTER_E1 | FL_ROLLBACK_ALTER_E1))
      sa_seq_no= thd->get_binlog_start_alter_seq_no();
    flags2|= FL_DDL;
  }

  DBUG_ASSERT(thd_arg->lex->sql_command != SQLCOM_CREATE_SEQUENCE ||
              (flags2 & FL_DDL) || thd_arg->in_multi_stmt_transaction_mode());
}


/*
  Used to record GTID while sending binlog to slave, without having to
  fully construct every Gtid_log_event() needlessly.
*/
bool
Gtid_log_event::peek(const uchar *event_start, size_t event_len,
                     enum_binlog_checksum_alg checksum_alg,
                     uint32 *domain_id, uint32 *server_id, uint64 *seq_no,
                     uchar *flags2, const Format_description_log_event *fdev)
{
  const uchar *p;

  if (checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
  {
    if (event_len > BINLOG_CHECKSUM_LEN)
      event_len-= BINLOG_CHECKSUM_LEN;
    else
      event_len= 0;
  }
  else
    DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
                checksum_alg == BINLOG_CHECKSUM_ALG_OFF);

  if (event_len < (uint32)fdev->common_header_len + GTID_HEADER_LEN)
    return true;
  *server_id= uint4korr(event_start + SERVER_ID_OFFSET);
  p= event_start + fdev->common_header_len;
  *seq_no= uint8korr(p);
  p+= 8;
  *domain_id= uint4korr(p);
  p+= 4;
  *flags2= *p;
  return false;
}


bool
Gtid_log_event::write(Log_event_writer *writer)
{
  uchar buf[max_data_length];
  size_t write_len= 13;

  int8store(buf, seq_no);
  int4store(buf+8, domain_id);
  buf[12]= flags2;
  if (flags2 & FL_GROUP_COMMIT_ID)
  {
    DBUG_ASSERT(write_len + 8 == GTID_HEADER_LEN + 2);

    int8store(buf+write_len, commit_id);
    write_len= GTID_HEADER_LEN + 2;
  }

  if (flags2 & (FL_PREPARED_XA | FL_COMPLETED_XA)
      && !DBUG_IF("negate_xid_from_gtid"))
  {
    int4store(&buf[write_len],   xid.formatID);
    buf[write_len +4]=   (uchar) xid.gtrid_length;
    buf[write_len +4+1]= (uchar) xid.bqual_length;
    write_len+= 6;
    long data_length= xid.bqual_length + xid.gtrid_length;

    if (!DBUG_IF("negate_xid_data_from_gtid"))
    {
      memcpy(buf+write_len, xid.data, data_length);
      write_len+= data_length;
    }
  }

#ifndef DBUG_OFF
  /*
    The following debug_dbug flags which simulate invalid events are only
    valid for pre-FL_EXTRA_THREAD_ID events (i.e. before 11.5). So do not write
    the thread id attribute when simulating these invalid events.
  */
  if (DBUG_IF("negate_xid_from_gtid") ||
      DBUG_IF("negate_xid_data_from_gtid") ||
      DBUG_IF("inject_fl_extra_multi_engine_into_gtid") ||
      DBUG_IF("negate_alter_fl_from_gtid"))
    flags_extra&= ~FL_EXTRA_THREAD_ID;
#endif

  DBUG_EXECUTE_IF("inject_fl_extra_multi_engine_into_gtid", {
    flags_extra|= FL_EXTRA_MULTI_ENGINE_E1;
  });
  if (flags_extra > 0)
  {
    buf[write_len]= flags_extra;
    write_len++;
  }
  DBUG_EXECUTE_IF("inject_fl_extra_multi_engine_into_gtid", {
    flags_extra&= ~FL_EXTRA_MULTI_ENGINE_E1;
  });

  if (flags_extra & FL_EXTRA_MULTI_ENGINE_E1)
  {
    buf[write_len]= extra_engines;
    write_len++;
  }

  if (flags_extra & (FL_COMMIT_ALTER_E1 | FL_ROLLBACK_ALTER_E1)
      && !DBUG_IF("negate_alter_fl_from_gtid")
  )
  {
    int8store(buf + write_len, sa_seq_no);
    write_len+= 8;
  }

  if (flags_extra & FL_EXTRA_THREAD_ID)
  {
    int4store(buf + write_len, thread_id);
    write_len+= 4;
  }

  if (write_len < GTID_HEADER_LEN)
  {
    bzero(buf+write_len, GTID_HEADER_LEN-write_len);
    write_len= GTID_HEADER_LEN;
  }

  if (unlikely(pad_to_size > write_len))
  {
    if (write_header(writer, pad_to_size) ||
        write_data(writer, buf, write_len))
      return true;

    pad_to_size-= write_len;

    char pad_buf[IO_SIZE];
    bzero(pad_buf,  pad_to_size);
    while (pad_to_size)
    {
      uint64 size= pad_to_size >= IO_SIZE ? IO_SIZE : pad_to_size;
      if (write_data(writer, pad_buf, size))
        return true;
      pad_to_size-= size;
    }
    return write_footer(writer);
  }

  return write_header(writer, write_len) ||
         write_data(writer, buf, write_len) ||
         write_footer(writer);
}


/*
  Replace a GTID event with either a BEGIN event, dummy event, or nothing, as
  appropriate to work with old slave that does not know global transaction id.

  The need_dummy_event argument is an IN/OUT argument. It is passed as TRUE
  if slave has capability lower than MARIA_SLAVE_CAPABILITY_TOLERATE_HOLES.
  It is returned TRUE if we return a BEGIN (or dummy) event to be sent to the
  slave, FALSE if event should be skipped completely.
*/
int
Gtid_log_event::make_compatible_event(String *packet, bool *need_dummy_event,
                                      ulong ev_offset,
                                      enum_binlog_checksum_alg checksum_alg)
{
  uchar flags2;
  if (packet->length() - ev_offset < LOG_EVENT_HEADER_LEN + GTID_HEADER_LEN)
    return 1;
  flags2= (*packet)[ev_offset + LOG_EVENT_HEADER_LEN + 12];
  if (flags2 & FL_STANDALONE)
  {
    if (*need_dummy_event)
      return Query_log_event::dummy_event(packet, ev_offset, checksum_alg);
    return 0;
  }

  *need_dummy_event= true;
  return Query_log_event::begin_event(packet, ev_offset, checksum_alg);
}


#ifdef HAVE_REPLICATION
void
Gtid_log_event::pack_info(Protocol *protocol)
{
  char buf[6+5+10+1+10+1+20+1+4+20+1+ ser_buf_size+5 /* sprintf */];
  char *p;
  p = strmov(buf, (flags2 & FL_STANDALONE  ? "GTID " :
                   flags2 & FL_PREPARED_XA ? "XA START " : "BEGIN GTID "));
  if (flags2 & FL_PREPARED_XA)
  {
    p+= sprintf(p, "%s GTID ", xid.serialize());
  }
  p= longlong10_to_str(domain_id, p, 10);
  *p++= '-';
  p= longlong10_to_str(server_id, p, 10);
  *p++= '-';
  p= longlong10_to_str(seq_no, p, 10);
  if (flags2 & FL_GROUP_COMMIT_ID)
  {
    p= strmov(p, " cid=");
    p= longlong10_to_str(commit_id, p, 10);
  }
  if (flags_extra & FL_START_ALTER_E1)
  {
    p= strmov(p, " START ALTER");
  }
  if (flags_extra & FL_COMMIT_ALTER_E1)
  {
    p= strmov(p, " COMMIT ALTER id=");
    p= longlong10_to_str(sa_seq_no, p, 10);
  }
  if (flags_extra & FL_ROLLBACK_ALTER_E1)
  {
    p= strmov(p, " ROLLBACK ALTER id=");
    p= longlong10_to_str(sa_seq_no, p, 10);
  }

  protocol->store(buf, p-buf, &my_charset_bin);
}

static char gtid_begin_string[] = "BEGIN";

int
Gtid_log_event::do_apply_event(rpl_group_info *rgi)
{
  Relay_log_info *rli= rgi->rli;
  ulonglong bits= thd->variables.option_bits;

  if (unlikely(thd->transaction->all.ha_list || (bits & OPTION_GTID_BEGIN)))
  {
    rli->report(WARNING_LEVEL, 0, NULL,
                "Rolling back unfinished transaction (no COMMIT "
                "or ROLLBACK in relay log). This indicates a corrupt binlog "
                "on the master, possibly caused by disk full or other write "
                "error.");
    rgi->cleanup_context(thd, 1);
    bits= thd->variables.option_bits;
  }

  thd->variables.server_id= this->server_id;
  thd->variables.gtid_domain_id= this->domain_id;
  thd->variables.gtid_seq_no= this->seq_no;
  thd->variables.pseudo_thread_id= this->thread_id;
  rgi->gtid_ev_flags2= flags2;

  rgi->gtid_ev_flags_extra= flags_extra;
  rgi->gtid_ev_sa_seq_no= sa_seq_no;
  thd->reset_for_next_command();

  if (opt_gtid_strict_mode && opt_bin_log && opt_log_slave_updates)
  {
    if (mysql_bin_log.check_strict_gtid_sequence(this->domain_id,
                                                 this->server_id, this->seq_no))
      return 1;
  }

  DBUG_ASSERT((bits & OPTION_GTID_BEGIN) == 0);

  Master_info *mi= rli->mi;
  switch (flags2 & (FL_DDL | FL_TRANSACTIONAL))
  {
    case FL_TRANSACTIONAL:
      mi->total_trans_groups++;
      break;
    case FL_DDL:
      mi->total_ddl_groups++;
    break;
    default:
      mi->total_non_trans_groups++;
  }

  if (flags2 & FL_STANDALONE)
    return 0;

  /* Execute this like a BEGIN query event. */
  bits|= OPTION_GTID_BEGIN;
  if (flags2 & FL_ALLOW_PARALLEL)
    bits&= ~(ulonglong)OPTION_RPL_SKIP_PARALLEL;
  else
    bits|= (ulonglong)OPTION_RPL_SKIP_PARALLEL;
  thd->variables.option_bits= bits;
  DBUG_PRINT("info", ("Set OPTION_GTID_BEGIN"));
  thd->is_slave_error= 0;

  char buf_xa[sizeof("XA START") + 1 + ser_buf_size];
  if (flags2 & FL_PREPARED_XA)
  {
    const char fmt[]= "XA START %s";

    thd->lex->xid= &xid;
    thd->lex->xa_opt= XA_NONE;
    sprintf(buf_xa, fmt, xid.serialize());
    thd->set_query_and_id(buf_xa, static_cast<uint32>(strlen(buf_xa)),
                          &my_charset_bin, next_query_id());
    thd->lex->sql_command= SQLCOM_XA_START;
    if (trans_xa_start(thd))
    {
      DBUG_PRINT("error", ("trans_xa_start() failed"));
      thd->is_slave_error= 1;
    }
  }
  else
  {
    thd->set_query_and_id(gtid_begin_string, sizeof(gtid_begin_string)-1,
                          &my_charset_bin, next_query_id());
    thd->lex->sql_command= SQLCOM_BEGIN;
    if (trans_begin(thd, 0))
    {
      DBUG_PRINT("error", ("trans_begin() failed"));
      thd->is_slave_error= 1;
    }
  }
  status_var_increment(thd->status_var.com_stat[thd->lex->sql_command]);
  thd->update_stats();

  if (likely(!thd->is_slave_error))
    general_log_write(thd, COM_QUERY, thd->query(), thd->query_length());

  thd->reset_query();
  free_root(thd->mem_root,MYF(MY_KEEP_PREALLOC));
  return thd->is_slave_error;
}


int
Gtid_log_event::do_update_pos(rpl_group_info *rgi)
{
  rgi->inc_event_relay_log_pos();
  return 0;
}


Log_event::enum_skip_reason
Gtid_log_event::do_shall_skip(rpl_group_info *rgi)
{
  Relay_log_info *rli= rgi->rli;
  /*
    An event skipped due to @@skip_replication must not be counted towards the
    number of events to be skipped due to @@sql_slave_skip_counter.
  */
  if (flags & LOG_EVENT_SKIP_REPLICATION_F &&
      opt_replicate_events_marked_for_skip != RPL_SKIP_REPLICATE)
    return Log_event::EVENT_SKIP_IGNORE;

  if (rli->slave_skip_counter > 0)
  {
    if (!(flags2 & FL_STANDALONE))
    {
      thd->variables.option_bits|= OPTION_BEGIN;
      DBUG_ASSERT(rgi->rli->get_flag(Relay_log_info::IN_TRANSACTION));
    }
    return Log_event::continue_group(rgi);
  }
  return Log_event::do_shall_skip(rgi);
}


#endif  /* HAVE_REPLICATION */



Gtid_list_log_event::Gtid_list_log_event(rpl_binlog_state *gtid_set,
                                         uint32 gl_flags_)
  : count(gtid_set->count()), gl_flags(gl_flags_), list(0), sub_id_list(0)
{
  cache_type= EVENT_NO_CACHE;
  /* Failure to allocate memory will be caught by is_valid() returning false. */
  if (count < (1<<28) &&
      (list = (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME,
                           count * sizeof(*list) + (count == 0), MYF(MY_WME))))
    gtid_set->get_gtid_list(list, count);
}


Gtid_list_log_event::Gtid_list_log_event(slave_connection_state *gtid_set,
                                         uint32 gl_flags_)
  : count(gtid_set->count()), gl_flags(gl_flags_), list(0), sub_id_list(0)
{
  cache_type= EVENT_NO_CACHE;
  /* Failure to allocate memory will be caught by is_valid() returning false. */
  if (count < (1<<28) &&
      (list = (rpl_gtid *)my_malloc(PSI_INSTRUMENT_ME,
                          count * sizeof(*list) + (count == 0), MYF(MY_WME))))
  {
    gtid_set->get_gtid_list(list, count);
#if defined(HAVE_REPLICATION)
    if (gl_flags & FLAG_IGN_GTIDS)
    {
      uint32 i;

      if (!(sub_id_list= (uint64 *)my_malloc(PSI_INSTRUMENT_ME,
                                         count * sizeof(uint64), MYF(MY_WME))))
      {
        my_free(list);
        list= NULL;
        return;
      }
      for (i= 0; i < count; ++i)
      {
        if (!(sub_id_list[i]=
              rpl_global_gtid_slave_state->next_sub_id(list[i].domain_id)))
        {
          my_free(list);
          my_free(sub_id_list);
          list= NULL;
          sub_id_list= NULL;
          return;
        }
      }
    }
#endif
  }
}


#if defined(HAVE_REPLICATION)
bool
Gtid_list_log_event::to_packet(String *packet)
{
  uint32 i;
  uchar *p;
  uint32 needed_length;

  DBUG_ASSERT(count < 1<<28);

  needed_length= packet->length() + get_data_size();
  if (packet->reserve(needed_length))
    return true;
  p= (uchar *)packet->ptr() + packet->length();;
  packet->length(needed_length);
  int4store(p, (count & ((1<<28)-1)) | gl_flags);
  p += 4;
  /* Initialise the padding for empty Gtid_list. */
  if (count == 0)
    int2store(p, 0);
  for (i= 0; i < count; ++i)
  {
    int4store(p, list[i].domain_id);
    int4store(p+4, list[i].server_id);
    int8store(p+8, list[i].seq_no);
    p += 16;
  }

  return false;
}


bool
Gtid_list_log_event::write(Log_event_writer *writer)
{
  char buf[128];
  String packet(buf, sizeof(buf), system_charset_info);

  packet.length(0);
  if (to_packet(&packet))
    return true;
  return write_header(writer, get_data_size()) ||
         write_data(writer, packet.ptr(), packet.length()) ||
         write_footer(writer);
}


int
Gtid_list_log_event::do_apply_event(rpl_group_info *rgi)
{
  Relay_log_info *rli= const_cast<Relay_log_info*>(rgi->rli);
  int ret;
  if (gl_flags & FLAG_IGN_GTIDS)
  {
    void *hton= NULL;
    uint32 i;

    for (i= 0; i < count; ++i)
    {
      if ((ret= rpl_global_gtid_slave_state->record_gtid(thd, &list[i],
                                                         sub_id_list[i],
                                                         false, false, &hton)))
        return ret;
      rpl_global_gtid_slave_state->update_state_hash(sub_id_list[i], &list[i],
                                                     hton, NULL);
    }
  }
  ret= Log_event::do_apply_event(rgi);
  if (rli->until_condition == Relay_log_info::UNTIL_GTID &&
      (gl_flags & FLAG_UNTIL_REACHED))
  {
    char str_buf[128];
    String str(str_buf, sizeof(str_buf), system_charset_info);
    rli->until_gtid_pos.to_string(&str);
    sql_print_information("Slave SQL thread stops because it reached its"
                          " UNTIL master_gtid_pos %s", str.c_ptr_safe());
    rli->abort_slave= true;
    rli->stop_for_until= true;
  }
  free_root(thd->mem_root, MYF(MY_KEEP_PREALLOC));
  return ret;
}


Log_event::enum_skip_reason
Gtid_list_log_event::do_shall_skip(rpl_group_info *rgi)
{
  enum_skip_reason reason= Log_event::do_shall_skip(rgi);
  if (reason == EVENT_SKIP_COUNT)
    reason= EVENT_SKIP_NOT;
  return reason;
}


void
Gtid_list_log_event::pack_info(Protocol *protocol)
{
  char buf_mem[1024];
  String buf(buf_mem, sizeof(buf_mem), system_charset_info);
  uint32 i;
  bool first;

  /*
    For output consistency and ease of reading, we sort the GTID list in
    ascending order
  */
  qsort(list, count, sizeof(rpl_gtid), compare_glle_gtids);

  buf.length(0);
  buf.append(STRING_WITH_LEN("["));
  first= true;
  for (i= 0; i < count; ++i)
    rpl_slave_state_tostring_helper(&buf, &list[i], &first);
  buf.append(STRING_WITH_LEN("]"));

  protocol->store(&buf);
}
#endif  /* HAVE_REPLICATION */



/**************************************************************************
	Intvar_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
void Intvar_log_event::pack_info(Protocol *protocol)
{
  char buf[256], *pos;
  pos= strmake(buf, get_var_type_name(), sizeof(buf)-23);
  *pos++= '=';
  pos= longlong10_to_str(val, pos, -10);
  protocol->store(buf, (uint) (pos-buf), &my_charset_bin);
}
#endif


bool Intvar_log_event::write(Log_event_writer *writer)
{
  uchar buf[9];
  buf[I_TYPE_OFFSET]= (uchar) type;
  int8store(buf + I_VAL_OFFSET, val);
  return write_header(writer, sizeof(buf)) ||
         write_data(writer, buf, sizeof(buf)) ||
         write_footer(writer);
}


#if defined(HAVE_REPLICATION)

/*
  Intvar_log_event::do_apply_event()
*/

int Intvar_log_event::do_apply_event(rpl_group_info *rgi)
{
  DBUG_ENTER("Intvar_log_event::do_apply_event");
  if (rgi->deferred_events_collecting)
  {
    DBUG_PRINT("info",("deferring event"));
    DBUG_RETURN(rgi->deferred_events->add(this));
  }

  switch (type) {
  case LAST_INSERT_ID_EVENT:
    thd->first_successful_insert_id_in_prev_stmt= val;
    DBUG_PRINT("info",("last_insert_id_event: %ld", (long) val));
    break;
  case INSERT_ID_EVENT:
    thd->force_one_auto_inc_interval(val);
    break;
  }
  DBUG_RETURN(0);
}

int Intvar_log_event::do_update_pos(rpl_group_info *rgi)
{
  rgi->inc_event_relay_log_pos();
  return 0;
}


Log_event::enum_skip_reason
Intvar_log_event::do_shall_skip(rpl_group_info *rgi)
{
  /*
    It is a common error to set the slave skip counter to 1 instead of
    2 when recovering from an insert which used a auto increment,
    rand, or user var.  Therefore, if the slave skip counter is 1, we
    just say that this event should be skipped by ignoring it, meaning
    that we do not change the value of the slave skip counter since it
    will be decreased by the following insert event.
  */
  return continue_group(rgi);
}

#endif


/**************************************************************************
  Rand_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
void Rand_log_event::pack_info(Protocol *protocol)
{
  char buf1[256], *pos;
  pos= strmov(buf1,"rand_seed1=");
  pos= int10_to_str((long) seed1, pos, 10);
  pos= strmov(pos, ",rand_seed2=");
  pos= int10_to_str((long) seed2, pos, 10);
  protocol->store(buf1, (uint) (pos-buf1), &my_charset_bin);
}
#endif


bool Rand_log_event::write(Log_event_writer *writer)
{
  uchar buf[16];
  int8store(buf + RAND_SEED1_OFFSET, seed1);
  int8store(buf + RAND_SEED2_OFFSET, seed2);
  return write_header(writer, sizeof(buf)) ||
         write_data(writer, buf, sizeof(buf)) ||
         write_footer(writer);
}


#if defined(HAVE_REPLICATION)
int Rand_log_event::do_apply_event(rpl_group_info *rgi)
{
  if (rgi->deferred_events_collecting)
    return rgi->deferred_events->add(this);

  thd->rand.seed1= (ulong) seed1;
  thd->rand.seed2= (ulong) seed2;
  return 0;
}

int Rand_log_event::do_update_pos(rpl_group_info *rgi)
{
  rgi->inc_event_relay_log_pos();
  return 0;
}


Log_event::enum_skip_reason
Rand_log_event::do_shall_skip(rpl_group_info *rgi)
{
  /*
    It is a common error to set the slave skip counter to 1 instead of
    2 when recovering from an insert which used a auto increment,
    rand, or user var.  Therefore, if the slave skip counter is 1, we
    just say that this event should be skipped by ignoring it, meaning
    that we do not change the value of the slave skip counter since it
    will be decreased by the following insert event.
  */
  return continue_group(rgi);
}

/**
   Exec deferred Int-, Rand- and User- var events prefixing
   a Query-log-event event.

   @param thd THD handle

   @return false on success, true if a failure in an event applying occurred.
*/
bool slave_execute_deferred_events(THD *thd)
{
  bool res= false;
  rpl_group_info *rgi= thd->rgi_slave;

  DBUG_ASSERT(rgi && (!rgi->deferred_events_collecting || rgi->deferred_events));

  if (!rgi->deferred_events_collecting || rgi->deferred_events->is_empty())
    return res;

  res= rgi->deferred_events->execute(rgi);
  rgi->deferred_events->rewind();

  return res;
}

#endif /* HAVE_REPLICATION */


/**************************************************************************
  Xid_apply_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)

int Xid_apply_log_event::do_record_gtid(THD *thd, rpl_group_info *rgi,
                                        bool in_trans, void **out_hton,
                                        bool force_err)
{
  int err= 0;
  Relay_log_info const *rli= rgi->rli;

  rgi->gtid_pending= false;
  err= rpl_global_gtid_slave_state->record_gtid(thd, &rgi->current_gtid,
                                                rgi->gtid_sub_id,
                                                in_trans, false, out_hton);

  if (unlikely(err))
  {
    int ec= thd->get_stmt_da()->sql_errno();
    /*
      Do not report an error if this is really a kill due to a deadlock.
      In this case, the transaction will be re-tried instead. Unless force_err
      is set, as in the case of XA PREPARE, as the GTID state is updated as a
      separate transaction, and if that fails, we should not retry but exit in
      error immediately.
    */
    if (!is_parallel_retry_error(rgi, ec) || force_err)
    {
      char buff[MAX_SLAVE_ERRMSG];
      buff[0]= 0;
      aggregate_da_errors(buff, sizeof(buff), thd->get_stmt_da());

      if (force_err)
        thd->clear_error();

      rli->report(ERROR_LEVEL, ER_CANNOT_UPDATE_GTID_STATE, rgi->gtid_info(),
                  "Error during XID COMMIT: failed to update GTID state in "
                  "%s.%s: %d: %s the event's master log %s, end_log_pos %llu",
                  "mysql", rpl_gtid_slave_state_table_name.str, ec,
                  buff, RPL_LOG_NAME, log_pos);
    }
    thd->is_slave_error= 1;
  }

  return err;
}

static bool wsrep_must_replay(THD *thd)
{
#ifdef WITH_WSREP
  mysql_mutex_lock(&thd->LOCK_thd_data);
  bool res= WSREP(thd) && thd->wsrep_trx().state() == wsrep::transaction::s_must_replay;
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  return res;
#else
  return false;
#endif
}


int Xid_apply_log_event::do_apply_event(rpl_group_info *rgi)
{
  bool res;
  int err;
  uint64 sub_id= 0;
  void *hton= NULL;
  rpl_gtid gtid;

  /*
    An instance of this class such as XID_EVENT works like a COMMIT
    statement. It updates mysql.gtid_slave_pos with the GTID of the
    current transaction.
    Therefore, it acts much like a normal SQL statement, so we need to do
    THD::reset_for_next_command() as if starting a new statement.

    XA_PREPARE_LOG_EVENT also updates the gtid table *but* the update gets
    committed as separate "autocommit" transaction.
  */
  thd->reset_for_next_command();
  /*
    Record any GTID in the same transaction, so slave state is transactionally
    consistent.
  */
#ifdef WITH_WSREP
  thd->wsrep_affected_rows= 0;
#endif

#ifndef DBUG_OFF
  bool record_gtid_delayed_for_xa= false;
#endif
  if (rgi->gtid_pending)
  {
    sub_id= rgi->gtid_sub_id;
    gtid= rgi->current_gtid;

    if (!thd->transaction->xid_state.is_explicit_XA())
    {
      if ((err= do_record_gtid(thd, rgi, true /* in_trans */, &hton)))
        return err;

      DBUG_EXECUTE_IF("gtid_fail_after_record_gtid",
                      {
                        my_error(ER_ERROR_DURING_COMMIT, MYF(0),
                                 HA_ERR_WRONG_COMMAND);
                        thd->is_slave_error= 1;
                        return 1;
                      });
    }
#ifndef DBUG_OFF
    else
      record_gtid_delayed_for_xa= true;
#endif
  }

  general_log_print(thd, COM_QUERY, "%s", get_query());
  thd->variables.option_bits&= ~OPTION_GTID_BEGIN;
  /*
    Use the time from the current Xid_log_event for the generated
    Xid_log_event in binlog_commit_flush_xid_caches().
    This ensures that the time for Xid_log_events does not change
    and allows slaves to give a consistent value for
    Slave_last_event_time.
  */
  thd->start_time= when;

  res= do_commit();
  if (!res && rgi->gtid_pending)
  {
    DBUG_ASSERT(!thd->transaction->xid_state.is_explicit_XA());

    DBUG_ASSERT_NO_ASSUME(record_gtid_delayed_for_xa);
    if (thd->rgi_slave->is_parallel_exec)
    {
      /*
        With XA, since the transaction is prepared/committed without updating
        the GTID pos (MDEV-32020...), we need here to clear any pending
        deadlock kill.

        Otherwise if the kill happened after the prepare/commit completed, it
        might end up killing the subsequent GTID position update, causing the
        slave to fail with error.
      */
      wait_for_pending_deadlock_kill(thd, thd->rgi_slave);
      thd->reset_killed();
    }

    if ((err= do_record_gtid(thd, rgi, false, &hton, true)))
      return err;
  }

  if (sub_id && (!res || wsrep_must_replay(thd)))
    rpl_global_gtid_slave_state->update_state_hash(sub_id, &gtid, hton, rgi);
  /*
    Increment the global status commit count variable
  */
  enum enum_sql_command cmd= !thd->transaction->xid_state.is_explicit_XA()
    ? SQLCOM_COMMIT : SQLCOM_XA_PREPARE;
  status_var_increment(thd->status_var.com_stat[cmd]);

  return res;
}

Log_event::enum_skip_reason
Xid_apply_log_event::do_shall_skip(rpl_group_info *rgi)
{
  DBUG_ENTER("Xid_apply_log_event::do_shall_skip");
  if (rgi->rli->slave_skip_counter > 0)
  {
    DBUG_ASSERT(!rgi->rli->get_flag(Relay_log_info::IN_TRANSACTION));
    thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_GTID_BEGIN);
    DBUG_RETURN(Log_event::EVENT_SKIP_COUNT);
  }
#ifdef WITH_WSREP
  else if (wsrep_mysql_replication_bundle && WSREP(thd) &&
           opt_slave_domain_parallel_threads == 0)
  {
    if (++thd->wsrep_mysql_replicated < (int)wsrep_mysql_replication_bundle)
    {
      WSREP_DEBUG("skipping wsrep commit %d", thd->wsrep_mysql_replicated);
      DBUG_RETURN(Log_event::EVENT_SKIP_IGNORE);
    }
    else
    {
      thd->wsrep_mysql_replicated = 0;
    }
  }
#endif
  DBUG_RETURN(Log_event::do_shall_skip(rgi));
}
#endif /* HAVE_REPLICATION */

/**************************************************************************
  Xid_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
void Xid_log_event::pack_info(Protocol *protocol)
{
  char buf[128], *pos;
  pos= strmov(buf, "COMMIT /* xid=");
  pos= longlong10_to_str(xid, pos, 10);
  pos= strmov(pos, " */");
  protocol->store(buf, (uint) (pos-buf), &my_charset_bin);
}


int Xid_log_event::do_commit()
{
  bool res;
  res= trans_commit(thd); /* Automatically rolls back on error. */
  thd->release_transactional_locks();
  return res;
}
#endif


bool Xid_log_event::write(Log_event_writer *writer)
{
  DBUG_EXECUTE_IF("do_not_write_xid", return 0;);
  return write_header(writer, sizeof(xid)) ||
         write_data(writer, (uchar*)&xid, sizeof(xid)) ||
         write_footer(writer);
}

/**************************************************************************
  XA_prepare_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
void XA_prepare_log_event::pack_info(Protocol *protocol)
{
  char query[sizeof("XA COMMIT ONE PHASE") + 1 + ser_buf_size];

  sprintf(query,
          (one_phase ? "XA COMMIT %s ONE PHASE" :  "XA PREPARE %s"),
          m_xid.serialize());

  protocol->store(query, strlen(query), &my_charset_bin);
}


int XA_prepare_log_event::do_commit()
{
  int res;
  xid_t xid;
  xid.set(m_xid.formatID,
          m_xid.data, m_xid.gtrid_length,
          m_xid.data + m_xid.gtrid_length, m_xid.bqual_length);

  thd->lex->xid= &xid;
  if (!one_phase)
  {
    if (thd->is_current_stmt_binlog_disabled() &&
        (res= thd->wait_for_prior_commit()))
      return res;

    thd->lex->sql_command= SQLCOM_XA_PREPARE;
    res= trans_xa_prepare(thd);
  }
  else
    res= trans_xa_commit(thd);

  return res;
}
#endif // HAVE_REPLICATION


bool XA_prepare_log_event::write(Log_event_writer *writer)
{
  uchar data[1 + 4 + 4 + 4]= {one_phase,};
  uint8 one_phase_byte= one_phase;

  int4store(data+1, static_cast<XID*>(xid)->formatID);
  int4store(data+(1+4), static_cast<XID*>(xid)->gtrid_length);
  int4store(data+(1+4+4), static_cast<XID*>(xid)->bqual_length);

  DBUG_ASSERT(xid_subheader_no_data == sizeof(data) - 1);

  return write_header(writer, sizeof(one_phase_byte) + xid_subheader_no_data +
                      static_cast<XID*>(xid)->gtrid_length +
                      static_cast<XID*>(xid)->bqual_length) ||
         write_data(writer, data, sizeof(data)) ||
         write_data(writer, (uchar*) static_cast<XID*>(xid)->data,
                     static_cast<XID*>(xid)->gtrid_length +
                     static_cast<XID*>(xid)->bqual_length) ||
         write_footer(writer);
}


/**************************************************************************
  User_var_log_event methods
**************************************************************************/

#if defined(HAVE_REPLICATION)
static bool
user_var_append_name_part(THD *thd, String *buf,
                          const char *name, size_t name_len,
                          const LEX_CSTRING &data_type_name)
{
  return buf->append('@') ||
    append_identifier(thd, buf, name, name_len) ||
    buf->append('=') ||
    (data_type_name.length &&
     (buf->append(STRING_WITH_LEN("/*")) ||
      buf->append(data_type_name.str, data_type_name.length) ||
      buf->append(STRING_WITH_LEN("*/"))));
}

void User_var_log_event::pack_info(Protocol* protocol)
{
  if (is_null)
  {
    char buf_mem[FN_REFLEN+7];
    String buf(buf_mem, sizeof(buf_mem), system_charset_info);
    buf.length(0);
    if (user_var_append_name_part(protocol->thd, &buf, name, name_len,
                                  m_data_type_name) ||
        buf.append(NULL_clex_str))
      return;
    protocol->store(buf.ptr(), buf.length(), &my_charset_bin);
  }
  else
  {
    switch (m_type) {
    case REAL_RESULT:
    {
      double real_val;
      char buf2[MY_GCVT_MAX_FIELD_WIDTH+1];
      char buf_mem[FN_REFLEN + MY_GCVT_MAX_FIELD_WIDTH + 1];
      String buf(buf_mem, sizeof(buf_mem), system_charset_info);
      float8get(real_val, val);
      buf.length(0);
      if (user_var_append_name_part(protocol->thd, &buf, name, name_len,
                                    m_data_type_name) ||
          buf.append(buf2, my_gcvt(real_val, MY_GCVT_ARG_DOUBLE,
                                   MY_GCVT_MAX_FIELD_WIDTH, buf2, NULL)))
        return;
      protocol->store(buf.ptr(), buf.length(), &my_charset_bin);
      break;
    }
    case INT_RESULT:
    {
      char buf2[22];
      char buf_mem[FN_REFLEN + 22];
      String buf(buf_mem, sizeof(buf_mem), system_charset_info);
      buf.length(0);
      if (user_var_append_name_part(protocol->thd, &buf, name, name_len,
                                    m_data_type_name) ||
          buf.append(buf2,
                 longlong10_to_str(uint8korr(val), buf2,
                   (is_unsigned() ? 10 : -10))-buf2))
        return;
      protocol->store(buf.ptr(), buf.length(), &my_charset_bin);
      break;
    }
    case DECIMAL_RESULT:
    {
      char buf_mem[FN_REFLEN + DECIMAL_MAX_STR_LENGTH];
      String buf(buf_mem, sizeof(buf_mem), system_charset_info);
      char buf2[DECIMAL_MAX_STR_LENGTH+1];
      String str(buf2, sizeof(buf2), &my_charset_bin);
      buf.length(0);
      my_decimal((const uchar *) (val + 2), val[0], val[1]).to_string(&str);
      if (user_var_append_name_part(protocol->thd, &buf, name, name_len,
                                    m_data_type_name) ||
          buf.append(str))
        return;
      protocol->store(buf.ptr(), buf.length(), &my_charset_bin);

      break;
    }
    case STRING_RESULT:
    {
      /* 15 is for 'COLLATE' and other chars */
      char buf_mem[FN_REFLEN + 512 + 1 + 15 +
                   MY_CS_CHARACTER_SET_NAME_SIZE +
                   MY_CS_COLLATION_NAME_SIZE];
      String buf(buf_mem, sizeof(buf_mem), system_charset_info);
      CHARSET_INFO *cs;
      buf.length(0);
      if (!(cs= get_charset(m_charset_number, MYF(0))))
      {
        if (buf.append(STRING_WITH_LEN("???")))
          return;
      }
      else
      {
        size_t old_len;
        char *beg, *end;
        if (user_var_append_name_part(protocol->thd, &buf, name, name_len,
                                      m_data_type_name) ||
            buf.append('_') ||
            buf.append(cs->cs_name) ||
            buf.append(' '))
          return;
        old_len= buf.length();
        if (buf.reserve(old_len + val_len * 2 + 3 + sizeof(" COLLATE ") +
                        MY_CS_COLLATION_NAME_SIZE))
          return;
        beg= const_cast<char *>(buf.ptr()) + old_len;
        end= str_to_hex(beg, (uchar*)val, val_len);
        buf.length(old_len + (end - beg));
        if (buf.append(STRING_WITH_LEN(" COLLATE ")) ||
            buf.append(cs->coll_name))
          return;
      }
      protocol->store(buf.ptr(), buf.length(), &my_charset_bin);
      break;
    }
    case ROW_RESULT:
    default:
      DBUG_ASSERT(0);
      return;
    }
  }
}
#endif // HAVE_REPLICATION


bool User_var_log_event::write(Log_event_writer *writer)
{
  char buf[UV_NAME_LEN_SIZE];
  char buf1[UV_VAL_IS_NULL + UV_VAL_TYPE_SIZE + 
	    UV_CHARSET_NUMBER_SIZE + UV_VAL_LEN_SIZE];
  uchar buf2[MY_MAX(8, DECIMAL_MAX_FIELD_SIZE + 2)], *pos= buf2;
  uint unsigned_len= 0;
  uint buf1_length;
  size_t event_length;

  int4store(buf, name_len);
  
  if ((buf1[0]= is_null))
  {
    buf1_length= 1;
    val_len= 0;                                 // Length of 'pos'
  }    
  else
  {
    buf1[1]= m_type;
    int4store(buf1 + 2, m_charset_number);

    switch (m_type) {
    case REAL_RESULT:
      float8store(buf2, *(double*) val);
      break;
    case INT_RESULT:
      int8store(buf2, *(longlong*) val);
      unsigned_len= 1;
      break;
    case DECIMAL_RESULT:
    {
      my_decimal *dec= (my_decimal *)val;
      dec->fix_buffer_pointer();
      buf2[0]= (char)(dec->intg + dec->frac);
      buf2[1]= (char)dec->frac;
      decimal2bin((decimal_t*)val, buf2+2, buf2[0], buf2[1]);
      val_len= decimal_bin_size(buf2[0], buf2[1]) + 2;
      break;
    }
    case STRING_RESULT:
      pos= (uchar*) val;
      break;
    case ROW_RESULT:
    default:
      DBUG_ASSERT_NO_ASSUME(0);
      return 0;
    }
    int4store(buf1 + 2 + UV_CHARSET_NUMBER_SIZE, val_len);
    buf1_length= 10;
  }

  uchar data_type_name_chunk_signature= (uchar) CHUNK_DATA_TYPE_NAME;
  uint data_type_name_chunk_signature_length= m_data_type_name.length ? 1 : 0;
  uchar data_type_name_length_length= m_data_type_name.length ? 1 : 0;

  /* Length of the whole event */
  event_length= sizeof(buf)+ name_len + buf1_length + val_len + unsigned_len +
                data_type_name_chunk_signature_length +
                data_type_name_length_length +
                (uint) m_data_type_name.length;

  uchar unsig= m_is_unsigned ? CHUNK_UNSIGNED : CHUNK_SIGNED;
  uchar data_type_name_length= (uchar) m_data_type_name.length;
  return write_header(writer, event_length) ||
         write_data(writer, buf, sizeof(buf))   ||
         write_data(writer, name, name_len)     ||
         write_data(writer, buf1, buf1_length) ||
         write_data(writer, pos, val_len) ||
         write_data(writer, &unsig, unsigned_len) ||
         write_data(writer, &data_type_name_chunk_signature,
                    data_type_name_chunk_signature_length) ||
         write_data(writer, &data_type_name_length,
                    data_type_name_length_length) ||
         write_data(writer, m_data_type_name.str,
                    (uint) m_data_type_name.length) ||
         write_footer(writer);
}


#if defined(HAVE_REPLICATION)
int User_var_log_event::do_apply_event(rpl_group_info *rgi)
{
  Item *it= 0;
  CHARSET_INFO *charset;
  DBUG_ENTER("User_var_log_event::do_apply_event");
  query_id_t sav_query_id= 0; /* memorize orig id when deferred applying */

  if (rgi->deferred_events_collecting)
  {
    set_deferred(current_thd->query_id);
    DBUG_RETURN(rgi->deferred_events->add(this));
  }
  else if (is_deferred())
  {
    sav_query_id= current_thd->query_id;
    current_thd->query_id= query_id; /* recreating original time context */
  }

  if (!(charset= get_charset(m_charset_number, MYF(MY_WME))))
  {
    rgi->rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                "Invalid character set for User var event");
    DBUG_RETURN(1);
  }
  LEX_CSTRING user_var_name;
  user_var_name.str= name;
  user_var_name.length= name_len;
  double real_val;
  longlong int_val;

  if (is_null)
  {
    it= new (thd->mem_root) Item_null(thd);
  }
  else
  {
    switch (m_type) {
    case REAL_RESULT:
      if (val_len != 8)
      {
        rgi->rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                    ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                    "Invalid variable length at User var event");
        return 1;
      }
      float8get(real_val, val);
      it= new (thd->mem_root) Item_float(thd, real_val, 0);
      val= (char*) &real_val;		// Pointer to value in native format
      val_len= 8;
      break;
    case INT_RESULT:
      if (val_len != 8)
      {
        rgi->rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                    ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                    "Invalid variable length at User var event");
        return 1;
      }
      int_val= (longlong) uint8korr(val);
      it= new (thd->mem_root) Item_int(thd, int_val);
      val= (char*) &int_val;		// Pointer to value in native format
      val_len= 8;
      break;
    case DECIMAL_RESULT:
    {
      if (val_len < 3)
      {
        rgi->rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR,
                    ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                    "Invalid variable length at User var event");
        return 1;
      }
      Item_decimal *dec= new (thd->mem_root) Item_decimal(thd, (uchar*) val+2, val[0], val[1]);
      it= dec;
      val= (char *)dec->val_decimal(NULL);
      val_len= sizeof(my_decimal);
      break;
    }
    case STRING_RESULT:
      it= new (thd->mem_root) Item_string(thd, val, (uint)val_len, charset);
      break;
    case ROW_RESULT:
    default:
      DBUG_ASSERT_NO_ASSUME(0);
      DBUG_RETURN(0);
    }
  }

  Item_func_set_user_var *e= new (thd->mem_root) Item_func_set_user_var(thd, &user_var_name, it);
  /*
    Item_func_set_user_var can't substitute something else on its place =>
    0 can be passed as last argument (reference on item)

    Fix_fields() can fail, in which case a call of update_hash() might
    crash the server, so if fix fields fails, we just return with an
    error.
  */
  if (e->fix_fields(thd, 0))
    DBUG_RETURN(1);

  const Type_handler *th= Type_handler::handler_by_log_event_data_type(thd,
                                                                       *this);
  e->update_hash((void*) val, val_len, th, charset);

  if (!is_deferred())
    free_root(thd->mem_root, 0);
  else
    current_thd->query_id= sav_query_id; /* restore current query's context */

  DBUG_RETURN(0);
}

int User_var_log_event::do_update_pos(rpl_group_info *rgi)
{
  rgi->inc_event_relay_log_pos();
  return 0;
}

Log_event::enum_skip_reason
User_var_log_event::do_shall_skip(rpl_group_info *rgi)
{
  /*
    It is a common error to set the slave skip counter to 1 instead
    of 2 when recovering from an insert which used a auto increment,
    rand, or user var.  Therefore, if the slave skip counter is 1, we
    just say that this event should be skipped by ignoring it, meaning
    that we do not change the value of the slave skip counter since it
    will be decreased by the following insert event.
  */
  return continue_group(rgi);
}
#endif // HAVE_REPLICATION


#ifdef HAVE_REPLICATION

/**************************************************************************
	Stop_log_event methods
**************************************************************************/

/*
  The master stopped.  We used to clean up all temporary tables but
  this is useless as, as the master has shut down properly, it has
  written all DROP TEMPORARY TABLE (prepared statements' deletion is
  TODO only when we binlog prep stmts).  We used to clean up
  slave_load_tmpdir, but this is useless as it has been cleared at the
  end of LOAD DATA INFILE.  So we have nothing to do here.  The place were we
  must do this cleaning is in Format_description_log_event::do_apply_event(),
  not here. Because if we come here, the master was sane.

  This must only be called from the Slave SQL thread, since it calls
  Relay_log_info::flush().
*/

int Stop_log_event::do_update_pos(rpl_group_info *rgi)
{
  int error= 0;
  Relay_log_info *rli= rgi->rli;
  DBUG_ENTER("Stop_log_event::do_update_pos");
  /*
    We do not want to update master_log pos because we get a rotate event
    before stop, so by now group_master_log_name is set to the next log.
    If we updated it, we will have incorrect master coordinates and this
    could give false triggers in MASTER_POS_WAIT() that we have reached
    the target position when in fact we have not.
  */
  if (rli->get_flag(Relay_log_info::IN_TRANSACTION))
    rgi->inc_event_relay_log_pos();
  else if (!rgi->is_parallel_exec)
  {
    rpl_global_gtid_slave_state->record_and_update_gtid(thd, rgi);
    rli->inc_group_relay_log_pos(0, rgi);
    if (rli->flush())
      error= 1;
  }
  DBUG_RETURN(error);
}

#endif /* HAVE_REPLICATION */


/**************************************************************************
	Append_block_log_event methods
**************************************************************************/

Append_block_log_event::Append_block_log_event(THD *thd_arg,
                                               const char *db_arg,
					       uchar *block_arg,
					       uint block_len_arg,
					       bool using_trans)
  :Log_event(thd_arg,0, using_trans), block(block_arg),
   block_len(block_len_arg), file_id(thd_arg->file_id), db(db_arg)
{
}


bool Append_block_log_event::write(Log_event_writer *writer)
{
  uchar buf[APPEND_BLOCK_HEADER_LEN];
  int4store(buf + AB_FILE_ID_OFFSET, file_id);
  return write_header(writer, APPEND_BLOCK_HEADER_LEN + block_len) ||
         write_data(writer, buf, APPEND_BLOCK_HEADER_LEN) ||
         write_data(writer, block, block_len) ||
         write_footer(writer);
}


#if defined(HAVE_REPLICATION)
void Append_block_log_event::pack_info(Protocol *protocol)
{
  char buf[256];
  uint length;
  length= (uint) sprintf(buf, ";file_id=%u;block_len=%u", file_id, block_len);
  protocol->store(buf, length, &my_charset_bin);
}


/*
  Append_block_log_event::get_create_or_append()
*/

int Append_block_log_event::get_create_or_append() const
{
  return 0; /* append to the file, fail if not exists */
}

/*
  Append_block_log_event::do_apply_event()
*/

int Append_block_log_event::do_apply_event(rpl_group_info *rgi)
{
  char fname[FN_REFLEN];
  int fd;
  int error = 1;
  Relay_log_info const *rli= rgi->rli;
  DBUG_ENTER("Append_block_log_event::do_apply_event");

  THD_STAGE_INFO(thd, stage_making_temp_file_append_before_load_data);
  slave_load_file_stem(fname, file_id, server_id, ".data",
                       &rli->mi->cmp_connection_name);
  if (get_create_or_append())
  {
    /*
      Usually lex_start() is called by mysql_parse(), but we need it here
      as the present method does not call mysql_parse().
    */
    lex_start(thd);
    thd->reset_for_next_command();
    /* old copy may exist already */
    mysql_file_delete(key_file_log_event_data, fname, MYF(0));
    if ((fd= mysql_file_create(key_file_log_event_data,
                               fname, CREATE_MODE,
                               O_WRONLY | O_BINARY | O_EXCL | O_NOFOLLOW,
                               MYF(MY_WME))) < 0)
    {
      rli->report(ERROR_LEVEL, my_errno, rgi->gtid_info(),
                  "Error in %s event: could not create file '%s'",
                  get_type_str(), fname);
      goto err;
    }
  }
  else if ((fd= mysql_file_open(key_file_log_event_data,
                                fname,
                                O_WRONLY | O_APPEND | O_BINARY | O_NOFOLLOW,
                                MYF(MY_WME))) < 0)
  {
    rli->report(ERROR_LEVEL, my_errno, rgi->gtid_info(),
                "Error in %s event: could not open file '%s'",
                get_type_str(), fname);
    goto err;
  }

  DBUG_EXECUTE_IF("remove_slave_load_file_before_write",
                  {
                    my_delete(fname, MYF(0));
                  });

  if (mysql_file_write(fd, (uchar*) block, block_len, MYF(MY_WME+MY_NABP)))
  {
    rli->report(ERROR_LEVEL, my_errno, rgi->gtid_info(),
                "Error in %s event: write to '%s' failed",
                get_type_str(), fname);
    goto err;
  }
  error=0;

err:
  if (fd >= 0)
    mysql_file_close(fd, MYF(0));
  DBUG_RETURN(error);
}
#endif // HAVE_REPLICATION


/**************************************************************************
	Delete_file_log_event methods
**************************************************************************/

Delete_file_log_event::Delete_file_log_event(THD *thd_arg, const char* db_arg,
					     bool using_trans)
  :Log_event(thd_arg, 0, using_trans), file_id(thd_arg->file_id), db(db_arg)
{
}


bool Delete_file_log_event::write(Log_event_writer *writer)
{
 uchar buf[DELETE_FILE_HEADER_LEN];
 int4store(buf + DF_FILE_ID_OFFSET, file_id);
 return write_header(writer, sizeof(buf)) ||
        write_data(writer, buf, sizeof(buf)) ||
        write_footer(writer);
}


#if defined(HAVE_REPLICATION)
void Delete_file_log_event::pack_info(Protocol *protocol)
{
  char buf[64];
  uint length;
  length= (uint) sprintf(buf, ";file_id=%u", (uint) file_id);
  protocol->store(buf, (int32) length, &my_charset_bin);
}
#endif


#if defined(HAVE_REPLICATION)
int Delete_file_log_event::do_apply_event(rpl_group_info *rgi)
{
  char fname[FN_REFLEN+10];
  Relay_log_info const *rli= rgi->rli;
  char *ext= slave_load_file_stem(fname, file_id, server_id, ".data",
                                  &rli->mi->cmp_connection_name);
  mysql_file_delete(key_file_log_event_data, fname, MYF(MY_WME));
  strmov(ext, ".info");
  mysql_file_delete(key_file_log_event_info, fname, MYF(MY_WME));
  return 0;
}
#endif /* defined(HAVE_REPLICATION) */


/**************************************************************************
	Begin_load_query_log_event methods
**************************************************************************/

Begin_load_query_log_event::
Begin_load_query_log_event(THD* thd_arg, const char* db_arg, uchar* block_arg,
                           uint block_len_arg, bool using_trans)
  :Append_block_log_event(thd_arg, db_arg, block_arg, block_len_arg,
                          using_trans)
{
   file_id= thd_arg->file_id= mysql_bin_log.next_file_id();
}


#if defined( HAVE_REPLICATION)
int Begin_load_query_log_event::get_create_or_append() const
{
  return 1; /* create the file */
}


Log_event::enum_skip_reason
Begin_load_query_log_event::do_shall_skip(rpl_group_info *rgi)
{
  /*
    If the slave skip counter is 1, then we should not start executing
    on the next event.
  */
  return continue_group(rgi);
}
#endif /* defined( HAVE_REPLICATION) */


/**************************************************************************
	Execute_load_query_log_event methods
**************************************************************************/

Execute_load_query_log_event::
Execute_load_query_log_event(THD *thd_arg, const char* query_arg,
                             ulong query_length_arg, uint fn_pos_start_arg,
                             uint fn_pos_end_arg,
                             enum_load_dup_handling dup_handling_arg,
                             bool using_trans, bool direct, bool suppress_use,
                             int errcode):
  Query_log_event(thd_arg, query_arg, query_length_arg, using_trans, direct,
                  suppress_use, errcode),
  file_id(thd_arg->file_id), fn_pos_start(fn_pos_start_arg),
  fn_pos_end(fn_pos_end_arg), dup_handling(dup_handling_arg)
{
}


bool
Execute_load_query_log_event::write_post_header_for_derived(Log_event_writer *writer)
{
  uchar buf[EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN];
  int4store(buf, file_id);
  int4store(buf + 4, fn_pos_start);
  int4store(buf + 4 + 4, fn_pos_end);
  *(buf + 4 + 4 + 4)= (uchar) dup_handling;
  return write_data(writer, buf, EXECUTE_LOAD_QUERY_EXTRA_HEADER_LEN);
}


#if defined(HAVE_REPLICATION)
void Execute_load_query_log_event::pack_info(Protocol *protocol)
{
  char buf_mem[1024];
  String buf(buf_mem, sizeof(buf_mem), system_charset_info);
  buf.real_alloc(9 + db_len + q_len + 10 + 21);
  if (db && db_len)
  {
    if (buf.append(STRING_WITH_LEN("use ")) ||
        append_identifier(protocol->thd, &buf, db, db_len) ||
        buf.append(STRING_WITH_LEN("; ")))
      return;
  }
  if (query && q_len && buf.append(query, q_len))
    return;
  if (buf.append(STRING_WITH_LEN(" ;file_id=")) ||
      buf.append_ulonglong(file_id))
    return;
  protocol->store(buf.ptr(), buf.length(), &my_charset_bin);
}


int
Execute_load_query_log_event::do_apply_event(rpl_group_info *rgi)
{
  char *p;
  char *buf;
  char *fname;
  char *fname_end;
  int error;
  Relay_log_info const *rli= rgi->rli;

  buf= (char*) my_malloc(PSI_INSTRUMENT_ME, q_len + 1 -
     (fn_pos_end - fn_pos_start) + (FN_REFLEN + 10) + 10 + 8 + 5, MYF(MY_WME));

  DBUG_EXECUTE_IF("LOAD_DATA_INFILE_has_fatal_error", my_free(buf); buf= NULL;);

  /* Replace filename and LOCAL keyword in query before executing it */
  if (buf == NULL)
  {
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, rgi->gtid_info(),
                ER_THD(rgi->thd, ER_SLAVE_FATAL_ERROR), "Not enough memory");
    return 1;
  }

  p= buf;
  memcpy(p, query, fn_pos_start);
  p+= fn_pos_start;
  fname= (p= strmake(p, STRING_WITH_LEN(" INFILE \'")));
  p= slave_load_file_stem(p, file_id, server_id, ".data",
                          &rli->mi->cmp_connection_name);
  fname_end= p= strend(p);                      // Safer than p=p+5
  *(p++)='\'';
  switch (dup_handling) {
  case LOAD_DUP_IGNORE:
    p= strmake(p, STRING_WITH_LEN(" IGNORE"));
    break;
  case LOAD_DUP_REPLACE:
    p= strmake(p, STRING_WITH_LEN(" REPLACE"));
    break;
  default:
    /* Ordinary load data */
    break;
  }
  p= strmake(p, STRING_WITH_LEN(" INTO "));
  p= strmake(p, query+fn_pos_end, q_len-fn_pos_end);

  error= Query_log_event::do_apply_event(rgi, buf, (uint32)(p-buf));

  /* Forging file name for deletion in same buffer */
  *fname_end= 0;

  /*
    If there was an error the slave is going to stop, leave the
    file so that we can re-execute this event at START SLAVE.
  */
  if (unlikely(!error))
    mysql_file_delete(key_file_log_event_data, fname, MYF(MY_WME));

  my_free(buf);
  return error;
}
#endif // HAVE_REPLICATION


/**************************************************************************
	sql_ex_info methods
**************************************************************************/

static bool write_str(Log_event_writer *writer, const char *str, uint length)
{
  uchar tmp[1];
  tmp[0]= (uchar) length;
  return (writer->write_data(tmp, sizeof(tmp)) ||
	  writer->write_data((uchar*) str, length));
}

bool sql_ex_info::write_data(Log_event_writer *writer)
{
  if (new_format())
  {
    return write_str(writer, field_term, field_term_len) ||
	   write_str(writer, enclosed,   enclosed_len) ||
	   write_str(writer, line_term,  line_term_len) ||
	   write_str(writer, line_start, line_start_len) ||
	   write_str(writer, escaped,    escaped_len) ||
	   writer->write_data((uchar*) &opt_flags, 1);
  }
  else
  {
    uchar old_ex[7];
    old_ex[0]= *field_term;
    old_ex[1]= *enclosed;
    old_ex[2]= *line_term;
    old_ex[3]= *line_start;
    old_ex[4]= *escaped;
    old_ex[5]=  opt_flags;
    old_ex[6]=  empty_flags;
    return writer->write_data(old_ex, sizeof(old_ex));
  }
}



/**************************************************************************
	Rows_log_event member functions
**************************************************************************/

Rows_log_event::Rows_log_event(THD *thd_arg, TABLE *tbl_arg,
                               ulonglong table_id,
                               MY_BITMAP const *cols, bool is_transactional,
                               Log_event_type event_type)
  : Log_event(thd_arg, 0, is_transactional),
    m_row_count(0),
    m_table(tbl_arg),
    m_table_id(table_id),
    m_width(tbl_arg ? tbl_arg->s->fields : 1),
    m_rows_buf(0), m_rows_cur(0), m_rows_end(0), m_flags(0),
    m_type(event_type), m_extra_row_data(0)
#ifdef HAVE_REPLICATION
    , m_curr_row(NULL), m_curr_row_end(NULL),
    m_key(NULL), m_key_info(NULL), m_key_nr(0),
    master_had_triggers(0)
#endif
{
  /*
    We allow a special form of dummy event when the table, and cols
    are null and the table id is UINT32_MAX.  This is a temporary
    solution, to be able to terminate a started statement in the
    binary log: the extraneous events will be removed in the future.
   */
  DBUG_ASSERT((tbl_arg && tbl_arg->s &&
               (table_id & MAX_TABLE_MAP_ID) != UINT32_MAX) ||
              (!tbl_arg && !cols && (table_id & MAX_TABLE_MAP_ID) == UINT32_MAX));

  if (thd_arg->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS)
    set_flags(NO_FOREIGN_KEY_CHECKS_F);
  if (thd_arg->variables.option_bits & OPTION_RELAXED_UNIQUE_CHECKS)
    set_flags(RELAXED_UNIQUE_CHECKS_F);
  if (thd_arg->variables.option_bits & OPTION_NO_CHECK_CONSTRAINT_CHECKS)
    set_flags(NO_CHECK_CONSTRAINT_CHECKS_F);
  /* if my_bitmap_init fails, caught in is_valid() */
  if (likely(!my_bitmap_init(&m_cols,
                             m_width <= sizeof(m_bitbuf)*8 ? m_bitbuf : NULL,
                             m_width)))
  {
    /* Cols can be zero if this is a dummy binrows event */
    if (likely(cols != NULL))
      bitmap_copy(&m_cols, cols);
  }
}


int Rows_log_event::do_add_row_data(uchar *row_data, size_t length)
{
  /*
    When the table has a primary key, we would probably want, by default, to
    log only the primary key value instead of the entire "before image". This
    would save binlog space. TODO
  */
  DBUG_ENTER("Rows_log_event::do_add_row_data");
  DBUG_PRINT("enter", ("row_data:%p  length: %lu", row_data,
                       (ulong) length));

  /*
    If length is zero, there is nothing to write, so we just
    return. Note that this is not an optimization, since calling
    realloc() with size 0 means free().
   */
  if (length == 0)
  {
    m_row_count++;
    DBUG_RETURN(0);
  }

  /*
    Don't print debug messages when running valgrind since they can
    trigger false warnings.
   */
#ifndef HAVE_valgrind
  DBUG_DUMP("row_data", row_data, MY_MIN(length, 32));
#endif

  DBUG_ASSERT(m_rows_buf <= m_rows_cur);
  DBUG_ASSERT(!m_rows_buf || (m_rows_end && m_rows_buf < m_rows_end));
  DBUG_ASSERT(m_rows_cur <= m_rows_end);

  /* The cast will always work since m_rows_cur <= m_rows_end */
  if (static_cast<size_t>(m_rows_end - m_rows_cur) <= length)
  {
    size_t const block_size= 1024;
    size_t cur_size= m_rows_cur - m_rows_buf;
    DBUG_EXECUTE_IF("simulate_too_big_row_case1",
                     cur_size= UINT_MAX32 - (block_size * 10);
                     length= UINT_MAX32 - (block_size * 10););
    DBUG_EXECUTE_IF("simulate_too_big_row_case2",
                     cur_size= UINT_MAX32 - (block_size * 10);
                     length= block_size * 10;);
    DBUG_EXECUTE_IF("simulate_too_big_row_case3",
                     cur_size= block_size * 10;
                     length= UINT_MAX32 - (block_size * 10););
    DBUG_EXECUTE_IF("simulate_too_big_row_case4",
                     cur_size= UINT_MAX32 - (block_size * 10);
                     length= (block_size * 10) - block_size + 1;);
    size_t remaining_space= UINT_MAX32 - cur_size;
    /* Check that the new data fits within remaining space and we can add
       block_size without wrapping.
     */
    if (cur_size > UINT_MAX32 || length > remaining_space ||
        ((length + block_size) > remaining_space))
    {
      sql_print_error("The row data is greater than 4GB, which is too big to "
                      "write to the binary log.");
      DBUG_RETURN(ER_BINLOG_ROW_LOGGING_FAILED);
    }
    size_t const new_alloc= 
        block_size * ((cur_size + length + block_size - 1) / block_size);

    uchar* const new_buf= (uchar*)my_realloc(PSI_INSTRUMENT_ME, m_rows_buf,
                                    new_alloc, MYF(MY_ALLOW_ZERO_PTR|MY_WME));
    if (unlikely(!new_buf))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    /* If the memory moved, we need to move the pointers */
    if (new_buf != m_rows_buf)
    {
      m_rows_buf= new_buf;
      m_rows_cur= m_rows_buf + cur_size;
    }

    /*
       The end pointer should always be changed to point to the end of
       the allocated memory.
    */
    m_rows_end= m_rows_buf + new_alloc;
  }

  DBUG_ASSERT(m_rows_cur + length <= m_rows_end);
  memcpy(m_rows_cur, row_data, length);
  m_rows_cur+= length;
  m_row_count++;
  DBUG_RETURN(0);
}


#if defined(HAVE_REPLICATION)

/**
  Restores empty table list as it was before trigger processing.

  @note We have a lot of ASSERTS that check the lists when we close tables.
  There was the same problem with MERGE MYISAM tables and so here we try to
  go the same way.
*/
inline void restore_empty_query_table_list(LEX *lex)
{
  if (lex->first_not_own_table())
      (*lex->first_not_own_table()->prev_global)= NULL;
  lex->query_tables= NULL;
  lex->query_tables_last= &lex->query_tables;
}


int Rows_log_event::do_apply_event(rpl_group_info *rgi)
{
  DBUG_ASSERT(rgi);
  Relay_log_info const *rli= rgi->rli;
  TABLE* table;
  DBUG_ENTER("Rows_log_event::do_apply_event(Relay_log_info*)");
  int error= 0;
  LEX *lex= thd->lex;
  uint8 new_trg_event_map= get_trg_event_map();
  /*
    If m_table_id == UINT32_MAX, then we have a dummy event that does not
    contain any data.  In that case, we just remove all tables in the
    tables_to_lock list, close the thread tables, and return with
    success.
   */
  if (m_table_id == UINT32_MAX)
  {
    /*
       This one is supposed to be set: just an extra check so that
       nothing strange has happened.
     */
    DBUG_ASSERT(get_flags(STMT_END_F));

    rgi->slave_close_thread_tables(thd);
    thd->clear_error();
    DBUG_RETURN(0);
  }

  /*
    'thd' has been set by exec_relay_log_event(), just before calling
    do_apply_event(). We still check here to prevent future coding
    errors.
  */
  DBUG_ASSERT(rgi->thd == thd);

  /*
    Where a Query_log_event can rely on the normal command execution logic to
    set/reset the slave thread's timer; a Rows_log_event update needs to set
    the timer itself
  */
  thd->set_query_timer_if_needed();

  /*
    If there are no tables open, this must be the first row event seen
    after the table map events. We should then open and lock all tables
    used in the transaction and proceed with execution of the actual event.
  */
  if (!thd->open_tables)
  {
    /*
      Lock_tables() reads the contents of thd->lex, so they must be
      initialized.

      We also call the THD::reset_for_next_command(), since this
      is the logical start of the next "statement". Note that this
      call might reset the value of current_stmt_binlog_format, so
      we need to do any changes to that value after this function.
    */
    delete_explain_query(thd->lex);
    lex_start(thd);
    thd->reset_for_next_command();
    /*
      The current statement is just about to begin and 
      has not yet modified anything. Note, all.modified is reset
      by THD::reset_for_next_command().
    */
    thd->transaction->stmt.modified_non_trans_table= FALSE;
    thd->transaction->stmt.m_unsafe_rollback_flags&= ~THD_TRANS::DID_WAIT;
    /*
      This is a row injection, so we flag the "statement" as
      such. Note that this code is called both when the slave does row
      injections and when the BINLOG statement is used to do row
      injections.
    */
    thd->lex->set_stmt_row_injection();

    /*
      There are a few flags that are replicated with each row event.
      Make sure to set/clear them before executing the main body of
      the event.
    */
    if (get_flags(NO_FOREIGN_KEY_CHECKS_F))
        thd->variables.option_bits|= OPTION_NO_FOREIGN_KEY_CHECKS;
    else
        thd->variables.option_bits&= ~OPTION_NO_FOREIGN_KEY_CHECKS;

    if (get_flags(RELAXED_UNIQUE_CHECKS_F))
        thd->variables.option_bits|= OPTION_RELAXED_UNIQUE_CHECKS;
    else
        thd->variables.option_bits&= ~OPTION_RELAXED_UNIQUE_CHECKS;

    if (get_flags(NO_CHECK_CONSTRAINT_CHECKS_F))
      thd->variables.option_bits|= OPTION_NO_CHECK_CONSTRAINT_CHECKS;
    else
      thd->variables.option_bits&= ~OPTION_NO_CHECK_CONSTRAINT_CHECKS;

    /* A small test to verify that objects have consistent types */
    DBUG_ASSERT(sizeof(thd->variables.option_bits) == sizeof(OPTION_RELAXED_UNIQUE_CHECKS));

    DBUG_EXECUTE_IF("rows_log_event_before_open_table",
                    {
                      const char action[] = "now SIGNAL before_open_table WAIT_FOR go_ahead_sql";
                      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(action)));
                    };);


    /*
      Trigger's procedures work with global table list. So we have to add
      rgi->tables_to_lock content there to get trigger's in the list.

      Then restore_empty_query_table_list() restore the list as it was
    */
    DBUG_ASSERT(lex->query_tables == NULL);
    if ((lex->query_tables= rgi->tables_to_lock))
      rgi->tables_to_lock->prev_global= &lex->query_tables;

    for (TABLE_LIST *tables= rgi->tables_to_lock; tables;
         tables= tables->next_global)
    {
      if (slave_run_triggers_for_rbr)
      {
        tables->trg_event_map= new_trg_event_map;
        lex->query_tables_last= &tables->next_global;
      }
      else
      {
        tables->slave_fk_event_map= new_trg_event_map;
        lex->query_tables_last= &tables->next_global;
      }
    }

    /*
      It is needed to set_time():
      1) it continues the property that "Time" in SHOW PROCESSLIST shows how
      much slave is behind
      2) it will be needed when we allow replication from a table with no
      TIMESTAMP column to a table with one.
      So we call set_time(), like in SBR. Presently it changes nothing.
      3) vers_set_hist_part() requires proper query time.
    */
    thd->set_time(when, when_sec_part);

    if (unlikely(open_and_lock_tables(thd, rgi->tables_to_lock, FALSE, 0)))
    {
#ifdef WITH_WSREP
      if (WSREP(thd) && !thd->slave_thread)
      {
        WSREP_WARN("BF applier thread=%lu failed to open_and_lock_tables for "
                   "%s, fatal: %d "
                   "wsrep = (exec_mode: %d conflict_state: %d seqno: %lld)",
                   thd_get_thread_id(thd),
                   thd->get_stmt_da()->message(),
                   thd->is_fatal_error,
                   thd->wsrep_cs().mode(),
                   thd->wsrep_trx().state(),
                   wsrep_thd_trx_seqno(thd));
      }
#endif /* WITH_WSREP */
      if (thd->is_error() &&
          !is_parallel_retry_error(rgi, error= thd->get_stmt_da()->sql_errno()))
      {
        /*
          Error reporting borrowed from Query_log_event with many excessive
          simplifications.
          We should not honour --slave-skip-errors at this point as we are
          having severe errors which should not be skipped.
        */
        rli->report(ERROR_LEVEL, error, rgi->gtid_info(),
                    "Error executing row event: '%s'",
                    (error ? thd->get_stmt_da()->message() :
                     "unexpected success or fatal error"));
        thd->is_slave_error= 1;
      }
      /* remove trigger's tables */
      goto err;
    }

    /*
      When the open and locking succeeded, we check all tables to
      ensure that they still have the correct type.
    */

    {
      DBUG_PRINT("debug", ("Checking compatibility of tables to lock - tables_to_lock: %p",
                           rgi->tables_to_lock));

      /**
        When using RBR and MyISAM MERGE tables the base tables that make
        up the MERGE table can be appended to the list of tables to lock.
  
        Thus, we just check compatibility for those that tables that have
        a correspondent table map event (ie, those that are actually going
        to be accessed while applying the event). That's why the loop stops
        at rli->tables_to_lock_count .

        NOTE: The base tables are added here are removed when 
              close_thread_tables is called.
       */
      TABLE_LIST *table_list_ptr= rgi->tables_to_lock;
      for (uint i=0 ; table_list_ptr && (i < rgi->tables_to_lock_count);
           table_list_ptr= table_list_ptr->next_global, i++)
      {
        /*
          Below if condition takes care of skipping base tables that
          make up the MERGE table (which are added by open_tables()
          call). They are added next to the merge table in the list.
          For eg: If RPL_TABLE_LIST is t3->t1->t2 (where t1 and t2
          are base tables for merge table 't3'), open_tables will modify
          the list by adding t1 and t2 again immediately after t3 in the
          list (*not at the end of the list*). New table_to_lock list will
          look like t3->t1'->t2'->t1->t2 (where t1' and t2' are TABLE_LIST
          objects added by open_tables() call). There is no flag(or logic) in
          open_tables() that can skip adding these base tables to the list.
          So the logic here should take care of skipping them.

          tables_to_lock_count logic will take care of skipping base tables
          that are added at the end of the list.
          For eg: If RPL_TABLE_LIST is t1->t2->t3, open_tables will modify
          the list into t1->t2->t3->t1'->t2'. t1' and t2' will be skipped
          because tables_to_lock_count logic in this for loop.
        */
        if (table_list_ptr->parent_l)
          continue;
        /*
          We can use a down cast here since we know that every table added
          to the tables_to_lock is a RPL_TABLE_LIST (or child table which is
          skipped above).
        */
        RPL_TABLE_LIST *ptr= static_cast<RPL_TABLE_LIST*>(table_list_ptr);
        DBUG_ASSERT(ptr->m_tabledef_valid);
        TABLE *conv_table;
        if (!ptr->m_tabledef.compatible_with(thd, rgi, ptr->table, &conv_table))
        {
          DBUG_PRINT("debug", ("Table: %s.%s is not compatible with master",
                               ptr->table->s->db.str,
                               ptr->table->s->table_name.str));
          /*
            We should not honour --slave-skip-errors at this point as we are
            having severe errors which should not be skiped.
          */
          thd->is_slave_error= 1;
          /* remove trigger's tables */
          error= ERR_BAD_TABLE_DEF;
          goto err;
        }
        DBUG_PRINT("debug", ("Table: %s.%s is compatible with master"
                             " - conv_table: %p",
                             ptr->table->s->db.str,
                             ptr->table->s->table_name.str, conv_table));
        ptr->m_conv_table= conv_table;
      }
    }

    /*
      ... and then we add all the tables to the table map and but keep
      them in the tables to lock list.

      We also invalidate the query cache for all the tables, since
      they will now be changed.

      TODO [/Matz]: Maybe the query cache should not be invalidated
      here? It might be that a table is not changed, even though it
      was locked for the statement.  We do know that each
      Rows_log_event contain at least one row, so after processing one
      Rows_log_event, we can invalidate the query cache for the
      associated table.
     */
    TABLE_LIST *ptr= rgi->tables_to_lock;
    for (uint i=0 ;  ptr && (i < rgi->tables_to_lock_count); ptr= ptr->next_global, i++)
    {
      /*
        Please see comment in above 'for' loop to know the reason
        for this if condition
      */
      if (ptr->parent_l)
        continue;
      rgi->m_table_map.set_table(ptr->table_id, ptr->table);
      /*
        Following is passing flag about triggers on the server. The problem was
        to pass it between table map event and row event. I do it via extended
        TABLE_LIST (RPL_TABLE_LIST) but row event uses only TABLE so I need to
        find somehow the corresponding TABLE_LIST.
      */
      if (m_table_id == ptr->table_id)
      {
        ptr->table->master_had_triggers=
          ((RPL_TABLE_LIST*)ptr)->master_had_triggers;
      }
    }

    /*
      Moved invalidation right before the call to rows_event_stmt_cleanup(),
      to avoid query cache being polluted with stale entries,
      Query cache is not invalidated on wsrep applier here
    */
    if (!(WSREP(thd) && wsrep_thd_is_applying(thd)))
      query_cache.invalidate_locked_for_write(thd, rgi->tables_to_lock);
  }

  table= m_table= rgi->m_table_map.get_table(m_table_id);

  DBUG_PRINT("debug", ("m_table:%p, m_table_id: %llu%s",
                       m_table, m_table_id,
                       table && master_had_triggers ?
                       " (master had triggers)" : ""));
  if (table)
  {
    Rows_log_event::Db_restore_ctx restore_ctx(this);
    master_had_triggers= table->master_had_triggers;
    bool transactional_table= table->file->has_transactions_and_rollback();
    table->file->prepare_for_modify(true,
                                  get_general_type_code() != WRITE_ROWS_EVENT);

    /*
      table == NULL means that this table should not be replicated
      (this was set up by Table_map_log_event::do_apply_event()
      which tested replicate-* rules).
    */

    if (m_width == table->s->fields && bitmap_is_set_all(&m_cols))
      set_flags(COMPLETE_ROWS_F);

    Rpl_table_data rpl_data= *(RPL_TABLE_LIST*)table->pos_in_table_list;

    /* 
      Set tables write and read sets.

      Read_set contains all slave columns (in case we are going to fetch
      a complete record from slave).

      Write_set equals the m_cols bitmap sent from master but it can be
      longer if slave has extra columns.
    */

    DBUG_PRINT_BITSET("debug", "Setting table's read_set from: %s", &m_cols);

    bitmap_set_all(table->read_set);
    bitmap_set_all(table->write_set);
    table->rpl_write_set= table->write_set;

    if (rpl_data.copy_fields)
      /* always full rows, all bits set */;
    else
    if (get_general_type_code() == WRITE_ROWS_EVENT)
      bitmap_copy(table->write_set, &m_cols); // for sequences
    else // If online alter, leave all columns set (i.e. skip intersects)
    if (!thd->slave_thread || !table->s->online_alter_binlog)
    {
      bitmap_intersect(table->read_set,&m_cols);
      if (get_general_type_code() == UPDATE_ROWS_EVENT)
        bitmap_intersect(table->write_set, &m_cols_ai);
      table->mark_columns_per_binlog_row_image();
      if (table->vfield)
        table->mark_virtual_columns_for_write(0);
    }

    if (table->versioned())
    {
      bitmap_set_bit(table->read_set, table->s->vers.start_fieldno);
      bitmap_set_bit(table->write_set, table->s->vers.start_fieldno);
      bitmap_set_bit(table->read_set, table->s->vers.end_fieldno);
      bitmap_set_bit(table->write_set, table->s->vers.end_fieldno);
    }
    m_table->mark_columns_per_binlog_row_image();

    if (!rpl_data.is_online_alter())
      this->slave_exec_mode= (enum_slave_exec_mode)slave_exec_mode_options;

    // Do event specific preparations 
    error= do_before_row_operations(rgi);

    /*
      Bug#56662 Assertion failed: next_insert_id == 0, file handler.cc
      Don't allow generation of auto_increment value when processing
      rows event by setting 'MODE_NO_AUTO_VALUE_ON_ZERO'. The exception
      to this rule happens when the auto_inc column exists on some
      extra columns on the slave. In that case, do not force
      MODE_NO_AUTO_VALUE_ON_ZERO.
    */
    sql_mode_t saved_sql_mode= thd->variables.sql_mode;
    if (!is_auto_inc_in_extra_columns())
      thd->variables.sql_mode= (rpl_data.copy_fields ? saved_sql_mode : 0)
                               | MODE_NO_AUTO_VALUE_ON_ZERO;

    // row processing loop

    /* 
      set the initial time of this ROWS statement if it was not done
      before in some other ROWS event. 
     */
    rgi->set_row_stmt_start_timestamp();

    THD_STAGE_INFO(thd, stage_executing);
    do
    {
      DBUG_ASSERT(table->in_use);

      error= do_exec_row(rgi);

      if (unlikely(error))
        DBUG_PRINT("info", ("error: %s", HA_ERR(error)));
      DBUG_ASSERT(error != HA_ERR_RECORD_DELETED);

      if (unlikely(error))
      {
        int actual_error= convert_handler_error(error, thd, table);
        bool idempotent_error= (idempotent_error_code(error) &&
                               (slave_exec_mode == SLAVE_EXEC_MODE_IDEMPOTENT));
        bool ignored_error= (idempotent_error == 0 ?
                             ignored_error_code(actual_error) : 0);

#ifdef WITH_WSREP
        if (WSREP(thd) && wsrep_thd_is_applying(thd) &&
            wsrep_ignored_error_code(this, actual_error))
        {
          idempotent_error= true;
          thd->wsrep_has_ignored_error= true;
        }
#endif /* WITH_WSREP */
        if (idempotent_error || ignored_error)
        {
          if (global_system_variables.log_warnings)
            slave_rows_error_report(WARNING_LEVEL, error, rgi, thd, table,
                                    get_type_str(),
                                    RPL_LOG_NAME, log_pos);
          thd->clear_error(1);
          error= 0;
          if (idempotent_error == 0)
            break;
        }
      }

      /*
       If m_curr_row_end  was not set during event execution (e.g., because
       of errors) we can't proceed to the next row. If the error is transient
       (i.e., error==0 at this point) we must call unpack_current_row() to set 
       m_curr_row_end.
      */ 
   
      DBUG_PRINT("info", ("curr_row: %p; curr_row_end: %p; rows_end:%p",
                          m_curr_row, m_curr_row_end, m_rows_end));

      if (!m_curr_row_end && likely(!error))
        error= unpack_current_row(rgi);

      m_curr_row= m_curr_row_end;
 
      if (likely(error == 0) && !transactional_table)
        thd->transaction->all.modified_non_trans_table=
          thd->transaction->stmt.modified_non_trans_table= TRUE;
      if (likely(error == 0))
      {
        m_row_count++;
        error= thd->killed_errno();
        if (error && !thd->is_error())
          my_error(error, MYF(0));
      }
    } // row processing loop
    while (error == 0 && (m_curr_row != m_rows_end));

    thd->inc_examined_row_count(m_row_count);

    /*
      Restore the sql_mode after the rows event is processed.
    */
    thd->variables.sql_mode= saved_sql_mode;

    {/**
         The following failure injecion works in cooperation with tests 
         setting @@global.debug= 'd,stop_slave_middle_group'.
         The sql thread receives the killed status and will proceed 
         to shutdown trying to finish incomplete events group.
     */
      DBUG_EXECUTE_IF("stop_slave_middle_group",
                      if (thd->transaction->all.modified_non_trans_table)
                        const_cast<Relay_log_info*>(rli)->abort_slave= 1;);
    }

    if (unlikely(error= do_after_row_operations(error)) &&
        ignored_error_code(convert_handler_error(error, thd, table)))
    {

      if (global_system_variables.log_warnings)
        slave_rows_error_report(WARNING_LEVEL, error, rgi, thd, table,
                                get_type_str(),
                                RPL_LOG_NAME, log_pos);
      thd->clear_error(1);
      error= 0;
    }

    if (unlikely(error))
    {
      if (rpl_data.is_online_alter())
        goto err;
      slave_rows_error_report(ERROR_LEVEL, error, rgi, thd, table,
                              get_type_str(),
                              RPL_LOG_NAME, log_pos);
      /*
        @todo We should probably not call
        reset_current_stmt_binlog_format_row() from here.
  
        Note: this applies to log_event_old.cc too.
        /Sven
      */
      thd->reset_current_stmt_binlog_format_row();
      thd->is_slave_error= 1;
      /* remove trigger's tables */
      goto err;
    }
  } // if (table)

  DBUG_ASSERT(error == 0);

  /*
    Remove trigger's tables. In case of ONLINE ALTER TABLE, event doesn't own
    the table (hence, no tables are locked), and therefore no cleanup should be
    done after each event.
  */
  if (rgi->tables_to_lock_count)
    restore_empty_query_table_list(thd->lex);

  if (WSREP(thd) && wsrep_thd_is_applying(thd))
    query_cache_invalidate_locked_for_write(thd, rgi->tables_to_lock);

  if (get_flags(STMT_END_F))
  {
    if (unlikely((error= rows_event_stmt_cleanup(rgi, thd))))
      slave_rows_error_report(ERROR_LEVEL, thd->is_error() ? 0 : error,
                              rgi, thd, table, get_type_str(),
                              RPL_LOG_NAME, log_pos);
    if (thd->slave_thread)
      free_root(thd->mem_root, MYF(MY_KEEP_PREALLOC));
  }

  thd->reset_query_timer();
  DBUG_RETURN(error);

err:
  if (rgi->tables_to_lock_count)
  {
    restore_empty_query_table_list(thd->lex);
    rgi->slave_close_thread_tables(thd);
  }
  thd->reset_query_timer();
  DBUG_RETURN(error);
}

Log_event::enum_skip_reason
Rows_log_event::do_shall_skip(rpl_group_info *rgi)
{
  /*
    If the slave skip counter is 1 and this event does not end a
    statement, then we should not start executing on the next event.
    Otherwise, we defer the decision to the normal skipping logic.
  */
  if (rgi->rli->slave_skip_counter == 1 && !get_flags(STMT_END_F))
    return Log_event::EVENT_SKIP_IGNORE;
  else
    return Log_event::do_shall_skip(rgi);
}

/**
   The function is called at Rows_log_event statement commit time,
   normally from Rows_log_event::do_update_pos() and possibly from
   Query_log_event::do_apply_event() of the COMMIT.
   The function commits the last statement for engines, binlog and
   releases resources have been allocated for the statement.
  
   @retval  0         Ok.
   @retval  non-zero  Error at the commit.
 */

static int rows_event_stmt_cleanup(rpl_group_info *rgi, THD * thd)
{
  int error;
  DBUG_ENTER("rows_event_stmt_cleanup");

  {
    /*
      This is the end of a statement or transaction, so close (and
      unlock) the tables we opened when processing the
      Table_map_log_event starting the statement.

      OBSERVER.  This will clear *all* mappings, not only those that
      are open for the table. There is not good handle for on-close
      actions for tables.

      NOTE. Even if we have no table ('table' == 0) we still need to be
      here, so that we increase the group relay log position. If we didn't, we
      could have a group relay log position which lags behind "forever"
      (assume the last master's transaction is ignored by the slave because of
      replicate-ignore rules).
    */
    error= thd->binlog_flush_pending_rows_event(TRUE);

    /*
      If this event is not in a transaction, the call below will, if some
      transactional storage engines are involved, commit the statement into
      them and flush the pending event to binlog.
      If this event is in a transaction, the call will do nothing, but a
      Xid_log_event will come next which will, if some transactional engines
      are involved, commit the transaction and flush the pending event to the
      binlog.
      We check for thd->transaction_rollback_request because it is possible
      there was a deadlock that was ignored by slave-skip-errors. Normally, the
      deadlock would have been rolled back already.
    */
    error|= (int) ((error || thd->transaction_rollback_request)
                       ? trans_rollback_stmt(thd)
                       : trans_commit_stmt(thd));

    /*
      Now what if this is not a transactional engine? we still need to
      flush the pending event to the binlog; we did it with
      thd->binlog_flush_pending_rows_event(). Note that we imitate
      what is done for real queries: a call to
      ha_autocommit_or_rollback() (sometimes only if involves a
      transactional engine), and a call to be sure to have the pending
      event flushed.
    */

    /*
      @todo We should probably not call
      reset_current_stmt_binlog_format_row() from here.

      Note: this applies to log_event_old.cc too

      Btw, the previous comment about transactional engines does not
      seem related to anything that happens here.
      /Sven
    */
    thd->reset_current_stmt_binlog_format_row();

    /*
      Reset modified_non_trans_table that we have set in
      rows_log_event::do_apply_event()
    */
    if (!thd->in_multi_stmt_transaction_mode())
    {
      thd->transaction->all.modified_non_trans_table= 0;
      thd->transaction->all.m_unsafe_rollback_flags&= ~THD_TRANS::DID_WAIT;
    }

    rgi->cleanup_context(thd, 0);
  }
  DBUG_RETURN(error);
}

/**
   The method either increments the relay log position or
   commits the current statement and increments the master group 
   position if the event is STMT_END_F flagged and
   the statement corresponds to the autocommit query (i.e replicated
   without wrapping in BEGIN/COMMIT)

   @retval 0         Success
   @retval non-zero  Error in the statement commit
 */
int
Rows_log_event::do_update_pos(rpl_group_info *rgi)
{
  Relay_log_info *rli= rgi->rli;
  int error= 0;
  DBUG_ENTER("Rows_log_event::do_update_pos");

  DBUG_PRINT("info", ("flags: %s",
                      get_flags(STMT_END_F) ? "STMT_END_F " : ""));

  if (get_flags(STMT_END_F))
  {
    /*
      Indicate that a statement is finished.
      Step the group log position if we are not in a transaction,
      otherwise increase the event log position.
    */
    error= rli->stmt_done(log_pos, thd, rgi);
    /*
      Clear any errors in thd->net.last_err*. It is not known if this is
      needed or not. It is believed that any errors that may exist in
      thd->net.last_err* are allowed. Examples of errors are "key not
      found", which is produced in the test case rpl_row_conflicts.test
    */
    thd->clear_error();
  }
  else
  {
    rgi->inc_event_relay_log_pos();
  }

  DBUG_RETURN(error);
}

#endif /* defined(HAVE_REPLICATION) */


bool Rows_log_event::write_data_header(Log_event_writer *writer)
{
  uchar buf[ROWS_HEADER_LEN_V1];        // No need to init the buffer
  DBUG_ASSERT(m_table_id != UINT32_MAX);
  DBUG_EXECUTE_IF("old_row_based_repl_4_byte_map_id_master",
                  {
                    int4store(buf + 0, (ulong) m_table_id);
                    int2store(buf + 4, m_flags);
                    return (write_data(writer, buf, 6));
                  });
  int6store(buf + RW_MAPID_OFFSET, m_table_id);
  int2store(buf + RW_FLAGS_OFFSET, m_flags);
  return write_data(writer, buf, ROWS_HEADER_LEN_V1);
}

bool Rows_log_event::write_data_body(Log_event_writer *writer)
{
  /*
     Note that this should be the number of *bits*, not the number of
     bytes.
  */
  uchar sbuf[MAX_INT_WIDTH];
  my_ptrdiff_t const data_size= m_rows_cur - m_rows_buf;
  bool res= false;
  uchar *const sbuf_end= net_store_length(sbuf, (size_t) m_width);
  uint bitmap_size= no_bytes_in_export_map(&m_cols);
  uchar *bitmap;
  DBUG_ASSERT(static_cast<size_t>(sbuf_end - sbuf) <= sizeof(sbuf));

  DBUG_DUMP("m_width", sbuf, (size_t) (sbuf_end - sbuf));
  res= res || write_data(writer, sbuf, (size_t) (sbuf_end - sbuf));

  bitmap= (uchar*) my_alloca(bitmap_size);
  bitmap_export(bitmap, &m_cols);

  DBUG_DUMP("m_cols", bitmap, bitmap_size);
  res= res || write_data(writer, bitmap, bitmap_size);
  /*
    TODO[refactor write]: Remove the "down cast" here (and elsewhere).
   */
  if (get_general_type_code() == UPDATE_ROWS_EVENT)
  {
    DBUG_ASSERT(m_cols.n_bits == m_cols_ai.n_bits);
    bitmap_export(bitmap, &m_cols_ai);

    DBUG_DUMP("m_cols_ai", bitmap, bitmap_size);
    res= res || write_data(writer, bitmap, bitmap_size);
  }
  DBUG_DUMP("rows", m_rows_buf, data_size);
  res= res || write_data(writer, m_rows_buf, (size_t) data_size);
  my_afree(bitmap);

  return res;
}


bool Rows_log_event::write_compressed(Log_event_writer *writer)
{
  uchar *m_rows_buf_tmp= m_rows_buf;
  uchar *m_rows_cur_tmp= m_rows_cur;
  bool ret= true;
  uint32 comlen, alloc_size;
  comlen= alloc_size= binlog_get_compress_len((uint32)(m_rows_cur_tmp -
                                                       m_rows_buf_tmp));
  m_rows_buf= (uchar*) my_safe_alloca(alloc_size);
  if(m_rows_buf &&
     !binlog_buf_compress(m_rows_buf_tmp, m_rows_buf,
                          (uint32)(m_rows_cur_tmp - m_rows_buf_tmp), &comlen))
  {
    m_rows_cur= comlen + m_rows_buf;
    ret= Log_event::write(writer);
  }
  my_safe_afree(m_rows_buf, alloc_size);
  m_rows_buf= m_rows_buf_tmp;
  m_rows_cur= m_rows_cur_tmp;
  return ret;
}


#if defined(HAVE_REPLICATION)
void Rows_log_event::pack_info(Protocol *protocol)
{
  char buf[256];
  char const *const flagstr=
    get_flags(STMT_END_F) ? " flags: STMT_END_F" : "";
  size_t bytes= my_snprintf(buf, sizeof(buf),
                               "table_id: %llu%s", m_table_id, flagstr);
  protocol->store(buf, bytes, &my_charset_bin);
}
#endif


/**************************************************************************
	Annotate_rows_log_event member functions
**************************************************************************/

Annotate_rows_log_event::Annotate_rows_log_event(THD *thd,
                                                 bool using_trans,
                                                 bool direct)
  : Log_event(thd, 0, using_trans),
    m_save_thd_query_txt(0),
    m_save_thd_query_len(0),
    m_saved_thd_query(false),
    m_used_query_txt(0)
{
  m_query_txt= thd->query();
  m_query_len= thd->query_length();
  if (direct)
    cache_type= Log_event::EVENT_NO_CACHE;
}


bool Annotate_rows_log_event::write_data_header(Log_event_writer *writer)
{ 
  return 0;
}


bool Annotate_rows_log_event::write_data_body(Log_event_writer *writer)
{
  return write_data(writer, m_query_txt, m_query_len);
}


#if defined(HAVE_REPLICATION)
void Annotate_rows_log_event::pack_info(Protocol* protocol)
{
  if (m_query_txt && m_query_len)
    protocol->store(m_query_txt, m_query_len, &my_charset_bin);
}
#endif


#if defined(HAVE_REPLICATION)
int Annotate_rows_log_event::do_apply_event(rpl_group_info *rgi)
{
  rgi->free_annotate_event();
  m_save_thd_query_txt= thd->query();
  m_save_thd_query_len= thd->query_length();
  m_saved_thd_query= true;
  m_used_query_txt= 1;
  thd->set_query(m_query_txt, m_query_len);
  return 0;
}
#endif


#if defined(HAVE_REPLICATION)
int Annotate_rows_log_event::do_update_pos(rpl_group_info *rgi)
{
  rgi->inc_event_relay_log_pos();
  return 0;
}
#endif


#if defined(HAVE_REPLICATION)
Log_event::enum_skip_reason
Annotate_rows_log_event::do_shall_skip(rpl_group_info *rgi)
{
  return continue_group(rgi);
}
#endif

/**************************************************************************
	Table_map_log_event member functions and support functions
**************************************************************************/

/**
  Save the field metadata based on the real_type of the field.
  The metadata saved depends on the type of the field. Some fields
  store a single byte for pack_length() while others store two bytes
  for field_length (max length).
  
  @retval  0  Ok.

  @todo
  We may want to consider changing the encoding of the information.
  Currently, the code attempts to minimize the number of bytes written to 
  the tablemap. There are at least two other alternatives; 1) using 
  net_store_length() to store the data allowing it to choose the number of
  bytes that are appropriate thereby making the code much easier to 
  maintain (only 1 place to change the encoding), or 2) use a fixed number
  of bytes for each field. The problem with option 1 is that net_store_length()
  will use one byte if the value < 251, but 3 bytes if it is > 250. Thus,
  for fields like CHAR which can be no larger than 255 characters, the method
  will use 3 bytes when the value is > 250. Further, every value that is
  encoded using 2 parts (e.g., pack_length, field_length) will be numerically
  > 250 therefore will use 3 bytes for eah value. The problem with option 2
  is less wasteful for space but does waste 1 byte for every field that does
  not encode 2 parts. 
*/
int Table_map_log_event::save_field_metadata()
{
  DBUG_ENTER("Table_map_log_event::save_field_metadata");
  int index= 0;
  Binlog_type_info *info;
  for (unsigned int i= 0 ; i < m_table->s->fields ; i++)
  {
    DBUG_PRINT("debug", ("field_type: %d", m_coltype[i]));
    info= binlog_type_info_array + i;
    int2store(&m_field_metadata[index], info->m_metadata);
    index+= info->m_metadata_size;
    DBUG_EXECUTE_IF("inject_invalid_blob_size",
                    {
                      if (m_coltype[i] == MYSQL_TYPE_BLOB)
                        m_field_metadata[index-1] = 5;
                    });
  }
  DBUG_RETURN(index);
}


/*
  Constructor used to build an event for writing to the binary log.
  Mats says tbl->s lives longer than this event so it's ok to copy pointers
  (tbl->s->db etc) and not pointer content.
 */
Table_map_log_event::Table_map_log_event(THD *thd, TABLE *tbl, ulonglong tid,
                                         bool is_transactional)
  : Log_event(thd, 0, is_transactional),
    m_table(tbl),
    m_dbnam(tbl->s->db.str),
    m_dblen(m_dbnam ? tbl->s->db.length : 0),
    m_tblnam(tbl->s->table_name.str),
    m_tbllen(tbl->s->table_name.length),
    m_colcnt(tbl->s->fields),
    m_memory(NULL),
    m_table_id(tid),
    m_flags(TM_BIT_LEN_EXACT_F),
    m_data_size(0),
    m_field_metadata(0),
    m_field_metadata_size(0),
    m_null_bits(0),
    m_meta_memory(NULL),
    m_optional_metadata_len(0),
    m_optional_metadata(NULL)
{
  uchar cbuf[MAX_INT_WIDTH];
  uchar *cbuf_end;
  DBUG_ENTER("Table_map_log_event::Table_map_log_event(TABLE)");
  DBUG_ASSERT(m_table_id != UINT32_MAX);
  /*
    In TABLE_SHARE, "db" and "table_name" are 0-terminated (see this comment in
    table.cc / alloc_table_share():
      Use the fact the key is db/0/table_name/0
    As we rely on this let's assert it.
  */
  DBUG_ASSERT((tbl->s->db.str == 0) ||
              (tbl->s->db.str[tbl->s->db.length] == 0));
  DBUG_ASSERT(tbl->s->table_name.str[tbl->s->table_name.length] == 0);

  binlog_type_info_array= thd->alloc<Binlog_type_info>(m_table->s->fields);
  for (uint i= 0; i <  m_table->s->fields; i++)
    binlog_type_info_array[i]= m_table->field[i]->binlog_type_info();

  m_data_size=  TABLE_MAP_HEADER_LEN;
  DBUG_EXECUTE_IF("old_row_based_repl_4_byte_map_id_master", m_data_size= 6;);
  m_data_size+= m_dblen + 2;	// Include length and terminating \0
  m_data_size+= m_tbllen + 2;	// Include length and terminating \0
  cbuf_end= net_store_length(cbuf, (size_t) m_colcnt);
  DBUG_ASSERT(static_cast<size_t>(cbuf_end - cbuf) <= sizeof(cbuf));
  m_data_size+= (cbuf_end - cbuf) + m_colcnt;	// COLCNT and column types

  if (tbl->triggers)
    m_flags|= TM_BIT_HAS_TRIGGERS_F;

  /* If malloc fails, caught in is_valid() */
  if ((m_memory= (uchar*) my_malloc(PSI_INSTRUMENT_ME, m_colcnt, MYF(MY_WME))))
  {
    m_coltype= reinterpret_cast<uchar*>(m_memory);
    for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
      m_coltype[i]= binlog_type_info_array[i].m_type_code;
    DBUG_EXECUTE_IF("inject_invalid_column_type", m_coltype[1]= 230;);
  }

  /*
    Calculate a bitmap for the results of maybe_null() for all columns.
    The bitmap is used to determine when there is a column from the master
    that is not on the slave and is null and thus not in the row data during
    replication.
  */
  uint num_null_bytes= (m_table->s->fields + 7) / 8;
  m_data_size+= num_null_bytes;
  m_meta_memory= (uchar *)my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
                                 &m_null_bits, num_null_bytes,
                                 &m_field_metadata, (m_colcnt * 2),
                                 NULL);

  bzero(m_field_metadata, (m_colcnt * 2));

  /*
    Create an array for the field metadata and store it.
  */
  m_field_metadata_size= save_field_metadata();
  DBUG_ASSERT(m_field_metadata_size <= (m_colcnt * 2));

  /*
    Now set the size of the data to the size of the field metadata array
    plus one or three bytes (see pack.c:net_store_length) for number of 
    elements in the field metadata array.
  */
  if (m_field_metadata_size < 251)
    m_data_size+= m_field_metadata_size + 1; 
  else
    m_data_size+= m_field_metadata_size + 3; 

  bzero(m_null_bits, num_null_bytes);
  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
    if (m_table->field[i]->maybe_null())
      m_null_bits[(i / 8)]+= 1 << (i % 8);

  init_metadata_fields();
  m_data_size+= m_metadata_buf.length();

  DBUG_VOID_RETURN;
}


/*
  Return value is an error code, one of:

      -1     Failure to open table   [from open_tables()]
       0     Success
       1     No room for more tables [from set_table()]
       2     Out of memory           [from set_table()]
       3     Wrong table definition
       4     Daisy-chaining RBR with SBR not possible
 */

#if defined(HAVE_REPLICATION)

enum enum_tbl_map_status
{
  /* no duplicate identifier found */
  OK_TO_PROCESS= 0,

  /* this table map must be filtered out */
  FILTERED_OUT= 1,

  /* identifier mapping table with different properties */
  SAME_ID_MAPPING_DIFFERENT_TABLE= 2,
  
  /* a duplicate identifier was found mapping the same table */
  SAME_ID_MAPPING_SAME_TABLE= 3
};

/*
  Checks if this table map event should be processed or not. First
  it checks the filtering rules, and then looks for duplicate identifiers
  in the existing list of rli->tables_to_lock.

  It checks that there hasn't been any corruption by verifying that there
  are no duplicate entries with different properties.

  In some cases, some binary logs could get corrupted, showing several
  tables mapped to the same table_id, 0 (see: BUG#56226). Thus we do this
  early sanity check for such cases and avoid that the server crashes 
  later.

  In some corner cases, the master logs duplicate table map events, i.e.,
  same id, same database name, same table name (see: BUG#37137). This is
  different from the above as it's the same table that is mapped again 
  to the same identifier. Thus we cannot just check for same ids and 
  assume that the event is corrupted we need to check every property. 

  NOTE: in the event that BUG#37137 ever gets fixed, this extra check 
        will still be valid because we would need to support old binary 
        logs anyway.

  @param rli The relay log info reference.
  @param table_list A list element containing the table to check against.
  @return OK_TO_PROCESS 
            if there was no identifier already in rli->tables_to_lock 
            
          FILTERED_OUT
            if the event is filtered according to the filtering rules

          SAME_ID_MAPPING_DIFFERENT_TABLE 
            if the same identifier already maps a different table in 
            rli->tables_to_lock

          SAME_ID_MAPPING_SAME_TABLE 
            if the same identifier already maps the same table in 
            rli->tables_to_lock.
*/
static enum_tbl_map_status
check_table_map(rpl_group_info *rgi, RPL_TABLE_LIST *table_list)
{
  DBUG_ENTER("check_table_map");
  enum_tbl_map_status res= OK_TO_PROCESS;
  Relay_log_info *rli= rgi->rli;
  if ((rgi->thd->slave_thread /* filtering is for slave only */ ||
        IF_WSREP((WSREP(rgi->thd) && rgi->thd->wsrep_applier), 0)) &&
      (!rli->mi->rpl_filter->db_ok(table_list->db.str) ||
       (rli->mi->rpl_filter->is_on() && !rli->mi->rpl_filter->tables_ok("", table_list))))
    res= FILTERED_OUT;
  else
  {
    RPL_TABLE_LIST *ptr= static_cast<RPL_TABLE_LIST*>(rgi->tables_to_lock);
    for(uint i=0 ; ptr && (i< rgi->tables_to_lock_count); 
        ptr= static_cast<RPL_TABLE_LIST*>(ptr->next_local), i++)
    {
      if (ptr->table_id == table_list->table_id)
      {

        if (cmp(&ptr->db, &table_list->db) ||
            cmp(&ptr->alias, &table_list->table_name) ||
            ptr->lock_type != TL_WRITE) // the ::do_apply_event always sets TL_WRITE
          res= SAME_ID_MAPPING_DIFFERENT_TABLE;
        else
          res= SAME_ID_MAPPING_SAME_TABLE;

        break;
      }
    }
  }

  DBUG_PRINT("debug", ("check of table map ended up with: %u", res));

  DBUG_RETURN(res);
}

table_def Table_map_log_event::get_table_def()
{
  return table_def(m_coltype, m_colcnt,
                   m_field_metadata, m_field_metadata_size,
                   m_null_bits, m_flags);
}

int Table_map_log_event::do_apply_event(rpl_group_info *rgi)
{
  RPL_TABLE_LIST *table_list;
  char *db_mem, *tname_mem, *ptr;
  size_t dummy_len, db_mem_length, tname_mem_length;
  /*
    The database name can be changed to a longer name after get_rewrite_db().
    Allocate the maximum possible size.
  */
  const size_t db_mem_alloced= NAME_LEN + 1;
  const size_t tname_mem_alloced= NAME_LEN + 1;
  void *memory;
  Rpl_filter *filter;
  Relay_log_info const *rli= rgi->rli;
  DBUG_ENTER("Table_map_log_event::do_apply_event(Relay_log_info*)");

  /* Step the query id to mark what columns that are actually used. */
  thd->set_query_id(next_query_id());

  if (!(memory= my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME),
                                &table_list, (uint) sizeof(RPL_TABLE_LIST),
                                &db_mem, (uint) db_mem_alloced,
                                &tname_mem, (uint) tname_mem_alloced,
                                NullS)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  if (lower_case_table_names)
  {
    db_mem_length= files_charset_info->casedn_z(m_dbnam, m_dblen,
                                                db_mem, db_mem_alloced);
    tname_mem_length= files_charset_info->casedn_z(m_tblnam, m_tbllen,
                                                   tname_mem,
                                                   tname_mem_alloced);
  }
  else
  {
    db_mem_length= strmov(db_mem, m_dbnam) - db_mem;
    tname_mem_length= strmov(tname_mem, m_tblnam) - tname_mem;
  }

  /* call from mysql_client_binlog_statement() will not set rli->mi */
  filter= rgi->thd->slave_thread ? rli->mi->rpl_filter : global_rpl_filter;

  /* rewrite rules changed the database */
  if (((ptr= (char*) filter->get_rewrite_db(db_mem, &dummy_len)) != db_mem))
    db_mem_length= strmov(db_mem, ptr) - db_mem;

  LEX_CSTRING tmp_db_name=  {db_mem, db_mem_length };
  LEX_CSTRING tmp_tbl_name= {tname_mem, tname_mem_length };

  /*
    The memory allocated by the table_def structure (i.e., not the
    memory allocated *for* the table_def structure) is released
    inside rpl_group_info::clear_tables_to_lock() by calling the
    table_def destructor explicitly.
  */
  new(table_list) RPL_TABLE_LIST(&tmp_db_name, &tmp_tbl_name, TL_WRITE,
                                 get_table_def(),
                                 m_flags & TM_BIT_HAS_TRIGGERS_F);

  table_list->table_id= DBUG_IF("inject_tblmap_same_id_maps_diff_table") ?
                                         0: m_table_id;
  table_list->required_type= TABLE_TYPE_NORMAL;
  table_list->open_type= OT_BASE_ONLY;
  DBUG_ASSERT(table_list->updating);

  DBUG_PRINT("debug", ("table: %s is mapped to %llu",
                       table_list->table_name.str,
                       table_list->table_id));
  DBUG_PRINT("debug", ("table->master_had_triggers=%d",
                       (int)table_list->master_had_triggers));

  enum_tbl_map_status tblmap_status= check_table_map(rgi, table_list);
  if (tblmap_status == OK_TO_PROCESS)
  {
    DBUG_ASSERT(thd->lex->query_tables != table_list);

    /*
      We record in the slave's information that the table should be
      locked by linking the table into the list of tables to lock.
    */
    table_list->next_global= table_list->next_local= rgi->tables_to_lock;
    rgi->tables_to_lock= table_list;
    rgi->tables_to_lock_count++;
    /* 'memory' is freed in clear_tables_to_lock */
  }
  else  // FILTERED_OUT, SAME_ID_MAPPING_*
  {
    /*
      If mapped already but with different properties, we raise an
      error.
      If mapped already but with same properties we skip the event.
      If filtered out we skip the event.

      In all three cases, we need to free the memory previously 
      allocated.
     */
    if (tblmap_status == SAME_ID_MAPPING_DIFFERENT_TABLE)
    {
      /*
        Something bad has happened. We need to stop the slave as strange things
        could happen if we proceed: slave crash, wrong table being updated, ...
        As a consequence we push an error in this case.
       */

      char buf[256];

      my_snprintf(buf, sizeof(buf), 
                  "Found table map event mapping table id %llu which "
                  "was already mapped but with different settings.",
                  table_list->table_id);

      if (thd->slave_thread)
        rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, rgi->gtid_info(),
                    ER_THD(thd, ER_SLAVE_FATAL_ERROR), buf);
      else
        /* 
          For the cases in which a 'BINLOG' statement is set to 
          execute in a user session 
         */
        my_error(ER_SLAVE_FATAL_ERROR, MYF(0), buf);
    }

    table_list->~RPL_TABLE_LIST();
    my_free(memory);
  }

  DBUG_RETURN(tblmap_status == SAME_ID_MAPPING_DIFFERENT_TABLE);
}

Log_event::enum_skip_reason
Table_map_log_event::do_shall_skip(rpl_group_info *rgi)
{
  /*
    If the slave skip counter is 1, then we should not start executing
    on the next event.
  */
  return continue_group(rgi);
}

int Table_map_log_event::do_update_pos(rpl_group_info *rgi)
{
  rgi->inc_event_relay_log_pos();
  return 0;
}

#endif /* defined(HAVE_REPLICATION) */

bool Table_map_log_event::write_data_header(Log_event_writer *writer)
{
  DBUG_ASSERT(m_table_id != UINT32_MAX);
  uchar buf[TABLE_MAP_HEADER_LEN];
  DBUG_EXECUTE_IF("old_row_based_repl_4_byte_map_id_master",
                  {
                    int4store(buf + 0, (ulong) m_table_id);
                    int2store(buf + 4, m_flags);
                    return (write_data(writer, buf, 6));
                  });
  int6store(buf + TM_MAPID_OFFSET, m_table_id);
  int2store(buf + TM_FLAGS_OFFSET, m_flags);
  return write_data(writer, buf, TABLE_MAP_HEADER_LEN);
}

bool Table_map_log_event::write_data_body(Log_event_writer *writer)
{
  DBUG_ASSERT(m_dbnam != NULL);
  DBUG_ASSERT(m_tblnam != NULL);
  /* We use only one byte per length for storage in event: */
  DBUG_ASSERT(m_dblen <= MY_MIN(NAME_LEN, 255));
  DBUG_ASSERT(m_tbllen <= MY_MIN(NAME_LEN, 255));

  uchar const dbuf[]= { (uchar) m_dblen };
  uchar const tbuf[]= { (uchar) m_tbllen };

  uchar cbuf[MAX_INT_WIDTH];
  uchar *const cbuf_end= net_store_length(cbuf, (size_t) m_colcnt);
  DBUG_ASSERT(static_cast<size_t>(cbuf_end - cbuf) <= sizeof(cbuf));

  /*
    Store the size of the field metadata.
  */
  uchar mbuf[MAX_INT_WIDTH];
  uchar *const mbuf_end= net_store_length(mbuf, m_field_metadata_size);

  return write_data(writer, dbuf,      sizeof(dbuf)) ||
         write_data(writer, m_dbnam,   m_dblen+1) ||
         write_data(writer, tbuf,      sizeof(tbuf)) ||
         write_data(writer, m_tblnam,  m_tbllen+1) ||
         write_data(writer, cbuf, (size_t) (cbuf_end - cbuf)) ||
         write_data(writer, m_coltype, m_colcnt) ||
         write_data(writer, mbuf, (size_t) (mbuf_end - mbuf)) ||
         write_data(writer, m_field_metadata, m_field_metadata_size),
         write_data(writer, m_null_bits, (m_colcnt + 7) / 8) ||
         write_data(writer, (const uchar*) m_metadata_buf.ptr(),
                                           m_metadata_buf.length());
 }

/**
   stores an integer into packed format.

   @param[out] str_buf  a buffer where the packed integer will be stored.
   @param[in] length  the integer will be packed.
 */
static inline
void store_compressed_length(String &str_buf, ulonglong length)
{
  // Store Type and packed length
  uchar buf[4];
  uchar *buf_ptr = net_store_length(buf, length);

  str_buf.append(reinterpret_cast<char *>(buf), buf_ptr-buf);
}

/**
  Write data into str_buf with Type|Length|Value(TLV) format.

  @param[out] str_buf a buffer where the field is stored.
  @param[in] type  type of the field
  @param[in] length  length of the field value
  @param[in] value  value of the field
*/
static inline
bool write_tlv_field(String &str_buf,
                     enum Table_map_log_event::Optional_metadata_field_type
                     type, uint length, const uchar *value)
{
  /* type is stored in one byte, so it should never bigger than 255. */
  DBUG_ASSERT(static_cast<int>(type) <= 255);
  str_buf.append((char) type);
  store_compressed_length(str_buf, length);
  return str_buf.append(reinterpret_cast<const char *>(value), length);
}

/**
  Write data into str_buf with Type|Length|Value(TLV) format.

  @param[out] str_buf a buffer where the field is stored.
  @param[in] type  type of the field
  @param[in] value  value of the field
*/
static inline
bool write_tlv_field(String &str_buf,
                     enum Table_map_log_event::Optional_metadata_field_type
                     type, const String &value)
{
  return write_tlv_field(str_buf, type, value.length(),
                         reinterpret_cast<const uchar *>(value.ptr()));
}

static inline bool is_character_field(Binlog_type_info *info_array, Field *field)
{
  Binlog_type_info *info= info_array + field->field_index;
  if (!info->m_cs)
    return 0;
  if (info->m_set_typelib || info->m_enum_typelib)
    return 0;
  return 1;
}

static inline bool is_enum_or_set_field(Binlog_type_info *info_array, Field *field) {
  Binlog_type_info *info= info_array + field->field_index;
  if (info->m_set_typelib || info->m_enum_typelib)
    return 1;
  return 0;
}


void Table_map_log_event::init_metadata_fields()
{
  DBUG_ENTER("init_metadata_fields");
  DBUG_EXECUTE_IF("simulate_no_optional_metadata", DBUG_VOID_RETURN;);

  if (binlog_row_metadata == BINLOG_ROW_METADATA_NO_LOG)
    DBUG_VOID_RETURN;
  if (init_signedness_field() ||
      init_charset_field(&is_character_field, DEFAULT_CHARSET,
                         COLUMN_CHARSET) ||
      init_geometry_type_field())
  {
    m_metadata_buf.length(0);
    DBUG_VOID_RETURN;
  }

  if (binlog_row_metadata == BINLOG_ROW_METADATA_FULL)
  {
    if ((!DBUG_IF("dont_log_column_name") && init_column_name_field()) ||
        init_charset_field(&is_enum_or_set_field, ENUM_AND_SET_DEFAULT_CHARSET,
                           ENUM_AND_SET_COLUMN_CHARSET) ||
        init_set_str_value_field() ||
        init_enum_str_value_field() ||
        init_primary_key_field())
      m_metadata_buf.length(0);
  }
  DBUG_VOID_RETURN;
}

bool Table_map_log_event::init_signedness_field()
{
  /* use it to store signed flags, each numeric column take a bit. */
  StringBuffer<128> buf;
  unsigned char flag= 0;
  unsigned char mask= 0x80;
  Binlog_type_info *info;

  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
  {
    info= binlog_type_info_array + i;
    if (info->m_signedness != Binlog_type_info::SIGN_NOT_APPLICABLE)
    {
      if (info->m_signedness == Binlog_type_info::SIGN_UNSIGNED)
        flag|= mask;
      mask >>= 1;

      // 8 fields are tested, store the result and clear the flag.
      if (mask == 0)
      {
        buf.append(flag);
        flag= 0;
        mask= 0x80;
      }
    }
  }

  // Stores the signedness flags of last few columns
  if (mask != 0x80)
    buf.append(flag);

  // The table has no numeric column, so don't log SIGNEDNESS field
  if (buf.is_empty())
    return false;

  return write_tlv_field(m_metadata_buf, SIGNEDNESS, buf);
}

bool Table_map_log_event::init_charset_field(
    bool (* include_type)(Binlog_type_info *, Field *),
    Optional_metadata_field_type default_charset_type,
    Optional_metadata_field_type column_charset_type)
{
  DBUG_EXECUTE_IF("simulate_init_charset_field_error", return true;);

  std::map<uint, uint> collation_map;
  // For counting characters columns
  uint char_col_cnt= 0;

  /* Find the collation number used by most fields */
  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
  {
    if ((*include_type)(binlog_type_info_array, m_table->field[i]))
    {
      collation_map[binlog_type_info_array[i].m_cs->number]++;
      char_col_cnt++;
    }
  }

  if (char_col_cnt == 0)
    return false;

  /* Find the most used collation */
  uint most_used_collation= 0;
  uint most_used_count= 0;
  for (std::map<uint, uint>::iterator it= collation_map.begin();
       it != collation_map.end(); it++)
  {
    if (it->second > most_used_count)
    {
      most_used_count= it->second;
      most_used_collation= it->first;
    }
  }

  /*
    Comparing length of COLUMN_CHARSET field and COLUMN_CHARSET_WITH_DEFAULT
    field to decide which field should be logged.

    Length of COLUMN_CHARSET = character column count * collation id size.
    Length of COLUMN_CHARSET_WITH_DEFAULT =
     default collation_id size + count of columns not use default charset *
     (column index size + collation id size)

    Assume column index just uses 1 byte and collation number also uses 1 byte.
  */
  if (char_col_cnt * 1 < (1 + (char_col_cnt - most_used_count) * 2))
  {
    StringBuffer<512> buf;

    /*
      Stores character set information into COLUMN_CHARSET format,
      character sets of all columns are stored one by one.
      -----------------------------------------
      | Charset number | .... |Charset number |
      -----------------------------------------
    */
    for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
    {
      if (include_type(binlog_type_info_array, m_table->field[i]))
        store_compressed_length(buf, binlog_type_info_array[i].m_cs->number);
    }
    return write_tlv_field(m_metadata_buf, column_charset_type, buf);
  }
  else
  {
    StringBuffer<512> buf;
    uint char_column_index= 0;
    uint default_collation= most_used_collation;

    /*
      Stores character set information into DEFAULT_CHARSET format,
      First stores the default character set, and then stores the character
      sets different to default character with their column index one by one.
      --------------------------------------------------------
      | Default Charset | Col Index | Charset number | ...   |
      --------------------------------------------------------
    */

    // Store the default collation number
    store_compressed_length(buf, default_collation);

    for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
    {
      if (include_type(binlog_type_info_array, m_table->field[i]))
      {
        CHARSET_INFO *cs= binlog_type_info_array[i].m_cs;
        DBUG_ASSERT(cs);
        if (cs->number != default_collation)
        {
          store_compressed_length(buf, char_column_index);
          store_compressed_length(buf, cs->number);
        }
        char_column_index++;
      }
    }
    return write_tlv_field(m_metadata_buf, default_charset_type, buf);
  }
}

bool Table_map_log_event::init_column_name_field()
{
  StringBuffer<2048> buf;

  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
  {
    size_t len= m_table->field[i]->field_name.length;

    store_compressed_length(buf, len);
    buf.append(m_table->field[i]->field_name.str, len);
  }
  return write_tlv_field(m_metadata_buf, COLUMN_NAME, buf);
}

bool Table_map_log_event::init_set_str_value_field()
{
  StringBuffer<1024> buf;
  const TYPELIB *typelib;

  /*
    SET string values are stored in the same format:
    ----------------------------------------------
    | Value number | value1 len | value 1|  .... |  // first SET column
    ----------------------------------------------
    | Value number | value1 len | value 1|  .... |  // second SET column
    ----------------------------------------------
   */
  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
  {
    if ((typelib= binlog_type_info_array[i].m_set_typelib))
    {
      store_compressed_length(buf, typelib->count);
      for (unsigned int i= 0; i < typelib->count; i++)
      {
        store_compressed_length(buf, typelib->type_lengths[i]);
        buf.append(typelib->type_names[i], typelib->type_lengths[i]);
      }
    }
  }
  if (buf.length() > 0)
    return write_tlv_field(m_metadata_buf, SET_STR_VALUE, buf);
  return false;
}

bool Table_map_log_event::init_enum_str_value_field()
{
  StringBuffer<1024> buf;
  const TYPELIB *typelib;

  /* ENUM is same to SET columns, see comment in init_set_str_value_field */
  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
  {
    if ((typelib= binlog_type_info_array[i].m_enum_typelib))
    {
      store_compressed_length(buf, typelib->count);
      for (unsigned int i= 0; i < typelib->count; i++)
      {
        store_compressed_length(buf, typelib->type_lengths[i]);
        buf.append(typelib->type_names[i], typelib->type_lengths[i]);
      }
    }
  }

  if (buf.length() > 0)
    return write_tlv_field(m_metadata_buf, ENUM_STR_VALUE, buf);
  return false;
}

bool Table_map_log_event::init_geometry_type_field()
{
  StringBuffer<256> buf;
  uint geom_type;

  /* Geometry type of geometry columns is stored one by one as packed length */
  for (unsigned int i= 0 ; i < m_table->s->fields ; ++i)
  {
    if (binlog_type_info_array[i].m_type_code == MYSQL_TYPE_GEOMETRY)
    {
      geom_type= binlog_type_info_array[i].m_geom_type;
      DBUG_EXECUTE_IF("inject_invalid_geometry_type", geom_type= 100;);
      store_compressed_length(buf, geom_type);
    }
  }

  if (buf.length() > 0)
    return write_tlv_field(m_metadata_buf, GEOMETRY_TYPE, buf);
  return false;
}

bool Table_map_log_event::init_primary_key_field()
{
  DBUG_EXECUTE_IF("simulate_init_primary_key_field_error", return true;);

  if (unlikely(m_table->s->primary_key == MAX_KEY))
    return false;

  // If any key column uses prefix like KEY(c1(10)) */
  bool has_prefix= false;
  KEY *pk= m_table->key_info + m_table->s->primary_key;

  DBUG_ASSERT(pk->user_defined_key_parts > 0);

  /* Check if any key column uses prefix */
  for (uint i= 0; i < pk->user_defined_key_parts; i++)
  {
    KEY_PART_INFO *key_part= pk->key_part+i;
    if (key_part->length != m_table->field[key_part->fieldnr-1]->key_length())
    {
      has_prefix= true;
      break;
    }
  }

  StringBuffer<128> buf;

  if (!has_prefix)
  {
    /* Index of PK columns are stored one by one. */
    for (uint i= 0; i < pk->user_defined_key_parts; i++)
    {
      KEY_PART_INFO *key_part= pk->key_part+i;
      store_compressed_length(buf, key_part->fieldnr-1);
    }
    return write_tlv_field(m_metadata_buf, SIMPLE_PRIMARY_KEY, buf);
  }
  else
  {
    /* Index of PK columns are stored with a prefix length one by one. */
    for (uint i= 0; i < pk->user_defined_key_parts; i++)
    {
      KEY_PART_INFO *key_part= pk->key_part+i;
      size_t prefix= 0;

      store_compressed_length(buf, key_part->fieldnr-1);

      // Store character length but not octet length
      if (key_part->length != m_table->field[key_part->fieldnr-1]->key_length())
        prefix= key_part->length / key_part->field->charset()->mbmaxlen;
      store_compressed_length(buf, prefix);
    }
    return write_tlv_field(m_metadata_buf, PRIMARY_KEY_WITH_PREFIX, buf);
  }
}

#if defined(HAVE_REPLICATION)
/*
  Print some useful information for the SHOW BINARY LOG information
  field.
 */

void Table_map_log_event::pack_info(Protocol *protocol)
{
    char buf[256];
    size_t bytes= my_snprintf(buf, sizeof(buf),
                              "table_id: %llu (%s.%s)",
                              m_table_id, m_dbnam, m_tblnam);
    protocol->store(buf, bytes, &my_charset_bin);
}
#endif


/**************************************************************************
	Write_rows_log_event member functions
**************************************************************************/

/*
  Constructor used to build an event for writing to the binary log.
 */
Write_rows_log_event::Write_rows_log_event(THD *thd_arg, TABLE *tbl_arg,
                                           ulonglong tid_arg,
                                           bool is_transactional)
  :Rows_log_event(thd_arg, tbl_arg, tid_arg, tbl_arg->rpl_write_set,
                  is_transactional, WRITE_ROWS_EVENT_V1)
{
}

Write_rows_compressed_log_event::Write_rows_compressed_log_event(
                                           THD *thd_arg,
                                           TABLE *tbl_arg,
                                           ulonglong tid_arg,
                                           bool is_transactional)
  : Write_rows_log_event(thd_arg, tbl_arg, tid_arg, is_transactional)
{
  m_type = WRITE_ROWS_COMPRESSED_EVENT_V1;
}

bool Write_rows_compressed_log_event::write(Log_event_writer *writer)
{
  return Rows_log_event::write_compressed(writer);
}


#if defined(HAVE_REPLICATION)
int 
Write_rows_log_event::do_before_row_operations(const rpl_group_info *)
{
  int error= 0;

  /*
    Increment the global status insert count variable
  */
  if (get_flags(STMT_END_F))
    status_var_increment(thd->status_var.com_stat[SQLCOM_INSERT]);

  /**
     todo: to introduce a property for the event (handler?) which forces
     applying the event in the replace (idempotent) fashion.
  */
  if (slave_exec_mode == SLAVE_EXEC_MODE_IDEMPOTENT)
  {
    /*
      We are using REPLACE semantics and not INSERT IGNORE semantics
      when writing rows, that is: new rows replace old rows.  We need to
      inform the storage engine that it should use this behaviour.
    */
    
    /* Tell the storage engine that we are using REPLACE semantics. */
    thd->lex->duplicates= DUP_REPLACE;
    
    /*
      Pretend we're executing a REPLACE command: this is needed for
      InnoDB since it is not (properly) checking the lex->duplicates flag.
    */
    thd->lex->sql_command= SQLCOM_REPLACE;
    /* 
       Do not raise the error flag in case of hitting to an unique attribute
    */
    m_table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
    /* 
       The following is needed in case if we have AFTER DELETE triggers.
    */
    m_table->file->extra(HA_EXTRA_WRITE_CAN_REPLACE);
    m_table->file->extra(HA_EXTRA_IGNORE_NO_KEY);
  }
  if (m_table->triggers && do_invoke_trigger())
    m_table->prepare_triggers_for_insert_stmt_or_event();

  /* Honor next number column if present */
  m_table->next_number_field= m_table->found_next_number_field;
  /*
   * Fixed Bug#45999, In RBR, Store engine of Slave auto-generates new
   * sequence numbers for auto_increment fields if the values of them are 0.
   * If generating a sequence number is decided by the values of
   * table->auto_increment_field_not_null and SQL_MODE(if includes
   * MODE_NO_AUTO_VALUE_ON_ZERO) in update_auto_increment function.
   * SQL_MODE of slave sql thread is always consistency with master's.
   * In RBR, auto_increment fields never are NULL, except if the auto_inc
   * column exists only on the slave side (i.e., in an extra column
   * on the slave's table).
   */
  if (!is_auto_inc_in_extra_columns())
    m_table->auto_increment_field_not_null= TRUE;
  else
  {
    /*
      Here we have checked that there is an extra field
      on this server's table that has an auto_inc column.

      Mark that the auto_increment field is null and mark
      the read and write set bits.

      (There can only be one AUTO_INC column, it is always
       indexed and it cannot have a DEFAULT value).
    */
    m_table->auto_increment_field_not_null= FALSE;
    m_table->mark_auto_increment_column(true);
  }

  return error;
}

int 
Write_rows_log_event::do_after_row_operations(int error)
{
  int local_error= 0;

  /**
    Clear the write_set bit for auto_inc field that only
    existed on the destination table as an extra column.
   */
  if (is_auto_inc_in_extra_columns())
  {
    bitmap_clear_bit(m_table->rpl_write_set,
                     m_table->next_number_field->field_index);
    bitmap_clear_bit(m_table->read_set,
                     m_table->next_number_field->field_index);

    if (get_flags(STMT_END_F))
      m_table->file->ha_release_auto_increment();
  }
  m_table->next_number_field=0;
  m_table->auto_increment_field_not_null= FALSE;
  if (slave_exec_mode == SLAVE_EXEC_MODE_IDEMPOTENT)
  {
    m_table->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);
    m_table->file->extra(HA_EXTRA_WRITE_CANNOT_REPLACE);
    /*
      resetting the extra with 
      table->file->extra(HA_EXTRA_NO_IGNORE_NO_KEY); 
      fires bug#27077
      explanation: file->reset() performs this duty
      ultimately. Still todo: fix
    */
  }
  if (unlikely((local_error= m_table->file->ha_end_bulk_insert())))
  {
    m_table->file->print_error(local_error, MYF(0));
  }
  return error? error : local_error;
}

bool Rows_log_event::process_triggers(trg_event_type event,
                                      trg_action_time_type time_type,
                                      bool old_row_is_record1,
                                      bool *skip_row_indicator)
{
  bool result;
  DBUG_ENTER("Rows_log_event::process_triggers");
  m_table->triggers->mark_fields_used(event);
  if (slave_run_triggers_for_rbr == SLAVE_RUN_TRIGGERS_FOR_RBR_YES)
  {
    result= m_table->triggers->process_triggers(thd, event,
                                                time_type,
                                                old_row_is_record1,
                                                skip_row_indicator);
  }
  else
    result= m_table->triggers->process_triggers(thd, event,
                                                time_type,
                                                old_row_is_record1,
                                                skip_row_indicator);

  DBUG_RETURN(result);
}
/*
  Check if there are more UNIQUE keys after the given key.
*/
static int
last_uniq_key(TABLE *table, uint keyno)
{
  while (++keyno < table->s->keys)
    if (table->key_info[keyno].flags & HA_NOSAME)
      return 0;
  return 1;
}


/*
  We need to set the null bytes to ensure that the filler bit are
  all set when returning.  There are storage engines that just set
  the necessary bits on the bytes and don't set the filler bits
  correctly.
*/
static void
normalize_null_bits(TABLE *table)
{
  if (table->s->null_bytes > 0)
  {
    DBUG_ASSERT(table->s->last_null_bit_pos < 8);
    /*
      Normalize any unused null bits.

      We need to set the highest (8 - last_null_bit_pos) bits to 1, except that
      if last_null_bit_pos is 0 then there are no unused bits and we should set
      no bits to 1.

      When N = last_null_bit_pos != 0, we can get a mask for this with

        0xff << N = (0xff << 1) << (N-1) = 0xfe << (N-1) = 0xfe << ((N-1) & 7)

      And we can get a mask=0 for the case N = last_null_bit_pos = 0 with

        0xfe << 7 = 0xfe << ((N-1) & 7)

     Thus we can set the desired bits in all cases by OR-ing with
     (0xfe << ((N-1) & 7)), avoiding a conditional jump.
    */
    table->record[0][table->s->null_bytes - 1]|=
      (uchar)(0xfe << ((table->s->last_null_bit_pos - 1) & 7));
    /* Normalize the delete marker bit, if any. */
    table->record[0][0]|=
      !(table->s->db_create_options & HA_OPTION_PACK_RECORD);
  }
}


/**
   Check if an error is a duplicate key error.

   This function is used to check if an error code is one of the
   duplicate key error, i.e., and error code for which it is sensible
   to do a <code>get_dup_key()</code> to retrieve the duplicate key.

   @param errcode The error code to check.

   @return <code>true</code> if the error code is such that
   <code>get_dup_key()</code> will return true, <code>false</code>
   otherwise.
 */
bool
is_duplicate_key_error(int errcode)
{
  switch (errcode)
  {
  case HA_ERR_FOUND_DUPP_KEY:
  case HA_ERR_FOUND_DUPP_UNIQUE:
    return true;
  }
  return false;
}

/**
  Write the current row into event's table.

  The row is located in the row buffer, pointed by @c m_curr_row member.
  Number of columns of the row is stored in @c m_width member (it can be 
  different from the number of columns in the table to which we insert). 
  Bitmap @c m_cols indicates which columns are present in the row. It is assumed 
  that event's table is already open and pointed by @c m_table.

  If the same record already exists in the table it can be either overwritten 
  or an error is reported depending on the value of @c overwrite flag 
  (error reporting not yet implemented). Note that the matching record can be
  different from the row we insert if we use primary keys to identify records in
  the table.

  The row to be inserted can contain values only for selected columns. The 
  missing columns are filled with default values using @c prepare_record() 
  function. If a matching record is found in the table and @c overwrite is
  true, the missing columns are taken from it.

  @param  rli   Relay log info (needed for row unpacking).
  @param  overwrite  
                Shall we overwrite if the row already exists or signal 
                error (currently ignored).

  @returns Error code on failure, 0 on success.

  This method, if successful, sets @c m_curr_row_end pointer to point at the
  next row in the rows buffer. This is done when unpacking the row to be 
  inserted.

  @note If a matching record is found, it is either updated using 
  @c ha_update_row() or first deleted and then new record written.
*/ 

int Rows_log_event::write_row(rpl_group_info *rgi, const bool overwrite)
{
  DBUG_ENTER("write_row");
  DBUG_ASSERT(m_table != NULL);
  DBUG_ASSERT(thd != NULL);

  TABLE *table= m_table;  // pointer to event's table
  int error;
  int UNINIT_VAR(keynum);
  const bool invoke_triggers= (m_table->triggers && do_invoke_trigger());
  auto_afree_ptr<char> key(NULL);

  prepare_record(table, m_width, true);

  /* unpack row into table->record[0] */
  if (unlikely((error= unpack_current_row(rgi))))
  {
    table->file->print_error(error, MYF(0));
    DBUG_RETURN(error);
  }

  if (m_curr_row == m_rows_buf && !invoke_triggers && !table->s->long_unique_table)
  {
    /*
       This table has no triggers so we can do bulk insert.

       This is the first row to be inserted, we estimate the rows with
       the size of the first row and use that value to initialize
       storage engine for bulk insertion.
    */
    /* this is the first row to be inserted, we estimate the rows with
       the size of the first row and use that value to initialize
       storage engine for bulk insertion */
    DBUG_ASSERT(!(m_curr_row > m_curr_row_end));
    ha_rows estimated_rows= 0;
    if (m_curr_row < m_curr_row_end)
      estimated_rows= (m_rows_end - m_curr_row) / (m_curr_row_end - m_curr_row);
    else if (m_curr_row == m_curr_row_end)
      estimated_rows= 1;

    table->file->ha_start_bulk_insert(estimated_rows);
  }

  /*
    Explicitly set the auto_inc to null to make sure that
    it gets an auto_generated value.
  */
  if (is_auto_inc_in_extra_columns())
    m_table->next_number_field->set_null();
  
  DBUG_DUMP("record[0]", table->record[0], table->s->reclength);
  DBUG_PRINT_BITSET("debug", "rpl_write_set: %s", table->rpl_write_set);
  DBUG_PRINT_BITSET("debug", "read_set:      %s", table->read_set);

  if (table->s->long_unique_table)
    table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE);

  bool trg_skip_row= false;
  if (invoke_triggers &&
      unlikely(process_triggers(TRG_EVENT_INSERT, TRG_ACTION_BEFORE, true,
                                &trg_skip_row)))
  {
    DBUG_RETURN(HA_ERR_GENERIC); // in case if error is not set yet
  }

  /* In case any of triggers signals to skip the current row, do it. */
  if (trg_skip_row)
    return false;

  // Handle INSERT.
  if (table->versioned(VERS_TIMESTAMP))
  {
    ulong sec_part;
    // Check whether a row came from unversioned table and fix vers fields.
    if (table->vers_start_field()->get_timestamp(&sec_part) == 0 && sec_part == 0)
      table->vers_update_fields();
    table->vers_fix_old_timestamp(rgi);
  }

  /* 
    Try to write record. If a corresponding record already exists in the table,
    we try to change it using ha_update_row() if possible. Otherwise we delete
    it and repeat the whole process again. 

    TODO: Add safety measures against infinite looping. 
   */

  DBUG_EXECUTE_IF("write_row_inject_sleep_before_ha_write_row",
                  my_sleep(20000););
  if (table->s->sequence)
    error= update_sequence();
  else while (unlikely(error= table->file->ha_write_row(table->record[0])))
  {
    if (error == HA_ERR_LOCK_DEADLOCK || error == HA_ERR_LOCK_WAIT_TIMEOUT ||
        (keynum= table->file->get_dup_key(error)) < 0 || !overwrite)
    {
      DBUG_PRINT("info",("get_dup_key returns %d)", keynum));
      /*
        Deadlock, waiting for lock or just an error from the handler
        such as HA_ERR_FOUND_DUPP_KEY when overwrite is false.
        Retrieval of the duplicate key number may fail
        - either because the error was not "duplicate key" error
        - or because the information which key is not available
      */
      table->file->print_error(error, MYF(0));
      DBUG_RETURN(error);
    }
    /*
       We need to retrieve the old row into record[1] to be able to
       either update or delete the offending record.  We either:

       - use rnd_pos() with a row-id (available as dupp_row) to the
         offending row, if that is possible (MyISAM and Blackhole), or else

       - use index_read_idx() with the key that is duplicated, to
         retrieve the offending row.
     */
    if (table->file->ha_table_flags() & HA_DUPLICATE_POS)
    {
      DBUG_PRINT("info",("Locating offending record using rnd_pos()"));

      if ((error= table->file->ha_rnd_init_with_error(0)))
      {
        DBUG_RETURN(error);
      }

      error= table->file->ha_rnd_pos(table->record[1], table->file->dup_ref);
      if (unlikely(error))
      {
        DBUG_PRINT("info",("rnd_pos() returns error %d",error));
        table->file->print_error(error, MYF(0));
        DBUG_RETURN(error);
      }
      table->file->ha_rnd_end();
    }
    else
    {
      DBUG_PRINT("info",("Locating offending record using index_read_idx()"));

      if (table->file->extra(HA_EXTRA_FLUSH_CACHE))
      {
        DBUG_PRINT("info",("Error when setting HA_EXTRA_FLUSH_CACHE"));
        DBUG_RETURN(my_errno);
      }

      if (key.get() == NULL)
      {
        key.assign(static_cast<char*>(my_alloca(table->s->max_unique_length)));
        if (key.get() == NULL)
        {
          DBUG_PRINT("info",("Can't allocate key buffer"));
          DBUG_RETURN(ENOMEM);
        }
      }

      key_copy((uchar*)key.get(), table->record[0], table->key_info + keynum,
               0);
      error= table->file->ha_index_read_idx_map(table->record[1], keynum,
                                                (const uchar*)key.get(),
                                                HA_WHOLE_KEY,
                                                HA_READ_KEY_EXACT);
      if (unlikely(error))
      {
        DBUG_PRINT("info",("index_read_idx() returns %s", HA_ERR(error)));
        table->file->print_error(error, MYF(0));
        DBUG_RETURN(error);
      }
    }

    /*
       Now, record[1] should contain the offending row.  That
       will enable us to update it or, alternatively, delete it (so
       that we can insert the new row afterwards).
    */
    if (table->s->long_unique_table)
    {
      /* same as for REPLACE/ODKU */
      table->move_fields(table->field, table->record[1], table->record[0]);
      table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_REPLACE);
      table->move_fields(table->field, table->record[0], table->record[1]);
    }

    /*
      If row is incomplete we will use the record found to fill 
      missing columns.  
    */
    if (!get_flags(COMPLETE_ROWS_F))
    {
      restore_record(table,record[1]);
      error= unpack_current_row(rgi);
      if (table->s->long_unique_table)
        table->update_virtual_fields(table->file, VCOL_UPDATE_FOR_WRITE);
    }

    DBUG_PRINT("debug",("preparing for update: before and after image"));
    DBUG_DUMP("record[1] (before)", table->record[1], table->s->reclength);
    DBUG_DUMP("record[0] (after)", table->record[0], table->s->reclength);

    /*
       REPLACE is defined as either INSERT or DELETE + INSERT.  If
       possible, we can replace it with an UPDATE, but that will not
       work on InnoDB if FOREIGN KEY checks are necessary.

       I (Matz) am not sure of the reason for the last_uniq_key()
       check as, but I'm guessing that it's something along the
       following lines.

       Suppose that we got the duplicate key to be a key that is not
       the last unique key for the table and we perform an update:
       then there might be another key for which the unique check will
       fail, so we're better off just deleting the row and inserting
       the correct row.

       Additionally we don't use UPDATE if rbr triggers should be invoked -
       when triggers are used we want a simple and predictable execution path.
     */
    if (last_uniq_key(table, keynum) && !invoke_triggers &&
        !table->file->referenced_by_foreign_key())
    {
      DBUG_PRINT("info",("Updating row using ha_update_row()"));
      error= table->file->ha_update_row(table->record[1],
                                       table->record[0]);
      switch (error) {

      case HA_ERR_RECORD_IS_THE_SAME:
        DBUG_PRINT("info",("ignoring HA_ERR_RECORD_IS_THE_SAME error from"
                           " ha_update_row()"));
        error= 0;

      case 0:
        break;

      default:
        DBUG_PRINT("info",("ha_update_row() returns error %d",error));
        table->file->print_error(error, MYF(0));
      }

      DBUG_RETURN(error);
    }
    else
    {
      DBUG_PRINT("info",("Deleting offending row and trying to write new one again"));
      if (invoke_triggers &&
          unlikely(process_triggers(TRG_EVENT_DELETE, TRG_ACTION_BEFORE,
                                    true, &trg_skip_row)))
        error= HA_ERR_GENERIC; // in case if error is not set yet
      else
      {
        if (unlikely((error= table->file->ha_delete_row(table->record[1]))))
        {
          DBUG_PRINT("info",("ha_delete_row() returns error %d",error));
          table->file->print_error(error, MYF(0));
          DBUG_RETURN(error);
        }
        if (invoke_triggers && !trg_skip_row &&
            unlikely(process_triggers(TRG_EVENT_DELETE, TRG_ACTION_AFTER,
                                      true, nullptr)))
          DBUG_RETURN(HA_ERR_GENERIC); // in case if error is not set yet
      }
      /* Will retry ha_write_row() with the offending row removed. */
    }
  }

  if (invoke_triggers && !trg_skip_row &&
      unlikely(process_triggers(TRG_EVENT_INSERT, TRG_ACTION_AFTER, true,
                                nullptr)))
    error= HA_ERR_GENERIC; // in case if error is not set yet

  DBUG_RETURN(error);
}


int Rows_log_event::update_sequence()
{
  TABLE *table= m_table;  // pointer to event's table
  bool old_master= false;
  int err= 0;

  if (!bitmap_is_set(table->rpl_write_set, MIN_VALUE_FIELD_NO) ||
      (
#if defined(WITH_WSREP)
       ! WSREP(thd) &&
#endif
       table->in_use->rgi_slave &&
       !(table->in_use->rgi_slave->gtid_ev_flags2 & Gtid_log_event::FL_DDL) &&
       !(old_master=
         rpl_master_has_bug(thd->rgi_slave->rli,
                            29621, FALSE, FALSE, FALSE, TRUE))))
  {
    /* This event come from a setval function executed on the master.
       Update the sequence next_number and round, like we do with setval()
    */
    MY_BITMAP *old_map= dbug_tmp_use_all_columns(table,
                                                 &table->read_set);
    longlong nextval= table->field[NEXT_FIELD_NO]->val_int();
    longlong round= table->field[ROUND_FIELD_NO]->val_int();
    dbug_tmp_restore_column_map(&table->read_set, old_map);

    return table->s->sequence->set_value(table, nextval, round, 0) > 0;
  }
  if (old_master && !WSREP(thd) && thd->rgi_slave->is_parallel_exec)
  {
    DBUG_ASSERT(thd->rgi_slave->parallel_entry);
    /*
      With parallel replication enabled, we can't execute alongside any other
      transaction in which we may depend, so we force retry to release
      the server layer table lock for possible prior in binlog order
      same table transactions.
    */
    if (thd->rgi_slave->parallel_entry->last_committed_sub_id <
        thd->rgi_slave->wait_commit_sub_id)
    {
      err= ER_LOCK_DEADLOCK;
      my_error(err, MYF(0));
    }
  }
  /*
    Update all fields in table and update the active sequence, like with
    ALTER SEQUENCE
  */
  return err == 0 ? table->file->ha_write_row(table->record[0]) : err;
}


#endif


#if defined(HAVE_REPLICATION)

int
Write_rows_log_event::do_exec_row(rpl_group_info *rgi)
{
  DBUG_ASSERT(m_table != NULL);
  const char *tmp= thd->get_proc_info();
  char *message, msg[128];
  const LEX_CSTRING &table_name= m_table->s->table_name;
  const char quote_char=
    get_quote_char_for_identifier(thd, table_name.str, table_name.length);
  my_snprintf(msg, sizeof msg,
              "Write_rows_log_event::write_row() on table %c%.*s%c",
              quote_char, int(table_name.length), table_name.str, quote_char);
  message= msg;
  int error;

#ifdef WSREP_PROC_INFO
  my_snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
              "Write_rows_log_event::write_row(%lld) on table %c%.*s%c",
              (long long) wsrep_thd_trx_seqno(thd), quote_char,
              int(table_name.length), table_name.str, quote_char);
  message= thd->wsrep_info;
#endif /* WSREP_PROC_INFO */

  thd_proc_info(thd, message);
  error= write_row(rgi, slave_exec_mode == SLAVE_EXEC_MODE_IDEMPOTENT);
  thd_proc_info(thd, tmp);

  if (unlikely(error) && unlikely(!thd->is_error()))
  {
    DBUG_ASSERT_NO_ASSUME(0);
    my_error(ER_UNKNOWN_ERROR, MYF(0));
  }

  return error;
}

#endif /* defined(HAVE_REPLICATION) */


#if defined(HAVE_REPLICATION)
uint8 Write_rows_log_event::get_trg_event_map() const
{
  return trg2bit(TRG_EVENT_INSERT) | trg2bit(TRG_EVENT_UPDATE) |
         trg2bit(TRG_EVENT_DELETE);
}
#endif

/**************************************************************************
	Delete_rows_log_event member functions
**************************************************************************/

#if defined(HAVE_REPLICATION)
/**
  @brief Compares table->record[0] and table->record[1]

  @returns true if different.
*/
static bool record_compare(TABLE *table, bool vers_from_plain= false)
{
  bool result= false;
  bool all_values_set= bitmap_is_set_all(&table->has_value_set);

  /**
    Compare full record only if:
    - all fields were given values
    - there are no blob fields (otherwise we would also need 
      to compare blobs contents as well);
    - there are no varchar fields (otherwise we would also need
      to compare varchar contents as well);
    - there are no null fields, otherwise NULLed fields 
      contents (i.e., the don't care bytes) may show arbitrary 
      values, depending on how each engine handles internally.
    */
  if ((table->s->blob_fields + 
       table->s->varchar_fields + 
       table->s->null_fields) == 0
      && all_values_set)
  {
    normalize_null_bits(table);
    result= cmp_record(table, record[1]);
    goto record_compare_exit;
  }

  /* Compare null bits */
  if (all_values_set && memcmp(table->null_flags,
                               table->null_flags + table->s->rec_buff_length,
                               table->s->null_bytes))
    goto record_compare_differ;                         // Diff in NULL value

  /* Compare fields */
  for (Field **ptr=table->field ; *ptr ; ptr++)
  {
    Field *f= *ptr;
    /*
      If the table is versioned, don't compare using the version if there is a
      primary key. If there isn't a primary key, we need the version to
      identify the correct record if there are duplicate rows in the data set.
      However, if the primary server is unversioned (vers_from_plain is true),
      then we implicitly use row_end as the primary key on our side. This is
      because the implicit row_end value will be set to the maximum value for
      the latest row update (which is what we care about).
    */
    if (table->versioned() && f->vers_sys_field() &&
        (table->s->primary_key < MAX_KEY ||
         (vers_from_plain && table->vers_start_field() == f)))
      continue;

    /*
      We only compare fields that exist on the master (or in ONLINE
      ALTER case, that were in the original table).
    */
    if (!all_values_set)
    {
      if (!f->has_explicit_value() &&
          /* Don't skip row_end if replicating unversioned -> versioned */
          !(vers_from_plain && table->vers_end_field() == f))
        continue;
      if (f->is_null() != f->is_null(table->s->rec_buff_length))
        goto record_compare_differ;
    }

    if (!f->is_null() && !f->vcol_info &&
        f->cmp_binary_offset(table->s->rec_buff_length))
      goto record_compare_differ;
  }

record_compare_exit:
  return result;
record_compare_differ:
  return true;
}
/**
  Traverses default item expr of a field, and underlying field's default values.
  If it is an extra field and has no value replicated, then its default expr
  should be also checked.
 */
class Rpl_key_part_checker: public Field_enumerator
{
  bool online_alter;
  Field *next_number_field;
  bool field_usable;
public:


  void visit_field(Item_field *item) override
  {
    if (!field_usable)
      return;
    field_usable= check_field(item->field);
  }

  bool check_field(Field *f)
  {
    if (f->has_explicit_value())
      return true;

    if ((!f->vcol_info && !online_alter) || f == next_number_field)
      return false;

    Virtual_column_info *computed= f->vcol_info ? f->vcol_info
                                   : f->default_value;

    if (computed == NULL)
      return true; // No DEFAULT, or constant DEFAULT

    // Deterministic DEFAULT or vcol expression
    return !(computed->flags & VCOL_NOT_STRICTLY_DETERMINISTIC)
           && !computed->expr->walk(&Item::enumerate_field_refs_processor,
                                    false, this)
           && field_usable;
  }

  Rpl_key_part_checker(bool online_alter, Field *next_number_field):
    online_alter(online_alter), next_number_field(next_number_field),
    field_usable(true) {}
};


/**
  Newly added fields with non-deterministic defaults (i.e. DEFAULT(RANDOM()),
  CURRENT_TIMESTAMP, AUTO_INCREMENT) should be excluded from key search.
  Basically we exclude all the default-filled fields based on
  has_explicit_value bitmap.
*/
uint Rows_log_event::find_key_parts(const KEY *key) const
{
  RPL_TABLE_LIST *tl= (RPL_TABLE_LIST*)m_table->pos_in_table_list;
  const bool online_alter= tl->m_online_alter_copy_fields;
  uint p;

  if (!m_table->s->keys_in_use.is_set(uint(key - m_table->key_info)))
    return 0;

  if (!online_alter)
  {
    if (m_cols.n_bits >= m_table->s->fields) // replicated more than slave has
      return key->user_defined_key_parts;
    if (m_table->s->virtual_fields == 0)
    {
      for (p= 0; p < key->user_defined_key_parts; p++)
        if (key->key_part[p].fieldnr > m_cols.n_bits) // extra
          break;
      return p;
    }
  }

  Rpl_key_part_checker key_part_checker(online_alter,
                                        m_table->found_next_number_field);
  for (p= 0; p < key->user_defined_key_parts; p++)
  {
    if (!key_part_checker.check_field(key->key_part[p].field))
      break;
  }
  return p;
}


/**
  Find the best key to use when locating the row in @c find_row().

  A primary key is preferred if it exists; otherwise a unique index is
  preferred. Else we pick the index with the smallest rec_per_key value.

  If a suitable key is found, set @c m_key, @c m_key_nr, @c m_key_info,
  and @c m_usable_key_parts member fields appropriately.

  @returns Error code on failure, 0 on success.
*/
int Rows_log_event::find_key(const rpl_group_info *rgi)
{
  DBUG_ASSERT(m_table);
  RPL_TABLE_LIST *tl= (RPL_TABLE_LIST*)m_table->pos_in_table_list;
  uint i, best_key_nr= 0, best_usable_key_parts= 0;
  KEY *key;
  ulong UNINIT_VAR(best_rec_per_key), tmp;
  DBUG_ENTER("Rows_log_event::find_key");

  if ((best_key_nr= tl->cached_key_nr) != ~0U)
  {
    DBUG_ASSERT(best_key_nr <= MAX_KEY); // use the cached value
    best_usable_key_parts= tl->cached_usable_key_parts;
  }
  else
  {
    best_key_nr= MAX_KEY;

    /*
      if the source (in the row event) and destination (in m_table) records
      don't have the same structure, some keys below might be unusable
      for find_row().

      If it's a replication and slave table (m_table) has less columns
      than the master's - easy, all keys are usable.

      If slave's table has more columns, but none of them are generated -
      then any column beyond m_cols.n_bits makes an index unusable.

      If slave's table has generated columns or it's the online alter table
      where arbitrary structure conversion is possible (in the replication case
      one table must be a prefix of the other, see table_def::compatible_with)
      we cannot deduce what destination columns will be affected by m_cols,
      we have to actually unpack one row and examine has_explicit_value()
    */

    if (tl->m_online_alter_copy_fields ||
        (m_cols.n_bits < m_table->s->fields &&
         m_table->s->virtual_fields))
    {
      const uchar *curr_row_end= m_curr_row_end;
      Check_level_instant_set clis(m_table->in_use, CHECK_FIELD_IGNORE);
      if (int err= unpack_row(rgi, m_table, m_width, m_curr_row, &m_cols,
                              &curr_row_end, &m_master_reclength, m_rows_end))
        DBUG_RETURN(err);
    }

    /*
      Keys are sorted so that any primary key is first, followed by unique keys,
      followed by any other. So we will automatically pick the primary key if
      it exists.
    */
    for (i= 0, key= m_table->key_info; i < m_table->s->keys; i++, key++)
    {
      uint usable_key_parts= find_key_parts(key);
      if (usable_key_parts == 0)
        continue;
      /*
        We cannot use a unique key with NULL-able columns to uniquely identify
        a row (but we can still select it for range scan below if nothing better
        is available).
      */
      if ((key->flags & (HA_NOSAME | HA_NULL_PART_KEY)) == HA_NOSAME &&
           usable_key_parts == key->user_defined_key_parts)
      {
        best_key_nr= i;
        best_usable_key_parts= usable_key_parts;
        break;
      }
      /*
        We can only use a non-unique key if it allows range scans (ie. skip
        FULLTEXT indexes and such).
      */
      uint last_part= usable_key_parts - 1;
      DBUG_PRINT("info", ("Index %s rec_per_key[%u]= %lu",
                          key->name.str, last_part, key->rec_per_key[last_part]));
      if (!(m_table->file->index_flags(i, last_part, 1) & HA_READ_NEXT))
        continue;

      tmp= key->rec_per_key[last_part];
      if (best_key_nr == MAX_KEY || (tmp > 0 && tmp < best_rec_per_key))
      {
        best_key_nr= i;
        best_usable_key_parts= usable_key_parts;
        best_rec_per_key= tmp;
      }
    }
    tl->cached_key_nr= best_key_nr;
    tl->cached_usable_key_parts= best_usable_key_parts;
  }

  m_key_nr= best_key_nr;
  m_usable_key_parts= best_usable_key_parts;
  if (best_key_nr == MAX_KEY)
    m_key_info= NULL;
  else
  {
    m_key_info= m_table->key_info + best_key_nr;

    if (!use_pk_position())
    {
      // Allocate buffer for key searches
      m_key= (uchar *) my_malloc(PSI_INSTRUMENT_ME, m_key_info->key_length, MYF(MY_WME));
      if (m_key == NULL)
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
  }

  DBUG_EXECUTE_IF("rpl_report_chosen_key",
                  push_warning_printf(m_table->in_use,
                                      Sql_condition::WARN_LEVEL_NOTE,
                                      ER_UNKNOWN_ERROR, "Key chosen: %d",
                                      m_key_nr == MAX_KEY ?
                                      -1 : m_key_nr););

  DBUG_RETURN(0);
}


/* 
  Check if we are already spending too much time on this statement.
  if we are, warn user that it might be because table does not have
  a PK, but only if the warning was not printed before for this STMT.

  @param type          The event type code.
  @param table_name    The name of the table that the slave is 
                       operating.
  @param is_index_scan States whether the slave is doing an index scan 
                       or not.
  @param rli           The relay metadata info.
*/
static inline 
void issue_long_find_row_warning(Log_event_type type, 
                                 const char *table_name,
                                 bool is_index_scan,
                                 rpl_group_info *rgi)
{
  if ((global_system_variables.log_warnings > 1 && 
       !rgi->is_long_find_row_note_printed()))
  {
    ulonglong now= microsecond_interval_timer();
    ulonglong stmt_ts= rgi->get_row_stmt_start_timestamp();
    
    DBUG_EXECUTE_IF("inject_long_find_row_note", 
                    stmt_ts-=(LONG_FIND_ROW_THRESHOLD*2*HRTIME_RESOLUTION););

    longlong delta= (now - stmt_ts)/HRTIME_RESOLUTION;

    if (delta > LONG_FIND_ROW_THRESHOLD)
    {
      rgi->set_long_find_row_note_printed();
      const char* evt_type= LOG_EVENT_IS_DELETE_ROW(type) ? " DELETE" : "n UPDATE";
      const char* scan_type= is_index_scan ? "scanning an index" : "scanning the table";

      sql_print_information("The slave is applying a ROW event on behalf of a%s statement "
                            "on table %s and is currently taking a considerable amount "
                            "of time (%lld seconds). This is due to the fact that it is %s "
                            "while looking up records to be processed. Consider adding a "
                            "primary key (or unique key) to the table to improve "
                            "performance.",
                            evt_type, table_name, delta, scan_type);
    }
  }
}


/*
  HA_ERR_KEY_NOT_FOUND is a fatal error normally, but it's an expected
  error in speculate optimistic mode, so use something non-fatal instead
*/
static int row_not_found_error(rpl_group_info *rgi)
{
  return rgi->speculation != rpl_group_info::SPECULATE_OPTIMISTIC
         ? HA_ERR_KEY_NOT_FOUND : HA_ERR_RECORD_CHANGED;
}

bool Rows_log_event::use_pk_position() const
{
  return m_table->file->ha_table_flags() & HA_PRIMARY_KEY_REQUIRED_FOR_POSITION
      && m_table->s->primary_key < MAX_KEY
      && m_key_nr == m_table->s->primary_key
      && m_usable_key_parts == m_table->key_info->user_defined_key_parts;
}

static int end_of_file_error(rpl_group_info *rgi)
{
  return rgi->speculation != rpl_group_info::SPECULATE_OPTIMISTIC
         ? HA_ERR_END_OF_FILE : HA_ERR_RECORD_CHANGED;
}

/**
  Locate the current row in event's table.

  The current row is pointed by @c m_curr_row. Member @c m_width tells
  how many columns are there in the row (this can be differnet from
  the number of columns in the table). It is assumed that event's
  table is already open and pointed by @c m_table.

  If a corresponding record is found in the table it is stored in 
  @c m_table->record[0]. Note that when record is located based on a primary 
  key, it is possible that the record found differs from the row being located.

  If no key is specified or table does not have keys, a table scan is used to 
  find the row. In that case the row should be complete and contain values for
  all columns. However, it can still be shorter than the table, i.e. the table 
  can contain extra columns not present in the row. It is also possible that 
  the table has fewer columns than the row being located. 

  @returns Error code on failure, 0 on success. 
  
  @post In case of success @c m_table->record[0] contains the record found. 
  Also, the internal "cursor" of the table is positioned at the record found.

  @note If the engine allows random access of the records, a combination of
  @c position() and @c rnd_pos() will be used. 

  Note that one MUST call ha_index_or_rnd_end() after this function if
  it returns 0 as we must leave the row position in the handler intact
  for any following update/delete command.
*/

int Rows_log_event::find_row(rpl_group_info *rgi)
{
  DBUG_ENTER("Rows_log_event::find_row");

  DBUG_ASSERT(m_table);
  DBUG_ASSERT(m_table->in_use != NULL);

  TABLE *table= m_table;
  int error= 0;
  bool is_table_scan= false, is_index_scan= false;
  Check_level_instant_set clis(table->in_use, CHECK_FIELD_IGNORE);

  /*
    rpl_row_tabledefs.test specifies that
    if the extra field on the slave does not have a default value
    and this is okay with Delete or Update events.
    Todo: fix wl3228 hld that requires defauls for all types of events
  */
  
  prepare_record(table, m_width, FALSE);
  error= unpack_current_row(rgi);

  m_vers_from_plain= false;
  if (table->versioned())
  {
    Field *row_end= table->vers_end_field();
    DBUG_ASSERT(table->read_set);
    // check whether master table is unversioned
    if (row_end->val_int() == 0)
    {
      // Plain source table may have a PRIMARY KEY. And row_end is always
      // a part of PRIMARY KEY. Set it to max value for engine to find it in
      // index. Needed for an UPDATE/DELETE cases.
      table->vers_end_field()->set_max();
      m_vers_from_plain= true;
    }
    else if (m_table->versioned(VERS_TIMESTAMP))
    {
      /* Change row_end in record[0] to new end date if old server */
      m_table->vers_fix_old_timestamp(rgi);
    }
  }

  DBUG_PRINT("info",("looking for the following record"));
  DBUG_DUMP("record[0]", table->record[0], table->s->reclength);

  if (use_pk_position())
  {
    /*
      Use a more efficient method to fetch the record given by
      table->record[0] if the engine allows it.  We first compute a
      row reference using the position() member function (it will be
      stored in table->file->ref) and the use rnd_pos() to position
      the "cursor" (i.e., record[0] in this case) at the correct row.

      TODO: Add a check that the correct record has been fetched by
      comparing with the original record. Take into account that the
      record on the master and slave can be of different
      length. Something along these lines should work:

      ADD>>>  store_record(table,record[1]);
              int error= table->file->ha_rnd_pos(table->record[0],
              table->file->ref);
      ADD>>>  DBUG_ASSERT(memcmp(table->record[1], table->record[0],
                                 table->s->reclength) == 0);

    */
    DBUG_PRINT("info",("locating record using primary key (position)"));

    error= table->file->ha_rnd_pos_by_record(table->record[0]);
    if (unlikely(error))
    {
      DBUG_PRINT("info",("rnd_pos returns error %d",error));
      if (error == HA_ERR_KEY_NOT_FOUND)
        error= row_not_found_error(rgi);
      table->file->print_error(error, MYF(0));
    }
    DBUG_RETURN(error);
  }

  // We can't use position() - try other methods.
  
  normalize_null_bits(table);

  /*
    Save copy of the record in table->record[1]. It might be needed 
    later if linear search is used to find exact match.
   */ 
  store_record(table,record[1]);    

  if (m_key_info)
  {
    DBUG_PRINT("info",("locating record using key #%u [%s] (index_read)",
                       m_key_nr, m_key_info->name.str));
    /* We use this to test that the correct key is used in test cases. */
    DBUG_EXECUTE_IF("slave_crash_if_wrong_index",
                    if(0 != strcmp(m_key_info->name.str,"expected_key")) abort(););

    /* The key is active: search the table using the index */
    if (!table->file->inited &&
        (error= table->file->ha_index_init(m_key_nr, FALSE)))
    {
      DBUG_PRINT("info",("ha_index_init returns error %d",error));
      table->file->print_error(error, MYF(0));
      goto end;
    }

    /* Fill key data for the row */

    DBUG_ASSERT(m_key);
    key_copy(m_key, table->record[0], m_key_info, 0);

    /*
      Don't print debug messages when running valgrind since they can
      trigger false warnings.
     */
#ifndef HAVE_valgrind
    DBUG_DUMP("key data", m_key, m_key_info->key_length);
#endif

    const enum ha_rkey_function find_flag=
      m_usable_key_parts == m_key_info->user_defined_key_parts
      ? HA_READ_KEY_EXACT : HA_READ_KEY_OR_NEXT;
    error= table->file->ha_index_read_map(table->record[0], m_key,
                                          make_keypart_map(m_usable_key_parts),
                                          find_flag);
    if (unlikely(error))
    {
      DBUG_PRINT("info",("no record matching the key found in the table"));
      if (error == HA_ERR_KEY_NOT_FOUND)
        error= row_not_found_error(rgi);
      table->file->print_error(error, MYF(0));
      table->file->ha_index_end();
      goto end;
    }

  /*
    Don't print debug messages when running valgrind since they can
    trigger false warnings.
   */
#ifndef HAVE_valgrind
    DBUG_PRINT("info",("found first matching record")); 
    DBUG_DUMP("record[0]", table->record[0], table->s->reclength);
#endif
    /*
      Below is a minor "optimization".  If the key (i.e., key number
      0) has the HA_NOSAME flag set, we know that we have found the
      correct record (since there can be no duplicates); otherwise, we
      have to compare the record with the one found to see if it is
      the correct one.

      CAVEAT! This behaviour is essential for the replication of,
      e.g., the mysql.proc table since the correct record *shall* be
      found using the primary key *only*.  There shall be no
      comparison of non-PK columns to decide if the correct record is
      found.  I can see no scenario where it would be incorrect to
      chose the row to change only using a PK or an UNNI.
    */
    if (find_flag == HA_READ_KEY_EXACT && table->key_info->flags & HA_NOSAME)
    {
      /* Unique does not have non nullable part */
      if (!(table->key_info->flags & HA_NULL_PART_KEY))
      {
        error= 0;
        goto end;
      }
      else
      {
        KEY *keyinfo= table->key_info;
        /*
          Unique has nullable part. We need to check if there is any
          field in the BI image that is null and part of UNNI.
        */
        bool null_found= FALSE;
        for (uint i=0; i < keyinfo->user_defined_key_parts && !null_found; i++)
        {
          uint fieldnr= keyinfo->key_part[i].fieldnr - 1;
          Field **f= table->field+fieldnr;
          null_found= (*f)->is_null();
        }

        if (!null_found)
        {
          error= 0;
          goto end;
        }

        /* else fall through to index scan */
      }
    }

    is_index_scan=true;

    /*
      In case key is not unique, we still have to iterate over records found
      and find the one which is identical to the row given. A copy of the 
      record we are looking for is stored in record[1].
     */ 
    DBUG_PRINT("info",("non-unique index, scanning it to find matching record")); 
    /* We use this to test that the correct key is used in test cases. */
    DBUG_EXECUTE_IF("slave_crash_if_index_scan", abort(););

    while (record_compare(table, m_vers_from_plain))
    {
      while ((error= table->file->ha_index_next(table->record[0])))
      {
        DBUG_PRINT("info",("no record matching the given row found"));
        if (error == HA_ERR_END_OF_FILE)
          error= end_of_file_error(rgi);
        table->file->print_error(error, MYF(0));
        table->file->ha_index_end();
        goto end;
      }
    }
  }
  else
  {
    DBUG_PRINT("info",("locating record using table scan (rnd_next)"));
    /* We use this to test that the correct key is used in test cases. */
    DBUG_EXECUTE_IF("slave_crash_if_table_scan", abort(););

    /* We don't have a key: search the table using rnd_next() */
    if (unlikely((error= table->file->ha_rnd_init_with_error(1))))
    {
      DBUG_PRINT("info",("error initializing table scan"
                         " (ha_rnd_init returns %d)",error));
      goto end;
    }

    is_table_scan= true;

    /* Continue until we find the right record or have made a full loop */
    do
    {
      if (unlikely((error= table->file->ha_rnd_next(table->record[0]))))
        DBUG_PRINT("info", ("error: %s", HA_ERR(error)));
      switch (error) {

      case 0:
        DBUG_DUMP("record found", table->record[0], table->s->reclength);
        break;

      case HA_ERR_END_OF_FILE:
        error= end_of_file_error(rgi);
        DBUG_PRINT("info", ("Record not found"));
        table->file->ha_rnd_end();
        goto end;

      default:
        DBUG_PRINT("info", ("Failed to get next record"
                            " (rnd_next returns %d)",error));
        table->file->print_error(error, MYF(0));
        table->file->ha_rnd_end();
        goto end;
      }
    }
    while (record_compare(table, m_vers_from_plain));
    
    /* 
      Note: above record_compare will take into accout all record fields 
      which might be incorrect in case a partial row was given in the event
     */

    DBUG_ASSERT(error == HA_ERR_END_OF_FILE || error == 0);
  }

end:
  if (is_table_scan || is_index_scan)
    issue_long_find_row_warning(get_general_type_code(), m_table->alias.c_ptr(), 
                                is_index_scan, rgi);
  DBUG_RETURN(error);
}

#endif

/*
  Constructor used to build an event for writing to the binary log.
 */

Delete_rows_log_event::Delete_rows_log_event(THD *thd_arg, TABLE *tbl_arg,
                                             ulonglong tid,
                                             bool is_transactional)
  : Rows_log_event(thd_arg, tbl_arg, tid, tbl_arg->read_set, is_transactional,
                   DELETE_ROWS_EVENT_V1)
{
}

Delete_rows_compressed_log_event::Delete_rows_compressed_log_event(
                                           THD *thd_arg, TABLE *tbl_arg,
                                           ulonglong tid_arg,
                                           bool is_transactional)
  : Delete_rows_log_event(thd_arg, tbl_arg, tid_arg, is_transactional)
{
  m_type= DELETE_ROWS_COMPRESSED_EVENT_V1;
}

bool Delete_rows_compressed_log_event::write(Log_event_writer *writer)
{
  return Rows_log_event::write_compressed(writer);
}


#if defined(HAVE_REPLICATION)

int 
Delete_rows_log_event::do_before_row_operations(const rpl_group_info *rgi)
{
  /*
    Increment the global status delete count variable
   */
  if (get_flags(STMT_END_F))
    status_var_increment(thd->status_var.com_stat[SQLCOM_DELETE]);

  if (do_invoke_trigger())
    m_table->prepare_triggers_for_delete_stmt_or_event();

  return find_key(rgi);
}

int 
Delete_rows_log_event::do_after_row_operations(int error)
{
  m_table->file->ha_index_or_rnd_end();
  my_free(m_key);
  m_key= NULL;
  m_key_info= NULL;

  return error;
}

int Delete_rows_log_event::do_exec_row(rpl_group_info *rgi)
{
  int error;
  const char *tmp= thd->get_proc_info();
  char *message, msg[128];
  const LEX_CSTRING &table_name= m_table->s->table_name;
  const char quote_char=
    get_quote_char_for_identifier(thd, table_name.str, table_name.length);
  my_snprintf(msg, sizeof msg,
              "Delete_rows_log_event::find_row() on table %c%.*s%c",
              quote_char, int(table_name.length), table_name.str, quote_char);
  message= msg;
  const bool invoke_triggers= (m_table->triggers && do_invoke_trigger());
  DBUG_ASSERT(m_table != NULL);

#ifdef WSREP_PROC_INFO
  my_snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
              "Delete_rows_log_event::find_row(%lld) on table %c%.*s%c",
              (long long) wsrep_thd_trx_seqno(thd), quote_char,
              int(table_name.length), table_name.str,
              quote_char);
  message= thd->wsrep_info;
#endif /* WSREP_PROC_INFO */

  thd_proc_info(thd, message);
  if (likely(!(error= find_row(rgi))))
  {
    /*
      Delete the record found, located in record[0]
    */
    my_snprintf(msg, sizeof msg,
                "Delete_rows_log_event::ha_delete_row() on table %c%.*s%c",
                quote_char, int(table_name.length), table_name.str,
                quote_char);
    message= msg;
#ifdef WSREP_PROC_INFO
    snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
             "Delete_rows_log_event::ha_delete_row(%lld) on table %c%.*s%c",
             (long long) wsrep_thd_trx_seqno(thd), quote_char,
             int(table_name.length), table_name.str, quote_char);
    message= thd->wsrep_info;
#endif
    thd_proc_info(thd, message);

    bool trg_skip_row= false;
    if (invoke_triggers &&
        unlikely(process_triggers(TRG_EVENT_DELETE, TRG_ACTION_BEFORE, false,
                                  &trg_skip_row)))
      error= HA_ERR_GENERIC; // in case if error is not set yet
    if (likely(!error) && !trg_skip_row)
    {
      if (m_vers_from_plain && m_table->versioned(VERS_TIMESTAMP))
      {
        Field *end= m_table->vers_end_field();
        store_record(m_table, record[1]);
        end->set_time();
        error= m_table->file->ha_update_row(m_table->record[1],
                                            m_table->record[0]);
      }
      else
      {
        error= m_table->file->ha_delete_row(m_table->record[0]);
      }
    }
    if (invoke_triggers && likely(!error) && !trg_skip_row &&
        unlikely(process_triggers(TRG_EVENT_DELETE, TRG_ACTION_AFTER, false,
                                  nullptr)))
      error= HA_ERR_GENERIC; // in case if error is not set yet
    m_table->file->ha_index_or_rnd_end();
  }
  thd_proc_info(thd, tmp);
  return error;
}

#endif /* defined(HAVE_REPLICATION) */

#if defined(HAVE_REPLICATION)
uint8 Delete_rows_log_event::get_trg_event_map() const
{
  return trg2bit(TRG_EVENT_DELETE);
}
#endif

/**************************************************************************
	Update_rows_log_event member functions
**************************************************************************/

/*
  Constructor used to build an event for writing to the binary log.
 */
Update_rows_log_event::Update_rows_log_event(THD *thd_arg, TABLE *tbl_arg,
                                             ulonglong tid,
                                             bool is_transactional)
: Rows_log_event(thd_arg, tbl_arg, tid, tbl_arg->read_set, is_transactional,
                 UPDATE_ROWS_EVENT_V1)
{
  init(tbl_arg->rpl_write_set);
}

Update_rows_compressed_log_event::
Update_rows_compressed_log_event(THD *thd_arg, TABLE *tbl_arg,
                                 ulonglong tid, bool is_transactional)
: Update_rows_log_event(thd_arg, tbl_arg, tid, is_transactional)
{
  m_type = UPDATE_ROWS_COMPRESSED_EVENT_V1;
}

bool Update_rows_compressed_log_event::write(Log_event_writer *writer)
{
  return Rows_log_event::write_compressed(writer);
}

void Update_rows_log_event::init(MY_BITMAP const *cols)
{
  /* if my_bitmap_init fails, caught in is_valid() */
  if (likely(!my_bitmap_init(&m_cols_ai,
                          m_width <= sizeof(m_bitbuf_ai)*8 ? m_bitbuf_ai : NULL,
                          m_width)))
  {
    /* Cols can be zero if this is a dummy binrows event */
    if (likely(cols != NULL))
      bitmap_copy(&m_cols_ai, cols);
  }
}


#if defined(HAVE_REPLICATION)

int 
Update_rows_log_event::do_before_row_operations(const rpl_group_info *rgi)
{
  /*
    Increment the global status update count variable
  */
  if (get_flags(STMT_END_F))
    status_var_increment(thd->status_var.com_stat[SQLCOM_UPDATE]);

  int err;
  if ((err= find_key(rgi)))
    return err;

  if (do_invoke_trigger())
    m_table->prepare_triggers_for_update_stmt_or_event();

  return 0;
}

int 
Update_rows_log_event::do_after_row_operations(int error)
{
  /*error= ToDo:find out what this should really be, this triggers close_scan in nbd, returning error?*/
  m_table->file->ha_index_or_rnd_end();
  my_free(m_key); // Free for multi_malloc
  m_key= NULL;
  m_key_info= NULL;

  return error;
}

int
Update_rows_log_event::do_exec_row(rpl_group_info *rgi)
{
  const bool invoke_triggers= (m_table->triggers && do_invoke_trigger());
  const char *tmp= thd->get_proc_info();
  DBUG_ASSERT(m_table != NULL);
  char *message, msg[128];
  const LEX_CSTRING &table_name= m_table->s->table_name;
  const char quote_char=
    get_quote_char_for_identifier(thd, table_name.str, table_name.length);
  bool trg_skip_row= false;
  my_snprintf(msg, sizeof msg,
              "Update_rows_log_event::find_row() on table %c%.*s%c",
              quote_char, int(table_name.length), table_name.str, quote_char);
  message= msg;

#ifdef WSREP_PROC_INFO
  my_snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
              "Update_rows_log_event::find_row(%lld) on table %c%.*s%c",
              (long long) wsrep_thd_trx_seqno(thd), quote_char,
              int(table_name.length), table_name.str,
              quote_char);
  message= thd->wsrep_info;
#endif /* WSREP_PROC_INFO */

  thd_proc_info(thd, message);

  int error= find_row(rgi);
  if (unlikely(error))
  {
    /*
      We need to read the second image in the event of error to be
      able to skip to the next pair of updates
    */
    if ((m_curr_row= m_curr_row_end))
      unpack_current_row(rgi, &m_cols_ai);
    thd_proc_info(thd, tmp);
    return error;
  }

  const bool history_change= m_table->versioned() ?
    !m_table->vers_end_field()->is_max() : false;
  TABLE_LIST *tl= m_table->pos_in_table_list;
  uint8 trg_event_map_save= tl->trg_event_map;

  /*
    This is the situation after locating BI:

    ===|=== before image ====|=== after image ===|===
       ^                     ^
       m_curr_row            m_curr_row_end

    BI found in the table is stored in record[0]. We copy it to record[1]
    and unpack AI to record[0].
   */

  store_record(m_table,record[1]);

  m_curr_row= m_curr_row_end;
  my_snprintf(msg, sizeof msg,
              "Update_rows_log_event::unpack_current_row() on table %c%.*s%c",
              quote_char, int(table_name.length), table_name.str, quote_char);
  message= msg;
#ifdef WSREP_PROC_INFO
  my_snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
              "Update_rows_log_event::unpack_current_row(%lld) on table %c%.*s%c",
              (long long) wsrep_thd_trx_seqno(thd), quote_char,
              int(table_name.length), table_name.str, quote_char);
  message= thd->wsrep_info;
#endif /* WSREP_PROC_INFO */

  /* this also updates m_curr_row_end */
  thd_proc_info(thd, message);
  if (unlikely((error= unpack_current_row(rgi, &m_cols_ai))))
    goto err;
  if (m_table->s->long_unique_table)
    m_table->update_virtual_fields(m_table->file, VCOL_UPDATE_FOR_WRITE);

  /*
    Now we have the right row to update.  The old row (the one we're
    looking for) is in record[1] and the new row is in record[0].
  */
#ifndef HAVE_valgrind
  /*
    Don't print debug messages when running valgrind since they can
    trigger false warnings.
   */
  DBUG_PRINT("info",("Updating row in table"));
  DBUG_DUMP("old record", m_table->record[1], m_table->s->reclength);
  DBUG_DUMP("new values", m_table->record[0], m_table->s->reclength);
#endif

  my_snprintf(msg, sizeof msg,
              "Update_rows_log_event::ha_update_row() on table %c%.*s%c",
              quote_char, int(table_name.length), table_name.str, quote_char);
  message= msg;
#ifdef WSREP_PROC_INFO
  my_snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
              "Update_rows_log_event::ha_update_row(%lld) on table %c%.*s%c",
              (long long) wsrep_thd_trx_seqno(thd), quote_char,
              int(table_name.length), table_name.str, quote_char);
  message= thd->wsrep_info;
#endif /* WSREP_PROC_INFO */

  thd_proc_info(thd, message);
  if (invoke_triggers &&
      unlikely(process_triggers(TRG_EVENT_UPDATE, TRG_ACTION_BEFORE, true,
                                &trg_skip_row)))
  {
    error= HA_ERR_GENERIC; // in case if error is not set yet
    goto err;
  }

  if (trg_skip_row)
  {
    error= 0;
    goto err;
  }
  if (m_table->versioned())
  {
    if (m_table->versioned(VERS_TIMESTAMP))
    {
      if (m_vers_from_plain)
        m_table->vers_update_fields();
      m_table->vers_fix_old_timestamp(rgi);
    }
    if (!history_change && !m_table->vers_end_field()->is_max())
    {
      tl->trg_event_map|= trg2bit(TRG_EVENT_DELETE);
    }
  }
  error= m_table->file->ha_update_row(m_table->record[1], m_table->record[0]);
  tl->trg_event_map= trg_event_map_save;
  if (unlikely(error == HA_ERR_RECORD_IS_THE_SAME))
    error= 0;
  if (m_vers_from_plain && m_table->versioned(VERS_TIMESTAMP))
  {
    store_record(m_table, record[2]);
    error= vers_insert_history_row(m_table);
    restore_record(m_table, record[2]);
  }

  if (invoke_triggers && likely(!error) &&
      unlikely(process_triggers(TRG_EVENT_UPDATE, TRG_ACTION_AFTER, true,
                                nullptr)))
    error= HA_ERR_GENERIC; // in case if error is not set yet


err:
  thd_proc_info(thd, tmp);
  m_table->file->ha_index_or_rnd_end();
  return error;
}

#endif /* defined(HAVE_REPLICATION) */


#if defined(HAVE_REPLICATION)
uint8 Update_rows_log_event::get_trg_event_map() const
{
  return trg2bit(TRG_EVENT_UPDATE);
}
#endif


#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
void Incident_log_event::pack_info(Protocol *protocol)
{
  char buf[256];
  size_t bytes;
  if (m_message.length > 0)
    bytes= my_snprintf(buf, sizeof(buf), "#%d (%s)",
                       m_incident, description());
  else
    bytes= my_snprintf(buf, sizeof(buf), "#%d (%s): %s",
                       m_incident, description(), m_message.str);
  protocol->store(buf, bytes, &my_charset_bin);
}
#endif

#if defined(WITH_WSREP)
/*
  read the first event from (*buf). The size of the (*buf) is (*buf_len).
  At the end (*buf) is shitfed to point to the following event or NULL and
  (*buf_len) will be changed to account just being read bytes of the 1st event.
*/
#define WSREP_MAX_ALLOWED_PACKET 1024*1024*1024 // current protocol max

Log_event* wsrep_read_log_event(
  char **arg_buf, size_t *arg_buf_len,
  const Format_description_log_event *description_event)
{
  uchar *head= (uchar*) (*arg_buf);
  uint data_len = uint4korr(head + EVENT_LEN_OFFSET);
  const char *error= 0;
  Log_event *res=  0;
  DBUG_ENTER("wsrep_read_log_event");

  if (data_len > WSREP_MAX_ALLOWED_PACKET)
  {
    error = "Event too big";
    goto err;
  }

  res= Log_event::read_log_event(head, data_len, &error, description_event,
                                 false);

err:
  if (!res)
  {
    DBUG_ASSERT(error != 0);
    sql_print_error("Error in Log_event::read_log_event(): "
                    "'%s', data_len: %u, event_type: %d",
		    error, data_len, (int) head[EVENT_TYPE_OFFSET]);
  }
  (*arg_buf)+= data_len;
  (*arg_buf_len)-= data_len;
  DBUG_RETURN(res);
}
#endif


#if defined(HAVE_REPLICATION)
int Incident_log_event::do_apply_event(rpl_group_info *rgi)
{
  Relay_log_info const *rli= rgi->rli;
  DBUG_ENTER("Incident_log_event::do_apply_event");

  if (ignored_error_code(ER_SLAVE_INCIDENT))
  {
    DBUG_PRINT("info", ("Ignoring Incident"));
    DBUG_RETURN(0);
  }

  rli->report(ERROR_LEVEL, ER_SLAVE_INCIDENT, NULL,
              ER_THD(rgi->thd, ER_SLAVE_INCIDENT),
              description(),
              m_message.length > 0 ? m_message.str : "<none>");
  DBUG_RETURN(1);
}
#endif


bool
Incident_log_event::write_data_header(Log_event_writer *writer)
{
  DBUG_ENTER("Incident_log_event::write_data_header");
  DBUG_PRINT("enter", ("m_incident: %d", m_incident));
  uchar buf[sizeof(int16)];
  int2store(buf, (int16) m_incident);
  DBUG_RETURN(write_data(writer, buf, sizeof(buf)));
}

bool
Incident_log_event::write_data_body(Log_event_writer *writer)
{
  uchar tmp[1];
  DBUG_ENTER("Incident_log_event::write_data_body");
  tmp[0]= (uchar) m_message.length;
  DBUG_RETURN(write_data(writer, tmp, sizeof(tmp)) ||
              write_data(writer, m_message.str, m_message.length));
}


#if defined(MYSQL_SERVER) && defined(HAVE_REPLICATION)
/* Pack info for its unrecognized ignorable event */
void Ignorable_log_event::pack_info(Protocol *protocol)
{
  char buf[256];
  size_t bytes;
  bytes= my_snprintf(buf, sizeof(buf), "# Ignorable event type %d (%s)",
                     number, description);
  protocol->store(buf, bytes, &my_charset_bin);
}
#endif

#if defined(HAVE_REPLICATION)
Heartbeat_log_event::Heartbeat_log_event(const uchar *buf, uint event_len,
                    const Format_description_log_event* description_event)
  :Log_event(buf, description_event)
{
  uint8 header_size= description_event->common_header_len;
  if (log_pos == 0)
  {
    log_pos= uint8korr(buf + header_size);
    log_ident= buf + header_size + HB_SUB_HEADER_LEN;
    ident_len= event_len - (header_size + HB_SUB_HEADER_LEN);
  }
  else
  {
    log_ident= buf + header_size;
    ident_len = event_len - header_size;
  }
}
#endif


/**
   Check if we should write event to the relay log

   This is used to skip events that is only supported by MySQL

   Return:
   0 ok
   1 Don't write event
*/

bool event_that_should_be_ignored(const uchar *buf)
{
  uint event_type= buf[EVENT_TYPE_OFFSET];
  if (event_type == GTID_LOG_EVENT ||
      event_type == ANONYMOUS_GTID_LOG_EVENT ||
      event_type == PREVIOUS_GTIDS_LOG_EVENT ||
      event_type == TRANSACTION_CONTEXT_EVENT ||
      event_type == VIEW_CHANGE_EVENT ||
      (uint2korr(buf + FLAGS_OFFSET) & LOG_EVENT_IGNORABLE_F))
    return 1;
  return 0;
}
