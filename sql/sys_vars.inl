/* Copyright (c) 2002, 2011, Oracle and/or its affiliates.
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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/**
  @file
  "private" interface to sys_var - server configuration variables.

  This header is included only by the file that contains declarations
  of sys_var variables (sys_vars.cc).
*/

#include "sys_vars_shared.h"
#include <my_getopt.h>
#include <my_bit.h>
#include <my_dir.h>
#include "keycaches.h"
#include "strfunc.h"
#include "tztime.h"     // my_tz_find, my_tz_SYSTEM, struct Time_zone
#include "rpl_mi.h" // For Multi-Source Replication
#include "debug_sync.h"
#include "sql_acl.h"    // check_global_access()
#include "optimizer_defaults.h"   // create_optimizer_costs

/*
  a set of mostly trivial (as in f(X)=X) defines below to make system variable
  declarations more readable
*/
#define VALID_RANGE(X,Y) X,Y
#define DEFAULT(X) X
#define BLOCK_SIZE(X) X
#define COST_ADJUST(X) X
#define GLOBAL_VAR(X) sys_var::GLOBAL, (((char*)&(X))-(char*)&global_system_variables), sizeof(X)
#define SESSION_VAR(X) sys_var::SESSION, offsetof(SV, X), sizeof(((SV *)0)->X)
#define SESSION_ONLY(X) sys_var::ONLY_SESSION, offsetof(SV, X), sizeof(((SV *)0)->X)
#define NO_CMD_LINE CMD_LINE(NO_ARG, sys_var::NO_GETOPT)
#define CMD_LINE_HELP_ONLY CMD_LINE(NO_ARG, sys_var::GETOPT_ONLY_HELP)
/*
  the define below means that there's no *second* mutex guard,
  LOCK_global_system_variables always guards all system variables
*/
#define NO_MUTEX_GUARD ((PolyLock*)0)
#define IN_BINLOG sys_var::SESSION_VARIABLE_IN_BINLOG
#define NOT_IN_BINLOG sys_var::VARIABLE_NOT_IN_BINLOG
#define ON_READ(X) X
#define ON_CHECK(X) X
#define ON_UPDATE(X) X
#define READ_ONLY sys_var::READONLY+
#define AUTO_SET sys_var::AUTO_SET+
// this means that Sys_var_charptr initial value was malloc()ed
#define PREALLOCATED sys_var::ALLOCATED+
#define PARSED_EARLY sys_var::PARSE_EARLY+
#define NO_SET_STMT sys_var::NO_SET_STATEMENT+

extern const char *UNUSED_HELP;

/*
  Sys_var_bit meaning is reversed, like in
  @@foreign_key_checks <-> OPTION_NO_FOREIGN_KEY_CHECKS
*/
#define REVERSE(X) ~(X)
#define DEPRECATED(V, REPL) (check_deprecated_version<V>(), REPL)
#define DEPRECATED_NO_REPLACEMENT(V) DEPRECATED(V, "")

#define session_var(THD, TYPE) (*(TYPE*)session_var_ptr(THD))
#define global_var(TYPE) (*(TYPE*)global_var_ptr())

#if SIZEOF_OFF_T > 4
#define GET_HA_ROWS GET_ULL
#else
#define GET_HA_ROWS GET_ULONG
#endif

// Disable warning caused by SESSION_VAR() macro
#ifdef __clang__
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif

/*
  special assert for sysvars. Tells the name of the variable,
  and fails even in non-debug builds.

  It is supposed to be used *only* in Sys_var* constructors,
  and has name_arg hard-coded to prevent incorrect usage.
*/
#define SYSVAR_ASSERT(X)                                                \
    while(!(X))                                                         \
    {                                                                   \
      fprintf(stderr, "Sysvar '%s' failed '%s'\n", name_arg, #X);       \
      DBUG_ASSERT_NO_ASSUME(0);                                                   \
      exit(255);                                                        \
    }


static const char *bool_values[3]= {"OFF", "ON", 0};
TYPELIB bool_typelib= CREATE_TYPELIB_FOR(bool_values);


template<class BASE, privilege_t GLOBAL_PRIV, privilege_t SESSION_PRIV>
class Sys_var_on_access: public BASE
{
  using BASE::BASE;
  bool on_check_access_global(THD *thd) const override
  {
    return check_global_access(thd, GLOBAL_PRIV);
  }
  bool on_check_access_session(THD *thd) const override
  {
    return check_global_access(thd, SESSION_PRIV);
  }
};


template<class BASE, privilege_t GLOBAL_PRIV>
class Sys_var_on_access_global: public BASE
{
  using BASE::BASE;
  bool on_check_access_global(THD *thd) const override
  {
    return check_global_access(thd, GLOBAL_PRIV);
  }
};


template<class BASE, privilege_t SESSION_PRIV>
class Sys_var_on_access_session: public BASE
{
  using BASE::BASE;
  bool on_check_access_session(THD *thd) const override
  {
    return check_global_access(thd, SESSION_PRIV);
  }
};


/**
  A small wrapper class to pass getopt arguments as a pair
  to the Sys_var_* constructors. It improves type safety and helps
  to catch errors in the argument order.
*/
struct CMD_LINE
{
  int id;
  enum get_opt_arg_type arg_type;
  CMD_LINE(enum get_opt_arg_type getopt_arg_type, int getopt_id=0)
    : id(getopt_id), arg_type(getopt_arg_type) {}
};

/**
  Sys_var_integer template is used to generate Sys_var_* classes
  for variables that represent the value as an integer number.
  They are Sys_var_uint, Sys_var_ulong, Sys_var_harows, Sys_var_ulonglong,
  Sys_var_int.

  An integer variable has a minimal and maximal values, and a "block_size"
  (any valid value of the variable must be divisible by the block_size).

  Class specific constructor arguments: min, max, block_size
  Backing store: int, uint, ulong, ha_rows, ulonglong, depending on the class
*/
template <typename T, ulong ARGT, enum enum_mysql_show_type SHOWT>
class Sys_var_integer: public sys_var
{
public:
  Sys_var_integer(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          T min_val, T max_val, T def_val, uint block_size, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOWT, def_val, lock, binlog_status_arg,
              on_check_func, on_update_func, substitute)
  {
    option.var_type|= ARGT;
    option.min_value= min_val;
    option.max_value= max_val;
    option.block_size= block_size;
    if ((option.u_max_value= (uchar**) max_var_ptr()))
    {
      *((T*) option.u_max_value)= max_val;
    }

    global_var(T)= def_val;
    SYSVAR_ASSERT(size == sizeof(T));
    SYSVAR_ASSERT(min_val < max_val);
    SYSVAR_ASSERT(min_val <= def_val);
    SYSVAR_ASSERT(max_val >= def_val);
    SYSVAR_ASSERT(block_size > 0);
    SYSVAR_ASSERT(def_val % block_size == 0);
  }
  bool do_check(THD *thd, set_var *var) override
  {
    my_bool fixed= FALSE, unused;
    longlong v= var->value->val_int();

    if ((ARGT == GET_HA_ROWS) || (ARGT == GET_UINT) ||
        (ARGT == GET_ULONG)   || (ARGT == GET_ULL))
    {
      ulonglong uv;

      /*
        if the value is signed and negative,
        and a variable is unsigned, it is set to zero
      */
      if ((fixed= (!var->value->unsigned_flag && v < 0)))
        uv= 0;
      else
        uv= v;

      var->save_result.ulonglong_value=
        getopt_ull_limit_value(uv, &option, &unused);

      if (max_var_ptr() && (T)var->save_result.ulonglong_value > get_max_var())
        var->save_result.ulonglong_value= get_max_var();

      fixed= fixed || var->save_result.ulonglong_value != uv;
    }
    else
    {
      /*
        if the value is unsigned and has the highest bit set
        and a variable is signed, it is set to max signed value
      */
      if ((fixed= (var->value->unsigned_flag && v < 0)))
        v= LONGLONG_MAX;

      var->save_result.longlong_value=
        getopt_ll_limit_value(v, &option, &unused);

      if (max_var_ptr() && (T)var->save_result.longlong_value > get_max_var())
        var->save_result.longlong_value= get_max_var();

      fixed= fixed || var->save_result.longlong_value != v;
    }
    return throw_bounds_warning(thd, name.str, fixed,
                                var->value->unsigned_flag, v);
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, T)= static_cast<T>(var->save_result.ulonglong_value);
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(T)= static_cast<T>(var->save_result.ulonglong_value);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= (ulonglong)*(T*)global_value_ptr(thd, 0); }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= option.def_value; }
  private:
  T get_max_var() { return *((T*) max_var_ptr()); }
  const uchar *default_value_ptr(THD *thd) const override { return (uchar*) &option.def_value; }
};

typedef Sys_var_integer<int, GET_INT, SHOW_SINT> Sys_var_int;
typedef Sys_var_integer<uint, GET_UINT, SHOW_UINT> Sys_var_uint;
typedef Sys_var_integer<ulong, GET_ULONG, SHOW_ULONG> Sys_var_ulong;
typedef Sys_var_integer<ha_rows, GET_HA_ROWS, SHOW_HA_ROWS> Sys_var_harows;
typedef Sys_var_integer<ulonglong, GET_ULL, SHOW_ULONGLONG> Sys_var_ulonglong;
typedef Sys_var_integer<long, GET_LONG, SHOW_SLONG> Sys_var_long;


template<> const uchar *Sys_var_int::default_value_ptr(THD *thd) const
{
  thd->sys_var_tmp.int_value= (int)option.def_value;
  return (uchar*) &thd->sys_var_tmp.int_value;
}

template<> const uchar *Sys_var_uint::default_value_ptr(THD *thd) const
{
  thd->sys_var_tmp.uint_value= (uint)option.def_value;
  return (uchar*) &thd->sys_var_tmp.uint_value;
}

template<> const uchar *Sys_var_long::default_value_ptr(THD *thd) const
{
  thd->sys_var_tmp.long_value= (long)option.def_value;
  return (uchar*) &thd->sys_var_tmp.long_value;
}

template<> const uchar *Sys_var_ulong::default_value_ptr(THD *thd) const
{
  thd->sys_var_tmp.ulong_value= (ulong)option.def_value;
  return (uchar*) &thd->sys_var_tmp.ulong_value;
}


/**
  Helper class for variables that take values from a TYPELIB
*/
class Sys_var_typelib: public sys_var
{
protected:
  TYPELIB typelib;
  virtual bool check_maximum(THD *thd, set_var *var,
                             const char *c_val, longlong i_val)
    { return FALSE; }
public:
  Sys_var_typelib(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off,
          CMD_LINE getopt,
          SHOW_TYPE show_val_type_arg, const char *values[],
          ulonglong def_val, PolyLock *lock,
          enum binlog_status_enum binlog_status_arg,
          on_check_function on_check_func, on_update_function on_update_func,
          const char *substitute)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, show_val_type_arg, def_val, lock,
              binlog_status_arg, on_check_func,
              on_update_func, substitute)
  {
    for (typelib.count= 0; values[typelib.count]; typelib.count++) /*no-op */;
    typelib.name="";
    typelib.type_names= values;
    typelib.type_lengths= 0;    // only used by Fields_enum and Field_set
    option.typelib= &typelib;
  }
  bool do_check(THD *thd, set_var *var) override // works for enums and my_bool
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String str(buff, sizeof(buff), system_charset_info), *res;

    if (var->value->result_type() == STRING_RESULT)
    {
      /*
        Convert from the expression character set to ascii.
        This is OK, as typelib values cannot have non-ascii characters.
      */
      if (!(res= var->value->val_str_ascii(&str)))
        return true;
      else
      if (!(var->save_result.ulonglong_value=
            find_type(&typelib, res->ptr(), res->length(), false)))
        return true;
      else
        var->save_result.ulonglong_value--;
      return check_maximum(thd, var, res->ptr(), 0);
    }

    longlong tmp=var->value->val_int();
    if (tmp < 0 || tmp >= typelib.count)
      return true;
    var->save_result.ulonglong_value= tmp;
    return check_maximum(thd, var, 0, tmp);
  }
};

/**
  The class for ENUM variables - variables that take one value from a fixed
  list of values. 

  Class specific constructor arguments:
    char* values[]    - 0-terminated list of strings of valid values

  Backing store: ulong

  @note
  Do *not* use "enum FOO" variables as a backing store, there is no
  guarantee that sizeof(enum FOO) == sizeof(uint), there is no guarantee
  even that sizeof(enum FOO) == sizeof(enum BAR)
*/
class Sys_var_enum: public Sys_var_typelib
{
public:
  Sys_var_enum(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *values[], uint def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_typelib(name_arg, comment, flag_args, off, getopt,
                      SHOW_CHAR, values, def_val, lock,
                      binlog_status_arg, on_check_func, on_update_func,
                      substitute)
  {
    option.var_type|= GET_ENUM;
    option.min_value= 0;
    option.max_value= ULONG_MAX;
    global_var(ulong)= def_val;
    if ((option.u_max_value= (uchar**)max_var_ptr()))
    {
      *((ulong *) option.u_max_value)= ULONG_MAX;
    }
    SYSVAR_ASSERT(def_val < typelib.count);
    SYSVAR_ASSERT(size == sizeof(ulong));
  }
  bool check_maximum(THD *thd, set_var *var,
                     const char *c_val, longlong i_val) override
  {
    if (!max_var_ptr() ||
        var->save_result.ulonglong_value <= get_max_var())
      return FALSE;
    var->save_result.ulonglong_value= get_max_var();

    return c_val ? throw_bounds_warning(thd, name.str, c_val) :
                   throw_bounds_warning(thd, name.str, TRUE,
                                        var->value->unsigned_flag, i_val);
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, ulong)= static_cast<ulong>(var->save_result.ulonglong_value);
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(ulong)= static_cast<ulong>(var->save_result.ulonglong_value);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= global_var(ulong); }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= option.def_value; }
  const uchar *valptr(THD *thd, ulong val) const
  { return reinterpret_cast<const uchar*>(typelib.type_names[val]); }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, ulong)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(ulong)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, (ulong)option.def_value); }

  ulong get_max_var() { return *((ulong *) max_var_ptr()); }
};

/**
  The class for boolean variables - a variant of ENUM variables
  with the fixed list of values of { OFF , ON }

  Backing store: my_bool
*/
class Sys_var_mybool: public Sys_var_typelib
{
public:
  Sys_var_mybool(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          my_bool def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_typelib(name_arg, comment, flag_args, off, getopt,
                      SHOW_MY_BOOL, bool_values, def_val, lock,
                      binlog_status_arg, on_check_func, on_update_func,
                      substitute)
  {
    option.var_type|= GET_BOOL;
    global_var(my_bool)= def_val;
    SYSVAR_ASSERT(def_val < 2);
    SYSVAR_ASSERT(getopt.arg_type == OPT_ARG || getopt.id < 0);
    SYSVAR_ASSERT(size == sizeof(my_bool));
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, my_bool)= var->save_result.ulonglong_value != 0;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(my_bool)= var->save_result.ulonglong_value != 0;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= (ulonglong)*(my_bool *)global_value_ptr(thd, 0); }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= option.def_value; }
  const uchar *default_value_ptr(THD *thd) const override
  {
    thd->sys_var_tmp.my_bool_value=(my_bool) option.def_value;
    return (uchar*) &thd->sys_var_tmp.my_bool_value;
  }
};

/**
  The class for string variables. The string can be in character_set_filesystem
  or in character_set_system. The string can be allocated with my_malloc()
  or not. The state of the initial value is specified in the constructor,
  after that it's managed automatically. The value of NULL is supported.

  Backing store: char*

  @note

  Note that the memory management for SESSION_VAR's is manual, the
  value must be strdup'ed in THD::init() and freed in
  plugin_thdvar_cleanup(), see e.g. redirect_url. TODO: it should be
  done automatically when we'll have more session string variables to
  justify it. Maybe some kind of a loop over all variables, like
  sys_var_end() in set_var.cc?
*/
class Sys_var_charptr: public sys_var
{
  const size_t max_length= 2000;
public:
  Sys_var_charptr(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR_PTR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  {
    /*
     use GET_STR_ALLOC - if ALLOCATED it must be *always* allocated,
     otherwise (GET_STR) you'll never know whether to free it or not.
     (think of an exit because of an error right after my_getopt)
    */
    option.var_type|= (flags & ALLOCATED) ? GET_STR_ALLOC : GET_STR;
    global_var(const char*)= def_val;
    SYSVAR_ASSERT(size == sizeof(char *));
  }
  void cleanup() override
  {
    if (flags & ALLOCATED)
    {
      my_free(global_var(char*));
      global_var(char *)= NULL;
    }
    flags&= ~ALLOCATED;
  }
  static bool do_string_check(THD *thd, set_var *var, CHARSET_INFO *charset)
  {
    char buff[STRING_BUFFER_USUAL_SIZE], buff2[STRING_BUFFER_USUAL_SIZE];
    String str(buff, sizeof(buff), charset);
    String str2(buff2, sizeof(buff2), charset), *res;

    if (!(res=var->value->val_str(&str)))
    {
      var->save_result.string_value.str= 0;
      var->save_result.string_value.length= 0; // safety
    }
    else
    {
      uint32 unused;
      if (String::needs_conversion(res->length(), res->charset(),
                                   charset, &unused))
      {
        uint errors;
        str2.copy(res->ptr(), res->length(), res->charset(), charset,
                  &errors);
        res=&str2;

      }
      var->save_result.string_value.str= thd->strmake(res->ptr(), res->length());
      var->save_result.string_value.length= res->length();
    }

    return false;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    if (do_string_check(thd, var, charset(thd)))
      return true;
    if (var->save_result.string_value.length > max_length)
    {
      my_error(ER_WRONG_STRING_LENGTH, MYF(0), var->save_result.string_value.str,
               name.str, (int) max_length);
      return true;
    }
    return false;
  }
  char *update_prepare(set_var *var, myf my_flags)
  {
    char *new_val, *ptr= var->save_result.string_value.str;
    size_t len=var->save_result.string_value.length;
    if (ptr)
    {
      new_val= (char*)my_memdup(key_memory_Sys_var_charptr_value,
                                ptr, len+1, my_flags);
      if (!new_val) return 0;
      new_val[len]=0;
    }
    else
      new_val= 0;
    return new_val;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    char *new_val= update_prepare(var, MYF(MY_WME | MY_THREAD_SPECIFIC));
    my_free(session_var(thd, char*));
    session_var(thd, char*)= new_val;
    return (new_val == 0 && var->save_result.string_value.str != 0);
  }
  void global_update_finish(char *new_val)
  {
    if (flags & ALLOCATED)
      my_free(global_var(char*));
    flags|= ALLOCATED;
    global_var(char*)= new_val;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    char *new_val= update_prepare(var, MYF(MY_WME));
    global_update_finish(new_val);
    return (new_val == 0 && var->save_result.string_value.str != 0);
  }
  void session_save_default(THD *, set_var *var) override
  {
    var->save_result.string_value.str= global_var(char*);
    var->save_result.string_value.length=
      strlen(var->save_result.string_value.str);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    char *ptr= (char*)(intptr)option.def_value;
    var->save_result.string_value.str= ptr;
    var->save_result.string_value.length= ptr ? strlen(ptr) : 0;
  }
};

class Sys_var_charptr_fscs: public Sys_var_charptr
{
  using Sys_var_charptr::Sys_var_charptr;
public:
  CHARSET_INFO *charset(THD *thd) const override
  {
    return thd->variables.character_set_filesystem;
  }
};

#ifndef EMBEDDED_LIBRARY
class Sys_var_sesvartrack: public Sys_var_charptr
{
public:
  Sys_var_sesvartrack(const char *name_arg,
                      const char *comment,
                      CMD_LINE getopt,
                      const char *def_val, PolyLock *lock= 0) :
    Sys_var_charptr(name_arg, comment,
                    SESSION_VAR(session_track_system_variables),
                    getopt, def_val, lock,
                    VARIABLE_NOT_IN_BINLOG, 0, 0, 0)
    {}
  bool do_check(THD *thd, set_var *var) override
  {
     if (Sys_var_charptr::do_string_check(thd, var, charset(thd)) ||
         sysvartrack_validate_value(thd, var->save_result.string_value.str,
                                    var->save_result.string_value.length))
       return TRUE;
     return FALSE;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    char *new_val= update_prepare(var, MYF(MY_WME));
    if (new_val)
    {
      if (sysvartrack_global_update(thd, new_val,
                                    var->save_result.string_value.length))
      {
        if (new_val)
          my_free(new_val);
        new_val= 0;
      }
    }
    global_update_finish(new_val);
    return (new_val == 0 && var->save_result.string_value.str != 0);
  }
  bool session_update(THD *thd, set_var *var) override
  { return thd->session_tracker.sysvars.update(thd, var); }
  void session_save_default(THD *thd, set_var *var) override
  {
     var->save_result.string_value.str= global_var(char*);
     var->save_result.string_value.length=
       strlen(var->save_result.string_value.str);
     /* parse and feel list with default values */
     if (thd)
     {
#ifdef DBUG_ASSERT_EXISTS
       bool res=
#endif
         sysvartrack_validate_value(thd,
                                    var->save_result.string_value.str,
                                    var->save_result.string_value.length);
       DBUG_ASSERT_NO_ASSUME(res == 0);
     }
  }
};
#endif //EMBEDDED_LIBRARY


class Sys_var_proxy_user: public sys_var
{
public:
  Sys_var_proxy_user(const char *name_arg, const char *comment)
    : sys_var(&all_sys_vars, name_arg, comment,
              sys_var::READONLY+sys_var::ONLY_SESSION, 0, NO_GETOPT,
              NO_ARG, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL)
  {
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { DBUG_ASSERT(FALSE); }
  void global_save_default(THD *thd, set_var *var) override
  { DBUG_ASSERT(FALSE); }
protected:
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return thd->security_ctx->proxy_user[0] ?
      (uchar *) &(thd->security_ctx->proxy_user[0]) : NULL;
  }
};

class Sys_var_external_user : public Sys_var_proxy_user
{
public:
  Sys_var_external_user(const char *name_arg, const char *comment_arg)
    : Sys_var_proxy_user (name_arg, comment_arg)
  {}

protected:
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return (uchar*)thd->security_ctx->external_user;
  }
};

class Master_info;
class Sys_var_rpl_filter: public sys_var
{
private:
  int opt_id;
  privilege_t m_access_global;

public:
  Sys_var_rpl_filter(const char *name, int getopt_id, const char *comment,
                     privilege_t access_global)
    : sys_var(&all_sys_vars, name, comment, sys_var::GLOBAL, 0, NO_GETOPT,
              NO_ARG, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL), opt_id(getopt_id),
      m_access_global(access_global)
  {
    option.var_type|= GET_STR | GET_ASK_ADDR;
  }

  bool do_check(THD *thd, set_var *var) override
  {
    return Sys_var_charptr::do_string_check(thd, var, charset(thd));
  }
  void session_save_default(THD *, set_var *) override
  { DBUG_ASSERT(FALSE); }

  void global_save_default(THD *thd, set_var *var) override
  {
    char *ptr= (char*)(intptr)option.def_value;
    var->save_result.string_value.str= ptr;
    var->save_result.string_value.length= ptr ? strlen(ptr) : 0;
  }

  bool session_update(THD *, set_var *) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }

  bool global_update(THD *thd, set_var *var) override;

  bool on_check_access_global(THD *thd) const override
  {
    return check_global_access(thd, m_access_global);
  }

protected:
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base)
    const override;
  bool set_filter_value(const char *value, Master_info *mi);
};

class Sys_var_binlog_filter: public sys_var
{
private:
  int opt_id;
  privilege_t m_access_global;

public:
  Sys_var_binlog_filter(const char *name, int getopt_id, const char *comment,
                     privilege_t access_global)
    : sys_var(&all_sys_vars, name, comment, sys_var::READONLY+sys_var::GLOBAL, 0, NO_GETOPT,
              NO_ARG, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL), opt_id(getopt_id),
      m_access_global(access_global)
  {
    option.var_type|= GET_STR;
  }

  bool do_check(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  void session_save_default(THD *, set_var *) override
  { DBUG_ASSERT(FALSE); }

  void global_save_default(THD *thd, set_var *var) override
  { DBUG_ASSERT(FALSE); }

  bool session_update(THD *, set_var *) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }

  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }

  bool on_check_access_global(THD *thd) const override
  {
    return check_global_access(thd, m_access_global);
  }

  protected:
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base)
    const override;
};

/**
  The class for string variables. Useful for strings that aren't necessarily
  \0-terminated. Otherwise the same as Sys_var_charptr.

  Backing store: LEX_CSTRING

  @note
  Behaves exactly as Sys_var_charptr, only the backing store is
  different.

  Note that for global variables handle_options() only sets the
  pointer, whereas the length must be updated manually to match, which
  is done in mysqld.cc. See e.g. opt_init_connect. TODO: it should be
  done automatically when we'll have more Sys_var_lexstring variables
  to justify it. Maybe some kind of a loop over all variables, like
  sys_var_end() in set_var.cc?

  Note that as a subclass of Sys_var_charptr, the memory management
  for session Sys_var_lexstring's is manual too, see notes of
  Sys_var_charptr and for example default_master_connection.
*/
class Sys_var_lexstring: public Sys_var_charptr
{
public:
  Sys_var_lexstring(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_charptr(name_arg, comment, flag_args, off, sizeof(char*),
              getopt, def_val, lock, binlog_status_arg,
              on_check_func, on_update_func, substitute)
  {
    global_var(LEX_CSTRING).length= strlen(def_val);
    SYSVAR_ASSERT(size == sizeof(LEX_CSTRING));
    *const_cast<SHOW_TYPE*>(&show_val_type)= SHOW_LEX_STRING;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    if (Sys_var_charptr::global_update(thd, var))
      return true;
    global_var(LEX_CSTRING).length= var->save_result.string_value.length;
    return false;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    if (Sys_var_charptr::session_update(thd, var))
      return true;
    session_var(thd, LEX_CSTRING).length= var->save_result.string_value.length;
    return false;
  }
};

#ifndef DBUG_OFF
/**
  @@session.debug_dbug and @@global.debug_dbug variables.

  @@dbug variable differs from other variables in one aspect:
  if its value is not assigned in the session, it "points" to the global
  value, and so when the global value is changed, the change
  immediately takes effect in the session.

  This semantics is intentional, to be able to debug one session from
  another.
*/
class Sys_var_dbug: public sys_var
{
public:
  Sys_var_dbug(const char *name_arg,
               const char *comment, int flag_args,
               CMD_LINE getopt,
               const char *def_val, PolyLock *lock=0,
               enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
               on_check_function on_check_func=0,
               on_update_function on_update_func=0,
               const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args,
              (char*)&current_dbug_option-(char*)&global_system_variables, getopt.id,
              getopt.arg_type, SHOW_CHAR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  { option.var_type|= GET_STR; }
  bool do_check(THD *thd, set_var *var) override
  {
    bool rc= Sys_var_charptr::do_string_check(thd, var, charset(thd));
    if (var->save_result.string_value.str == nullptr)
      var->save_result.string_value.str= const_cast<char*>("");
    return rc;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    const char *val= var->save_result.string_value.str;
    if (!var->value)
      DBUG_POP();
    else
      DBUG_SET(val);
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    const char *val= var->save_result.string_value.str;
    DBUG_SET_INITIAL(val);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { }
  void global_save_default(THD *thd, set_var *var) override
  {
    char *ptr= (char*)(intptr)option.def_value;
    var->save_result.string_value.str= ptr;
    var->save_result.string_value.length= safe_strlen(ptr);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    char buf[256];
    DBUG_EXPLAIN(buf, sizeof(buf));
    return (uchar*) thd->strdup(buf);
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    char buf[256];
    DBUG_EXPLAIN_INITIAL(buf, sizeof(buf));
    return (uchar*) thd->strdup(buf);
  }
  const uchar *default_value_ptr(THD *thd) const override
  { return (uchar*)""; }
};
#endif

#define KEYCACHE_VAR(X) GLOBAL_VAR(dflt_key_cache_var.X)
#define keycache_var_ptr(KC, OFF) (((uchar*)(KC))+(OFF))
#define keycache_var(KC, OFF) (*(ulonglong*)keycache_var_ptr(KC, OFF))
typedef bool (*keycache_update_function)(THD *, KEY_CACHE *, ptrdiff_t, ulonglong);

/**
  The class for keycache_* variables. Supports structured names,
  keycache_name.variable_name.

  Class specific constructor arguments:
    everything derived from Sys_var_ulonglong

  Backing store: ulonglong

  @note these variables can be only GLOBAL
*/
class Sys_var_keycache: public Sys_var_ulonglong
{
  keycache_update_function keycache_update;
public:
  Sys_var_keycache(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          ulonglong min_val, ulonglong max_val, ulonglong def_val,
          uint block_size, PolyLock *lock,
          enum binlog_status_enum binlog_status_arg,
          on_check_function on_check_func,
          keycache_update_function on_update_func,
          const char *substitute=0)
    : Sys_var_ulonglong(name_arg, comment, flag_args, off, size,
              getopt, min_val, max_val, def_val,
              block_size, lock, binlog_status_arg, on_check_func, 0,
              substitute),
    keycache_update(on_update_func)
  {
    option.var_type|= GET_ASK_ADDR;
    option.value= (uchar**)1; // crash me, please
    // fix an offset from global_system_variables to be an offset in KEY_CACHE
    offset= global_var_ptr() - (uchar*)dflt_key_cache;
    SYSVAR_ASSERT(scope() == GLOBAL);
  }
  bool global_update(THD *thd, set_var *var) override
  {
    ulonglong new_value= var->save_result.ulonglong_value;
    LEX_CSTRING *base_name= &var->base;
    KEY_CACHE *key_cache;

    /* If no basename, assume it's for the key cache named 'default' */
    if (!base_name->length)
      base_name= &default_base;

    key_cache= get_key_cache(base_name);

    if (!key_cache)
    {                                           // Key cache didn't exists */
      if (!new_value)                           // Tried to delete cache
        return false;                           // Ok, nothing to do
      if (!(key_cache= create_key_cache(base_name->str, base_name->length)))
        return true;
    }

    /**
      Abort if some other thread is changing the key cache
      @todo This should be changed so that we wait until the previous
      assignment is done and then do the new assign
    */
    if (key_cache->in_init)
      return true;

    return keycache_update(thd, key_cache, offset, new_value);
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    KEY_CACHE *key_cache= get_key_cache(base);
    if (!key_cache)
      key_cache= &zero_key_cache;
    return keycache_var_ptr(key_cache, offset);
  }
};

static bool update_buffer_size(THD *thd, KEY_CACHE *key_cache,
                               ptrdiff_t offset, ulonglong new_value)
{
  bool error= false;
  DBUG_ASSERT(offset == offsetof(KEY_CACHE, param_buff_size));

  if (new_value == 0)
  {
    if (key_cache == dflt_key_cache)
    {
      my_error(ER_WARN_CANT_DROP_DEFAULT_KEYCACHE, MYF(0));
      return true;
    }

    if (key_cache->key_cache_inited)            // If initied
    {
      /*
        Move tables using this key cache to the default key cache
        and clear the old key cache.
      */
      key_cache->in_init= 1;
      mysql_mutex_unlock(&LOCK_global_system_variables);
      key_cache->param_buff_size= 0;
      ha_resize_key_cache(key_cache);
      ha_change_key_cache(key_cache, dflt_key_cache);
      /*
        We don't delete the key cache as some running threads my still be in
        the key cache code with a pointer to the deleted (empty) key cache
      */
      mysql_mutex_lock(&LOCK_global_system_variables);
      key_cache->in_init= 0;
    }
    return error;
  }

  key_cache->param_buff_size= new_value;

  /* If key cache didn't exist initialize it, else resize it */
  key_cache->in_init= 1;
  mysql_mutex_unlock(&LOCK_global_system_variables);

  if (!key_cache->key_cache_inited)
    error= ha_init_key_cache(0, key_cache, 0);
  else
    error= ha_resize_key_cache(key_cache);

  mysql_mutex_lock(&LOCK_global_system_variables);
  key_cache->in_init= 0;

  return error;
}

static bool update_keycache(THD *thd, KEY_CACHE *key_cache,
                            ptrdiff_t offset, ulonglong new_value,
                            int (*func)(KEY_CACHE *))
{
  bool error= false;
  DBUG_ASSERT(offset != offsetof(KEY_CACHE, param_buff_size));

  keycache_var(key_cache, offset)= new_value;

  key_cache->in_init= 1;
  mysql_mutex_unlock(&LOCK_global_system_variables);
  error= func(key_cache);
  mysql_mutex_lock(&LOCK_global_system_variables);
  key_cache->in_init= 0;

  return error;
}

static bool resize_keycache(THD *thd, KEY_CACHE *key_cache,
                            ptrdiff_t offset, ulonglong new_value)
{
  return update_keycache(thd, key_cache, offset, new_value,
                         ha_resize_key_cache);
}

static bool change_keycache_param(THD *thd, KEY_CACHE *key_cache,
                                  ptrdiff_t offset, ulonglong new_value)
{
  return update_keycache(thd, key_cache, offset, new_value,
                         ha_change_key_cache_param);
}

static bool repartition_keycache(THD *thd, KEY_CACHE *key_cache,
                                 ptrdiff_t offset, ulonglong new_value)
{
  return update_keycache(thd, key_cache, offset, new_value,
                         ha_repartition_key_cache);
}


/**
  The class for floating point variables

  Class specific constructor arguments: min, max

  Backing store: double
*/
class Sys_var_double: public sys_var
{
public:
  Sys_var_double(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          double min_val, double max_val, double def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_DOUBLE,
              (longlong) getopt_double2ulonglong(def_val),
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  {
    option.var_type|= GET_DOUBLE;
    option.min_value= (longlong) getopt_double2ulonglong(min_val);
    option.max_value= (longlong) getopt_double2ulonglong(max_val);
    SYSVAR_ASSERT(min_val < max_val);
    SYSVAR_ASSERT(min_val <= def_val);
    SYSVAR_ASSERT(max_val >= def_val);
    SYSVAR_ASSERT(size == sizeof(double));
  }
  bool do_check(THD *thd, set_var *var) override
  {
    my_bool fixed;
    double v= var->value->val_real();
    var->save_result.double_value= getopt_double_limit_value(v, &option, &fixed);

    return throw_bounds_warning(thd, name.str, fixed, v);
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, double)= var->save_result.double_value;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(double)= var->save_result.double_value;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.double_value= global_var(double); }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.double_value= getopt_ulonglong2double(option.def_value); }
};


/*
  Optimizer costs
  Stored as cost factor (1 cost = 1 ms).
  Given and displayed as microsconds (as most values are very small)
*/

class Sys_var_optimizer_cost: public Sys_var_double
{
public:
  double cost_adjust;
  Sys_var_optimizer_cost(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          double min_val, double max_val, double def_val,
          ulong arg_cost_adjust, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    :Sys_var_double(name_arg, comment, flag_args, off, size, getopt,
                   min_val, max_val, def_val * arg_cost_adjust, lock,
                   binlog_status_arg,
                   on_check_func,
                   on_update_func,
                   substitute)
  {
    cost_adjust= (double) arg_cost_adjust;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, double)= var->save_result.double_value/cost_adjust;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(double)= var->save_result.double_value/cost_adjust;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.double_value= global_var(double) * cost_adjust; }

  void global_save_default(THD *thd, set_var *var) override
  {
    var->save_result.double_value= getopt_ulonglong2double(option.def_value);
  }
  const uchar *tmp_ptr(THD *thd) const
  {
    if (thd->sys_var_tmp.double_value > 0)
      thd->sys_var_tmp.double_value*= cost_adjust;
    return (uchar*) &thd->sys_var_tmp.double_value;
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    thd->sys_var_tmp.double_value= session_var(thd, double);
    return tmp_ptr(thd);
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    thd->sys_var_tmp.double_value= global_var(double);
    return tmp_ptr(thd);
  }
};


/*
   The class for optimizer costs with structured names, unique for each engine.
   Used as 'engine.variable_name'

   Class specific constructor arguments:
   everything derived from Sys_var_optimizer_cost

  Backing store: double

  @note these variables can be only GLOBAL
*/

#define COST_VAR(X) GLOBAL_VAR(default_optimizer_costs.X)
#define cost_var_ptr(KC, OFF) (((uchar*)(KC))+(OFF))
#define cost_var(KC, OFF) (*(double*)cost_var_ptr(KC, OFF))

class Sys_var_engine_optimizer_cost: public Sys_var_optimizer_cost
{
  public:
  Sys_var_engine_optimizer_cost(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          double min_val, double max_val, double def_val,
          long cost_adjust, PolyLock *lock= 0,
          const char *substitute=0)
    : Sys_var_optimizer_cost(name_arg, comment, flag_args, off, size,
                             getopt, min_val, max_val, def_val, cost_adjust,
                             lock, VARIABLE_NOT_IN_BINLOG, 0,
                             0, substitute)
  {
    option.var_type|= GET_ASK_ADDR;
    option.value= (uchar**)1; // crash me, please
    // fix an offset from global_system_variables to be an offset in OPTIMIZER_COSTS
    offset= global_var_ptr() - (uchar*) &default_optimizer_costs;
    SYSVAR_ASSERT(scope() == GLOBAL);
  }
  bool global_update(THD *thd, set_var *var) override
  {
    double new_value= var->save_result.double_value;
    LEX_CSTRING *base_name= &var->base;
    OPTIMIZER_COSTS *optimizer_costs;

    /* If no basename, assume it's for the default costs */
    if (!base_name->length)
      base_name= &default_base;

    mysql_mutex_lock(&LOCK_optimizer_costs);
    if (!(optimizer_costs= get_or_create_optimizer_costs(base_name->str,
                                                         base_name->length)))
    {
      mysql_mutex_unlock(&LOCK_optimizer_costs);
      return true;
    }
    cost_var(optimizer_costs, offset)= new_value / cost_adjust;
    mysql_mutex_unlock(&LOCK_optimizer_costs);
    return 0;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const
  override
  {
    OPTIMIZER_COSTS *optimizer_costs= get_optimizer_costs(base);
    if (!optimizer_costs)
      optimizer_costs= &default_optimizer_costs;
    thd->sys_var_tmp.double_value= cost_var(optimizer_costs, offset);
    return tmp_ptr(thd);
  }
};


/**
  The class for the @max_user_connections.
  It's derived from Sys_var_uint, but non-standard session value
  requires a new class.

  Class specific constructor arguments:
    everything derived from Sys_var_uint

  Backing store: uint
*/
class Sys_var_max_user_conn: public Sys_var_int
{
public:
  Sys_var_max_user_conn(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          int min_val, int max_val, int def_val,
          uint block_size, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_int(name_arg, comment, SESSION, off, size, getopt,
              min_val, max_val, def_val, block_size,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  { }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    if (thd->user_connect && thd->user_connect->user_resources.user_conn)
      return (uchar*) &(thd->user_connect->user_resources.user_conn);
    return global_value_ptr(thd, base);
  }
};

/**
  The class for flagset variables - a variant of SET that allows in-place
  editing (turning on/off individual bits). String representations looks like
  a "flag=val,flag=val,...". Example: @@optimizer_switch

  Class specific constructor arguments:
    char* values[]    - 0-terminated list of strings of valid values

  Backing store: ulonglong

  @note
  the last value in the values[] array should
  *always* be the string "default".
*/
class Sys_var_flagset: public Sys_var_typelib
{
public:
  Sys_var_flagset(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *values[], ulonglong def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_typelib(name_arg, comment, flag_args, off, getopt,
                      SHOW_CHAR, values, def_val, lock,
                      binlog_status_arg, on_check_func, on_update_func,
                      substitute)
  {
    option.var_type|= GET_FLAGSET;
    global_var(ulonglong)= def_val;
    SYSVAR_ASSERT(typelib.count > 1);
    SYSVAR_ASSERT(typelib.count <= 65);
    SYSVAR_ASSERT(def_val <= my_set_bits(typelib.count-1));
    SYSVAR_ASSERT(strcmp(values[typelib.count-1], "default") == 0);
    SYSVAR_ASSERT(size == sizeof(ulonglong));
  }
  bool do_check(THD *thd, set_var *var) override
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String str(buff, sizeof(buff), system_charset_info), *res;
    ulonglong default_value, current_value;
    if (var->type == OPT_GLOBAL)
    {
      default_value= option.def_value;
      current_value= global_var(ulonglong);
    }
    else
    {
      default_value= global_var(ulonglong);
      current_value= session_var(thd, ulonglong);
    }

    if (var->value->result_type() == STRING_RESULT)
    {
      if (!(res=var->value->val_str(&str)))
        return true;
      else
      {
        char *error;
        uint error_len;

        var->save_result.ulonglong_value=
              find_set_from_flags(&typelib,
                                  typelib.count,
                                  current_value,
                                  default_value,
                                  res->ptr(), res->length(),
                                  &error, &error_len);
        if (unlikely(error))
        {
          ErrConvString err(error, error_len, res->charset());
          my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name.str, err.ptr());
          return true;
        }
      }
    }
    else
    {
      longlong tmp=var->value->val_int();
      if ((tmp < 0 && ! var->value->unsigned_flag)
          || (ulonglong)tmp > my_set_bits(typelib.count))
        return true;
      else
        var->save_result.ulonglong_value= tmp;
    }

    return false;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, ulonglong)= var->save_result.ulonglong_value;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(ulonglong)= var->save_result.ulonglong_value;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= global_var(ulonglong); }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= option.def_value; }
  const uchar *valptr(THD *thd, ulonglong val) const
  { return (uchar*)flagset_to_string(thd, 0, val, typelib.type_names); }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, ulonglong)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(ulonglong)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, option.def_value); }
};

/**
  The class for SET variables - variables taking zero or more values
  from the given list. Example: @@sql_mode

  Class specific constructor arguments:
    char* values[]    - 0-terminated list of strings of valid values

  Backing store: ulonglong
*/

static const LEX_CSTRING all_clex_str= {STRING_WITH_LEN("all")};


class Sys_var_set: public Sys_var_typelib
{
public:
  Sys_var_set(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *values[], ulonglong def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_typelib(name_arg, comment, flag_args, off, getopt,
                      SHOW_CHAR, values, def_val, lock,
                      binlog_status_arg, on_check_func, on_update_func,
                      substitute)
  {
    option.var_type|= GET_SET;
    option.min_value= 0;
    option.max_value= ~0ULL;
    global_var(ulonglong)= def_val;
    if ((option.u_max_value= (uchar**)max_var_ptr()))
    {
      *((ulonglong*) option.u_max_value)= ~0ULL;
    }
    SYSVAR_ASSERT(typelib.count > 0);
    SYSVAR_ASSERT(typelib.count <= 64);
    SYSVAR_ASSERT(def_val <= my_set_bits(typelib.count));
    SYSVAR_ASSERT(size == sizeof(ulonglong));
  }
  bool check_maximum(THD *thd, set_var *var,
                     const char *c_val, longlong i_val) override
  {
    if (!max_var_ptr() ||
        (var->save_result.ulonglong_value & ~(get_max_var())) == 0)
      return FALSE;
    var->save_result.ulonglong_value&= get_max_var();

    return c_val ? throw_bounds_warning(thd, name.str, c_val) :
                   throw_bounds_warning(thd, name.str, TRUE,
                                        var->value->unsigned_flag, i_val);
  }
  bool do_check(THD *thd, set_var *var) override
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String str(buff, sizeof(buff), system_charset_info), *res;

    if (var->value->result_type() == STRING_RESULT)
    {
      char *error;
      uint error_len;
      bool not_used;

      if (!(res= var->value->val_str_ascii_revert_empty_string_is_null(thd,
                                                                       &str)))
        return true;

      var->save_result.ulonglong_value=
            find_set(&typelib, res->ptr(), res->length(), NULL,
                    &error, &error_len, &not_used);
      if (error_len &&
          !my_charset_latin1.strnncollsp(res->to_lex_cstring(), all_clex_str))
      {
        var->save_result.ulonglong_value= ((1ULL << (typelib.count)) -1);
        error_len= 0;
      }
      /*
        note, we only issue an error if error_len > 0.
        That is even while empty (zero-length) values are considered
        errors by find_set(), these errors are ignored here
      */
      if (error_len)
      {
        ErrConvString err(error, error_len, res->charset());
        my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name.str, err.ptr());
        return true;
      }
      return check_maximum(thd, var, res->ptr(), 0);
    }

    longlong tmp=var->value->val_int();
    if ((tmp < 0 && ! var->value->unsigned_flag)
        || (ulonglong)tmp > my_set_bits(typelib.count))
      return true;

    var->save_result.ulonglong_value= tmp;
    return check_maximum(thd, var, 0, tmp);
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, ulonglong)= var->save_result.ulonglong_value;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(ulonglong)= var->save_result.ulonglong_value;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= global_var(ulonglong); }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= option.def_value; }
  const uchar *valptr(THD *thd, ulonglong val) const
  { return reinterpret_cast<const uchar*>(set_to_string(thd, 0, val, typelib.type_names)); }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, ulonglong)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(ulonglong)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, option.def_value); }

  ulonglong get_max_var() { return *((ulonglong*) max_var_ptr()); }
};

/**
  The class for variables which value is a plugin.
  Example: @@default_storage_engine

  Class specific constructor arguments:
    int plugin_type_arg (for example MYSQL_STORAGE_ENGINE_PLUGIN)

  Backing store: plugin_ref

  @note
  these variables don't support command-line equivalents, any such
  command-line options should be added manually to my_long_options in mysqld.cc
*/
class Sys_var_plugin: public sys_var
{
  int plugin_type;
public:
  Sys_var_plugin(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          int plugin_type_arg, const char **def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute),
    plugin_type(plugin_type_arg)
  {
    option.var_type|= GET_STR;
    SYSVAR_ASSERT(size == sizeof(plugin_ref));
    SYSVAR_ASSERT(getopt.id < 0); // force NO_CMD_LINE
  }
  bool do_check(THD *thd, set_var *var) override
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String str(buff,sizeof(buff), system_charset_info), *res;
    if (!(res=var->value->val_str(&str)))
      var->save_result.plugin= NULL;
    else
    {
      const LEX_CSTRING pname= { const_cast<char*>(res->ptr()), res->length() };
      plugin_ref plugin;

      // special code for storage engines (e.g. to handle historical aliases)
      if (plugin_type == MYSQL_STORAGE_ENGINE_PLUGIN)
        plugin= ha_resolve_by_name(thd, &pname, false);
      else
        plugin= my_plugin_lock_by_name(thd, &pname, plugin_type);
      if (unlikely(!plugin))
      {
        // historically different error code
        if (plugin_type == MYSQL_STORAGE_ENGINE_PLUGIN)
        {
          ErrConvString err(res);
          my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), err.ptr());
        }
        return true;
      }
      var->save_result.plugin= plugin;
    }
    return false;
  }
  void do_update(plugin_ref *valptr, plugin_ref newval)
  {
    plugin_ref oldval= *valptr;
    if (oldval != newval)
    {
      *valptr= newval ? my_plugin_lock(NULL, newval) : 0;
      plugin_unlock(NULL, oldval);
    }
  }
  bool session_update(THD *thd, set_var *var) override
  {
    do_update((plugin_ref*)session_var_ptr(thd),
              var->save_result.plugin);
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    do_update((plugin_ref*)global_var_ptr(),
              var->save_result.plugin);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    plugin_ref plugin= global_var(plugin_ref);
    var->save_result.plugin= plugin ? my_plugin_lock(thd, plugin) : 0;
  }
  plugin_ref get_default(THD *thd) const
  {
    char *default_value= *reinterpret_cast<char**>(option.def_value);
    if (!default_value)
      return 0;

    LEX_CSTRING pname= { default_value, strlen(default_value) };
    plugin_ref plugin;

    if (plugin_type == MYSQL_STORAGE_ENGINE_PLUGIN)
      plugin= ha_resolve_by_name(thd, &pname, false);
    else
      plugin= my_plugin_lock_by_name(thd, &pname, plugin_type);
    DBUG_ASSERT(plugin);
    return my_plugin_lock(thd, plugin);
  }

  void global_save_default(THD *thd, set_var *var) override
  {
    var->save_result.plugin= get_default(thd);
  }

  uchar *valptr(THD *thd, plugin_ref plugin) const
  {
    return (uchar*)(plugin ? thd->strmake(plugin_name(plugin)->str,
                                          plugin_name(plugin)->length) : 0);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, plugin_ref)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(plugin_ref)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, get_default(thd)); }
};

/**
  Class for variables that containg a list of plugins.
  Currently this is used only for @@gtid_pos_auto_create_engines

  Backing store: plugin_ref

  @note
  Currently this is only used for storage engine type plugins, and thus only
  storage engine type plugin is implemented. It could be extended to other
  plugin types later if needed, similar to Sys_var_plugin.

  These variables don't support command-line equivalents, any such
  command-line options should be added manually to my_long_options in mysqld.cc

  Note on lifetimes of resources allocated: We allocate a zero-terminated array
  of plugin_ref*, and lock the contained plugins. The list in the global
  variable must be freed (with free_engine_list()). However, the way Sys_var
  works, there is no place to explicitly free other lists, like the one
  returned from get_default().

  Therefore, the code needs to work with temporary lists, which are
  registered in the THD to be automatically freed (and plugins similarly
  automatically unlocked). This is why do_check() allocates a temporary
  list, from which do_update() then makes a permanent copy.
*/
class Sys_var_pluginlist: public sys_var
{
public:
  Sys_var_pluginlist(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          char **def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  {
    option.var_type|= GET_STR;
    SYSVAR_ASSERT(size == sizeof(plugin_ref));
    SYSVAR_ASSERT(getopt.id < 0); // force NO_CMD_LINE
  }
  bool do_check(THD *thd, set_var *var) override
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String str(buff,sizeof(buff), system_charset_info), *res;
    plugin_ref *plugins;

    if (!(res=var->value->val_str(&str)))
      plugins= resolve_engine_list(thd, "", 0, true, true);
    else
      plugins= resolve_engine_list(thd, res->ptr(), res->length(), true, true);
    if (!plugins)
      return true;
    var->save_result.plugins= plugins;
    return false;
  }
  void do_update(plugin_ref **valptr, plugin_ref* newval)
  {
    plugin_ref *oldval= *valptr;
    *valptr= copy_engine_list(newval);
    free_engine_list(oldval);
  }
  bool session_update(THD *thd, set_var *var) override
  {
    do_update((plugin_ref**)session_var_ptr(thd),
              var->save_result.plugins);
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    do_update((plugin_ref**)global_var_ptr(),
              var->save_result.plugins);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    plugin_ref* plugins= global_var(plugin_ref *);
    var->save_result.plugins= plugins ? temp_copy_engine_list(thd, plugins) : 0;
  }
  plugin_ref *get_default(THD *thd) const
  {
    char *default_value= *reinterpret_cast<char**>(option.def_value);
    if (!default_value)
      return 0;
    return resolve_engine_list(thd, default_value, strlen(default_value),
                               false, true);
  }

  void global_save_default(THD *thd, set_var *var) override
  {
    var->save_result.plugins= get_default(thd);
  }

  uchar *valptr(THD *thd, plugin_ref *plugins) const
  {
    return reinterpret_cast<uchar*>(pretty_print_engine_list(thd, plugins));
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, plugin_ref*)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(plugin_ref*)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, get_default(thd)); }
};

#if defined(ENABLED_DEBUG_SYNC)

#include "debug_sync.h"

/**
  The class for @@debug_sync session-only variable
*/
class Sys_var_debug_sync :public sys_var
{
public:
  Sys_var_debug_sync(const char *name_arg,
               const char *comment, int flag_args,
               CMD_LINE getopt,
               const char *def_val, PolyLock *lock=0,
               enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
               on_check_function on_check_func=0,
               on_update_function on_update_func=0,
               const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, 0, getopt.id,
              getopt.arg_type, SHOW_CHAR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  {
    SYSVAR_ASSERT(scope() == ONLY_SESSION);
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    char buff[STRING_BUFFER_USUAL_SIZE];
    String str(buff, sizeof(buff), system_charset_info), *res;

    if (!(res=var->value->val_str(&str)))
      var->save_result.string_value= empty_lex_str;
    else
    {
      if (!thd->make_lex_string(&var->save_result.string_value,
                                res->ptr(), res->length()))
        return true;
    }
    return false;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    return debug_sync_update(thd, var->save_result.string_value.str,
                                  var->save_result.string_value.length);
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    var->save_result.string_value.str= const_cast<char*>("");
    var->save_result.string_value.length= 0;
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return debug_sync_value_ptr(thd);
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(FALSE);
    return 0;
  }
  const uchar *default_value_ptr(THD *thd) const override
  { return (uchar*)""; }
};
#endif /* defined(ENABLED_DEBUG_SYNC) */


/**
  The class for bit variables - a variant of boolean that stores the value
  in a bit.

  Class specific constructor arguments:
    ulonglong bitmask_arg - the mask for the bit to set in the ulonglong
                            backing store

  Backing store: ulonglong

  @note
  This class supports the "reverse" semantics, when the value of the bit
  being 0 corresponds to the value of variable being set. To activate it
  use REVERSE(bitmask) instead of simply bitmask in the constructor.

  @note
  variables of this class cannot be set from the command line as
  my_getopt does not support bits.
*/
class Sys_var_bit: public Sys_var_typelib
{
  ulonglong bitmask;
  bool reverse_semantics;
  void set(uchar *ptr, ulonglong value)
  {
    if ((value != 0) ^ reverse_semantics)
      (*(ulonglong *)ptr)|= bitmask;
    else
      (*(ulonglong *)ptr)&= ~bitmask;
  }
public:
  Sys_var_bit(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          ulonglong bitmask_arg, my_bool def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : Sys_var_typelib(name_arg, comment, flag_args, off, getopt,
                      SHOW_MY_BOOL, bool_values, def_val, lock,
                      binlog_status_arg, on_check_func, on_update_func,
                      substitute)
  {
    option.var_type|= GET_BIT;
    reverse_semantics= my_count_bits(bitmask_arg) > 1;
    bitmask= reverse_semantics ? ~bitmask_arg : bitmask_arg;
    option.block_size= reverse_semantics ? -(long) bitmask : (long)bitmask;
    set(global_var_ptr(), def_val);
    SYSVAR_ASSERT(def_val < 2);
    SYSVAR_ASSERT(size == sizeof(ulonglong));
  }
  bool session_update(THD *thd, set_var *var) override
  {
    set(session_var_ptr(thd), var->save_result.ulonglong_value);
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    set(global_var_ptr(), var->save_result.ulonglong_value);
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    var->save_result.ulonglong_value=
      (reverse_semantics == !(global_var(ulonglong) & bitmask));
  }
  void global_save_default(THD *thd, set_var *var) override
  { var->save_result.ulonglong_value= option.def_value; }

  uchar *valptr(THD *thd, ulonglong val) const
  {
    thd->sys_var_tmp.my_bool_value= (reverse_semantics == !(val & bitmask));
    return (uchar*) &thd->sys_var_tmp.my_bool_value;
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, ulonglong)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(ulonglong)); }
  const uchar *default_value_ptr(THD *thd) const override
  {
    thd->sys_var_tmp.my_bool_value= option.def_value != 0;
    return (uchar*) &thd->sys_var_tmp.my_bool_value;
  }
};

/**
  The class for variables that have a special meaning for a session,
  such as @@timestamp or @@rnd_seed1, their values typically cannot be read
  from SV structure, and a special "read" callback is provided.

  Class specific constructor arguments:
    everything derived from Sys_var_ulonglong
    session_special_read_function read_func_arg

  Backing store: ulonglong

  @note
  These variables are session-only, global or command-line equivalents
  are not supported as they're generally meaningless.
*/
class Sys_var_session_special: public Sys_var_ulonglong
{
  typedef bool (*session_special_update_function)(THD *thd, set_var *var);
  typedef ulonglong (*session_special_read_function)(THD *thd);

  session_special_read_function read_func;
  session_special_update_function update_func;
public:
  Sys_var_session_special(const char *name_arg,
               const char *comment, int flag_args,
               CMD_LINE getopt,
               ulonglong min_val, ulonglong max_val, uint block_size,
               PolyLock *lock, enum binlog_status_enum binlog_status_arg,
               on_check_function on_check_func,
               session_special_update_function update_func_arg,
               session_special_read_function read_func_arg,
               const char *substitute=0)
    : Sys_var_ulonglong(name_arg, comment, flag_args, 0,
              sizeof(ulonglong), getopt, min_val,
              max_val, 0, block_size, lock, binlog_status_arg, on_check_func, 0,
              substitute),
      read_func(read_func_arg), update_func(update_func_arg)
  {
    SYSVAR_ASSERT(scope() == ONLY_SESSION);
    SYSVAR_ASSERT(getopt.id < 0); // NO_CMD_LINE, because the offset is fake
  }
  bool session_update(THD *thd, set_var *var) override
  { return update_func(thd, var); }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->value= 0; }
  void global_save_default(THD *thd, set_var *var) override
  { DBUG_ASSERT(FALSE); }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    thd->sys_var_tmp.ulonglong_value= read_func(thd);
    return (uchar*) &thd->sys_var_tmp.ulonglong_value;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(FALSE);
    return 0;
  }
  const uchar *default_value_ptr(THD *thd) const override
  {
    thd->sys_var_tmp.ulonglong_value= 0;
    return (uchar*) &thd->sys_var_tmp.ulonglong_value;
  }
};


/*
  Dedicated class because of a weird behavior of a default value.
  Assigning timestamp to itself

    SET @@timestamp = @@timestamp

  make it non-default and stops the time flow.
*/
class Sys_var_timestamp: public Sys_var_double
{
public:
  Sys_var_timestamp(const char *name_arg,
               const char *comment, int flag_args,
               CMD_LINE getopt,
               double min_val, double max_val,
               PolyLock *lock, enum binlog_status_enum binlog_status_arg,
               on_check_function on_check_func=0)
    : Sys_var_double(name_arg, comment, flag_args, 0,
              sizeof(double), getopt, min_val,
              max_val, 0, lock, binlog_status_arg, on_check_func)
  {
    SYSVAR_ASSERT(scope() == ONLY_SESSION);
    SYSVAR_ASSERT(getopt.id < 0); // NO_CMD_LINE, because the offset is fake
  }
  bool session_update(THD *thd, set_var *var) override
  {
    if (var->value)
    {
      my_hrtime_t hrtime = { hrtime_from_time(var->save_result.double_value) };
      thd->set_time(hrtime);
    }
    else // SET timestamp=DEFAULT
      thd->user_time.val= 0;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  bool session_is_default(THD *thd) override
  {
    return thd->user_time.val == 0;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->value= 0; }
  void global_save_default(THD *thd, set_var *var) override
  { DBUG_ASSERT(FALSE); }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    thd->sys_var_tmp.double_value= thd->start_time +
          thd->start_time_sec_part/(double)TIME_SECOND_PART_FACTOR;
    return (uchar*) &thd->sys_var_tmp.double_value;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(FALSE);
    return 0;
  }
  const uchar *default_value_ptr(THD *thd) const override
  {
    thd->sys_var_tmp.double_value= 0;
    return (uchar*) &thd->sys_var_tmp.double_value;
  }
  bool on_check_access_session(THD *thd) const override;
};


/**
  The class for read-only variables that show whether a particular
  feature is supported by the server. Example: have_compression

  Backing store: enum SHOW_COMP_OPTION

  @note
  These variables are necessarily read-only, only global, and have no
  command-line equivalent.
*/
class Sys_var_have: public sys_var
{
public:
  Sys_var_have(const char *name_arg,
               const char *comment, int flag_args, ptrdiff_t off, size_t size,
               CMD_LINE getopt,
               PolyLock *lock=0,
               enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
               on_check_function on_check_func=0,
               on_update_function on_update_func=0,
               const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, 0,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  {
    SYSVAR_ASSERT(scope() == GLOBAL);
    SYSVAR_ASSERT(getopt.id < 0);
    SYSVAR_ASSERT(lock == 0);
    SYSVAR_ASSERT(binlog_status_arg == VARIABLE_NOT_IN_BINLOG);
    SYSVAR_ASSERT(is_readonly());
    SYSVAR_ASSERT(on_update == 0);
    SYSVAR_ASSERT(size == sizeof(enum SHOW_COMP_OPTION));
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override {
    DBUG_ASSERT(FALSE);
    return true;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(FALSE);
    return true;
  }
  void session_save_default(THD *thd, set_var *var) override { }
  void global_save_default(THD *thd, set_var *var) override { }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(FALSE);
    return 0;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return (uchar*)show_comp_option_name[global_var(enum SHOW_COMP_OPTION)];
  }
};

/**
  Generic class for variables for storing entities that are internally
  represented as structures, have names, and possibly can be referred to by
  numbers.  Examples: character sets, collations, locales,

  Class specific constructor arguments:
    ptrdiff_t name_offset  - offset of the 'name' field in the structure

  Backing store: void*

  @note
  As every such a structure requires special treatment from my_getopt,
  these variables don't support command-line equivalents, any such
  command-line options should be added manually to my_long_options in mysqld.cc
*/
class Sys_var_struct: public sys_var
{
  ptrdiff_t name_offset; // offset to the 'name' property in the structure
public:
  Sys_var_struct(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          ptrdiff_t name_off, void *def_val, PolyLock *lock=0,
          enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func=0,
          on_update_function on_update_func=0,
          const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute),
      name_offset(name_off)
  {
    option.var_type|= GET_ENUM; // because we accept INT and STRING here
    /*
      struct variables are special on the command line - often (e.g. for
      charsets) the name cannot be immediately resolved, but only after all
      options (in particular, basedir) are parsed.

      thus all struct command-line options should be added manually
      to my_long_options in mysqld.cc
    */
    SYSVAR_ASSERT(getopt.id < 0);
    SYSVAR_ASSERT(size == sizeof(void *));
  }
  bool do_check(THD *thd, set_var *var) override
  { return false; }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, const void*)= var->save_result.ptr;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(const void*)= var->save_result.ptr;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  { var->save_result.ptr= global_var(void*); }
  void global_save_default(THD *thd, set_var *var) override
  {
    void **default_value= reinterpret_cast<void**>(option.def_value);
    var->save_result.ptr= *default_value;
  }
  uchar *valptr(THD *thd, uchar *val) const
  { return val ? *(uchar**)(val+name_offset) : 0; }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, session_var(thd, uchar*)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(uchar*)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, *(uchar**)option.def_value); }
};


/**
  The class to store character sets.
*/
class Sys_var_charset: public Sys_var_struct
{
public:
  using Sys_var_struct::Sys_var_struct;
  void global_save_default(THD *, set_var *var) override
  {
    /*
      The default value can point to an arbitrary collation,
      e.g. default_charset_info.
      Let's convert it to the compiled default collation.
      This makes the code easier in various places such as SET NAMES.
    */
    void **default_value= reinterpret_cast<void**>(option.def_value);
    var->save_result.ptr=
      Lex_exact_charset_opt_extended_collate((CHARSET_INFO *) *default_value,
                                             true).
        find_compiled_default_collation();
  }
};


/**
  The class for variables that store time zones

  Backing store: Time_zone*

  @note
  Time zones cannot be supported directly by my_getopt, thus
  these variables don't support command-line equivalents, any such
  command-line options should be added manually to my_long_options in mysqld.cc
*/
class Sys_var_tz: public sys_var
{
public:
  Sys_var_tz(const char *name_arg,
             const char *comment, int flag_args, ptrdiff_t off, size_t size,
             CMD_LINE getopt,
             Time_zone **def_val, PolyLock *lock=0,
             enum binlog_status_enum binlog_status_arg=VARIABLE_NOT_IN_BINLOG,
             on_check_function on_check_func=0,
             on_update_function on_update_func=0,
             const char *substitute=0)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, (intptr)def_val,
              lock, binlog_status_arg, on_check_func, on_update_func,
              substitute)
  {
    SYSVAR_ASSERT(getopt.id < 0);
    SYSVAR_ASSERT(size == sizeof(Time_zone *));
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    char buff[MAX_TIME_ZONE_NAME_LENGTH];
    String str(buff, sizeof(buff), &my_charset_latin1);
    String *res= var->value->val_str(&str);

    if (!res)
      return true;

    if (!(var->save_result.time_zone= my_tz_find(thd, res)))
    {
      ErrConvString err(res);
      my_error(ER_UNKNOWN_TIME_ZONE, MYF(0), err.ptr());
      return true;
    }
    return false;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    session_var(thd, Time_zone*)= var->save_result.time_zone;
    return false;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    global_var(Time_zone*)= var->save_result.time_zone;
    return false;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    var->save_result.time_zone= global_var(Time_zone*);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    var->save_result.time_zone=
      *(Time_zone**)(intptr)option.def_value;
  }
  const uchar *valptr(THD *thd, Time_zone *val) const
  { return reinterpret_cast<const uchar*>(val->get_name()->ptr()); }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    /*
      This is an ugly fix for replication: we don't replicate properly queries
      invoking system variables' values to update tables; but
      CONVERT_TZ(,,@@session.time_zone) is so popular that we make it
      replicable (i.e. we tell the binlog code to store the session
      timezone). If it's the global value which was used we can't replicate
      (binlog code stores session value only).
    */
    thd->used|= THD::TIME_ZONE_USED;
    return valptr(thd, session_var(thd, Time_zone *));
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return valptr(thd, global_var(Time_zone*)); }
  const uchar *default_value_ptr(THD *thd) const override
  { return valptr(thd, *(Time_zone**)option.def_value); }
};

/**
  Special implementation for transaction isolation, that
  distingushes between

  SET GLOBAL TRANSACTION ISOLATION (stored in global_system_variables)
  SET SESSION TRANSACTION ISOLATION (stored in thd->variables)
  SET TRANSACTION ISOLATION (stored in thd->tx_isolation)

  where the last statement sets isolation level for the next transaction only
*/
class Sys_var_tx_isolation: public Sys_var_enum
{
public:
  Sys_var_tx_isolation(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *values[], uint def_val, PolyLock *lock,
          enum binlog_status_enum binlog_status_arg,
          on_check_function on_check_func,
          on_update_function on_update_func=0,
          const char *substitute=0)
    :Sys_var_enum(name_arg, comment, flag_args, off, size, getopt,
                  values, def_val, lock, binlog_status_arg, on_check_func,
                  on_update_func, substitute)
  {}
  bool session_update(THD *thd, set_var *var) override
  {
    if (var->type == OPT_SESSION && Sys_var_enum::session_update(thd, var))
      return TRUE;
    if (var->type == OPT_DEFAULT || !thd->in_active_multi_stmt_transaction())
    {
      thd->tx_isolation= (enum_tx_isolation) var->save_result.ulonglong_value;

#ifndef EMBEDDED_LIBRARY
      if (var->type == OPT_DEFAULT)
      {
        enum enum_tx_isol_level l;
        switch (thd->tx_isolation) {
        case ISO_READ_UNCOMMITTED:
          l=  TX_ISOL_UNCOMMITTED;
          break;
        case ISO_READ_COMMITTED:
          l=  TX_ISOL_COMMITTED;
          break;
        case ISO_REPEATABLE_READ:
          l= TX_ISOL_REPEATABLE;
          break;
        case ISO_SERIALIZABLE:
          l= TX_ISOL_SERIALIZABLE;
          break;
        default:
          DBUG_ASSERT_NO_ASSUME(0);
          return TRUE;
        }
        if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
          thd->session_tracker.transaction_info.set_isol_level(thd, l);
      }
      else if (thd->variables.session_track_transaction_info > TX_TRACK_NONE)
        thd->session_tracker.transaction_info.set_isol_level(thd, TX_ISOL_INHERIT);
#endif //EMBEDDED_LIBRARY
    }
    return FALSE;
  }
};


/**
  Class representing the transaction_read_only system variable for setting
  default transaction access mode.

  Note that there is a special syntax - SET TRANSACTION READ ONLY
  (or READ WRITE) that sets the access mode for the next transaction
  only.
*/

class Sys_var_tx_read_only: public Sys_var_mybool
{
public:
  Sys_var_tx_read_only(const char *name_arg, const char *comment, int flag_args,
                       ptrdiff_t off, size_t size, CMD_LINE getopt,
                       my_bool def_val, PolyLock *lock,
                       enum binlog_status_enum binlog_status_arg,
                       on_check_function on_check_func,
                       on_update_function on_update_func=0,
                       const char *substitute=0)
    :Sys_var_mybool(name_arg, comment, flag_args, off, size, getopt,
                    def_val, lock, binlog_status_arg, on_check_func,
                    on_update_func, substitute)
  {}
  bool session_update(THD *thd, set_var *var) override;
};

/*
  Class for replicate_events_marked_for_skip.
  We need a custom update function that ensures the slave is stopped when
  the update is happening.
*/
class Sys_var_replicate_events_marked_for_skip: public Sys_var_enum
{
public:
  Sys_var_replicate_events_marked_for_skip(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt,
          const char *values[], uint def_val, PolyLock *lock= 0,
          enum binlog_status_enum binlog_status_arg= VARIABLE_NOT_IN_BINLOG)
    :Sys_var_enum(name_arg, comment, flag_args, off, size, getopt,
                  values, def_val, lock, binlog_status_arg)
  {}
  bool global_update(THD *thd, set_var *var) override;
};

/*
  Class for handing multi-source replication variables
  Variable values are store in Master_info, but to make it possible to
  access variable without locks we also store it thd->variables.
  These can be used as GLOBAL or SESSION, but both points to the same
  variable.  This is to make things compatible with MySQL 5.5 where variables
  like sql_slave_skip_counter are GLOBAL.
*/

class Sys_var_multi_source_ulonglong;
class Master_info;

typedef ulonglong (Master_info::*mi_ulonglong_accessor_function)(void);
typedef bool (*on_multi_source_update_function)(sys_var *self, THD *thd,
                                                Master_info *mi);
bool update_multi_source_variable(sys_var *self,
                                  THD *thd, enum_var_type type);


class Sys_var_multi_source_ulonglong :public Sys_var_ulonglong
{ 
  mi_ulonglong_accessor_function mi_accessor_func;
  on_multi_source_update_function update_multi_source_variable_func;
public:
  Sys_var_multi_source_ulonglong(const char *name_arg,
                             const char *comment, int flag_args,
                             ptrdiff_t off, size_t size,
                             CMD_LINE getopt,
                             mi_ulonglong_accessor_function mi_accessor_arg,
                             ulonglong min_val, ulonglong max_val,
                             ulonglong def_val, uint block_size,
                             on_multi_source_update_function on_update_func)
    :Sys_var_ulonglong(name_arg, comment, flag_args, off, size,
                       getopt, min_val, max_val, def_val, block_size,
                       0, VARIABLE_NOT_IN_BINLOG, 0, update_multi_source_variable),
    mi_accessor_func(mi_accessor_arg),
    update_multi_source_variable_func(on_update_func)
  { }
  bool global_update(THD *thd, set_var *var) override
  {
    return session_update(thd, var);
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    /* Use value given in variable declaration */
    global_save_default(thd, var);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    ulonglong *tmp, res;
    tmp= (ulonglong*) (((uchar*)&(thd->variables)) + offset);
    res= get_master_info_ulonglong_value(thd);
    *tmp= res;
    return (uchar*) tmp;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return session_value_ptr(thd, base);
  }
  ulonglong get_master_info_ulonglong_value(THD *thd) const;
  bool update_variable(THD *thd, Master_info *mi)
  {
    return update_multi_source_variable_func(this, thd, mi);
  }
};


/**
  Class for @@global.gtid_current_pos.
*/
class Sys_var_gtid_current_pos: public sys_var
{
public:
  Sys_var_gtid_current_pos(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL)
  {
    SYSVAR_ASSERT(getopt.id < 0);
    SYSVAR_ASSERT(is_readonly());
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(false);
    return NULL;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override;
};


/**
  Class for @@global.gtid_binlog_pos.
*/
class Sys_var_gtid_binlog_pos: public sys_var
{
public:
  Sys_var_gtid_binlog_pos(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL)
  {
    SYSVAR_ASSERT(getopt.id < 0);
    SYSVAR_ASSERT(is_readonly());
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(false);
    return NULL;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override;
};


/**
  Class for @@global.gtid_slave_pos.
*/
class Sys_var_gtid_slave_pos: public sys_var
{
public:
  Sys_var_gtid_slave_pos(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL)
  {
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override;
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override;
  void session_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    /* Record the attempt to use default so we can error. */
    var->value= 0;
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(false);
    return NULL;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override;
  const uchar *default_value_ptr(THD *thd) const override
  { return 0; }
  bool on_check_access_global(THD *thd) const override
  {
    return check_global_access(thd, PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_SLAVE_POS);
  }
};


/**
  Class for @@global.gtid_binlog_state.
*/
class Sys_var_gtid_binlog_state: public sys_var
{
public:
  Sys_var_gtid_binlog_state(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off, getopt.id,
              getopt.arg_type, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL)
  {
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override;
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override;
  void session_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    /* Record the attempt to use default so we can error. */
    var->value= 0;
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(false);
    return NULL;
  }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override;
  const uchar *default_value_ptr(THD *thd) const override
  { return 0; }
  bool on_check_access_global(THD *thd) const override
  {
    return
      check_global_access(thd, PRIV_SET_SYSTEM_GLOBAL_VAR_GTID_BINLOG_STATE);
  }
};


/**
  Class for @@session.last_gtid.
*/
class Sys_var_last_gtid: public sys_var
{
public:
  Sys_var_last_gtid(const char *name_arg,
          const char *comment, int flag_args, CMD_LINE getopt)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, 0, getopt.id,
              getopt.arg_type, SHOW_CHAR, 0, NULL, VARIABLE_NOT_IN_BINLOG,
              NULL, NULL, NULL)
  {
    SYSVAR_ASSERT(getopt.id < 0);
    SYSVAR_ASSERT(is_readonly());
    option.var_type|= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool session_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  bool global_update(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
    return true;
  }
  void session_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    DBUG_ASSERT(false);
  }
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override;
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    DBUG_ASSERT(false);
    return NULL;
  }
};


/**
   Class for connection_name.slave_parallel_mode.
*/
class Sys_var_slave_parallel_mode: public Sys_var_enum
{
public:
  Sys_var_slave_parallel_mode(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt, const char *values[],
          enum_slave_parallel_mode def_val)
    : Sys_var_enum(name_arg, comment, flag_args, off, size,
                   getopt, values, def_val)
  {
    option.var_type|= GET_ASK_ADDR;
    option.value= (uchar**)1; // crash me, please
    SYSVAR_ASSERT(scope() == GLOBAL);
  }
  bool global_update(THD *thd, set_var *var) override;
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override;
};


class Sys_var_vers_asof: public sys_var
{
public:
  Sys_var_vers_asof(const char *name_arg,
          const char *comment, int flag_args, ptrdiff_t off, size_t size,
          CMD_LINE getopt, uint def_val,
          PolyLock *lock= NO_MUTEX_GUARD,
          binlog_status_enum binlog_status_arg= VARIABLE_NOT_IN_BINLOG,
          on_check_function on_check_func= NULL,
          on_update_function on_update_func= NULL,
          const char *substitute= NULL)
    : sys_var(&all_sys_vars, name_arg, comment, flag_args, off,
              getopt.id, getopt.arg_type, SHOW_CHAR, def_val, lock,
              binlog_status_arg, on_check_func, on_update_func, substitute)
  {
    option.var_type= GET_STR;
  }
  bool do_check(THD *thd, set_var *var) override
  {
    if (!var->value)
      return false;

    MYSQL_TIME ltime;
    Datetime::Options opt(TIME_CONV_NONE |
                          TIME_NO_ZERO_IN_DATE |
                          TIME_NO_ZERO_DATE, thd);
    bool res= var->value->get_date(thd, &ltime, opt);
    if (!res)
    {
      uint error;
      var->save_result.timestamp.unix_time=
              thd->variables.time_zone->TIME_to_gmt_sec(&ltime, &error);
      var->save_result.timestamp.second_part= ltime.second_part;
      res= error != 0;
    }
    return res;
  }

private:
  static bool update(THD *thd, set_var *var, vers_asof_timestamp_t *out)
  {
    if (var->value)
    {
      out->type       = SYSTEM_TIME_AS_OF;
      out->unix_time  = var->save_result.timestamp.unix_time;
      out->second_part= var->save_result.timestamp.second_part;
    }
    return 0;
  }

  static void save_default(set_var *var, vers_asof_timestamp_t *out)
  {
    out->type= SYSTEM_TIME_UNSPECIFIED;
  }

public:
  bool global_update(THD *thd, set_var *var) override
  {
    return update(thd, var, &global_var(vers_asof_timestamp_t));
  }
  bool session_update(THD *thd, set_var *var) override
  {
    return update(thd, var, &session_var(thd, vers_asof_timestamp_t));
  }

  bool session_is_default(THD *thd) override
  {
    const vers_asof_timestamp_t &var= session_var(thd, vers_asof_timestamp_t);
    return var.type == SYSTEM_TIME_UNSPECIFIED;
  }

  void session_save_default(THD *thd, set_var *var) override
  {
    save_default(var, &session_var(thd, vers_asof_timestamp_t));
  }
  void global_save_default(THD *thd, set_var *var) override
  {
    save_default(var, &global_var(vers_asof_timestamp_t));
  }

private:
  const uchar *value_ptr(THD *thd, vers_asof_timestamp_t &val) const
  {
    const char *value;
    switch (val.type)
    {
    case SYSTEM_TIME_UNSPECIFIED:
      return (uchar*)"DEFAULT";
      break;
    case SYSTEM_TIME_AS_OF:
    {
      char *buf= thd->alloc(MAX_DATE_STRING_REP_LENGTH);
      MYSQL_TIME ltime;

      thd->variables.time_zone->gmt_sec_to_TIME(&ltime, val.unix_time);
      ltime.second_part= val.second_part;

      value= buf;
      if (buf && !my_datetime_to_str(&ltime, buf, 6))
      {
        my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name.str, "NULL (wrong datetime)");
        value= thd->strdup("Error: wrong datetime");
      }
      break;
    }
    default:
      my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), name.str, "NULL (wrong range type)");
      value= thd->strdup("Error: wrong range type");
    }
    return reinterpret_cast<const uchar *>(value);
  }

public:
  const uchar *session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return value_ptr(thd, session_var(thd, vers_asof_timestamp_t)); }
  const uchar *global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  { return value_ptr(thd, global_var(vers_asof_timestamp_t)); }
};


class Sys_var_charset_collation_map: public sys_var
{
public:
  Sys_var_charset_collation_map(const char *name_arg, const char *comment,
                                int flag_args, ptrdiff_t off, size_t size,
                                CMD_LINE getopt,
                                enum binlog_status_enum binlog_status_arg)
   :sys_var(&all_sys_vars, name_arg, comment,
            flag_args, off, getopt.id, getopt.arg_type,
            SHOW_CHAR,
            DEFAULT(0), nullptr, binlog_status_arg,
            nullptr, nullptr, nullptr)
  {
    option.var_type|= GET_STR;
  }

private:

  static bool charset_collation_map_from_item(Charset_collation_map_st *map,
                                              Item *item,
                                              myf utf8_flag)
  {
    String *value, buffer;
    if (!(value= item->val_str_ascii(&buffer)))
      return true;
    return map->from_text(value->to_lex_cstring(), utf8_flag);
  }

  static const uchar *make_value_ptr(THD *thd,
                                     const Charset_collation_map_st &map)
  {
    size_t nbytes= map.text_format_nbytes_needed();
    char *buf= thd->alloc(nbytes + 1);
    size_t length= map.print(buf, nbytes);
    buf[length]= '\0';
    return (uchar *) buf;
  }

private:

  bool do_check(THD *thd, set_var *var) override
  {
    Charset_collation_map_st *map= thd->alloc<Charset_collation_map_st>(1);
    if (!map || charset_collation_map_from_item(map, var->value,
                                                thd->get_utf8_flag()))
      return true;
    var->save_result.ptr= map;
    return false;
  }

  void session_save_default(THD *thd, set_var *var) override
  {
    thd->variables.character_set_collations.set(
      global_system_variables.character_set_collations, 1);
  }

  void global_save_default(THD *thd, set_var *var) override
  {
    global_system_variables.character_set_collations.init();
  }

  bool session_update(THD *thd, set_var *var) override
  {
    if (!var->value)
    {
      session_save_default(thd, var);
      return false;
    }
    thd->variables.character_set_collations.
      set(*(Charset_collation_map_st*) var->save_result.ptr, 1);
    return false;
  }

  bool global_update(THD *thd, set_var *var) override
  {
    if (!var->value)
    {
      global_save_default(thd, var);
      return false;
    }
    global_system_variables.character_set_collations=
      *(Charset_collation_map_st*) var->save_result.ptr;
    return false;
  }

  const uchar *
  session_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return make_value_ptr(thd, thd->variables.character_set_collations);
  }

  const uchar *
  global_value_ptr(THD *thd, const LEX_CSTRING *base) const override
  {
    return make_value_ptr(thd, global_system_variables.
                                 character_set_collations);
  }
};
