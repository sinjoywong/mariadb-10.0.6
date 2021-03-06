/* Copyright (C) Olivier Bertrand 2004 - 2013

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/**
  @file ha_connect.cc

  @brief
  The ha_connect engine is a stubbed storage engine that enables to create tables
  based on external data. Principally they are based on plain files of many
  different types, but also on collections of such files, collection of tables,
  ODBC tables retrieving data from other DBMS having an ODBC server, and even
  virtual tables.

  @details
  ha_connect will let you create/open/delete tables, the created table can be
  done specifying an already existing file, the drop table command will just
  suppress the table definition but not the eventual data file.
  Indexes are not supported for all table types but data can be inserted, 
  updated or deleted.

  You can enable the CONNECT storage engine in your build by doing the
  following during your build process:<br> ./configure
  --with-connect-storage-engine

  You can install the CONNECT handler as all other storage handlers.

  Once this is done, MySQL will let you create tables with:<br>
  CREATE TABLE <table name> (...) ENGINE=CONNECT;

  The example storage engine does not use table locks. It
  implements an example "SHARE" that is inserted into a hash by table
  name. This is not used yet.

  Please read the object definition in ha_connect.h before reading the rest
  of this file.

  @note
  This MariaDB CONNECT handler is currently an adaptation of the XDB handler
  that was written for MySQL version 4.1.2-alpha. Its overall design should
  be enhanced in the future to meet MariaDB requirements.

  @note
  It was written also from the Brian's ha_example handler and contains parts
  of it that are there but not currently used, such as table variables.

  @note
  When you create an CONNECT table, the MySQL Server creates a table .frm
  (format) file in the database directory, using the table name as the file
  name as is customary with MySQL. No other files are created. To get an idea
  of what occurs, here is an example select that would do a scan of an entire
  table:

  @code
  ha-connect::open
  ha_connect::store_lock
  ha_connect::external_lock
  ha_connect::info
  ha_connect::rnd_init
  ha_connect::extra
  ENUM HA_EXTRA_CACHE        Cache record in HA_rrnd()
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::rnd_next
  ha_connect::extra
  ENUM HA_EXTRA_NO_CACHE     End caching of records (def)
  ha_connect::external_lock
  ha_connect::extra
  ENUM HA_EXTRA_RESET        Reset database to after open
  @endcode

  Here you see that the connect storage engine has 9 rows called before
  rnd_next signals that it has reached the end of its data. Calls to
  ha_connect::extra() are hints as to what will be occuring to the request.

  Happy use!<br>
    -Olivier
*/

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation        // gcc: Class implementation
#endif

#define MYSQL_SERVER 1
#define DONT_DEFINE_VOID
//#include "sql_partition.h"
#include "sql_class.h"
#include "create_options.h"
#include "mysql_com.h"
#include "field.h"
#include "sql_parse.h"
#include "sql_base.h"
#if defined(NEW_WAY)
#include "sql_table.h"
#endif   // NEW_WAY
#undef  OFFSET

#define NOPARSE
#if defined(UNIX)
#include "osutil.h"
#endif   // UNIX
#include "global.h"
#include "plgdbsem.h"
#if defined(ODBC_SUPPORT)
#include "odbccat.h"
#endif   // ODBC_SUPPORT
#if defined(MYSQL_SUPPORT)
#include "xtable.h"
#include "tabmysql.h"
#endif   // MYSQL_SUPPORT
#include "filamdbf.h"
#include "tabxcl.h"
#include "tabfmt.h"
#include "reldef.h"
#include "tabcol.h"
#include "xindex.h"
#if defined(WIN32)
#include <io.h>
#include "tabwmi.h"
#endif   // WIN32
#include "connect.h"
#include "user_connect.h"
#include "ha_connect.h"
#include "mycat.h"
#include "myutil.h"
#include "preparse.h"
#include "inihandl.h"

#define PLGXINI     "plgcnx.ini"       /* Configuration settings file  */
#define my_strupr(p)    my_caseup_str(default_charset_info, (p));
#define my_strlwr(p)    my_casedn_str(default_charset_info, (p));
#define my_stricmp(a,b) my_strcasecmp(default_charset_info, (a), (b))

#ifdef LIBXML2_SUPPORT
#include "libdoc.h"
#endif   // LIBXML2_SUPPORT

#include "taboccur.h"
#include "tabpivot.h"


/***********************************************************************/
/*  DB static variables.                                               */
/***********************************************************************/
extern "C" char  plgxini[];
extern "C" char  plgini[];
extern "C" char  nmfile[];
extern "C" char  pdebug[];

extern "C" {
       char  version[]= "Version 1.01.0008 August 18, 2013";

#if defined(XMSG)
       char  msglang[];            // Default message language
#endif
       int  trace= 0;              // The general trace value
} // extern "C"

/****************************************************************************/
/*  Initialize the ha_connect static members.                               */
/****************************************************************************/
#define CONNECT_INI "connect.ini"
char  connectini[_MAX_PATH]= CONNECT_INI;
int   xtrace= 0;
ulong ha_connect::num= 0;
//int  DTVAL::Shift= 0;

static PCONNECT GetUser(THD *thd, PCONNECT xp);
static PGLOBAL GetPlug(THD *thd, PCONNECT lxp);

static handler *connect_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root);

static int connect_assisted_discovery(handlerton *hton, THD* thd,
                                      TABLE_SHARE *table_s,
                                      HA_CREATE_INFO *info);

handlerton *connect_hton;

/**
  CREATE TABLE option list (table options)

  These can be specified in the CREATE TABLE:
  CREATE TABLE ( ... ) {...here...}
*/
ha_create_table_option connect_table_option_list[]=
{
  HA_TOPTION_STRING("TABLE_TYPE", type),
  HA_TOPTION_STRING("FILE_NAME", filename),
  HA_TOPTION_STRING("XFILE_NAME", optname),
//HA_TOPTION_STRING("CONNECT_STRING", connect),
  HA_TOPTION_STRING("TABNAME", tabname),
  HA_TOPTION_STRING("TABLE_LIST", tablist),
  HA_TOPTION_STRING("DBNAME", dbname),
  HA_TOPTION_STRING("SEP_CHAR", separator),
  HA_TOPTION_STRING("QCHAR", qchar),
  HA_TOPTION_STRING("MODULE", module),
  HA_TOPTION_STRING("SUBTYPE", subtype),
  HA_TOPTION_STRING("CATFUNC", catfunc),
  HA_TOPTION_STRING("SRCDEF", srcdef),
  HA_TOPTION_STRING("COLIST", colist),
  HA_TOPTION_STRING("OPTION_LIST", oplist),
  HA_TOPTION_STRING("DATA_CHARSET", data_charset),
  HA_TOPTION_NUMBER("LRECL", lrecl, 0, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("BLOCK_SIZE", elements, 0, 0, INT_MAX32, 1),
//HA_TOPTION_NUMBER("ESTIMATE", estimate, 0, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("MULTIPLE", multiple, 0, 0, 2, 1),
  HA_TOPTION_NUMBER("HEADER", header, 0, 0, 3, 1),
  HA_TOPTION_NUMBER("QUOTED", quoted, (ulonglong) -1, 0, 3, 1),
  HA_TOPTION_NUMBER("ENDING", ending, (ulonglong) -1, 0, INT_MAX32, 1),
  HA_TOPTION_NUMBER("COMPRESS", compressed, 0, 0, 2, 1),
//HA_TOPTION_BOOL("COMPRESS", compressed, 0),
  HA_TOPTION_BOOL("MAPPED", mapped, 0),
  HA_TOPTION_BOOL("HUGE", huge, 0),
  HA_TOPTION_BOOL("SPLIT", split, 0),
  HA_TOPTION_BOOL("READONLY", readonly, 0),
  HA_TOPTION_BOOL("SEPINDEX", sepindex, 0),
  HA_TOPTION_END
};


/**
  CREATE TABLE option list (field options)

  These can be specified in the CREATE TABLE per field:
  CREATE TABLE ( field ... {...here...}, ... )
*/
ha_create_table_option connect_field_option_list[]=
{
  HA_FOPTION_NUMBER("FLAG", offset, (ulonglong) -1, 0, INT_MAX32, 1),
  HA_FOPTION_NUMBER("FREQUENCY", freq, 0, 0, INT_MAX32, 1), // not used
  HA_FOPTION_NUMBER("OPT_VALUE", opt, 0, 0, 2, 1),  // used for indexing
  HA_FOPTION_NUMBER("FIELD_LENGTH", fldlen, 0, 0, INT_MAX32, 1),
  HA_FOPTION_STRING("DATE_FORMAT", dateformat),
  HA_FOPTION_STRING("FIELD_FORMAT", fieldformat),
  HA_FOPTION_STRING("SPECIAL", special),
  HA_FOPTION_END
};

/***********************************************************************/
/*  Push G->Message as a MySQL warning.                                */
/***********************************************************************/
bool PushWarning(PGLOBAL g, PTDBASE tdbp)
  {
  PHC    phc;
  THD   *thd;
  MYCAT *cat= (MYCAT*)tdbp->GetDef()->GetCat();

  if (!cat || !(phc= cat->GetHandler()) || !phc->GetTable() ||
      !(thd= (phc->GetTable())->in_use))
    return true;

  push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
  return false;
  } // end of PushWarning

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key con_key_mutex_CONNECT_SHARE_mutex;

static PSI_mutex_info all_connect_mutexes[]=
{
  { &con_key_mutex_CONNECT_SHARE_mutex, "CONNECT_SHARE::mutex", 0}
};

static void init_connect_psi_keys()
{
  const char* category= "connect";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_connect_mutexes);
  PSI_server->register_mutex(category, all_connect_mutexes, count);
}
#else
static void init_connect_psi_keys() {}
#endif


DllExport LPCSTR PlugSetPath(LPSTR to, LPCSTR name, LPCSTR dir)
{
  const char *res= PlugSetPath(to, mysql_data_home, name, dir);
  return res;
}


/**
  @brief
  If frm_error() is called then we will use this to determine
  the file extensions that exist for the storage engine. This is also
  used by the default rename_table and delete_table method in
  handler.cc.

  For engines that have two file name extentions (separate meta/index file
  and data file), the order of elements is relevant. First element of engine
  file name extentions array should be meta/index file extention. Second
  element - data file extention. This order is assumed by
  prepare_for_repair() when REPAIR TABLE ... USE_FRM is issued.

  @see
  rename_table method in handler.cc and
  delete_table method in handler.cc
*/
static const char *ha_connect_exts[]= {
  ".dos", ".fix", ".csv",".bin", ".fmt", ".dbf", ".xml", ".ini", ".vec",
  ".dnx", ".fnx", ".bnx", ".vnx", ".dbx",
  NULL
};


/**
  @brief
  Plugin initialization
*/
static int connect_init_func(void *p)
{
  DBUG_ENTER("connect_init_func");
  char dir[_MAX_PATH - sizeof(CONNECT_INI) - 1];

#ifdef LIBXML2_SUPPORT
  XmlInitParserLib();
#endif   // LIBXML2_SUPPORT

  /* Build connect.ini file name */
  my_getwd(dir, sizeof(dir) - 1, MYF(0));
  snprintf(connectini, sizeof(connectini), "%s%s", dir, CONNECT_INI);
  sql_print_information("CONNECT: %s=%s", CONNECT_INI, connectini);

  if ((xtrace= GetPrivateProfileInt("CONNECT", "Trace", 0, connectini)))
  {
    sql_print_information("CONNECT: xtrace=%d", xtrace);
    sql_print_information("CONNECT: plgini=%s", plgini);
    sql_print_information("CONNECT: plgxini=%s", plgxini);
    sql_print_information("CONNECT: nmfile=%s", nmfile);
    sql_print_information("CONNECT: pdebug=%s", pdebug);
    sql_print_information("CONNECT: version=%s", version);
    trace= xtrace;
  } // endif xtrace

#if !defined(WIN32)
  PROFILE_Close(connectini);
#endif   // !WIN32

  init_connect_psi_keys();

  connect_hton= (handlerton *)p;
  connect_hton->state=   SHOW_OPTION_YES;
  connect_hton->create=  connect_create_handler;
  connect_hton->flags=   HTON_TEMPORARY_NOT_SUPPORTED | HTON_NO_PARTITION;
  connect_hton->table_options= connect_table_option_list;
  connect_hton->field_options= connect_field_option_list;
  connect_hton->tablefile_extensions= ha_connect_exts;
  connect_hton->discover_table_structure= connect_assisted_discovery;

  if (xtrace)
    sql_print_information("connect_init: hton=%p", p);

  DTVAL::SetTimeShift();      // Initialize time zone shift once for all
  DBUG_RETURN(0);
}


/**
  @brief
  Plugin clean up
*/
static int connect_done_func(void *p)
{
  int error= 0;
  PCONNECT pc, pn;
  DBUG_ENTER("connect_done_func");

#ifdef LIBXML2_SUPPORT
  XmlCleanupParserLib();
#endif   // LIBXML2_SUPPORT

#if !defined(WIN32)
  PROFILE_End();
#endif   // !WIN32

  for (pc= user_connect::to_users; pc; pc= pn) {
    if (pc->g)
      PlugCleanup(pc->g, true);

    pn= pc->next;
    delete pc;
    } // endfor pc

  DBUG_RETURN(error);
}


/**
  @brief
  Example of simple lock controls. The "share" it creates is a
  structure we will pass to each example handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

CONNECT_SHARE *ha_connect::get_share()
{
  CONNECT_SHARE *tmp_share;
  lock_shared_ha_data();
  if (!(tmp_share= static_cast<CONNECT_SHARE*>(get_ha_share_ptr())))
  {
    tmp_share= new CONNECT_SHARE;
    if (!tmp_share)
      goto err;
    mysql_mutex_init(con_key_mutex_CONNECT_SHARE_mutex,
                     &tmp_share->mutex, MY_MUTEX_INIT_FAST);
    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}


static handler* connect_create_handler(handlerton *hton,
                                   TABLE_SHARE *table,
                                   MEM_ROOT *mem_root)
{
  handler *h= new (mem_root) ha_connect(hton, table);

  if (xtrace)
    printf("New CONNECT %p, table: %s\n",
                         h, table ? table->table_name.str : "<null>");

  return h;
} // end of connect_create_handler

/****************************************************************************/
/*  ha_connect constructor.                                                 */
/****************************************************************************/
ha_connect::ha_connect(handlerton *hton, TABLE_SHARE *table_arg)
       :handler(hton, table_arg)
{
  hnum= ++num;
  xp= (table) ? GetUser(ha_thd(), NULL) : NULL;
  if (xp)
    xp->SetHandler(this);
  tdbp= NULL;
  sdvalin= NULL;
  sdvalout= NULL;
  xmod= MODE_ANY;
  istable= false;
//*tname= '\0';
  bzero((char*) &xinfo, sizeof(XINFO));
  valid_info= false;
  valid_query_id= 0;
  creat_query_id= (table && table->in_use) ? table->in_use->query_id : 0;
  stop= false;
  indexing= -1;
  locked= 0;
  data_file_name= NULL;
  index_file_name= NULL;
  enable_activate_all_index= 0;
  int_table_flags= (HA_NO_TRANSACTIONS | HA_NO_PREFIX_CHAR_KEYS);
  ref_length= sizeof(int);
  share= NULL;
  tshp= NULL;
} // end of ha_connect constructor


/****************************************************************************/
/*  ha_connect destructor.                                                  */
/****************************************************************************/
ha_connect::~ha_connect(void)
{
  if (xp) {
    PCONNECT p;

    xp->count--;

    for (p= user_connect::to_users; p; p= p->next)
      if (p == xp)
        break;

    if (p && !p->count) {
      if (p->next)
        p->next->previous= p->previous;

      if (p->previous)
        p->previous->next= p->next;
      else
        user_connect::to_users= p->next;

      } // endif p

    if (!xp->count) {
      PlugCleanup(xp->g, true);
      delete xp;
      } // endif count

    } // endif xp

} // end of ha_connect destructor


/****************************************************************************/
/*  Get a pointer to the user of this handler.                              */
/****************************************************************************/
static PCONNECT GetUser(THD *thd, PCONNECT xp)
{
  const char *dbn= NULL;

  if (!thd)
    return NULL;

  if (xp && thd == xp->thdp)
    return xp;

  for (xp= user_connect::to_users; xp; xp= xp->next)
    if (thd == xp->thdp)
      break;

  if (!xp) {
    xp= new user_connect(thd, dbn);

    if (xp->user_init()) {
      delete xp;
      xp= NULL;
      } // endif user_init

  } else
    xp->count++;

  return xp;
} // end of GetUser


/****************************************************************************/
/*  Get the global pointer of the user of this handler.                     */
/****************************************************************************/
static PGLOBAL GetPlug(THD *thd, PCONNECT lxp)
{
  lxp= GetUser(thd, lxp);
  return (lxp) ? lxp->g : NULL;
} // end of GetPlug


/****************************************************************************/
/*  Return the value of an option specified in the option list.             */
/****************************************************************************/
static char *GetListOption(PGLOBAL g, const char *opname,
                           const char *oplist, const char *def=NULL)
{
  char  key[16], val[256];
  char *pk, *pv, *pn;
  char *opval= (char*) def;
  int   n;

  for (pk= (char*)oplist; pk; pk= ++pn) {
    pn= strchr(pk, ',');
    pv= strchr(pk, '=');

    if (pv && (!pn || pv < pn)) {
      n= pv - pk;
      memcpy(key, pk, n);
      key[n]= 0;
      pv++;

      if (pn) {
        n= pn - pv;
        memcpy(val, pv, n);
        val[n]= 0;
      } else
        strcpy(val, pv);

    } else {
      if (pn) {
        n= min(pn - pk, 15);
        memcpy(key, pk, n);
        key[n]= 0;
      } else
        strcpy(key, pk);

      val[0]= 0;
    } // endif pv

    if (!stricmp(opname, key)) {
      opval= (char*)PlugSubAlloc(g, NULL, strlen(val) + 1);
      strcpy(opval, val);
      break;
    } else if (!pn)
      break;

    } // endfor pk

  return opval;
} // end of GetListOption

/****************************************************************************/
/*  Return the table option structure.                                      */
/****************************************************************************/
PTOS ha_connect::GetTableOptionStruct(TABLE *tab)
{
  return (tshp) ? tshp->option_struct : tab->s->option_struct;
} // end of GetTableOptionStruct

/****************************************************************************/
/*  Return the value of a string option or NULL if not specified.           */
/****************************************************************************/
char *ha_connect::GetStringOption(char *opname, char *sdef)
{
  char *opval= NULL;
  PTOS  options= GetTableOptionStruct(table);

  if (!options)
    ;
  else if (!stricmp(opname, "Type"))
    opval= (char*)options->type;
  else if (!stricmp(opname, "Filename"))
    opval= (char*)options->filename;
  else if (!stricmp(opname, "Optname"))
    opval= (char*)options->optname;
  else if (!stricmp(opname, "Tabname"))
    opval= (char*)options->tabname;
  else if (!stricmp(opname, "Tablist"))
    opval= (char*)options->tablist;
  else if (!stricmp(opname, "Database") ||
           !stricmp(opname, "DBname"))
    opval= (char*)options->dbname;
  else if (!stricmp(opname, "Separator"))
    opval= (char*)options->separator;
  else if (!stricmp(opname, "Connect"))
    opval= (tshp) ? tshp->connect_string.str : table->s->connect_string.str;
  else if (!stricmp(opname, "Qchar"))
    opval= (char*)options->qchar;
  else if (!stricmp(opname, "Module"))
    opval= (char*)options->module;
  else if (!stricmp(opname, "Subtype"))
    opval= (char*)options->subtype;
  else if (!stricmp(opname, "Catfunc"))
    opval= (char*)options->catfunc;
  else if (!stricmp(opname, "Srcdef"))
    opval= (char*)options->srcdef;
  else if (!stricmp(opname, "Colist"))
    opval= (char*)options->colist;
  else if (!stricmp(opname, "Data_charset"))
    opval= (char*)options->data_charset;

  if (!opval && options && options->oplist)
    opval= GetListOption(xp->g, opname, options->oplist);

  if (!opval) {
    if (sdef && !strcmp(sdef, "*")) {
      // Return the handler default value
      if (!stricmp(opname, "Dbname") || !stricmp(opname, "Database"))
        opval= (char*)GetDBName(NULL);    // Current database
      else if (!stricmp(opname, "Type"))  // Default type
        opval= (!options) ? NULL : 
               (options->srcdef)  ? (char*)"MYSQL" :
               (options->tabname) ? (char*)"PROXY" : (char*)"DOS";
      else if (!stricmp(opname, "User"))  // Connected user
        opval= (char *) "root";
      else if (!stricmp(opname, "Host"))  // Connected user host
        opval= (char *) "localhost";
      else
        opval= sdef;                      // Caller default

    } else
      opval= sdef;                        // Caller default

    } // endif !opval

  return opval;
} // end of GetStringOption

/****************************************************************************/
/*  Return the value of a Boolean option or bdef if not specified.          */
/****************************************************************************/
bool ha_connect::GetBooleanOption(char *opname, bool bdef)
{
  bool  opval= bdef;
  char *pv;
  PTOS  options= GetTableOptionStruct(table);

  if (!stricmp(opname, "View"))
    opval= (tshp) ? tshp->is_view : table->s->is_view;
  else if (!options)
    ;
  else if (!stricmp(opname, "Mapped"))
    opval= options->mapped;
  else if (!stricmp(opname, "Huge"))
    opval= options->huge;
//else if (!stricmp(opname, "Compressed"))
//  opval= options->compressed;
  else if (!stricmp(opname, "Split"))
    opval= options->split;
  else if (!stricmp(opname, "Readonly"))
    opval= options->readonly;
  else if (!stricmp(opname, "SepIndex"))
    opval= options->sepindex;
  else if (options->oplist)
    if ((pv= GetListOption(xp->g, opname, options->oplist)))
      opval= (!*pv || *pv == 'y' || *pv == 'Y' || atoi(pv) != 0);

  return opval;
} // end of GetBooleanOption

/****************************************************************************/
/*  Set the value of the opname option (does not work for oplist options)   */
/*  Currently used only to set the Sepindex value.                          */
/****************************************************************************/
bool ha_connect::SetBooleanOption(char *opname, bool b)
{
  PTOS options= GetTableOptionStruct(table);

  if (!options)
    return true;

  if (!stricmp(opname, "SepIndex"))
    options->sepindex= b;
  else
    return true;

  return false;
} // end of SetBooleanOption

/****************************************************************************/
/*  Return the value of an integer option or NO_IVAL if not specified.      */
/****************************************************************************/
int ha_connect::GetIntegerOption(char *opname)
{
  ulonglong opval= NO_IVAL;
  char     *pv;
  PTOS      options= GetTableOptionStruct(table);

  if (!options)
    ;
  else if (!stricmp(opname, "Lrecl"))
    opval= options->lrecl;
  else if (!stricmp(opname, "Elements"))
    opval= options->elements;
  else if (!stricmp(opname, "Estimate"))
//  opval= options->estimate;
    opval= (int)table->s->max_rows;
  else if (!stricmp(opname, "Avglen"))
    opval= (int)table->s->avg_row_length;
  else if (!stricmp(opname, "Multiple"))
    opval= options->multiple;
  else if (!stricmp(opname, "Header"))
    opval= options->header;
  else if (!stricmp(opname, "Quoted"))
    opval= options->quoted;
  else if (!stricmp(opname, "Ending"))
    opval= options->ending;
  else if (!stricmp(opname, "Compressed"))
    opval= (options->compressed);

  if (opval == (ulonglong)NO_IVAL && options && options->oplist)
    if ((pv= GetListOption(xp->g, opname, options->oplist)))
      opval= (unsigned)atoll(pv);

  return (int)opval;
} // end of GetIntegerOption

/****************************************************************************/
/*  Set the value of the opname option (does not work for oplist options)   */
/*  Currently used only to set the Lrecl value.                             */
/****************************************************************************/
bool ha_connect::SetIntegerOption(char *opname, int n)
{
  PTOS options= GetTableOptionStruct(table);

  if (!options)
    return true;

  if (!stricmp(opname, "Lrecl"))
    options->lrecl= n;
  else if (!stricmp(opname, "Elements"))
    options->elements= n;
//else if (!stricmp(opname, "Estimate"))
//  options->estimate= n;
  else if (!stricmp(opname, "Multiple"))
    options->multiple= n;
  else if (!stricmp(opname, "Header"))
    options->header= n;
  else if (!stricmp(opname, "Quoted"))
    options->quoted= n;
  else if (!stricmp(opname, "Ending"))
    options->ending= n;
  else if (!stricmp(opname, "Compressed"))
    options->compressed= n;
  else
    return true;
//else if (options->oplist)
//  SetListOption(opname, options->oplist, n);

  return false;
} // end of SetIntegerOption

/****************************************************************************/
/*  Return a field option structure.                                        */
/****************************************************************************/
PFOS ha_connect::GetFieldOptionStruct(Field *fdp)
{
  return fdp->option_struct;
} // end of GetFildOptionStruct

/****************************************************************************/
/*  Returns the column description structure used to make the column.       */
/****************************************************************************/
void *ha_connect::GetColumnOption(PGLOBAL g, void *field, PCOLINFO pcf)
{
  const char *cp;
  ha_field_option_struct *fop;
  Field*  fp;
  Field* *fldp;

  // Double test to be on the safe side
  if (!table)
    return NULL;

  // Find the column to describe
  if (field) {
    fldp= (Field**)field;
    fldp++;
  } else
    fldp= (tshp) ? tshp->field : table->field;

  if (!fldp || !(fp= *fldp))
    return NULL;

  // Get the CONNECT field options structure
  fop= GetFieldOptionStruct(fp);
  pcf->Flags= 0;

  // Now get column information
  pcf->Name= (char*)fp->field_name;

  if (fop && fop->special) {
    pcf->Fieldfmt= (char*)fop->special;
    pcf->Flags= U_SPECIAL;
    return fldp;
    } // endif special

  pcf->Prec= 0;
  pcf->Opt= (fop) ? (int)fop->opt : 0;

  if ((pcf->Length= fp->field_length) < 0)
    pcf->Length= 256;            // BLOB?

  if (fop) {
    pcf->Offset= (int)fop->offset;
//  pcf->Freq= fop->freq;
    pcf->Datefmt= (char*)fop->dateformat;
    pcf->Fieldfmt= (char*)fop->fieldformat;
  } else {
    pcf->Offset= -1;
//  pcf->Freq= 0;
    pcf->Datefmt= NULL;
    pcf->Fieldfmt= NULL;
  } // endif fop

  switch (fp->type()) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
      pcf->Flags |= U_VAR;
      /* no break */
    default:
      pcf->Type= MYSQLtoPLG(fp->type());
      break;
    } // endswitch SQL type

  switch (pcf->Type) {
    case TYPE_STRING:
      // Do something for case
      cp= fp->charset()->name;

      // Find if collation name ends by _ci
      if (!strcmp(cp + strlen(cp) - 3, "_ci")) {
        pcf->Prec= 1;      // Case insensitive
        pcf->Opt= 0;       // Prevent index opt until it is safe
        } // endif ci

      break;
    case TYPE_FLOAT:
      pcf->Prec= max(min(fp->decimals(), ((unsigned)pcf->Length - 2)), 0);
      break;
    case TYPE_DATE:
      // Field_length is only used for DATE columns
      if (fop->fldlen)
        pcf->Length= (int)fop->fldlen;
      else { 
        int len;

        if (pcf->Datefmt) {
          // Find the (max) length produced by the date format
          char    buf[256];
          PGLOBAL g= GetPlug(table->in_use, xp);
          PDTP    pdtp= MakeDateFormat(g, pcf->Datefmt, false, true, 0);
          struct tm datm;
          bzero(&datm, sizeof(datm));
          datm.tm_mday= 12;
          datm.tm_mon= 11;
          datm.tm_year= 112;
          len= strftime(buf, 256, pdtp->OutFmt, &datm);
        } else
          len= 0;

        // 11 is for signed numeric representation of the date
        pcf->Length= (len) ? len : 11;
        } // endelse

      break;
    default:
      break;
    } // endswitch type

  // This is used to skip null bit
  if (fp->real_maybe_null())
    pcf->Flags |= U_NULLS;

  // Mark virtual columns as such
  if (fp->vcol_info && !fp->stored_in_db)
    pcf->Flags |= U_VIRTUAL;

  pcf->Key= 0;   // Not used when called from MySQL

  // Get the comment if any
  if (fp->comment.str && fp->comment.length) {
    pcf->Remark= (char*)PlugSubAlloc(g, NULL, fp->comment.length + 1);
    memcpy(pcf->Remark, fp->comment.str, fp->comment.length);
    pcf->Remark[fp->comment.length]= 0;
  } else
    pcf->Remark= NULL;

  return fldp;
} // end of GetColumnOption

/****************************************************************************/
/*  Returns the index description structure used to make the index.         */
/****************************************************************************/
PIXDEF ha_connect::GetIndexInfo(void)
{
  char    *name, *pn;
  bool     unique;
  PIXDEF   xdp, pxd=NULL, toidx= NULL;
  PKPDEF   kpp, pkp;
  PGLOBAL& g= xp->g;
  KEY      kp;

  for (int n= 0; (unsigned)n < table->s->keynames.count; n++) {
    if (xtrace)
      printf("Getting created index %d info\n", n + 1);

    // Find the index to describe
    kp= table->s->key_info[n];

    // Now get index information
    pn= (char*)table->s->keynames.type_names[n];
    name= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
    strcpy(name, pn);    // This is probably unuseful
    unique= (kp.flags & 1) != 0;
    pkp= NULL;

    // Allocate the index description block
    xdp= new(g) INDEXDEF(name, unique, n);

    // Get the the key parts info
    for (int k= 0; (unsigned)k < kp.user_defined_key_parts; k++) {
      pn= (char*)kp.key_part[k].field->field_name;
      name= (char*)PlugSubAlloc(g, NULL, strlen(pn) + 1);
      strcpy(name, pn);    // This is probably unuseful

      // Allocate the key part description block
      kpp= new(g) KPARTDEF(name, k + 1);
      kpp->SetKlen(kp.key_part[k].length);

#if 0             // NIY
    // Index on auto increment column can be an XXROW index
    if (kp.key_part[k].field->flags & AUTO_INCREMENT_FLAG && 
        kp.uder_defined_key_parts == 1) {
      char   *type= GetStringOption("Type", "DOS");
      TABTYPE typ= GetTypeID(type);

      xdp->SetAuto(IsTypeFixed(typ));
      } // endif AUTO_INCREMENT
#endif // 0

      if (pkp)
        pkp->SetNext(kpp);
      else
        xdp->SetToKeyParts(kpp);

      pkp= kpp;
      } // endfor k

    xdp->SetNParts(kp.user_defined_key_parts);

    if (pxd)
      pxd->SetNext(xdp);
    else
      toidx= xdp;

    pxd= xdp;
    } // endfor n

  return toidx;
} // end of GetIndexInfo

const char *ha_connect::GetDBName(const char* name)
{
  return (name) ? name : table->s->db.str;
} // end of GetDBName

const char *ha_connect::GetTableName(void)
{
  return (tshp) ? tshp->table_name.str : table->s->table_name.str;
} // end of GetTableName

#if 0
/****************************************************************************/
/*  Returns the column real or special name length of a field.              */
/****************************************************************************/
int ha_connect::GetColNameLen(Field *fp)
{
  int n;
  PFOS fop= GetFieldOptionStruct(fp);

  // Now get the column name length
  if (fop && fop->special)
    n= strlen(fop->special) + 1;
  else
    n= strlen(fp->field_name);

  return n;
} // end of GetColNameLen

/****************************************************************************/
/*  Returns the column real or special name of a field.                     */
/****************************************************************************/
char *ha_connect::GetColName(Field *fp)
{
  PFOS fop= GetFieldOptionStruct(fp);

  return (fop && fop->special) ? fop->special : (char*)fp->field_name;
} // end of GetColName

/****************************************************************************/
/*  Adds the column real or special name of a field to a string.            */
/****************************************************************************/
void ha_connect::AddColName(char *cp, Field *fp)
{
  PFOS fop= GetFieldOptionStruct(fp);

  // Now add the column name
  if (fop && fop->special)
    // The prefix * mark the column as "special"
    strcat(strcpy(cp, "*"), strupr(fop->special));
  else
    strcpy(cp, (char*)fp->field_name);

} // end of AddColName
#endif // 0

/****************************************************************************/
/*  Get the table description block of a CONNECT table.                     */
/****************************************************************************/
PTDB ha_connect::GetTDB(PGLOBAL g)
{
  const char *table_name;
  PTDB        tp;

  // Double test to be on the safe side
  if (!g || !table)
    return NULL;

  table_name= GetTableName();

  if (!xp->CheckQuery(valid_query_id) && tdbp
                      && !stricmp(tdbp->GetName(), table_name)
                      && (tdbp->GetMode() == xmod 
                       || tdbp->GetAmType() == TYPE_AM_XML)) {
    tp= tdbp;
    tp->SetMode(xmod);
  } else if ((tp= CntGetTDB(g, table_name, xmod, this)))
    valid_query_id= xp->last_query_id;
  else
    printf("GetTDB: %s\n", g->Message);

  return tp;
} // end of GetTDB

/****************************************************************************/
/*  Open a CONNECT table, restricting column list if cols is true.          */
/****************************************************************************/
bool ha_connect::OpenTable(PGLOBAL g, bool del)
{
  bool  rc= false;
  char *c1= NULL, *c2=NULL;

  // Double test to be on the safe side
  if (!g || !table) {
    printf("OpenTable logical error; g=%p table=%p\n", g, table);
    return true;
    } // endif g

  if (!(tdbp= GetTDB(g)))
    return true;
  else if (tdbp->IsReadOnly())
    switch (xmod) {
      case MODE_WRITE:
      case MODE_INSERT:
      case MODE_UPDATE:
      case MODE_DELETE:
        strcpy(g->Message, MSG(READ_ONLY));
        return true;
      default:
        break;
      } // endswitch xmode

  if (xmod != MODE_INSERT || tdbp->GetAmType() == TYPE_AM_ODBC
                          || tdbp->GetAmType() == TYPE_AM_MYSQL) {
    // Get the list of used fields (columns)
    char        *p;
    unsigned int k1, k2, n1, n2;
    Field*      *field;
    Field*       fp;
    MY_BITMAP   *map= (xmod == MODE_INSERT) ? table->write_set : table->read_set;
    MY_BITMAP   *ump= (xmod == MODE_UPDATE) ? table->write_set : NULL;

    k1= k2= 0;
    n1= n2= 1;         // 1 is space for final null character

    for (field= table->field; fp= *field; field++) {
      if (bitmap_is_set(map, fp->field_index)) {
        n1+= (strlen(fp->field_name) + 1);
        k1++;
        } // endif

      if (ump && bitmap_is_set(ump, fp->field_index)) {
        n2+= (strlen(fp->field_name) + 1);
        k2++;
        } // endif

      } // endfor field

    if (k1) {
      p= c1= (char*)PlugSubAlloc(g, NULL, n1);

      for (field= table->field; fp= *field; field++)
        if (bitmap_is_set(map, fp->field_index)) {
          strcpy(p, (char*)fp->field_name);
          p+= (strlen(p) + 1);
          } // endif used field

      *p= '\0';          // mark end of list
      } // endif k1

    if (k2) {
      p= c2= (char*)PlugSubAlloc(g, NULL, n2);

      for (field= table->field; fp= *field; field++)
        if (bitmap_is_set(ump, fp->field_index)) {
          strcpy(p, (char*)fp->field_name);
          p+= (strlen(p) + 1);
          } // endif used field

      *p= '\0';          // mark end of list
      } // endif k2

    } // endif xmod

  // Open the table
  if (!(rc= CntOpenTable(g, tdbp, xmod, c1, c2, del, this))) {
    istable= true;
//  strmake(tname, table_name, sizeof(tname)-1);

    // We may be in a create index query
    if (xmod == MODE_ANY && *tdbp->GetName() != '#') {
      // The current indexes
      PIXDEF oldpix= GetIndexInfo();
      } // endif xmod

//  tdbp->SetOrig((PTBX)table);  // used by CheckCond
  } else
    printf("OpenTable: %s\n", g->Message);

  if (rc) {
    tdbp= NULL;
    valid_info= false;
    } // endif rc

  return rc;
} // end of OpenTable


/****************************************************************************/
/*  IsOpened: returns true if the table is already opened.                  */
/****************************************************************************/
bool ha_connect::IsOpened(void)
{
  return (!xp->CheckQuery(valid_query_id) && tdbp
                                          && tdbp->GetUse() == USE_OPEN);
} // end of IsOpened


/****************************************************************************/
/*  Close a CONNECT table.                                                  */
/****************************************************************************/
int ha_connect::CloseTable(PGLOBAL g)
{
  int rc= CntCloseTable(g, tdbp);
  tdbp= NULL;
  sdvalin=NULL;
  sdvalout=NULL;
  valid_info= false;
  indexing= -1;
  return rc;
} // end of CloseTable


/***********************************************************************/
/*  Make a pseudo record from current row values. Specific to MySQL.   */
/***********************************************************************/
int ha_connect::MakeRecord(char *buf)
{
  char            *p, *fmt, val[32];
  int              rc= 0;
  Field*          *field;
  Field           *fp;
  my_bitmap_map   *org_bitmap;
  CHARSET_INFO    *charset= tdbp->data_charset();
  const MY_BITMAP *map;
  PVAL             value;
  PCOL             colp= NULL;
  DBUG_ENTER("ha_connect::MakeRecord");

  if (xtrace > 1)
    printf("Maps: read=%08X write=%08X vcol=%08X defr=%08X defw=%08X\n",
            *table->read_set->bitmap, *table->write_set->bitmap,
            *table->vcol_set->bitmap,
            *table->def_read_set.bitmap, *table->def_write_set.bitmap);

  // Avoid asserts in field::store() for columns that are not updated
  org_bitmap= dbug_tmp_use_all_columns(table, table->write_set);

  // This is for variable_length rows
  memset(buf, 0, table->s->null_bytes);

  // When sorting read_set selects all columns, so we use def_read_set
  map= (const MY_BITMAP *)&table->def_read_set;

  // Make the pseudo record from field values
  for (field= table->field; *field && !rc; field++) {
    fp= *field;

    if (fp->vcol_info && !fp->stored_in_db)
      continue;            // This is a virtual column

    if (bitmap_is_set(map, fp->field_index)) {
      // This is a used field, fill the buffer with value
      for (colp= tdbp->GetColumns(); colp; colp= colp->GetNext())
        if (!stricmp(colp->GetName(), (char*)fp->field_name))
          break;

      if (!colp) {
        printf("Column %s not found\n", fp->field_name);
        dbug_tmp_restore_column_map(table->write_set, org_bitmap);
        DBUG_RETURN(HA_ERR_WRONG_IN_RECORD);
        } // endif colp

      value= colp->GetValue();

      // All this could be better optimized
      if (!value->IsNull()) {
        switch (value->GetType()) {
          case TYPE_DATE:
            if (!sdvalout)
              sdvalout= AllocateValue(xp->g, TYPE_STRING, 20);
      
            switch (fp->type()) {
              case MYSQL_TYPE_DATE:
                fmt= "%Y-%m-%d";
                break;
              case MYSQL_TYPE_TIME:
                fmt= "%H:%M:%S";
                break;
              default:
                fmt= "%Y-%m-%d %H:%M:%S";
                break;
              } // endswitch type
      
            // Get date in the format required by MySQL fields
            value->FormatValue(sdvalout, fmt);
            p= sdvalout->GetCharValue();
            break;
          case TYPE_FLOAT:
            p= NULL;
            break;
          case TYPE_STRING:
            // Passthru
          default:
            p= value->GetCharString(val);
            break;
          } // endswitch Type

        if (p) {
          if (fp->store(p, strlen(p), charset, CHECK_FIELD_WARN)) {
            // Avoid "error" on null fields
            if (value->GetIntValue())
              rc= HA_ERR_WRONG_IN_RECORD;
    
            DBUG_PRINT("MakeRecord", ("%s", p));
            } // endif store
    
        } else
          if (fp->store(value->GetFloatValue())) {
            rc= HA_ERR_WRONG_IN_RECORD;
            DBUG_PRINT("MakeRecord", ("%s", value->GetCharString(val)));
            } // endif store

        fp->set_notnull();
      } else
        fp->set_null();

      } // endif bitmap

    } // endfor field

  // This is copied from ha_tina and is necessary to avoid asserts
  dbug_tmp_restore_column_map(table->write_set, org_bitmap);
  DBUG_RETURN(rc);
} // end of MakeRecord


/***********************************************************************/
/*  Set row values from a MySQL pseudo record. Specific to MySQL.      */
/***********************************************************************/
int ha_connect::ScanRecord(PGLOBAL g, uchar *buf)
{
  char    attr_buffer[1024];
  char    data_buffer[1024];
  char   *fmt;
  int     rc= 0;
  PCOL    colp;
  PVAL    value;
  Field  *fp;
  PTDBASE tp= (PTDBASE)tdbp;
  String  attribute(attr_buffer, sizeof(attr_buffer),
                    table->s->table_charset);
  my_bitmap_map *bmap= dbug_tmp_use_all_columns(table, table->read_set);
  const CHARSET_INFO *charset= tdbp->data_charset();
  String  data_charset_value(data_buffer, sizeof(data_buffer),  charset);

  // Scan the pseudo record for field values and set column values
  for (Field **field=table->field ; *field ; field++) {
    fp= *field;

    if ((fp->vcol_info && !fp->stored_in_db) ||
         fp->option_struct->special)
      continue;            // Is a virtual column possible here ???

    if ((xmod == MODE_INSERT && tdbp->GetAmType() != TYPE_AM_MYSQL
                             && tdbp->GetAmType() != TYPE_AM_ODBC) ||
        bitmap_is_set(table->write_set, fp->field_index)) {
      for (colp= tp->GetSetCols(); colp; colp= colp->GetNext())
        if (!stricmp(colp->GetName(), fp->field_name))
          break;

      if (!colp) {
        printf("Column %s not found\n", fp->field_name);
        rc= HA_ERR_WRONG_IN_RECORD;
        goto err;
      } else
        value= colp->GetValue();

      // This is a used field, fill the value from the row buffer
      // All this could be better optimized
      if (fp->is_null()) {
        if (colp->IsNullable())
          value->SetNull(true);

        value->Reset();
      } else switch (value->GetType()) {
        case TYPE_FLOAT:
          value->SetValue(fp->val_real());
          break;
        case TYPE_DATE:
          if (!sdvalin) {
            sdvalin= (DTVAL*)AllocateValue(xp->g, TYPE_DATE, 19);

            // Get date in the format produced by MySQL fields
            switch (fp->type()) {
              case MYSQL_TYPE_DATE:
                fmt= "YYYY-MM-DD";
                break;
              case MYSQL_TYPE_TIME:
                fmt= "hh:mm:ss";
                break;
              default:
                fmt= "YYYY-MM-DD hh:mm:ss";
              } // endswitch type

            ((DTVAL*)sdvalin)->SetFormat(g, fmt, strlen(fmt));
            } // endif sdvalin

          fp->val_str(&attribute);
          sdvalin->SetValue_psz(attribute.c_ptr_safe());
          value->SetValue_pval(sdvalin);
          break;
        default:
          fp->val_str(&attribute);
          if (charset == &my_charset_bin)
          {
            value->SetValue_psz(attribute.c_ptr_safe());
          }
          else
          {
            // Convert from SQL field charset to DATA_CHARSET
            uint cnv_errors;
            data_charset_value.copy(attribute.ptr(), attribute.length(),
                                    attribute.charset(), charset, &cnv_errors);
            value->SetValue_psz(data_charset_value.c_ptr_safe());
          }
          break;
        } // endswitch Type

#ifdef NEWCHANGE
    } else if (xmod == MODE_UPDATE) {
      PCOL cp;

      for (cp= tp->GetColumns(); cp; cp= cp->GetNext())
        if (!stricmp(colp->GetName(), cp->GetName()))
          break;

      if (!cp) {
        rc= HA_ERR_WRONG_IN_RECORD;
        goto err;
        } // endif cp

      value->SetValue_pval(cp->GetValue());
    } else // mode Insert
      value->Reset();
#else
    } // endif bitmap_is_set
#endif

    } // endfor field

 err:
  dbug_tmp_restore_column_map(table->read_set, bmap);
  return rc;
} // end of ScanRecord


/***********************************************************************/
/*  Check change in index column. Specific to MySQL.                   */
/*  Should be elaborated to check for real changes.                    */
/***********************************************************************/
int ha_connect::CheckRecord(PGLOBAL g, const uchar *oldbuf, uchar *newbuf)
{
  return ScanRecord(g, newbuf);
} // end of dummy CheckRecord


/***********************************************************************/
/*  Return the string representing an operator.                        */
/***********************************************************************/
const char *ha_connect::GetValStr(OPVAL vop, bool neg)
{
  const char *val;

  switch (vop) {
    case OP_EQ:
      val= " = ";
      break;
    case OP_NE:
      val= " <> ";
      break;
    case OP_GT:
      val= " > ";
      break;
    case OP_GE:
      val= " >= ";
      break;
    case OP_LT:
      val= " < ";
      break;
    case OP_LE:
      val= " <= ";
      break;
    case OP_IN:
      val= (neg) ? " NOT IN (" : " IN (";
      break;
    case OP_NULL:
      val= " IS NULL";
      break;
    case OP_LIKE:
      val= " LIKE ";
      break;
    case OP_XX:
      val= " BETWEEN ";
      break;
    case OP_EXIST:
      val= " EXISTS ";
      break;
    case OP_AND:
      val= " AND ";
      break;
    case OP_OR:
      val= " OR ";
      break;
    case OP_NOT:
      val= " NOT ";
      break;
    case OP_CNC:
      val= " || ";
      break;
    case OP_ADD:
      val= " + ";
      break;
    case OP_SUB:
      val= " - ";
      break;
    case OP_MULT:
      val= " * ";
      break;
    case OP_DIV:
      val= " / ";
      break;
    default:
      val= " ? ";
      break;
    } /* endswitch */

  return val;
} // end of GetValStr


/***********************************************************************/
/*  Check the WHERE condition and return an ODBC/WQL filter.           */
/***********************************************************************/
PFIL ha_connect::CheckCond(PGLOBAL g, PFIL filp, AMT tty, Item *cond)
{
  unsigned int i;
  bool  ismul= false;
  PPARM pfirst= NULL, pprec= NULL, pp[2]= {NULL, NULL};
  OPVAL vop= OP_XX;

  if (!cond)
    return NULL;

  if (xtrace > 1)
    printf("Cond type=%d\n", cond->type());

  if (cond->type() == COND::COND_ITEM) {
    char      *p1, *p2;
    Item_cond *cond_item= (Item_cond *)cond;

    if (xtrace > 1)
      printf("Cond: Ftype=%d name=%s\n", cond_item->functype(),
                                         cond_item->func_name());

    switch (cond_item->functype()) {
      case Item_func::COND_AND_FUNC: vop= OP_AND; break;
      case Item_func::COND_OR_FUNC:  vop= OP_OR;  break;
      default: return NULL;
      } // endswitch functype

    List<Item>* arglist= cond_item->argument_list();
    List_iterator<Item> li(*arglist);
    Item *subitem;

    p1= filp + strlen(filp);
    strcpy(p1, "(");
    p2= p1 + 1;

    for (i= 0; i < arglist->elements; i++)
      if ((subitem= li++)) {
        if (!CheckCond(g, filp, tty, subitem)) {
          if (vop == OP_OR)
            return NULL;
          else
            *p2= 0;

        } else {
          p1= p2 + strlen(p2);
          strcpy(p1, GetValStr(vop, FALSE));
          p2= p1 + strlen(p1);
        } // endif CheckCond

      } else
        return NULL;

    if (*p1 != '(')
      strcpy(p1, ")");
    else
      return NULL;

  } else if (cond->type() == COND::FUNC_ITEM) {
    unsigned int i;
//  int   n;
    bool  iscol, neg= FALSE;
    Item_func *condf= (Item_func *)cond;
    Item*     *args= condf->arguments();

    if (xtrace > 1)
      printf("Func type=%d argnum=%d\n", condf->functype(),
                                         condf->argument_count());

//  neg= condf->

    switch (condf->functype()) {
      case Item_func::EQUAL_FUNC:
      case Item_func::EQ_FUNC: vop= OP_EQ;  break;
      case Item_func::NE_FUNC: vop= OP_NE;  break;
      case Item_func::LT_FUNC: vop= OP_LT;  break;
      case Item_func::LE_FUNC: vop= OP_LE;  break;
      case Item_func::GE_FUNC: vop= OP_GE;  break;
      case Item_func::GT_FUNC: vop= OP_GT;  break;
      case Item_func::IN_FUNC: vop= OP_IN;
        neg= ((Item_func_opt_neg *)condf)->negated;
      case Item_func::BETWEEN: ismul= true; break;
      default: return NULL;
      } // endswitch functype

    if (condf->argument_count() < 2)
      return NULL;
    else if (ismul && tty == TYPE_AM_WMI)
      return NULL;        // Not supported by WQL

    for (i= 0; i < condf->argument_count(); i++) {
      if (xtrace > 1)
        printf("Argtype(%d)=%d\n", i, args[i]->type());

      if (i >= 2 && !ismul) {
        if (xtrace > 1)
          printf("Unexpected arg for vop=%d\n", vop);

        continue;
        } // endif i

      if ((iscol= args[i]->type() == COND::FIELD_ITEM)) {
        const char *fnm;
        ha_field_option_struct *fop;
        Item_field *pField= (Item_field *)args[i];

        if (pField->field->table != table)
          return NULL;  // Field does not belong to this table
        else
          fop= GetFieldOptionStruct(pField->field);

        if (fop && fop->special) {
          if (tty == TYPE_AM_TBL && !stricmp(fop->special, "TABID"))
            fnm= "TABID";
          else
            return NULL;

        } else if (tty == TYPE_AM_TBL)
          return NULL;
        else
          fnm= pField->field->field_name;

        if (xtrace > 1) {
          printf("Field index=%d\n", pField->field->field_index);
          printf("Field name=%s\n", pField->field->field_name);
          } // endif xtrace

        // IN and BETWEEN clauses should be col VOP list
        if (i && ismul)
          return NULL;

        strcat(filp, fnm);
      } else {
        char   buff[256];
        String *res, tmp(buff,sizeof(buff), &my_charset_bin);
        Item_basic_constant *pval= (Item_basic_constant *)args[i];

        if ((res= pval->val_str(&tmp)) == NULL)
          return NULL;                      // To be clarified

        if (xtrace > 1)
          printf("Value=%.*s\n", res->length(), res->ptr());

        // IN and BETWEEN clauses should be col VOP list
        if (!i && ismul)
          return NULL;

        // Append the value to the filter
        if (args[i]->type() == COND::STRING_ITEM)
          strcat(strcat(strcat(filp, "'"), res->ptr()), "'");
        else
          strncat(filp, res->ptr(), res->length());

      } // endif

      if (!i)
        strcat(filp, GetValStr(vop, neg));
      else if (vop == OP_XX && i == 1)
        strcat(filp, " AND ");
      else if (vop == OP_IN)
        strcat(filp, (i == condf->argument_count() - 1) ? ")" : ",");

      } // endfor i

  } else {
    if (xtrace > 1)
      printf("Unsupported condition\n");

    return NULL;
  } // endif's type

  return filp;
} // end of CheckCond


 /**
   Push condition down to the table handler.

   @param  cond   Condition to be pushed. The condition tree must not be
                  modified by the caller.

   @return
     The 'remainder' condition that caller must use to filter out records.
     NULL means the handler will not return rows that do not match the
     passed condition.

   @note
     CONNECT handles the filtering only for table types that construct
     an SQL or WQL query, but still leaves it to MySQL because only some
     parts of the filter may be relevant.
     The first suballocate finds the position where the string will be
     constructed in the sarea. The second one does make the suballocation
     with the proper length.
 */
const COND *ha_connect::cond_push(const COND *cond)
{
  DBUG_ENTER("ha_connect::cond_push");

  if (tdbp) {
    AMT tty= tdbp->GetAmType();

    if (tty == TYPE_AM_WMI || tty == TYPE_AM_ODBC ||
        tty == TYPE_AM_TBL || tty == TYPE_AM_MYSQL) {
      PGLOBAL& g= xp->g;
      PFIL filp= (PFIL)PlugSubAlloc(g, NULL, 0);

      *filp= 0;

      if (CheckCond(g, filp, tty, (Item *)cond)) {
        if (xtrace)
          puts(filp);

        tdbp->SetFilter(filp);
//      cond= NULL;     // This does not work anyway
        PlugSubAlloc(g, NULL, strlen(filp) + 1);
        } // endif filp

      } // endif tty

    } // endif tdbp

  // Let MySQL do the filtering
  DBUG_RETURN(cond);
} // end of cond_push

/**
  Number of rows in table. It will only be called if
  (table_flags() & (HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT)) != 0
*/
ha_rows ha_connect::records()
{
  if (!valid_info)
    info(HA_STATUS_VARIABLE);

  if (tdbp && tdbp->Cardinality(NULL))
    return stats.records;
  else
    return HA_POS_ERROR;

} // end of records


/**
  Return an error message specific to this handler.

  @param error  error code previously returned by handler
  @param buf    pointer to String where to add error message

  @return
    Returns true if this is a temporary error
*/
bool ha_connect::get_error_message(int error, String* buf)
{
  DBUG_ENTER("ha_connect::get_error_message");

  if (xp && xp->g)
    buf->copy(xp->g->Message, (uint)strlen(xp->g->Message),
              system_charset_info);

  DBUG_RETURN(false);
} // end of get_error_message


/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @note
  For CONNECT no open can be done here because field information is not yet
  updated. >>>>> TO BE CHECKED <<<<<
  (Thread information could be get by using 'ha_thd')

  @see
  handler::ha_open() in handler.cc
*/
int ha_connect::open(const char *name, int mode, uint test_if_locked)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::open");

  if (xtrace)
     printf("open: name=%s mode=%d test=%u\n", name, mode, test_if_locked);

  if (!(share= get_share()))
    DBUG_RETURN(1);

  thr_lock_data_init(&share->lock,&lock,NULL);

  // Try to get the user if possible
  xp= GetUser(ha_thd(), xp);
  PGLOBAL g= (xp) ? xp->g : NULL;

  // Try to set the database environment
  if (g)
    rc= (CntCheckDB(g, this, name)) ? (-2) : 0;
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of open

/**
  @brief
  Make the indexes for this table
*/
int ha_connect::optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  PDBUSER  dup= PlgGetUser(g);

  // Ignore error on the opt file
  dup->Check &= ~CHK_OPT;
  tdbp= GetTDB(g);
  dup->Check |= CHK_OPT;

  if (tdbp || (tdbp= GetTDB(g))) {
    if (!((PTDBASE)tdbp)->GetDef()->Indexable()) {
      sprintf(g->Message, "Table %s is not indexable", tdbp->GetName());
      rc= HA_ERR_INTERNAL_ERROR;
    } else if ((rc= ((PTDBASE)tdbp)->ResetTableOpt(g, true))) {
      if (rc == RC_INFO) {
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        rc= 0;
      } else
        rc= HA_ERR_INTERNAL_ERROR;

    } // endif's

  } else
    rc= HA_ERR_INTERNAL_ERROR;

  return rc;
} // end of optimize

/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/
int ha_connect::close(void)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::close");

  // If this is called by a later query, the table may have
  // been already closed and the tdbp is not valid anymore.
  if (tdbp && xp->last_query_id == valid_query_id)
    rc= CloseTable(xp->g);

  DBUG_RETURN(rc);
} // end of close


/**
  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

    @details
  Example of this would be:
    @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
    @endcode

  See ha_tina.cc for an example of extracting all of the data as strings.
  ha_berekly.cc has an example of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments and timestamps. This
  case also applies to write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

    @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/
int ha_connect::write_row(uchar *buf)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("ha_connect::write_row");

  // Open the table if it was not opened yet (locked)
  if (!IsOpened() || xmod != tdbp->GetMode()) {
    if (IsOpened())
      CloseTable(g);

    if (OpenTable(g)) {
      if (strstr(g->Message, "read only"))
        rc= HA_ERR_TABLE_READONLY;
      else
        rc= HA_ERR_INITIALIZATION;

      DBUG_RETURN(rc);
      } // endif tdbp

    } // endif isopened

  if (tdbp->GetMode() == MODE_ANY)
    DBUG_RETURN(0);

#if 0                // AUTO_INCREMENT NIY
  if (table->next_number_field && buf == table->record[0]) {
    int error;

    if ((error= update_auto_increment()))
      return error;

    } // endif nex_number_field
#endif // 0

  // Set column values from the passed pseudo record
  if ((rc= ScanRecord(g, buf)))
    DBUG_RETURN(rc);

  // Return result code from write operation
  if (CntWriteRow(g, tdbp)) {
    DBUG_PRINT("write_row", ("%s", g->Message));
    printf("write_row: %s\n", g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
    } // endif RC

  DBUG_RETURN(rc);
} // end of write_row


/**
  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

    @details
  Currently new_data will not have an updated auto_increament record, or
  and updated timestamp field. You can do these for example by doing:
    @code
  if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
    table->timestamp_field->set_time();
  if (table->next_number_field && record == table->record[0])
    update_auto_increment();
    @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

    @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_connect::update_row(const uchar *old_data, uchar *new_data)
{
  int      rc= 0;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("ha_connect::update_row");

  if (xtrace > 1)
    printf("update_row: old=%s new=%s\n", old_data, new_data);

  // Check values for possible change in indexed column
  if ((rc= CheckRecord(g, old_data, new_data)))
    return rc;

  if (CntUpdateRow(g, tdbp)) {
    DBUG_PRINT("update_row", ("%s", g->Message));
    printf("update_row CONNECT: %s\n", g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
    } // endif RC

  DBUG_RETURN(rc);
} // end of update_row


/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/
int ha_connect::delete_row(const uchar *buf)
{
  int rc= 0;
  DBUG_ENTER("ha_connect::delete_row");

  if (CntDeleteRow(xp->g, tdbp, false)) {
    rc= HA_ERR_INTERNAL_ERROR;
    printf("delete_row CONNECT: %s\n", xp->g->Message);
    } // endif DeleteRow

  DBUG_RETURN(rc);
} // end of delete_row


/****************************************************************************/
/*  We seem to come here at the begining of an index use.                   */
/****************************************************************************/
int ha_connect::index_init(uint idx, bool sorted)
{
  int rc;
  PGLOBAL& g= xp->g;
  DBUG_ENTER("index_init");

  if ((rc= rnd_init(0)))
    return rc;

  if (locked == 2) {
    // Indexes are not updated in lock write mode
    active_index= MAX_KEY;
    indexing= 0;
    DBUG_RETURN(0);
    } // endif locked

  indexing= CntIndexInit(g, tdbp, (signed)idx);

  if (indexing <= 0) {
    DBUG_PRINT("index_init", ("%s", g->Message));
    printf("index_init CONNECT: %s\n", g->Message);
    active_index= MAX_KEY;
    rc= HA_ERR_INTERNAL_ERROR;
  } else {
    if (((PTDBDOX)tdbp)->To_Kindex->GetNum_K()) {
      if (((PTDBASE)tdbp)->GetFtype() != RECFM_NAF)
        ((PTDBDOX)tdbp)->GetTxfp()->ResetBuffer(g);

      active_index= idx;
    } else        // Void table
      indexing= 0;

    rc= 0;
  } // endif indexing

  DBUG_RETURN(rc);
} // end of index_init

/****************************************************************************/
/*  We seem to come here at the end of an index use.                        */
/****************************************************************************/
int ha_connect::index_end()
{
  DBUG_ENTER("index_end");
  active_index= MAX_KEY;
  DBUG_RETURN(rnd_end());
} // end of index_end


/****************************************************************************/
/*  This is internally called by all indexed reading functions.             */
/****************************************************************************/
int ha_connect::ReadIndexed(uchar *buf, OPVAL op, const uchar *key, uint key_len)
{
  int rc;

//statistic_increment(ha_read_key_count, &LOCK_status);

  switch (CntIndexRead(xp->g, tdbp, op, key, (int)key_len)) {
    case RC_OK:
      xp->fnd++;
      rc= MakeRecord((char*)buf);
      break;
    case RC_EF:         // End of file
      rc= HA_ERR_END_OF_FILE;
      break;
    case RC_NF:         // Not found
      xp->nfd++;
      rc= (op == OP_SAME) ? HA_ERR_END_OF_FILE : HA_ERR_KEY_NOT_FOUND;
      break;
    default:          // Read error
      DBUG_PRINT("ReadIndexed", ("%s", xp->g->Message));
      printf("ReadIndexed: %s\n", xp->g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
      break;
    } // endswitch RC

  if (xtrace > 1)
    printf("ReadIndexed: op=%d rc=%d\n", op, rc);

  table->status= (rc == RC_OK) ? 0 : STATUS_NOT_FOUND;
  return rc;
} // end of ReadIndexed


#ifdef NOT_USED
/**
  @brief
  Positions an index cursor to the index specified in the handle. Fetches the
  row if available. If the key value is null, begin at the first key of the
  index.
*/
int ha_connect::index_read_map(uchar *buf, const uchar *key,
                               key_part_map keypart_map __attribute__((unused)),
                               enum ha_rkey_function find_flag
                               __attribute__((unused)))
{
  DBUG_ENTER("ha_connect::index_read");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/****************************************************************************/
/*  This is called by handler::index_read_map.                              */
/****************************************************************************/
int ha_connect::index_read(uchar * buf, const uchar * key, uint key_len,
                           enum ha_rkey_function find_flag)
{
  int rc;
  OPVAL op= OP_XX;
  DBUG_ENTER("ha_connect::index_read");

  switch(find_flag) {
    case HA_READ_KEY_EXACT:   op= OP_EQ; break;
    case HA_READ_AFTER_KEY:   op= OP_GT; break;
    case HA_READ_KEY_OR_NEXT: op= OP_GE; break;
    default: DBUG_RETURN(-1);			 break;
    } // endswitch find_flag

  if (xtrace > 1)
    printf("%p index_read: op=%d\n", this, op);

  if (indexing > 0)
    rc= ReadIndexed(buf, op, key, key_len);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_read


/**
  @brief
  Used to read forward through the index.
*/
int ha_connect::index_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::index_next");
  //statistic_increment(ha_read_next_count, &LOCK_status);

  if (indexing > 0)
    rc= ReadIndexed(buf, OP_NEXT);
  else if (!indexing)
    rc= rnd_next(buf);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_next


#ifdef NOT_USED
/**
  @brief
  Used to read backwards through the index.
*/
int ha_connect::index_prev(uchar *buf)
{
  DBUG_ENTER("ha_connect::index_prev");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/**
  @brief
  index_first() asks for the first key in the index.

    @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

    @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_connect::index_first(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::index_first");

  if (indexing > 0)
    rc= ReadIndexed(buf, OP_FIRST);
  else if (indexing < 0)
    rc= HA_ERR_INTERNAL_ERROR;
  else if (CntRewindTable(xp->g, tdbp)) {
    table->status= STATUS_NOT_FOUND;
    rc= HA_ERR_INTERNAL_ERROR;
  } else
    rc= rnd_next(buf);

  DBUG_RETURN(rc);
} // end of index_first


#ifdef NOT_USED
/**
  @brief
  index_last() asks for the last key in the index.

    @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

    @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_connect::index_last(uchar *buf)
{
  DBUG_ENTER("ha_connect::index_last");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif // NOT_USED


/****************************************************************************/
/*  This is called to get more rows having the same index value.            */
/****************************************************************************/
int ha_connect::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  int rc;
  DBUG_ENTER("ha_connect::index_next_same");
//statistic_increment(ha_read_next_count, &LOCK_status);

  if (!indexing)
    rc= rnd_next(buf);
  else if (indexing > 0)
    rc= ReadIndexed(buf, OP_SAME);
  else
    rc= HA_ERR_INTERNAL_ERROR;

  DBUG_RETURN(rc);
} // end of index_next_same


/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the example in the introduction at the top of this file to see when
  rnd_init() is called.

    @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

    @note
  We always call open and extern_lock/start_stmt before comming here.

    @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_connect::rnd_init(bool scan)
{
  PGLOBAL g= ((table && table->in_use) ? GetPlug(table->in_use, xp) :
              (xp) ? xp->g : NULL);
  DBUG_ENTER("ha_connect::rnd_init");

  if (xtrace)
    printf("%p in rnd_init: scan=%d\n", this, scan);

  if (g) {
    if (!table || xmod == MODE_INSERT)
      DBUG_RETURN(HA_ERR_INITIALIZATION);

    // Close the table if it was opened yet (locked?)
    if (IsOpened())
      CloseTable(g);

    if (OpenTable(g, xmod == MODE_DELETE))
      DBUG_RETURN(HA_ERR_INITIALIZATION);

    } // endif g

  xp->nrd= xp->fnd= xp->nfd= 0;
  xp->tb1= my_interval_timer();
  DBUG_RETURN(0);
} // end of rnd_init

/**
  @brief
  Not described.

  @note
  The previous version said:
  Stop scanning of table. Note that this may be called several times during
  execution of a sub select.
  =====> This has been moved to external lock to avoid closing subselect tables.
*/
int ha_connect::rnd_end()
{
  int rc= 0;
  DBUG_ENTER("ha_connect::rnd_end");

  // If this is called by a later query, the table may have
  // been already closed and the tdbp is not valid anymore.
//  if (tdbp && xp->last_query_id == valid_query_id)
//    rc= CloseTable(xp->g);

  DBUG_RETURN(rc);
} // end of rnd_end


/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

    @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

    @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_connect::rnd_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_connect::rnd_next");
//statistic_increment(ha_read_rnd_next_count, &LOCK_status);

  if (tdbp->GetMode() == MODE_ANY) {
    // We will stop on next read
    if (!stop) {
      stop= true;
      DBUG_RETURN(RC_OK);
    } else
      DBUG_RETURN(HA_ERR_END_OF_FILE);

    } // endif Mode

  switch (CntReadNext(xp->g, tdbp)) {
    case RC_OK:
      rc= MakeRecord((char*)buf);
      break;
    case RC_EF:         // End of file
      rc= HA_ERR_END_OF_FILE;
      break;
    case RC_NF:         // Not found
      rc= HA_ERR_RECORD_DELETED;
      break;
    default:            // Read error
      printf("rnd_next CONNECT: %s\n", xp->g->Message);
      rc= (records()) ? HA_ERR_INTERNAL_ERROR : HA_ERR_END_OF_FILE;
      break;
    } // endswitch RC

#ifndef DBUG_OFF
  if (rc || !(xp->nrd++ % 16384)) {
    ulonglong tb2= my_interval_timer();
    double elapsed= (double) (tb2 - xp->tb1) / 1000000000ULL;
    DBUG_PRINT("rnd_next", ("rc=%d nrd=%u fnd=%u nfd=%u sec=%.3lf\n",
                             rc, (uint)xp->nrd, (uint)xp->fnd,
                             (uint)xp->nfd, elapsed));
    xp->tb1= tb2;
    xp->fnd= xp->nfd= 0;
    } // endif nrd
#endif

  table->status= (!rc) ? 0 : STATUS_NOT_FOUND;
  DBUG_RETURN(rc);
} // end of rnd_next


/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
    @code
  my_store_ptr(ref, ref_length, current_position);
    @endcode

    @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

    @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_connect::position(const uchar *record)
{
  DBUG_ENTER("ha_connect::position");
  if (((PTDBASE)tdbp)->GetDef()->Indexable())
    my_store_ptr(ref, ref_length, (my_off_t)((PTDBASE)tdbp)->GetRecpos());
  DBUG_VOID_RETURN;
} // end of position


/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use my_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

    @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

    @note
  Is this really useful? It was never called even when sorting.

    @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_connect::rnd_pos(uchar *buf, uchar *pos)
{
  int     rc;
  PTDBASE tp= (PTDBASE)tdbp;
  DBUG_ENTER("ha_connect::rnd_pos");

  if (!tp->SetRecpos(xp->g, (int)my_get_ptr(pos, ref_length)))
    rc= rnd_next(buf);
  else
    rc= HA_ERR_KEY_NOT_FOUND;

  DBUG_RETURN(rc);
} // end of rnd_pos


/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

    @details
  Currently this table handler doesn't implement most of the fields really needed.
  SHOW also makes use of this data.

  You will probably want to have the following in your code:
    @code
  if (records < 2)
    records= 2;
    @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_table.cc, sql_union.cc, and sql_update.cc.

    @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
  sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
  sql_union.cc and sql_update.cc
*/
int ha_connect::info(uint flag)
{
  bool    pure= false;
  PGLOBAL g= GetPlug((table) ? table->in_use : NULL, xp);

  DBUG_ENTER("ha_connect::info");

  if (xtrace)
    printf("%p In info: flag=%u valid_info=%d\n", this, flag, valid_info);

  if (!valid_info) {
    // tdbp must be available to get updated info
    if (xp->CheckQuery(valid_query_id) || !tdbp) {
      if (xmod == MODE_ANY) {               // Pure info, not a query
        pure= true;
        xp->CheckCleanup();
        } // endif xmod

//    tdbp= OpenTable(g, xmod == MODE_DELETE);
      tdbp= GetTDB(g);
      } // endif tdbp

    valid_info= CntInfo(g, tdbp, &xinfo);
    } // endif valid_info

  if (flag & HA_STATUS_VARIABLE) {
    stats.records= xinfo.records;
    stats.deleted= 0;
    stats.data_file_length= xinfo.data_file_length;
    stats.index_file_length= 0;
    stats.delete_length= 0;
    stats.check_time= 0;
    stats.mean_rec_length= xinfo.mean_rec_length;
    } // endif HA_STATUS_VARIABLE

  if (flag & HA_STATUS_CONST) {
    // This is imported from the previous handler and must be reconsidered
    stats.max_data_file_length= 4294967295;
    stats.max_index_file_length= 4398046510080;
    stats.create_time= 0;
    data_file_name= xinfo.data_file_name;
    index_file_name= NULL;
//  sortkey= (uint) - 1;           // Table is not sorted
    ref_length= sizeof(int);      // Pointer size to row
    table->s->db_options_in_use= 03;
    stats.block_size= 1024;
    table->s->keys_in_use.set_prefix(table->s->keys);
    table->s->keys_for_keyread= table->s->keys_in_use;
//  table->s->keys_for_keyread.subtract(table->s->read_only_keys);
    table->s->db_record_offset= 0;
    } // endif HA_STATUS_CONST

  if (flag & HA_STATUS_ERRKEY) {
    errkey= 0;
    } // endif HA_STATUS_ERRKEY

  if (flag & HA_STATUS_TIME)
    stats.update_time= 0;

  if (flag & HA_STATUS_AUTO)
    stats.auto_increment_value= 1;

  if (tdbp && pure)
    CloseTable(g);        // Not used anymore

  DBUG_RETURN(0);
} // end of info


/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

  @note
  This is not yet implemented for CONNECT.

  @see
  ha_innodb.cc
*/
int ha_connect::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_connect::extra");
  DBUG_RETURN(0);
} // end of extra


/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases where
  the optimizer realizes that all rows will be removed as a result of an SQL statement.

    @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_select_lex_unit::exec().

    @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_connect::delete_all_rows()
{
  int     rc= 0;
  PGLOBAL g= xp->g;
  DBUG_ENTER("ha_connect::delete_all_rows");

  if (tdbp && tdbp->GetAmType() != TYPE_AM_XML)
    // Close and reopen the table so it will be deleted
    rc= CloseTable(g);

  if (!(OpenTable(g))) {
    if (CntDeleteRow(g, tdbp, true)) {
      printf("%s\n", g->Message);
      rc= HA_ERR_INTERNAL_ERROR;
      } // endif

  } else
    rc= HA_ERR_INITIALIZATION;

  DBUG_RETURN(rc);
} // end of delete_all_rows


bool ha_connect::check_privileges(THD *thd, PTOS options)
{
  if (!options->type) {
    if (options->srcdef)
      options->type= "MYSQL";
    else if (options->tabname)
      options->type= "PROXY";
    else
      options->type= "DOS";

    } // endif type

  switch (GetTypeID(options->type))
  {
    case TAB_UNDEF:
//  case TAB_CATLG:
    case TAB_PLG:
    case TAB_JCT:
    case TAB_DMY:
    case TAB_NIY:
      my_printf_error(ER_UNKNOWN_ERROR,
                      "Unsupported table type %s", MYF(0), options->type);
      return true;

    case TAB_DOS:
    case TAB_FIX:
    case TAB_BIN:
    case TAB_CSV:
    case TAB_FMT:
    case TAB_DBF:
    case TAB_XML:
    case TAB_INI:
    case TAB_VEC:
      if (!options->filename)
        return false;
      char path[FN_REFLEN];
      (void) fn_format(path, options->filename, mysql_real_data_home, "",
                       MY_RELATIVE_PATH | MY_UNPACK_FILENAME);
      if (!is_secure_file_path(path))
      {
        my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--secure-file-priv");
        return true;
      }
      /* Fall through to check FILE_ACL */

    case TAB_ODBC:
    case TAB_MYSQL:
    case TAB_DIR:
    case TAB_MAC:
    case TAB_WMI:
    case TAB_OEM:
      return check_access(thd, FILE_ACL, NULL, NULL, NULL, 0, 0);

    // This is temporary until a solution is found
    case TAB_TBL:
    case TAB_XCL:
    case TAB_PRX:
    case TAB_OCCUR:
    case TAB_PIVOT:
      return false;
  }

  my_printf_error(ER_UNKNOWN_ERROR, "check_privileges failed", MYF(0));
  return true;
} // end of check_privileges

// Check that two indexes are equivalent 
bool ha_connect::IsSameIndex(PIXDEF xp1, PIXDEF xp2)
{
  bool   b= true;
  PKPDEF kp1, kp2;

  if (stricmp(xp1->Name, xp2->Name))
    b= false;
  else if (xp1->Nparts  != xp2->Nparts  ||
           xp1->MaxSame != xp2->MaxSame ||
           xp1->Unique  != xp2->Unique)
    b= false;
  else for (kp1= xp1->ToKeyParts, kp2= xp2->ToKeyParts;
            b && (kp1 || kp2);
            kp1= kp1->Next, kp2= kp2->Next)
    if (!kp1 || !kp2)
      b= false;
    else if (stricmp(kp1->Name, kp2->Name))
      b= false;
    else if (kp1->Klen != kp2->Klen)
      b= false;

  return b;
} // end of IsSameIndex

MODE ha_connect::CheckMode(PGLOBAL g, THD *thd, 
                           MODE newmode, bool *chk, bool *cras)
{
  if (xtrace) {
    LEX_STRING *query_string= thd_query_string(thd);
    printf("%p check_mode: cmdtype=%d\n", this, thd_sql_command(thd));
    printf("Cmd=%.*s\n", (int) query_string->length, query_string->str);
    } // endif xtrace

  // Next code is temporarily replaced until sql_command is set
  stop= false;

  if (newmode == MODE_WRITE) {
    switch (thd_sql_command(thd)) {
      case SQLCOM_LOCK_TABLES:
        locked= 2;
      case SQLCOM_CREATE_TABLE:
      case SQLCOM_INSERT:
      case SQLCOM_LOAD:
      case SQLCOM_INSERT_SELECT:
        newmode= MODE_INSERT;
        break;
//    case SQLCOM_REPLACE:
//    case SQLCOM_REPLACE_SELECT:
//      newmode= MODE_UPDATE;               // To be checked
//      break;
      case SQLCOM_DELETE:
      case SQLCOM_DELETE_MULTI:
      case SQLCOM_TRUNCATE:
        newmode= MODE_DELETE;
        break;
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
        newmode= MODE_UPDATE;
        break;
      case SQLCOM_SELECT:
      case SQLCOM_OPTIMIZE:
        newmode= MODE_READ;
        break;
      case SQLCOM_DROP_TABLE:
      case SQLCOM_RENAME_TABLE:
      case SQLCOM_ALTER_TABLE:
        newmode= MODE_ANY;
        break;
      case SQLCOM_DROP_INDEX:
      case SQLCOM_CREATE_INDEX:
        newmode= MODE_ANY;
//      stop= true;
        break;
      case SQLCOM_CREATE_VIEW:
      case SQLCOM_DROP_VIEW:
        newmode= MODE_ANY;
        break;
      default:
        printf("Unsupported sql_command=%d", thd_sql_command(thd));
        strcpy(g->Message, "CONNECT Unsupported command");
        my_message(ER_NOT_ALLOWED_COMMAND, g->Message, MYF(0));
        newmode= MODE_ERROR;
        break;
      } // endswitch newmode

  } else if (newmode == MODE_READ) {
    switch (thd_sql_command(thd)) {
      case SQLCOM_CREATE_TABLE:
        *chk= true;
        *cras= true;
      case SQLCOM_INSERT:
      case SQLCOM_LOAD:
      case SQLCOM_INSERT_SELECT:
//    case SQLCOM_REPLACE:
//    case SQLCOM_REPLACE_SELECT:
      case SQLCOM_DELETE:
      case SQLCOM_DELETE_MULTI:
      case SQLCOM_TRUNCATE:
      case SQLCOM_UPDATE:
      case SQLCOM_UPDATE_MULTI:
      case SQLCOM_SELECT:
      case SQLCOM_OPTIMIZE:
        break;
      case SQLCOM_LOCK_TABLES:
        locked= 1;
        break;
      case SQLCOM_DROP_INDEX:
      case SQLCOM_CREATE_INDEX:
      case SQLCOM_ALTER_TABLE:
        *chk= true;
//      stop= true;
      case SQLCOM_DROP_TABLE:
      case SQLCOM_RENAME_TABLE:
        newmode= MODE_ANY;
        break;
      case SQLCOM_CREATE_VIEW:
      case SQLCOM_DROP_VIEW:
        newmode= MODE_ANY;
        break;
      default:
        printf("Unsupported sql_command=%d", thd_sql_command(thd));
        strcpy(g->Message, "CONNECT Unsupported command");
        my_message(ER_NOT_ALLOWED_COMMAND, g->Message, MYF(0));
        newmode= MODE_ERROR;
        break;
      } // endswitch newmode

  } // endif's newmode

  if (xtrace)
    printf("New mode=%d\n", newmode);

  return newmode;
} // end of check_mode

int ha_connect::start_stmt(THD *thd, thr_lock_type lock_type)
{
  int     rc= 0;
  bool    chk=false, cras= false;
  MODE    newmode;
  PGLOBAL g= GetPlug(thd, xp);
  DBUG_ENTER("ha_connect::start_stmt");

  // Action will depend on lock_type
  switch (lock_type) {
    case TL_WRITE_ALLOW_WRITE:
    case TL_WRITE_CONCURRENT_INSERT:
    case TL_WRITE_DELAYED:
    case TL_WRITE_DEFAULT:
    case TL_WRITE_LOW_PRIORITY:
    case TL_WRITE:
    case TL_WRITE_ONLY:
      newmode= MODE_WRITE;
      break;
    case TL_READ:
    case TL_READ_WITH_SHARED_LOCKS:
    case TL_READ_HIGH_PRIORITY:
    case TL_READ_NO_INSERT:
    case TL_READ_DEFAULT:
      newmode= MODE_READ;
      break;
    case TL_UNLOCK:
    default:
      newmode= MODE_ANY;
      break;
    } // endswitch mode

  xmod= CheckMode(g, thd, newmode, &chk, &cras);
  DBUG_RETURN((xmod == MODE_ERROR) ? HA_ERR_INTERNAL_ERROR : 0);
} // end of start_stmt

/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to understand
  this.

    @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

    @note
  Following what we did in the MySQL XDB handler, we use this call to actually
  physically open the table. This could be reconsider when finalizing this handler
  design, which means we have a better understanding of what MariaDB does.

    @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_connect::external_lock(THD *thd, int lock_type)
{
  int     rc= 0;
  bool    xcheck=false, cras= false;
  MODE    newmode;
  PTOS    options= GetTableOptionStruct(table);
  PGLOBAL g= GetPlug(thd, xp);
  DBUG_ENTER("ha_connect::external_lock");

  DBUG_ASSERT(thd == current_thd);

  if (xtrace)
    printf("%p external_lock: lock_type=%d\n", this, lock_type);

  if (!g)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  if (lock_type != F_UNLCK && check_privileges(thd, options)) {
    strcpy(g->Message, "This operation requires the FILE privilege");
    printf("%s\n", g->Message);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
    } // endif check_privileges

  // Action will depend on lock_type
  switch (lock_type) {
    case F_WRLCK:
      newmode= MODE_WRITE;
      break;
    case F_RDLCK:
      newmode= MODE_READ;
      break;
    case F_UNLCK:
    default:
      newmode= MODE_ANY;
      break;
    } // endswitch mode

  if (newmode == MODE_ANY) {
    // This is unlocking, do it by closing the table
    if (xp->CheckQueryID() && thd_sql_command(thd) != SQLCOM_UNLOCK_TABLES
                           && thd_sql_command(thd) != SQLCOM_LOCK_TABLES)
      rc= 2;          // Logical error ???
    else if (g->Xchk) {
      if (!tdbp || *tdbp->GetName() == '#') {
        bool    oldsep= ((PCHK)g->Xchk)->oldsep;
        bool    newsep= ((PCHK)g->Xchk)->newsep;
        PTDBDOS tdp= (PTDBDOS)(tdbp ? tdbp : GetTDB(g));

        if (!tdp)
          DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

        PDOSDEF ddp= (PDOSDEF)tdp->GetDef();
        PIXDEF  xp, xp1, xp2, drp=NULL, adp= NULL;
        PIXDEF  oldpix= ((PCHK)g->Xchk)->oldpix;
        PIXDEF  newpix= ((PCHK)g->Xchk)->newpix;
        PIXDEF *xlst, *xprc; 

        ddp->SetIndx(oldpix);

        if (oldsep != newsep) {
          // All indexes have to be remade
          ddp->DeleteIndexFile(g, NULL);
          oldpix= NULL;
          ddp->SetIndx(NULL);
          SetBooleanOption("Sepindex", newsep);
        } else if (newsep) {
          // Make the list of dropped indexes
          xlst= &drp; xprc= &oldpix;
      
          for (xp2= oldpix; xp2; xp2= xp) {
            for (xp1= newpix; xp1; xp1= xp1->Next)
              if (IsSameIndex(xp1, xp2))
                break;        // Index not to drop
      
            xp= xp2->GetNext();
      
            if (!xp1) {
              *xlst= xp2;
              *xprc= xp;
              *(xlst= &xp2->Next)= NULL;
            } else
              xprc= &xp2->Next;
      
            } // endfor xp2
      
          if (drp) {
            // Here we erase the index files
            ddp->DeleteIndexFile(g, drp);
            } // endif xp1

        } else if (oldpix) {
          // TODO: optimize the case of just adding new indexes
          if (!newpix)
            ddp->DeleteIndexFile(g, NULL);

          oldpix= NULL;     // To remake all indexes
          ddp->SetIndx(NULL);
        } // endif sepindex

        // Make the list of new created indexes
        xlst= &adp; xprc= &newpix;

        for (xp1= newpix; xp1; xp1= xp) {
          for (xp2= oldpix; xp2; xp2= xp2->Next)
            if (IsSameIndex(xp1, xp2))
              break;        // Index already made

          xp= xp1->Next;

          if (!xp2) {
            *xlst= xp1;
            *xprc= xp;
            *(xlst= &xp1->Next)= NULL;
          } else
            xprc= &xp1->Next;

          } // endfor xp1

        if (adp)
          // Here we do make the new indexes
          tdp->MakeIndex(g, adp, true);

        } // endif Mode

      } // endelse Xchk

    if (CloseTable(g)) {
      // This is an error while builing index
//#if defined(_DEBUG)
      // Make it a warning to avoid crash on debug
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
      rc= 0;
//#else   // !_DEBUG
//      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
//      rc= HA_ERR_INTERNAL_ERROR;
//#endif  // !DEBUG
      } // endif Close

    locked= 0;
    DBUG_RETURN(rc);
    } // endif MODE_ANY

  // Table mode depends on the query type
  newmode= CheckMode(g, thd, newmode, &xcheck, &cras);

  if (newmode == MODE_ERROR)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  // If this is the start of a new query, cleanup the previous one
  if (xp->CheckCleanup()) {
    tdbp= NULL;
    valid_info= false;
    } // endif CheckCleanup

  if (xcheck) {
    // This must occur after CheckCleanup
    g->Xchk= new(g) XCHK;
    ((PCHK)g->Xchk)->oldsep= GetBooleanOption("Sepindex", false);
    ((PCHK)g->Xchk)->oldpix= GetIndexInfo();
    } // endif xcheck

  if (cras)
    g->Createas= 1;       // To tell created table to ignore FLAG

  if (xtrace)
    printf("Calling CntCheckDB db=%s\n", GetDBName(NULL));

  // Set or reset the good database environment
  if (CntCheckDB(g, this, GetDBName(NULL))) {
    printf("%p external_lock: %s\n", this, g->Message);
    rc= HA_ERR_INTERNAL_ERROR;
  // This can NOT be called without open called first, but
  // the table can have been closed since then
  } else if (!tdbp || xp->CheckQuery(valid_query_id) || xmod != newmode) {
    if (tdbp) {
      // If this is called by a later query, the table may have
      // been already closed and the tdbp is not valid anymore.
      if (xp->last_query_id == valid_query_id)
        rc= CloseTable(g);
      else
        tdbp= NULL;

      }// endif tdbp

    xmod= newmode;

    if (!table)
      rc= 3;          // Logical error

    // Delay open until used fields are known
  } // endif tdbp

  if (xtrace)
    printf("external_lock: rc=%d\n", rc);

  DBUG_RETURN(rc);
} // end of external_lock


/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

    @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for example, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

    @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

    @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_connect::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type=lock_type;
  *to++ = &lock;
  return to;
}


/**
  Searches for a pointer to the last occurrence of  the
  character c in the string src.
  Returns true on failure, false on success.
*/
static bool
strnrchr(LEX_CSTRING *ls, const char *src, size_t length, int c)
{
  const char *srcend, *s;
  for (s= srcend= src + length; s > src; s--)
  {
    if (s[-1] == c)
    {
      ls->str= s;
      ls->length= srcend - s;
      return false;
    }
  }
  return true;
}


/**
  Split filename into database and table name.
*/
static bool
filename_to_dbname_and_tablename(const char *filename,
                                 char *database, size_t database_size,
                                 char *table, size_t table_size)
{
#if defined(WIN32)
  char slash= '\\';
#else   // !WIN32
  char slash= '/';
#endif  // !WIN32
  LEX_CSTRING d, t;
  size_t length= strlen(filename);

  /* Find filename - the rightmost directory part */
  if (strnrchr(&t, filename, length, slash) || t.length + 1 > table_size)
    return true;
  memcpy(table, t.str, t.length);
  table[t.length]= '\0';
  if (!(length-= t.length))
    return true;

  length--; /* Skip slash */

  /* Find database name - the second rightmost directory part */
  if (strnrchr(&d, filename, length, slash) || d.length + 1 > database_size)
    return true;
  memcpy(database, d.str, d.length);
  database[d.length]= '\0';
  return false;
}


/**
  @brief
  Used to delete or rename a table. By the time delete_table() has been
  called all opened references to this table will have been closed 
  (and your globally shared references released) ===> too bad!!!
  The variable name will just be the name of the table.
  You will need to remove or rename any files you have created at 
  this point.

    @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions returned
  by bas_ext().

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

    @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_connect::delete_or_rename_table(const char *name, const char *to)
{
  DBUG_ENTER("ha_connect::delete_or_rename_table");
  /* We have to retrieve the information about this table options. */
  ha_table_option_struct *pos;
  char         key[MAX_DBKEY_LENGTH], db[128], tabname[128];
  int          rc= 0;
  uint         key_length;
  TABLE_SHARE *share;
  THD         *thd= current_thd;

  if (to && (filename_to_dbname_and_tablename(to, db, sizeof(db),
                                             tabname, sizeof(tabname)) ||
             *tabname == '#'))
    goto fin;

  if (filename_to_dbname_and_tablename(name, db, sizeof(db),
                                       tabname, sizeof(tabname)) ||
      *tabname == '#')
    goto fin;

  key_length= tdc_create_key(key, db, tabname);

  // share contains the option struct that we need
  if (!(share= alloc_table_share(db, tabname, key, key_length)))
    goto fin;

  // Get the share info from the .frm file
  if (open_table_def(thd, share))
    goto err;

  // Now we can work
  pos= share->option_struct;

  if (check_privileges(thd, pos))
  {
    free_table_share(share);
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  }

  if (IsFileType(GetTypeID(pos->type)) && !pos->filename) {
    // Let the base handler do the job
    if (to)
      rc= handler::rename_table(name, to);
    else
      rc= handler::delete_table(name);
    } // endif filename

  // Done no more need for this
 err:
  free_table_share(share);
 fin:
  DBUG_RETURN(rc);
} // end of delete_or_rename_table

int ha_connect::delete_table(const char *name)
{
  return delete_or_rename_table(name, NULL);
} // end of delete_table

int ha_connect::rename_table(const char *from, const char *to)
{
  return delete_or_rename_table(from, to);
} // end of rename_table

/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_connect::records_in_range(uint inx, key_range *min_key,
                                               key_range *max_key)
{
  ha_rows rows;
  DBUG_ENTER("ha_connect::records_in_range");

  if (indexing < 0 || inx != active_index)
    index_init(inx, false);

  if (xtrace)
    printf("records_in_range: inx=%d indexing=%d\n", inx, indexing);

  if (indexing > 0) {
    int          nval;
    uint         len[2];
    const uchar *key[2];
    bool         incl[2];
    key_part_map kmap[2];

    key[0]= (min_key) ? min_key->key : NULL;
    key[1]= (max_key) ? max_key->key : NULL;
    len[0]= (min_key) ? min_key->length : 0;
    len[1]= (max_key) ? max_key->length : 0;
    incl[0]= (min_key) ? (min_key->flag == HA_READ_KEY_EXACT) : false;
    incl[1]= (max_key) ? (max_key->flag == HA_READ_AFTER_KEY) : false;
    kmap[0]= (min_key) ? min_key->keypart_map : 0;
    kmap[1]= (max_key) ? max_key->keypart_map : 0;

    if ((nval= CntIndexRange(xp->g, tdbp, key, len, incl, kmap)) < 0)
      rows= HA_POS_ERROR;
    else
      rows= (ha_rows)nval;

  } else if (indexing < 0)
    rows= HA_POS_ERROR;
  else
    rows= 100000000;        // Don't use missing index

  DBUG_RETURN(rows);
} // end of records_in_range

/**
  Convert an ISO-8859-1 column name to UTF-8
*/
static char *encode(PGLOBAL g, char *cnm)
  {
  char  *buf= (char*)PlugSubAlloc(g, NULL, strlen(cnm) * 3);
  uint   dummy_errors;
  uint32 len= copy_and_convert(buf, strlen(cnm) * 3,
                               &my_charset_utf8_general_ci,
                               cnm, strlen(cnm),
                               &my_charset_latin1,
                               &dummy_errors);
  buf[len]= '\0';
  return buf;
  } // end of Encode

/**
  Store field definition for create.

  @return
    Return 0 if ok
*/
#if defined(NEW_WAY)
static bool add_fields(PGLOBAL g,
                       THD *thd,
                       Alter_info *alter_info,
                       char *name,
                       int typ, int len, int dec,
                       uint type_modifier,
                       char *rem,
//                     CHARSET_INFO *cs,
//                     void *vcolinfo,
//                     engine_option_value *create_options,
                       int flg,
                       bool dbf)
{
  register Create_field *new_field;
  char *length, *decimals;
  enum_field_types type= PLGtoMYSQL(typ, dbf);
//Virtual_column_info *vcol_info= (Virtual_column_info *)vcolinfo;
  engine_option_value *crop;
  LEX_STRING *comment= thd->make_lex_string(rem, strlen(rem));
  LEX_STRING *field_name= thd->make_lex_string(name, strlen(name));

  DBUG_ENTER("ha_connect::add_fields");
  length= (char*)PlugSubAlloc(g, NULL, 8);
  sprintf(length, "%d", len);

  if (dec) {
    decimals= (char*)PlugSubAlloc(g, NULL, 8);
    sprintf(decimals, "%d", dec);
  } else
    decimals= NULL;

  if (flg) {
    engine_option_value *start= NULL, *end= NULL;
    LEX_STRING *flag= thd->make_lex_string("flag", 4);

    crop= new(thd->mem_root) engine_option_value(*flag, (ulonglong)flg,
                                                 &start, &end, thd->mem_root);
  } else                               
    crop= NULL;

  if (check_string_char_length(field_name, "", NAME_CHAR_LEN,
                               system_charset_info, 1)) {
    my_error(ER_TOO_LONG_IDENT, MYF(0), field_name->str); /* purecov: inspected */
    DBUG_RETURN(1);       /* purecov: inspected */
    } // endif field_name

  if (!(new_field= new Create_field()) ||
        new_field->init(thd, field_name->str, type, length, decimals,
                        type_modifier, NULL, NULL, comment, NULL,
                        NULL, NULL, 0, NULL, crop, true))
    DBUG_RETURN(1);

  alter_info->create_list.push_back(new_field);
  DBUG_RETURN(0);
} // end of add_fields
#else   // !NEW_WAY
static bool add_field(String *sql, const char *field_name, int typ, int len,
                      int dec, uint tm, const char *rem, int flag, bool dbf)
{
  bool error= false;
  const char *type= PLGtoMYSQLtype(typ, dbf);
//        type= PLGtoMYSQLtype(typ, true);         ?????

  error|= sql->append('`');
  error|= sql->append(field_name);
  error|= sql->append("` ");
  error|= sql->append(type);

  if (len) {
    error|= sql->append('(');
    error|= sql->append_ulonglong(len);

    if (!strcmp(type, "DOUBLE")) {
      error|= sql->append(',');
      error|= sql->append_ulonglong(dec);
      } // endif dec

    error|= sql->append(')');
    } // endif len
  
  if (tm)
    error|= sql->append(STRING_WITH_LEN(" NOT NULL"), system_charset_info);

  if (rem && *rem) {
    error|= sql->append(" COMMENT '");
    error|= sql->append_for_single_quote(rem, strlen(rem));
    error|= sql->append("'");
    } // endif rem

  if (flag) {
    error|= sql->append(" FLAG=");
    error|= sql->append_ulonglong(flag);
    } // endif flag

  error|= sql->append(',');
  return error;
} // end of add_field
#endif  // !NEW_WAY

/**
  Initialise the table share with the new columns.

  @return
    Return 0 if ok
*/
#if defined(NEW_WAY)
//static bool sql_unusable_for_discovery(THD *thd, const char *sql);

static int init_table_share(THD *thd, 
                            TABLE_SHARE *table_s, 
                            HA_CREATE_INFO *create_info, 
                            Alter_info *alter_info)
{
  int          rc= 0;
  handler     *file;
  LEX_CUSTRING frm= {0,0};

  DBUG_ENTER("init_table_share");

#if 0
  ulonglong saved_mode= thd->variables.sql_mode;
  CHARSET_INFO *old_cs= thd->variables.character_set_client;
  Parser_state parser_state;
  char *sql_copy;
  LEX *old_lex;
  Query_arena *arena, backup;
  LEX tmp_lex;

  /*
    Ouch. Parser may *change* the string it's working on.
    Currently (2013-02-26) it is used to permanently disable
    conditional comments.
    Anyway, let's copy the caller's string...
  */
  if (!(sql_copy= thd->strmake(sql, sql_length)))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  if (parser_state.init(thd, sql_copy, sql_length))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  thd->variables.sql_mode= MODE_NO_ENGINE_SUBSTITUTION | MODE_NO_DIR_IN_CREATE;
  thd->variables.character_set_client= system_charset_info;
  old_lex= thd->lex;
  thd->lex= &tmp_lex;

  arena= thd->stmt_arena;

  if (arena->is_conventional())
    arena= 0;
  else
    thd->set_n_backup_active_arena(arena, &backup);

  lex_start(thd);

  if ((error= parse_sql(thd, & parser_state, NULL)))
    goto ret;

  if (table_s->sql_unusable_for_discovery(thd, NULL)) {
    my_error(ER_SQL_DISCOVER_ERROR, MYF(0), plugin_name(db_plugin)->str,
             db.str, table_name.str, sql_copy);
    goto ret;
    } // endif unusable

  thd->lex->create_info.db_type= plugin_data(db_plugin, handlerton *);

  if (tabledef_version.str)
    thd->lex->create_info.tabledef_version= tabledef_version;
#endif // 0

  tmp_disable_binlog(thd);

  file= mysql_create_frm_image(thd, table_s->db.str, table_s->table_name.str,
                               create_info, alter_info,
//                             &thd->lex->create_info, &thd->lex->alter_info,
                               C_ORDINARY_CREATE, &frm);
  if (file)
    delete file;
  else
    rc= OPEN_FRM_CORRUPTED;

  if (!rc && frm.str) {
    table_s->option_list= 0;     // cleanup existing options ...
    table_s->option_struct= 0;   // ... if it's an assisted discovery
    rc= table_s->init_from_binary_frm_image(thd, true, frm.str, frm.length);
    } // endif frm

//ret:
  my_free(const_cast<uchar*>(frm.str));
  reenable_binlog(thd);
#if 0
  lex_end(thd->lex);
  thd->lex= old_lex;
  if (arena)
    thd->restore_active_arena(arena, &backup);
  thd->variables.sql_mode= saved_mode;
  thd->variables.character_set_client= old_cs;
#endif // 0

  if (thd->is_error() || rc) {
    thd->clear_error();
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_s->db.str, 
                                       table_s->table_name.str);
    DBUG_RETURN(HA_ERR_NOT_A_TABLE);
  } else
    DBUG_RETURN(0);

} // end of init_table_share
#else   // !NEW_WAY
static int init_table_share(THD* thd, 
                            TABLE_SHARE *table_s, 
                            HA_CREATE_INFO *create_info,
//                          char *dsn,
                            String *sql)
{
  bool oom= false;
  PTOS topt= table_s->option_struct;

  sql->length(sql->length()-1); // remove the trailing comma
  sql->append(')');

  for (ha_create_table_option *opt= connect_table_option_list;
       opt->name; opt++) {
    ulonglong   vull;
    const char *vstr;

    switch (opt->type) {
      case HA_OPTION_TYPE_ULL:
        vull= *(ulonglong*)(((char*)topt) + opt->offset);

        if (vull != opt->def_value) {
          oom|= sql->append(' ');
          oom|= sql->append(opt->name);
          oom|= sql->append('=');
          oom|= sql->append_ulonglong(vull);
          } // endif vull

        break;
      case HA_OPTION_TYPE_STRING:
        vstr= *(char**)(((char*)topt) + opt->offset);

        if (vstr) {
          oom|= sql->append(' ');
          oom|= sql->append(opt->name);
          oom|= sql->append("='");
          oom|= sql->append_for_single_quote(vstr, strlen(vstr));
          oom|= sql->append('\'');
          } // endif vstr

        break;
      case HA_OPTION_TYPE_BOOL:
        vull= *(bool*)(((char*)topt) + opt->offset);

        if (vull != opt->def_value) {
          oom|= sql->append(' ');
          oom|= sql->append(opt->name);
          oom|= sql->append('=');
          oom|= sql->append(vull ? "ON" : "OFF");
          } // endif vull

        break;
      default: // no enums here, good :)
        break;
      } // endswitch type

    if (oom)
      return HA_ERR_OUT_OF_MEM;

    } // endfor opt

  if (create_info->connect_string.length) {
//if (dsn) {
    oom|= sql->append(' ');
    oom|= sql->append("CONNECTION='");
    oom|= sql->append_for_single_quote(create_info->connect_string.str,
                                       create_info->connect_string.length);
//  oom|= sql->append_for_single_quote(dsn, strlen(dsn));
    oom|= sql->append('\'');

    if (oom)
      return HA_ERR_OUT_OF_MEM;

    } // endif string

  if (create_info->default_table_charset) {
    oom|= sql->append(' ');
    oom|= sql->append("CHARSET=");
    oom|= sql->append(create_info->default_table_charset->csname);

    if (oom)
      return HA_ERR_OUT_OF_MEM;

    } // endif charset

  if (xtrace)
    htrc("s_init: %.*s\n", sql->length(), sql->ptr());

  return table_s->init_from_sql_statement_string(thd, true,
                                                 sql->ptr(), sql->length());
} // end of init_table_share
#endif  // !NEW_WAY

// Add an option to the create_info option list 
static void add_option(THD* thd, HA_CREATE_INFO *create_info, 
                       const char *opname, const char *opval)
{
#if defined(NEW_WAY)
  LEX_STRING *opn= thd->make_lex_string(opname, strlen(opname));
  LEX_STRING *val= thd->make_lex_string(opval, strlen(opval));
  engine_option_value *pov, **start= &create_info->option_list, *end= NULL;

  for (pov= *start; pov; pov= pov->next)
    end= pov;

  pov= new(thd->mem_root) engine_option_value(*opn, *val, false, start, &end);
#endif   // NEW_WAY
} // end of add_option

// Used to check whether a MYSQL table is created on itself
static bool CheckSelf(PGLOBAL g, TABLE_SHARE *s, const char *host, 
                      const char *db, char *tab, const char *src, int port)
{
  if (src)
    return false;
  else if (host && stricmp(host, "localhost") && strcmp(host, "127.0.0.1"))
    return false;
  else if (db && stricmp(db, s->db.str))
    return false;
  else if (tab && stricmp(tab, s->table_name.str))
    return false;
  else if (port && port != (signed)GetDefaultPort())
    return false;

  strcpy(g->Message, "This MySQL table is defined on itself");
  return true;
} // end of CheckSelf

/**
  @brief
  connect_assisted_discovery() is called when creating a table with no columns.

  @details
  When assisted discovery is used the .frm file have not already been
  created. You can overwrite some definitions at this point but the
  main purpose of it is to define the columns for some table types.

  @note
  this function is no more called in case of CREATE .. SELECT
*/
static int connect_assisted_discovery(handlerton *hton, THD* thd,
                                      TABLE_SHARE *table_s,
                                      HA_CREATE_INFO *create_info)
{
  char        spc= ',', qch= 0;
  const char *fncn= "?";
  const char *user, *fn, *db, *host, *pwd, *sep, *tbl, *src;
  const char *col, *ocl, *rnk, *pic, *fcl;
  char       *tab, *dsn; 
#if defined(WIN32)
  char       *nsp= NULL, *cls= NULL;
#endif   // WIN32
  int         port= 0, hdr= 0, mxr= 0, rc= 0;
  int         cop __attribute__((unused)) = 0;
  uint        tm, fnc= FNC_NO, supfnc= (FNC_NO | FNC_COL);
  bool        bif, ok= false, dbf= false;
  TABTYPE     ttp= TAB_UNDEF;
  PQRYRES     qrp= NULL;
  PCOLRES     crp;
  PGLOBAL     g= GetPlug(thd, NULL);
  PTOS        topt= table_s->option_struct;
#if defined(NEW_WAY)
//CHARSET_INFO *cs;
  Alter_info  alter_info;
#else   // !NEW_WAY
  char        buf[1024];
  String      sql(buf, sizeof(buf), system_charset_info);

  sql.copy(STRING_WITH_LEN("CREATE TABLE whatever ("), system_charset_info);
#endif  // !NEW_WAY

  if (!g)
    return HA_ERR_INTERNAL_ERROR;

  user= host= pwd= tbl= src= col= ocl= pic= fcl= rnk= dsn= NULL;

  // Get the useful create options
  ttp= GetTypeID(topt->type);
  fn=  topt->filename;
  tab= (char*)topt->tabname;
  src= topt->srcdef;
  db=  topt->dbname;
  fncn= topt->catfunc;
  fnc= GetFuncID(fncn);
  sep= topt->separator;
  spc= (!sep || !strcmp(sep, "\\t")) ? '\t' : *sep;
  qch= topt->qchar ? *topt->qchar : (signed)topt->quoted >= 0 ? '"' : 0;
  hdr= (int)topt->header;
  tbl= topt->tablist;
  col= topt->colist;

  if (topt->oplist) {
    host= GetListOption(g, "host", topt->oplist, "localhost");
    user= GetListOption(g, "user", topt->oplist, "root");
    // Default value db can come from the DBNAME=xxx option.
    db= GetListOption(g, "database", topt->oplist, db);
    col= GetListOption(g, "colist", topt->oplist, col);
    ocl= GetListOption(g, "occurcol", topt->oplist, NULL);
    pic= GetListOption(g, "pivotcol", topt->oplist, NULL);
    fcl= GetListOption(g, "fnccol", topt->oplist, NULL);
    rnk= GetListOption(g, "rankcol", topt->oplist, NULL);
    pwd= GetListOption(g, "password", topt->oplist);
#if defined(WIN32)
    nsp= GetListOption(g, "namespace", topt->oplist);
    cls= GetListOption(g, "class", topt->oplist);
#endif   // WIN32
    port= atoi(GetListOption(g, "port", topt->oplist, "0"));
    mxr= atoi(GetListOption(g,"maxerr", topt->oplist, "0"));
    cop= atoi(GetListOption(g, "checkdsn", topt->oplist, "0"));
  } else {
    host= "localhost";
    user= "root";
  } // endif option_list

  if (!db)
    db= table_s->db.str;                     // Default value

  // Check table type
  if (ttp == TAB_UNDEF) {
    topt->type= (src) ? "MYSQL" : (tab) ? "PROXY" : "DOS";
    ttp= GetTypeID(topt->type);
    sprintf(g->Message, "No table_type. Was set to %s", topt->type);
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
    add_option(thd, create_info, "table_type", topt->type);
  } else if (ttp == TAB_NIY) {
    sprintf(g->Message, "Unsupported table type %s", topt->type);
    my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
    return HA_ERR_INTERNAL_ERROR;
  } // endif ttp

  if (!tab) {
    if (ttp == TAB_TBL) {
      // Make tab the first table of the list
      char *p;

      if (!tbl) {
        strcpy(g->Message, "Missing table list");
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        return HA_ERR_INTERNAL_ERROR;
        } // endif tbl

      tab= (char*)PlugSubAlloc(g, NULL, strlen(tbl) + 1);
      strcpy(tab, tbl);

      if ((p= strchr(tab, ',')))
        *p= 0;

      if ((p=strchr(tab, '.'))) {
        *p= 0;
        db= tab;
        tab= p + 1;
        } // endif p

    } else if (ttp != TAB_ODBC || !(fnc & (FNC_TABLE | FNC_COL)))
      tab= table_s->table_name.str;              // Default value

#if defined(NEW_WAY)
    add_option(thd, create_info, "tabname", tab);
#endif   // NEW_WAY
    } // endif tab

  switch (ttp) {
#if defined(ODBC_SUPPORT)
    case TAB_ODBC:
      dsn= create_info->connect_string.str;

      if (fnc & (FNC_DSN | FNC_DRIVER))
        ok= true;
      else if (!stricmp(thd->main_security_ctx.host, "localhost")
                && cop == 1) {
        if ((dsn = ODBCCheckConnection(g, dsn, cop)) != NULL) {
          thd->make_lex_string(&create_info->connect_string, dsn, strlen(dsn));
          ok= true;
          } // endif dsn

      } else if (!dsn)
        sprintf(g->Message, "Missing %s connection string", topt->type);
      else
        ok= true;

      supfnc |= (FNC_TABLE | FNC_DSN | FNC_DRIVER);
      break;
#endif   // ODBC_SUPPORT
    case TAB_DBF:
      dbf= true;
      // Passthru
    case TAB_CSV:
      if (!fn && fnc != FNC_NO)
        sprintf(g->Message, "Missing %s file name", topt->type);
      else
        ok= true;

      break;
#if defined(MYSQL_SUPPORT)
    case TAB_MYSQL:
      ok= true;

      if (create_info->connect_string.str) {
        int     len= create_info->connect_string.length;
        PMYDEF  mydef= new(g) MYSQLDEF();
        PDBUSER dup= PlgGetUser(g);
        PCATLG  cat= (dup) ? dup->Catalog : NULL;

        dsn= (char*)PlugSubAlloc(g, NULL, len + 1);
        strncpy(dsn, create_info->connect_string.str, len);
        dsn[len]= 0;
        mydef->SetName(create_info->alias);
        mydef->SetCat(cat);

        if (!mydef->ParseURL(g, dsn, false)) {
          if (mydef->GetHostname())
            host= mydef->GetHostname();

          if (mydef->GetUsername())
            user= mydef->GetUsername();

          if (mydef->GetPassword())
            pwd=  mydef->GetPassword();

          if (mydef->GetDatabase())
            db= mydef->GetDatabase();

          if (mydef->GetTabname())
            tab= mydef->GetTabname();

          if (mydef->GetPortnumber())
            port= mydef->GetPortnumber();

        } else
          ok= false;

      } else if (!user)
        user= "root";

      if (CheckSelf(g, table_s, host, db, tab, src, port))
        ok= false;

      break;
#endif   // MYSQL_SUPPORT
#if defined(WIN32)
    case TAB_WMI:
      ok= true;
      break;
#endif   // WIN32
    case TAB_PIVOT:
      supfnc= FNC_NO;
    case TAB_PRX:
    case TAB_TBL:
    case TAB_XCL:
    case TAB_OCCUR:
      if (!stricmp(tab, create_info->alias) &&
         (!db || !stricmp(db, table_s->db.str)))
        sprintf(g->Message, "A %s table cannot refer to itself", topt->type);
      else
        ok= true;

      break;
    default:
      sprintf(g->Message, "Cannot get column info for table type %s", topt->type);
      break;
    } // endif ttp

  // Check for supported catalog function
  if (ok && !(supfnc & fnc)) {
    sprintf(g->Message, "Unsupported catalog function %s for table type %s",
                        fncn, topt->type);
    ok= false;
    } // endif supfnc

  if (src && fnc != FNC_NO) {
    strcpy(g->Message, "Cannot make catalog table from srcdef");
    ok= false;
    } // endif src

  if (ok) {
    char   *cnm, *rem;
    int     i, len, dec, typ, flg;
    PDBUSER dup= PlgGetUser(g);
    PCATLG  cat= (dup) ? dup->Catalog : NULL;

    if (cat)
      cat->SetDataPath(g, table_s->db.str);
    else
      return HA_ERR_INTERNAL_ERROR;           // Should never happen

    if (src && ttp != TAB_PIVOT && ttp != TAB_ODBC) {
      qrp= SrcColumns(g, host, db, user, pwd, src, port);

      if (qrp && ttp == TAB_OCCUR)
        if (OcrSrcCols(g, qrp, col, ocl, rnk)) {
          my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
          return HA_ERR_INTERNAL_ERROR;
          } // endif OcrSrcCols

    } else switch (ttp) {
      case TAB_DBF:
        qrp= DBFColumns(g, fn, fnc == FNC_COL);
        break;
#if defined(ODBC_SUPPORT)
      case TAB_ODBC:
        switch (fnc) {
          case FNC_NO:
          case FNC_COL:
            if (src) {
              qrp= ODBCSrcCols(g, dsn, (char*)src);
              src= NULL;     // for next tests
            } else 
              qrp= ODBCColumns(g, dsn, (char *) tab, NULL, fnc == FNC_COL);

            break;
          case FNC_TABLE:
            qrp= ODBCTables(g, dsn, (char *) tab, true);
            break;
          case FNC_DSN:
            qrp= ODBCDataSources(g, true);
            break;
          case FNC_DRIVER:
            qrp= ODBCDrivers(g, true);
            break;
          default:
            sprintf(g->Message, "invalid catfunc %s", fncn);
            break;
        } // endswitch info

        break;
#endif   // ODBC_SUPPORT
#if defined(MYSQL_SUPPORT)
      case TAB_MYSQL:
        qrp= MyColumns(g, host, db, user, pwd, tab, 
                       NULL, port, fnc == FNC_COL);
        break;
#endif   // MYSQL_SUPPORT
      case TAB_CSV:
        qrp= CSVColumns(g, fn, spc, qch, hdr, mxr, fnc == FNC_COL);
        break;
#if defined(WIN32)
      case TAB_WMI:
        qrp= WMIColumns(g, nsp, cls, fnc == FNC_COL);
        break;
#endif   // WIN32
      case TAB_PRX:
      case TAB_TBL:
      case TAB_XCL:
      case TAB_OCCUR:
        bif= fnc == FNC_COL;
        qrp= TabColumns(g, thd, db, tab, bif);

        if (!qrp && bif && fnc != FNC_COL)         // tab is a view
          qrp= MyColumns(g, host, db, user, pwd, tab, NULL, port, false);

        if (qrp && ttp == TAB_OCCUR && fnc != FNC_COL)
          if (OcrColumns(g, qrp, col, ocl, rnk)) {
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            return HA_ERR_INTERNAL_ERROR;
            } // endif OcrColumns

        break;
      case TAB_PIVOT:
        qrp= PivotColumns(g, tab, src, pic, fcl, host, db, user, pwd, port);
        break;
      default:
        strcpy(g->Message, "System error during assisted discovery");
        break;
      } // endswitch ttp

    if (!qrp) {
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      return HA_ERR_INTERNAL_ERROR;
      } // endif qrp

    if (fnc != FNC_NO || src || ttp == TAB_PIVOT) {
      // Catalog like table
      for (crp= qrp->Colresp; !rc && crp; crp= crp->Next) {
        cnm= encode(g, crp->Name);
        typ= crp->Type;
        len= crp->Length;
        dec= crp->Prec;
        flg= crp->Flag;
     
#if defined(NEW_WAY)
        // Now add the field
        rc= add_fields(g, thd, &alter_info, cnm, typ, len, dec,
                       NOT_NULL_FLAG, "", flg, dbf);
#else   // !NEW_WAY
        // Now add the field
        if (add_field(&sql, cnm, typ, len, dec, NOT_NULL_FLAG, 0, flg, dbf))
          rc= HA_ERR_OUT_OF_MEM;
#endif  // !NEW_WAY
      } // endfor crp

    } else              // Not a catalog table
      for (i= 0; !rc && i < qrp->Nblin; i++) {
        typ= len= dec= 0;
        tm= NOT_NULL_FLAG;
        cnm= (char*)"noname";
#if defined(NEW_WAY)
        rem= "";
//      cs= NULL;
#else   // !NEW_WAY
        rem= NULL;
#endif  // !NEW_WAY

        for (crp= qrp->Colresp; crp; crp= crp->Next)
          switch (crp->Fld) {
            case FLD_NAME:
              cnm= encode(g, crp->Kdata->GetCharValue(i));
              break;
            case FLD_TYPE:
              typ= crp->Kdata->GetIntValue(i);
              break;
            case FLD_PREC:
              len= crp->Kdata->GetIntValue(i);
              break;
            case FLD_SCALE:
              dec= crp->Kdata->GetIntValue(i);
              break;
            case FLD_NULL:
              if (crp->Kdata->GetIntValue(i))
                tm= 0;               // Nullable

              break;
            case FLD_REM:
              rem= crp->Kdata->GetCharValue(i);
              break;
//          case FLD_CHARSET:    
              // No good because remote table is already translated
//            if (*(csn= crp->Kdata->GetCharValue(i)))
//              cs= get_charset_by_name(csn, 0);

//            break;
            default:
              break;                 // Ignore
            } // endswitch Fld

#if defined(ODBC_SUPPORT)
        if (ttp == TAB_ODBC) {
          int plgtyp;

          // typ must be PLG type, not SQL type
          if (!(plgtyp= TranslateSQLType(typ, dec, len))) {
            sprintf(g->Message, "Unsupported SQL type %d", typ);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            return HA_ERR_INTERNAL_ERROR;
          } else
            typ= plgtyp;

          // Some data sources do not count dec in length
          if (typ == TYPE_FLOAT)
            len += (dec + 2);        // To be safe
          else
            dec= 0;

          } // endif ttp
#endif   // ODBC_SUPPORT

        // Make the arguments as required by add_fields
        if (typ == TYPE_DATE)
          len= 0;

        // Now add the field
#if defined(NEW_WAY)
        rc= add_fields(g, thd, &alter_info, cnm, typ, len, dec,
                       tm, rem, 0, true);
#else   // !NEW_WAY
        if (add_field(&sql, cnm, typ, len, dec, tm, rem, 0, true))
          rc= HA_ERR_OUT_OF_MEM;
#endif  // !NEW_WAY
        } // endfor i

#if defined(NEW_WAY)
    rc= init_table_share(thd, table_s, create_info, &alter_info);
#else   // !NEW_WAY
    if (!rc)
      rc= init_table_share(thd, table_s, create_info, &sql);
//    rc= init_table_share(thd, table_s, create_info, dsn, &sql);
#endif   // !NEW_WAY

    return rc;
    } // endif ok

  my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
  return HA_ERR_INTERNAL_ERROR;
} // end of connect_assisted_discovery

/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @note
  Currently we do some checking on the create definitions and stop
  creating if an error is found. We wish we could change the table
  definition such as providing a default table type. However, as said
  above, there are no method to do so.

  @see
  ha_create_table() in handle.cc
*/

int ha_connect::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
  int     rc= RC_OK;
  bool    dbf;
  Field* *field;
  Field  *fp;
  TABTYPE type;
  TABLE  *st= table;                       // Probably unuseful
  THD    *thd= ha_thd(); 
  xp= GetUser(thd, xp);
  PGLOBAL g= xp->g;

  DBUG_ENTER("ha_connect::create");
  PTOS options= GetTableOptionStruct(table_arg);

  // CONNECT engine specific table options:
  DBUG_ASSERT(options);
  type= GetTypeID(options->type);

  // Check table type
  if (type == TAB_UNDEF) {
    options->type= (options->srcdef)  ? "MYSQL" : 
                   (options->tabname) ? "PROXY" : "DOS";
    type= GetTypeID(options->type);
    sprintf(g->Message, "No table_type. Will be set to %s", options->type);
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
  } else if (type == TAB_NIY) {
    sprintf(g->Message, "Unsupported table type %s", options->type);
    my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
  } // endif ttp

  if (check_privileges(thd, options))
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  if (options->data_charset) {
    const CHARSET_INFO *data_charset;

    if (!(data_charset= get_charset_by_csname(options->data_charset,
                                              MY_CS_PRIMARY, MYF(0)))) {
      my_error(ER_UNKNOWN_CHARACTER_SET, MYF(0), options->data_charset);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endif charset

    if (type == TAB_XML && data_charset != &my_charset_utf8_general_ci) {
      my_printf_error(ER_UNKNOWN_ERROR,
                      "DATA_CHARSET='%s' is not supported for TABLE_TYPE=XML",
                        MYF(0), options->data_charset);
      DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
      } // endif utf8

    } // endif charset

  if (!g) {
    rc= HA_ERR_INTERNAL_ERROR;
    DBUG_RETURN(rc);
  } else
    dbf= (GetTypeID(options->type) == TAB_DBF && !options->catfunc);

  // Can be null in ALTER TABLE
  if (create_info->alias)
    // Check whether a table is defined on itself
    switch (type) {
      case TAB_PRX:
      case TAB_XCL:
      case TAB_PIVOT:
      case TAB_OCCUR:
        if (options->srcdef) {
          strcpy(g->Message, "Cannot check looping reference");
          push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        } else if (options->tabname) {
          if (!stricmp(options->tabname, create_info->alias) &&
             (!options->dbname || !stricmp(options->dbname, table_arg->s->db.str))) {
            sprintf(g->Message, "A %s table cannot refer to itself",
                                options->type);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
            } // endif tab

        } else {
          strcpy(g->Message, "Missing object table name or definition");
          my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
          DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
        } // endif tabname

      case TAB_MYSQL:
       {const char *src= options->srcdef;
        char *host, *db, *tab= (char*)options->tabname;
        int   port;

        host= GetListOption(g, "host", options->oplist, NULL);
        db= GetListOption(g, "database", options->oplist, NULL);
        port= atoi(GetListOption(g, "port", options->oplist, "0"));

        if (create_info->connect_string.str) {
          char   *dsn;
          int     len= create_info->connect_string.length;
          PMYDEF  mydef= new(g) MYSQLDEF();
          PDBUSER dup= PlgGetUser(g);
          PCATLG  cat= (dup) ? dup->Catalog : NULL;

          dsn= (char*)PlugSubAlloc(g, NULL, len + 1);
          strncpy(dsn, create_info->connect_string.str, len);
          dsn[len]= 0;
          mydef->SetName(create_info->alias);
          mydef->SetCat(cat);

          if (!mydef->ParseURL(g, dsn, false)) {
            if (mydef->GetHostname())
              host= mydef->GetHostname();

            if (mydef->GetDatabase())
              db= mydef->GetDatabase();

            if (mydef->GetTabname())
              tab= mydef->GetTabname();

            if (mydef->GetPortnumber())
              port= mydef->GetPortnumber();

          } else {
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
          } // endif ParseURL

          } // endif connect_string

        if (CheckSelf(g, table_arg->s, host, db, tab, src, port)) {
          my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
          DBUG_RETURN(HA_ERR_INTERNAL_ERROR);
          } // endif CheckSelf

       }break; 
      default: /* do nothing */;
        break;
     } // endswitch ttp

  if (type == TAB_XML) {
    bool  dom;                  // True: MS-DOM, False libxml2
    char *xsup= GetListOption(g, "Xmlsup", options->oplist, "*");

    // Note that if no support is specified, the default is MS-DOM
    // on Windows and libxml2 otherwise
    switch (*xsup) {
      case '*':
#if defined(WIN32)
        dom= true;
#else   // !WIN32
        dom= false;
#endif  // !WIN32
        break;
      case 'M':
      case 'D':
        dom= true;
        break;
      default:
        dom= false;
        break;
      } // endswitch xsup

#if !defined(DOMDOC_SUPPORT)
    if (dom) {
      strcpy(g->Message, "MS-DOM not supported by this version");
      xsup= NULL;
      } // endif DomDoc
#endif   // !DOMDOC_SUPPORT

#if !defined(LIBXML2_SUPPORT)
    if (!dom) {
      strcpy(g->Message, "libxml2 not supported by this version");
      xsup= NULL;
      } // endif Libxml2
#endif   // !LIBXML2_SUPPORT

    if (!xsup) {
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      rc= HA_ERR_INTERNAL_ERROR;
      DBUG_RETURN(rc);
      } // endif xsup

    } // endif type

  // Check column types
  for (field= table_arg->field; *field; field++) {
    fp= *field;

    if (fp->vcol_info && !fp->stored_in_db)
      continue;            // This is a virtual column

    if (fp->flags & AUTO_INCREMENT_FLAG) {
      strcpy(g->Message, "Auto_increment is not supported yet");
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      rc= HA_ERR_INTERNAL_ERROR;
      DBUG_RETURN(rc);
      } // endif flags

    if (fp->flags & (BLOB_FLAG | ENUM_FLAG | SET_FLAG)) {
      sprintf(g->Message, "Unsupported type for column %s",
                          fp->field_name);
      my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
      rc= HA_ERR_INTERNAL_ERROR;
      DBUG_RETURN(rc);
      } // endif flags

    switch (fp->type()) {
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_YEAR:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_TINY:
        break;                     // Ok
      case MYSQL_TYPE_VAR_STRING:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_INT24:
        break;                     // To be checked
      case MYSQL_TYPE_BIT:
      case MYSQL_TYPE_NULL:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET:
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_GEOMETRY:
      default:
//      fprintf(stderr, "Unsupported type column %s\n", fp->field_name);
        sprintf(g->Message, "Unsupported type for column %s",
                            fp->field_name);
        rc= HA_ERR_INTERNAL_ERROR;
        my_printf_error(ER_UNKNOWN_ERROR,
                        "Unsupported type for column '%s'",
                        MYF(0), fp->field_name);
        DBUG_RETURN(rc);
        break;
      } // endswitch type

    if ((fp)->real_maybe_null() && !IsTypeNullable(type)) {
      my_printf_error(ER_UNKNOWN_ERROR, 
                "Table type %s does not support nullable columns",
                MYF(0), options->type);
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
      } // endif !nullable

    if (dbf) {
      bool b= false;

      if ((b= strlen(fp->field_name) > 10))
        sprintf(g->Message, "DBF: Column name '%s' is too long (max=10)",
                            fp->field_name);
      else if ((b= fp->field_length > 255))
        sprintf(g->Message, "DBF: Column length too big for '%s' (max=255)",
                            fp->field_name);

      if (b) {
        my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
        rc= HA_ERR_INTERNAL_ERROR;
        DBUG_RETURN(rc);
        } // endif b

      } // endif dbf

    } // endfor field

  if (IsFileType(type)) {
    table= table_arg;       // Used by called functions

    if (!options->filename) {
      // The file name is not specified, create a default file in
      // the database directory named table_name.table_type.
      // (temporarily not done for XML because a void file causes
      // the XML parsers to report an error on the first Insert)
      char buf[256], fn[_MAX_PATH], dbpath[128], lwt[12];
      int  h;

      strcpy(buf, GetTableName());

      if (*buf != '#') {
        // Check for incompatible options
        if (options->sepindex) {
          my_message(ER_UNKNOWN_ERROR,
                "SEPINDEX is incompatible with unspecified file name",
                MYF(0));
          DBUG_RETURN(HA_ERR_UNSUPPORTED);
        } else if (GetTypeID(options->type) == TAB_VEC)
          if (!table->s->max_rows || options->split) {
            my_printf_error(ER_UNKNOWN_ERROR, 
                "%s tables whose file name is unspecified cannot be split",
                MYF(0), options->type);
            DBUG_RETURN(HA_ERR_UNSUPPORTED);
          } else if (options->header == 2) {
            my_printf_error(ER_UNKNOWN_ERROR, 
            "header=2 is not allowed for %s tables whose file name is unspecified",
                MYF(0), options->type);
            DBUG_RETURN(HA_ERR_UNSUPPORTED);
          } // endif's

        // Fold type to lower case
        for (int i= 0; i < 12; i++)
          if (!options->type[i]) {
            lwt[i]= 0;
            break;
          } else
            lwt[i]= tolower(options->type[i]);
        
        strcat(strcat(buf, "."), lwt);
        sprintf(g->Message, "No file name. Table will use %s", buf);
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        strcat(strcat(strcpy(dbpath, "./"), table->s->db.str), "/");
        PlugSetPath(fn, buf, dbpath);
    
        if ((h= ::open(fn, O_CREAT | O_EXCL, 0666)) == -1) {
          if (errno == EEXIST)
            sprintf(g->Message, "Default file %s already exists", fn);
          else
            sprintf(g->Message, "Error %d creating file %s", errno, fn);

          push_warning(table->in_use, 
                       Sql_condition::WARN_LEVEL_WARN, 0, g->Message);
        } else
          ::close(h);
    
        if (type == TAB_FMT || options->readonly)
          push_warning(table->in_use, Sql_condition::WARN_LEVEL_WARN, 0,
            "Congratulation, you just created a read-only void table!");

        } // endif buf

      } // endif filename

    // To check whether indexes have to be made or remade
    if (!g->Xchk) {
      PIXDEF xdp;

      // We should be in CREATE TABLE
      if (thd_sql_command(table->in_use) != SQLCOM_CREATE_TABLE)
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0,
          "Wrong command in create, please contact CONNECT team");

      // Get the index definitions
      if (xdp= GetIndexInfo()) {
        PDBUSER dup= PlgGetUser(g);
        PCATLG  cat= (dup) ? dup->Catalog : NULL;

        if (cat) {
          cat->SetDataPath(g, table_arg->s->db.str);

          if ((rc= optimize(table->in_use, NULL))) {
            printf("Create rc=%d %s\n", rc, g->Message);
            my_message(ER_UNKNOWN_ERROR, g->Message, MYF(0));
            rc= HA_ERR_INTERNAL_ERROR;
          } else
            CloseTable(g);

          } // endif cat

        } // endif xdp

    } else {
      ((PCHK)g->Xchk)->newsep= GetBooleanOption("Sepindex", false);
      ((PCHK)g->Xchk)->newpix= GetIndexInfo();
    } // endif Xchk

    table= st;
    } // endif type

  DBUG_RETURN(rc);
} // end of create


/**
  check_if_incompatible_data() called if ALTER TABLE can't detect otherwise
  if new and old definition are compatible

  @details If there are no other explicit signs like changed number of
  fields this function will be called by compare_tables()
  (sql/sql_tables.cc) to decide should we rewrite whole table or only .frm
  file.

*/

bool ha_connect::check_if_incompatible_data(HA_CREATE_INFO *info,
                                        uint table_changes)
{
//ha_table_option_struct *param_old, *param_new;
  DBUG_ENTER("ha_connect::check_if_incompatible_data");
  // TO DO: really implement and check it.
  THD *thd= current_thd;

  push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, 
    "The current version of CONNECT did not check what you changed in ALTER. Use at your own risk");
  
  if (table) {
    PTOS newopt= info->option_struct;
    PTOS oldopt= table->s->option_struct;

#if 0
    if (newopt->sepindex != oldopt->sepindex) {
      // All indexes to be remade
      PGLOBAL g= GetPlug(thd);

      if (!g)
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, 0, 
          "Execute OPTIMIZE TABLE to remake the indexes");
      else
        g->Xchk= new(g) XCHK;

      } // endif sepindex
#endif // 0

    if (oldopt->type != newopt->type)
      DBUG_RETURN(COMPATIBLE_DATA_NO);

    if (newopt->filename)
      DBUG_RETURN(COMPATIBLE_DATA_NO);

    } // endif table

  DBUG_RETURN(COMPATIBLE_DATA_YES);
} // end of check_if_incompatible_data


struct st_mysql_storage_engine connect_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(connect)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &connect_storage_engine,
  "CONNECT",
  "Olivier Bertrand",
  "Direct access to external data, including many file formats",
  PLUGIN_LICENSE_GPL,
  connect_init_func,                            /* Plugin Init */
  connect_done_func,                            /* Plugin Deinit */
  0x0001,                                       /* version number (0.1) */
  NULL,                                         /* status variables */
  NULL,                                         /* system variables */
  "0.1",                                        /* string version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL          /* maturity */
}
maria_declare_plugin_end;
