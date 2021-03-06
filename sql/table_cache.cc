/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2011 Monty Program Ab
   Copyright (C) 2013 Sergey Vojtovich and MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file
  Table definition cache and table cache implementation.

  Table definition cache actions:
  - add new TABLE_SHARE object to cache (tdc_acquire_share())
  - acquire TABLE_SHARE object from cache (tdc_acquire_share())
  - release TABLE_SHARE object to cache (tdc_release_share())
  - purge unused TABLE_SHARE objects from cache (tdc_purge())
  - remove TABLE_SHARE object from cache (tdc_remove_table())
  - get number of TABLE_SHARE objects in cache (tdc_records())

  Table cache actions:
  - add new TABLE object to cache (tc_add_table())
  - acquire TABLE object from cache (tc_acquire_table())
  - release TABLE object to cache (tc_release_table())
  - purge unused TABLE objects from cache (tc_purge())
  - purge unused TABLE objects of a table from cache (tdc_remove_table())
  - get number of TABLE objects in cache (tc_records())

  Dependencies:
  - intern_close_table(): frees TABLE object
  - kill_delayed_threads_for_table()
  - close_cached_tables(): flush tables on shutdown
  - alloc_table_share()
  - free_table_share()

  Table cache invariants:
  - TABLE_SHARE::used_tables shall not contain objects with TABLE::in_use == 0
  - TABLE_SHARE::free_tables shall not contain objects with TABLE::in_use != 0
  - unused_tables shall not contain objects with TABLE::in_use != 0
  - cached TABLE object must be either in TABLE_SHARE::used_tables or in
    TABLE_SHARE::free_tables
*/

#include "my_global.h"
#include "hash.h"
#include "table.h"
#include "sql_base.h"

/** Configuration. */
ulong tdc_size; /**< Table definition cache threshold for LRU eviction. */
ulong tc_size; /**< Table cache threshold for LRU eviction. */

/** Data collections. */
static HASH tdc_hash; /**< Collection of TABLE_SHARE objects. */
/** Collection of unused TABLE_SHARE objects. */
static TABLE_SHARE *oldest_unused_share, end_of_unused_share;
TABLE *unused_tables; /**< Collection of unused TABLE objects. */

static int64 tdc_version;  /* Increments on each reload */ 
static int64 last_table_id;
static bool tdc_inited;

static uint tc_count; /**< Number of TABLE objects in table cache. */


/**
  Protects used and unused lists in the TABLE_SHARE object,
  LRU lists of used TABLEs.

  tc_count
  unused_tables
  TABLE::next
  TABLE::prev
  TABLE_SHARE::tdc.free_tables
  TABLE_SHARE::tdc.used_tables
*/

mysql_mutex_t LOCK_open;


/**
  Protects unused shares list.

  TABLE_SHARE::tdc.prev
  TABLE_SHARE::tdc.next
  oldest_unused_share
  end_of_unused_share
*/

static mysql_mutex_t LOCK_unused_shares;
static mysql_rwlock_t LOCK_tdc; /**< Protects tdc_hash. */
static mysql_rwlock_t LOCK_flush; /**< Sync tc_purge() and tdc_remove_table(). */
my_atomic_rwlock_t LOCK_tdc_atomics; /**< Protects tdc_version. */

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_open, key_LOCK_unused_shares,
                     key_TABLE_SHARE_LOCK_table_share;
static PSI_mutex_info all_tc_mutexes[]=
{
  { &key_LOCK_open, "LOCK_open", PSI_FLAG_GLOBAL },
  { &key_LOCK_unused_shares, "LOCK_unused_shares", PSI_FLAG_GLOBAL },
  { &key_TABLE_SHARE_LOCK_table_share, "TABLE_SHARE::tdc.LOCK_table_share", 0 }
};

static PSI_rwlock_key key_rwlock_LOCK_tdc, key_rwlock_LOCK_flush;
static PSI_rwlock_info all_tc_rwlocks[]=
{
  { &key_rwlock_LOCK_tdc, "LOCK_tdc", PSI_FLAG_GLOBAL },
  { &key_rwlock_LOCK_flush, "LOCK_flush", PSI_FLAG_GLOBAL }
};


static void init_tc_psi_keys(void)
{
  const char *category= "sql";
  int count;

  count= array_elements(all_tc_mutexes);
  mysql_mutex_register(category, all_tc_mutexes, count);

  count= array_elements(all_tc_rwlocks);
  mysql_rwlock_register(category, all_tc_rwlocks, count);
}
#endif


/*
  Auxiliary routines for manipulating with per-share used/unused and
  global unused lists of TABLE objects and tc_count counter.
  Responsible for preserving invariants between those lists, counter
  and TABLE::in_use member.
  In fact those routines implement sort of implicit table cache as
  part of table definition cache.
*/


/**
  Get number of TABLE objects (used and unused) in table cache.

  @todo Protect tc_count so it is read atomically.
*/

uint tc_records(void)
{
  return tc_count;
}


/**
  Free all unused TABLE objects.

  While locked:
  - remove unused objects from TABLE_SHARE::tdc.free_tables lists
  - reset unused_tables
  - decrement tc_count

  While unlocked:
  - free resources related to unused objects

  @note This is called by 'handle_manager' when one wants to
        periodicly flush all not used tables.
*/

void tc_purge(void)
{
  mysql_mutex_lock(&LOCK_open);
  if (unused_tables)
  {
    TABLE *table= unused_tables, *next;
    unused_tables->prev->next= 0;
    do
    {
      unused_tables->s->tdc.free_tables.remove(unused_tables);
      tc_count--;
    } while ((unused_tables= unused_tables->next));
    mysql_rwlock_rdlock(&LOCK_flush);
    mysql_mutex_unlock(&LOCK_open);

    do
    {
      next= table->next;
      intern_close_table(table);
    } while ((table= next));
    mysql_rwlock_unlock(&LOCK_flush);
  }
  else
    mysql_mutex_unlock(&LOCK_open);
}


/**
  Verify consistency of used/unused lists (for debugging).
*/

#ifdef EXTRA_DEBUG
static void check_unused(THD *thd)
{
  uint count= 0, open_files= 0;
  TABLE *cur_link, *start_link, *entry;
  TABLE_SHARE *share;
  TDC_iterator tdc_it;

  tdc_it.init();
  mysql_mutex_lock(&LOCK_open);
  if ((start_link=cur_link=unused_tables))
  {
    do
    {
      if (cur_link != cur_link->next->prev || cur_link != cur_link->prev->next)
      {
	DBUG_PRINT("error",("Unused_links aren't linked properly")); /* purecov: inspected */
	return; /* purecov: inspected */
      }
    } while (count++ < tc_count &&
	     (cur_link=cur_link->next) != start_link);
    if (cur_link != start_link)
    {
      DBUG_PRINT("error",("Unused_links aren't connected")); /* purecov: inspected */
    }
  }
  while ((share= tdc_it.next()))
  {
    TABLE_SHARE::TABLE_list::Iterator it(share->tdc.free_tables);
    while ((entry= it++))
    {
      /*
        We must not have TABLEs in the free list that have their file closed.
      */
      DBUG_ASSERT(entry->db_stat && entry->file);
      /* Merge children should be detached from a merge parent */
      if (entry->in_use)
      {
        DBUG_PRINT("error",("Used table is in share's list of unused tables")); /* purecov: inspected */
      }
      /* extra() may assume that in_use is set */
      entry->in_use= thd;
      DBUG_ASSERT(!thd || !entry->file->extra(HA_EXTRA_IS_ATTACHED_CHILDREN));
      entry->in_use= 0;

      count--;
      open_files++;
    }
    it.init(share->tdc.used_tables);
    while ((entry= it++))
    {
      if (!entry->in_use)
      {
        DBUG_PRINT("error",("Unused table is in share's list of used tables")); /* purecov: inspected */
      }
      open_files++;
    }
  }
  mysql_mutex_unlock(&LOCK_open);
  tdc_it.deinit();
  if (count != 0)
  {
    DBUG_PRINT("error",("Unused_links doesn't match open_cache: diff: %d", /* purecov: inspected */
                       count)); /* purecov: inspected */
  }
}
#else
#define check_unused(A)
#endif


/**
  Remove unused TABLE object from table cache.

  @pre LOCK_open is locked, table is not used.

  While locked:
  - remove object from TABLE_SHARE::tdc.free_tables
  - remove object from unused_tables

  @note This is helper routine, supposed to be used by table cache
  methods only.
*/

static void tc_remove_table(TABLE *table)
{
  mysql_mutex_assert_owner(&LOCK_open);
  DBUG_ASSERT(!table->in_use);
  /* Remove from per-share chain of unused TABLE objects. */
  table->s->tdc.free_tables.remove(table);

  /* And global unused chain. */
  table->next->prev= table->prev;
  table->prev->next= table->next;
  if (table == unused_tables)
  {
    unused_tables= unused_tables->next;
    if (table == unused_tables)
      unused_tables= 0;
  }
  tc_count--;
}


/**
  Add new TABLE object to table cache.

  @pre TABLE object is used by caller.

  Added object cannot be evicted or acquired.

  While locked:
  - add object to TABLE_SHARE::tdc.used_tables
  - increment tc_count
  - evict LRU object from table cache if we reached threshold

  While unlocked: 
  - free evicted object
*/

void tc_add_table(THD *thd, TABLE *table)
{
  DBUG_ASSERT(table->in_use == thd);
  mysql_mutex_lock(&LOCK_open);
  table->s->tdc.used_tables.push_front(table);
  tc_count++;
  /* If we have too many TABLE instances around, try to get rid of them */
  if (tc_count > tc_size && unused_tables)
  {
    TABLE *purge_table= unused_tables;
    tc_remove_table(purge_table);
    mysql_rwlock_rdlock(&LOCK_flush);
    mysql_mutex_unlock(&LOCK_open);
    intern_close_table(purge_table);
    mysql_rwlock_unlock(&LOCK_flush);
    check_unused(thd);
  }
  else
    mysql_mutex_unlock(&LOCK_open);
}


/**
  Acquire TABLE object from table cache.

  @pre share must be protected against removal.

  Acquired object cannot be evicted or acquired again.

  While locked:
  - pop object from TABLE_SHARE::tdc.free_tables()
  - remove share protection
  - remove object from unused_tables
  - add object to TABLE_SHARE::tdc.used_tables()
  - mark object used by thd

  @note share protection is kept if there are no unused objects.

  @return TABLE object, or NULL if no unused objects.
*/

static TABLE *tc_acquire_table(THD *thd, TABLE_SHARE *share)
{
  TABLE *table;

  mysql_mutex_lock(&LOCK_open);
  if (!(table= share->tdc.free_tables.pop_front()))
  {
    mysql_mutex_unlock(&LOCK_open);
    return 0;
  }
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_ASSERT(!table->in_use);

  /* Unlink table from global unused tables list. */
  if (table == unused_tables)
  {                                             // First unused
    unused_tables=unused_tables->next;	        // Remove from link
    if (table == unused_tables)
      unused_tables=0;
  }
  table->prev->next=table->next;		/* Remove from unused list */
  table->next->prev=table->prev;
  table->in_use= thd;
  /* Add table to list of used tables for this share. */
  table->s->tdc.used_tables.push_front(table);
  mysql_mutex_unlock(&LOCK_open);

  /* The ex-unused table must be fully functional. */
  DBUG_ASSERT(table->db_stat && table->file);
  /* The children must be detached from the table. */
  DBUG_ASSERT(! table->file->extra(HA_EXTRA_IS_ATTACHED_CHILDREN));
  check_unused(thd);
  return table;
}


/**
  Release TABLE object to table cache.

  @pre object is used by caller.

  Released object may be evicted or acquired again.

  While locked:
  - mark object not in use by any thread
  - remove object from TABLE_SHARE::tdc.used_tables
  - if object is marked for purge, decrement tc_count
  - add object to TABLE_SHARE::tdc.free_tables
  - add object to unused_tables
  - evict LRU object from table cache if we reached threshold

  While unlocked: 
  - free evicted/purged object

  @note Another thread may mark share for purge any moment (even
  after version check). It means to-be-purged object may go to
  unused lists. This other thread is expected to call tc_purge(),
  which is synchronized with us on LOCK_open.

  @return
    @retval true  object purged
    @retval false object released
*/

bool tc_release_table(TABLE *table)
{
  THD *thd __attribute__((unused))= table->in_use;
  DBUG_ASSERT(table->in_use);
  DBUG_ASSERT(table->file);

  mysql_mutex_lock(&LOCK_open);
  /* Remove table from the list of tables used in this share. */
  table->s->tdc.used_tables.remove(table);
  table->in_use= 0;
  if (table->s->has_old_version() || table->needs_reopen() || !tdc_size)
  {
    tc_count--;
    mysql_rwlock_rdlock(&LOCK_flush);
    mysql_mutex_unlock(&LOCK_open);
    intern_close_table(table);
    mysql_rwlock_unlock(&LOCK_flush);
    return true;
  }
  /* Add table to the list of unused TABLE objects for this share. */
  table->s->tdc.free_tables.push_front(table);
  /* Also link it last in the global list of unused TABLE objects. */
  if (unused_tables)
  {
    table->next=unused_tables;
    table->prev=unused_tables->prev;
    unused_tables->prev=table;
    table->prev->next=table;
  }
  else
    unused_tables=table->next=table->prev=table;
  /*
    We free the least used table, not the subject table,
    to keep the LRU order.
  */
  if (tc_count > tc_size)
  {
    TABLE *purge_table= unused_tables;
    tc_remove_table(purge_table);
    mysql_rwlock_rdlock(&LOCK_flush);
    mysql_mutex_unlock(&LOCK_open);
    intern_close_table(purge_table);
    mysql_rwlock_unlock(&LOCK_flush);
  }
  else
    mysql_mutex_unlock(&LOCK_open);
  check_unused(thd);
  return false;
}


extern "C" uchar *tdc_key(const uchar *record, size_t *length,
                          my_bool not_used __attribute__((unused)))
{
  TABLE_SHARE *entry= (TABLE_SHARE*) record;
  *length= entry->table_cache_key.length;
  return (uchar*) entry->table_cache_key.str;
}


/**
  Delete share from hash and free share object.

  @return
    @retval 0 Success
    @retval 1 Share is referenced
*/

static int tdc_delete_share_from_hash(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_delete_share_from_hash");
  mysql_rwlock_wrlock(&LOCK_tdc);
  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  if (--share->tdc.ref_count)
  {
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_rwlock_unlock(&LOCK_tdc);
    DBUG_RETURN(1);
  }
  my_hash_delete(&tdc_hash, (uchar*) share);
  /* Notify PFS early, while still locked. */
  PSI_CALL_release_table_share(share->m_psi);
  share->m_psi= 0;
  mysql_rwlock_unlock(&LOCK_tdc);

  if (share->tdc.m_flush_tickets.is_empty())
  {
    /* No threads are waiting for this share to be flushed, destroy it. */
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    free_table_share(share);
  }
  else
  {
    Wait_for_flush_list::Iterator it(share->tdc.m_flush_tickets);
    Wait_for_flush *ticket;
    while ((ticket= it++))
      (void) ticket->get_ctx()->m_wait.set_status(MDL_wait::GRANTED);
    /*
      If there are threads waiting for this share to be flushed,
      the last one to receive the notification will destroy the
      share. At this point the share is removed from the table
      definition cache, so is OK to proceed here without waiting
      for this thread to do the work.
    */
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  }
  DBUG_RETURN(0);
}


/**
  Initialize table definition cache.

  @retval  0  Success
  @retval !0  Error
*/

int tdc_init(void)
{
  DBUG_ENTER("tdc_init");
#ifdef HAVE_PSI_INTERFACE
  init_tc_psi_keys();
#endif
  tdc_inited= true;
  mysql_mutex_init(key_LOCK_open, &LOCK_open, MY_MUTEX_INIT_FAST);
  mysql_mutex_record_order(&LOCK_active_mi, &LOCK_open);
  /*
    We must have LOCK_open before LOCK_global_system_variables because
    LOCK_open is held while sql_plugin.cc::intern_sys_var_ptr() is called.
  */
  mysql_mutex_record_order(&LOCK_open, &LOCK_global_system_variables);
  mysql_mutex_init(key_LOCK_unused_shares, &LOCK_unused_shares,
                   MY_MUTEX_INIT_FAST);
  mysql_rwlock_init(key_rwlock_LOCK_tdc, &LOCK_tdc);
  mysql_rwlock_init(key_rwlock_LOCK_flush, &LOCK_flush);
  my_atomic_rwlock_init(&LOCK_tdc_atomics);
  oldest_unused_share= &end_of_unused_share;
  end_of_unused_share.tdc.prev= &oldest_unused_share;
  tdc_version= 1L;  /* Increments on each reload */
  DBUG_RETURN(my_hash_init(&tdc_hash, &my_charset_bin, tdc_size, 0, 0, tdc_key,
                           0, 0));
}


/**
  Notify table definition cache that process of shutting down server
  has started so it has to keep number of TABLE and TABLE_SHARE objects
  minimal in order to reduce number of references to pluggable engines.
*/

void tdc_start_shutdown(void)
{
  DBUG_ENTER("table_def_start_shutdown");
  if (tdc_inited)
  {
    /*
      Ensure that TABLE and TABLE_SHARE objects which are created for
      tables that are open during process of plugins' shutdown are
      immediately released. This keeps number of references to engine
      plugins minimal and allows shutdown to proceed smoothly.
    */
    tdc_size= 0;
    /* Free all cached but unused TABLEs and TABLE_SHAREs. */
    close_cached_tables(NULL, NULL, FALSE, LONG_TIMEOUT);
  }
  DBUG_VOID_RETURN;
}


/**
  Deinitialize table definition cache.
*/

void tdc_deinit(void)
{
  DBUG_ENTER("tdc_deinit");
  if (tdc_inited)
  {
    tdc_inited= false;
    my_hash_free(&tdc_hash);
    my_atomic_rwlock_destroy(&LOCK_tdc_atomics);
    mysql_rwlock_destroy(&LOCK_flush);
    mysql_rwlock_destroy(&LOCK_tdc);
    mysql_mutex_destroy(&LOCK_unused_shares);
    mysql_mutex_destroy(&LOCK_open);
  }
  DBUG_VOID_RETURN;
}


/**
  Get number of cached table definitions.

  @return Number of cached table definitions
*/

ulong tdc_records(void)
{
  ulong records;
  DBUG_ENTER("tdc_records");
  mysql_rwlock_rdlock(&LOCK_tdc);
  records= tdc_hash.records;
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_RETURN(records);
}


void tdc_purge(bool all)
{
  DBUG_ENTER("tdc_purge");
  for (;;)
  {
    TABLE_SHARE *share;
    if (!all)
    {
      mysql_rwlock_rdlock(&LOCK_tdc);
      if (tdc_hash.records <= tdc_size)
      {
        mysql_rwlock_unlock(&LOCK_tdc);
        break;
      }
      mysql_rwlock_unlock(&LOCK_tdc);
    }

    mysql_mutex_lock(&LOCK_unused_shares);
    if (!oldest_unused_share->tdc.next)
    {
      mysql_mutex_unlock(&LOCK_unused_shares);
      break;
    }

    share= oldest_unused_share;
    *share->tdc.prev= share->tdc.next;
    share->tdc.next->tdc.prev= share->tdc.prev;
    /* Concurrent thread may start using share again, reset prev and next. */
    share->tdc.prev= 0;
    share->tdc.next= 0;
    mysql_mutex_lock(&share->tdc.LOCK_table_share);
    share->tdc.ref_count++;
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);

    tdc_delete_share_from_hash(share);
  }
  DBUG_VOID_RETURN;
}


/**
  Prepeare table share for use with table definition cache.
*/

void tdc_init_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_init_share");
  mysql_mutex_init(key_TABLE_SHARE_LOCK_table_share,
                   &share->tdc.LOCK_table_share, MY_MUTEX_INIT_FAST);
  share->tdc.m_flush_tickets.empty();
  share->tdc.used_tables.empty();
  share->tdc.free_tables.empty();
  tdc_assign_new_table_id(share);
  share->version= tdc_refresh_version();
  DBUG_VOID_RETURN;
}


/**
  Release table definition cache specific resources of table share.
*/

void tdc_deinit_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_deinit_share");
  DBUG_ASSERT(share->tdc.ref_count == 0);
  DBUG_ASSERT(share->tdc.m_flush_tickets.is_empty());
  DBUG_ASSERT(share->tdc.used_tables.is_empty());
  DBUG_ASSERT(share->tdc.free_tables.is_empty());
  mysql_mutex_destroy(&share->tdc.LOCK_table_share);
  DBUG_VOID_RETURN;
}


/**
  Lock table share.

  Find table share with given db.table_name in table definition cache. Return
  locked table share if found.

  Locked table share means:
  - table share is protected against removal from table definition cache
  - no other thread can acquire/release table share

  Caller is expected to unlock table share with tdc_unlock_share().

  @retval  0 Share not found
  @retval !0 Pointer to locked table share 
*/

TABLE_SHARE *tdc_lock_share(const char *db, const char *table_name)
{
  char key[MAX_DBKEY_LENGTH];
  uint key_length;

  DBUG_ENTER("tdc_lock_share");
  key_length= tdc_create_key(key, db, table_name);

  mysql_rwlock_rdlock(&LOCK_tdc);
  TABLE_SHARE* share= (TABLE_SHARE*) my_hash_search(&tdc_hash,
                                                    (uchar*) key, key_length);
  if (share && !share->error)
    mysql_mutex_lock(&share->tdc.LOCK_table_share);
  else
    share= 0;
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_RETURN(share);
}


/**
  Unlock share locked by tdc_lock_share().
*/

void tdc_unlock_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_unlock_share");
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  DBUG_VOID_RETURN;
}


/*
  Get TABLE_SHARE for a table.

  tdc_acquire_share()
  thd                   Thread handle
  table_list            Table that should be opened
  key                   Table cache key
  key_length            Length of key
  flags                 operation: what to open table or view

  IMPLEMENTATION
    Get a table definition from the table definition cache.
    If it doesn't exist, create a new from the table definition file.

  RETURN
   0  Error
   #  Share for table
*/

TABLE_SHARE *tdc_acquire_share(THD *thd, const char *db, const char *table_name,
                               const char *key, uint key_length, uint flags,
                               TABLE **out_table)
{
  TABLE_SHARE *share;
  bool was_unused;
  my_hash_value_type hash_value;
  DBUG_ENTER("tdc_acquire_share");

  hash_value= my_calc_hash(&tdc_hash, (uchar*) key, key_length);

  mysql_rwlock_rdlock(&LOCK_tdc);
  share= (TABLE_SHARE*) my_hash_search_using_hash_value(&tdc_hash, hash_value,
                                                        (uchar*) key,
                                                        key_length);
  if (!share)
  {
    TABLE_SHARE *new_share;
    mysql_rwlock_unlock(&LOCK_tdc);

    if (!(new_share= alloc_table_share(db, table_name, key, key_length)))
      DBUG_RETURN(0);
    new_share->error= OPEN_FRM_OPEN_ERROR;

    mysql_rwlock_wrlock(&LOCK_tdc);
    share= (TABLE_SHARE*) my_hash_search_using_hash_value(&tdc_hash, hash_value,
                                                          (uchar*) key,
                                                          key_length);
    if (!share)
    {
      bool need_purge;

      share= new_share;
      mysql_mutex_lock(&share->tdc.LOCK_table_share);
      if (my_hash_insert(&tdc_hash, (uchar*) share))
      {
        mysql_mutex_unlock(&share->tdc.LOCK_table_share);
        mysql_rwlock_unlock(&LOCK_tdc);
        free_table_share(share);
        DBUG_RETURN(0);
      }
      need_purge= tdc_hash.records > tdc_size;
      mysql_rwlock_unlock(&LOCK_tdc);

      /* note that tdc_acquire_share() *always* uses discovery */
      open_table_def(thd, share, flags | GTS_USE_DISCOVERY);
      share->tdc.ref_count++;
      mysql_mutex_unlock(&share->tdc.LOCK_table_share);

      if (share->error)
      {
        tdc_delete_share_from_hash(share);
        DBUG_RETURN(0);
      }
      else if (need_purge)
        tdc_purge(false);
      if (out_table)
        *out_table= 0;
      share->m_psi= PSI_CALL_get_table_share(false, share);
      goto end;
    }
    free_table_share(new_share);
  }

  /* cannot force discovery of a cached share */
  DBUG_ASSERT(!(flags & GTS_FORCE_DISCOVERY));

  if (out_table && (flags & GTS_TABLE))
  {
    if ((*out_table= tc_acquire_table(thd, share)))
    {
      DBUG_ASSERT(!(flags & GTS_NOLOCK));
      DBUG_ASSERT(!share->error);
      DBUG_ASSERT(!share->is_view);
      DBUG_RETURN(share);
    }
  }

  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  mysql_rwlock_unlock(&LOCK_tdc);

  /*
     We found an existing table definition. Return it if we didn't get
     an error when reading the table definition from file.
  */
  if (share->error)
  {
    open_table_error(share, share->error, share->open_errno);
    goto err;
  }

  if (share->is_view && !(flags & GTS_VIEW))
  {
    open_table_error(share, OPEN_FRM_NOT_A_TABLE, ENOENT);
    goto err;
  }
  if (!share->is_view && !(flags & GTS_TABLE))
  {
    open_table_error(share, OPEN_FRM_NOT_A_VIEW, ENOENT);
    goto err;
  }

  was_unused= !share->tdc.ref_count;
  share->tdc.ref_count++;
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  if (was_unused)
  {
    mysql_mutex_lock(&LOCK_unused_shares);
    if (share->tdc.prev)
    {
      /*
        Share was not used before and it was in the old_unused_share list
        Unlink share from this list
      */
      DBUG_PRINT("info", ("Unlinking from not used list"));
      *share->tdc.prev= share->tdc.next;
      share->tdc.next->tdc.prev= share->tdc.prev;
      share->tdc.next= 0;
      share->tdc.prev= 0;
    } 
    mysql_mutex_unlock(&LOCK_unused_shares);
  }

end:
  DBUG_PRINT("exit", ("share: 0x%lx  ref_count: %u",
                      (ulong) share, share->tdc.ref_count));
  if (flags & GTS_NOLOCK)
  {
    tdc_release_share(share);
    /*
      if GTS_NOLOCK is requested, the returned share pointer cannot be used,
      the share it points to may go away any moment.
      But perhaps the caller is only interested to know whether a share or
      table existed?
      Let's return an invalid pointer here to catch dereferencing attempts.
    */
    share= (TABLE_SHARE*) 1;
  }
  DBUG_RETURN(share);

err:
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  DBUG_RETURN(0);
}


/**
  Release table share acquired by tdc_acquire_share().
*/

void tdc_release_share(TABLE_SHARE *share)
{
  DBUG_ENTER("tdc_release_share");
  
  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  DBUG_PRINT("enter",
             ("share: 0x%lx  table: %s.%s  ref_count: %u  version: %lu",
              (ulong) share, share->db.str, share->table_name.str,
              share->tdc.ref_count, share->version));
  DBUG_ASSERT(share->tdc.ref_count);

  if (share->tdc.ref_count > 1)
  {
    share->tdc.ref_count--;
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    DBUG_VOID_RETURN;
  }
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);

  mysql_mutex_lock(&LOCK_unused_shares);
  mysql_mutex_lock(&share->tdc.LOCK_table_share);
  if (share->has_old_version())
  {
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    tdc_delete_share_from_hash(share);
    DBUG_VOID_RETURN;
  }
  if (--share->tdc.ref_count)
  {
    mysql_mutex_unlock(&share->tdc.LOCK_table_share);
    mysql_mutex_unlock(&LOCK_unused_shares);
    DBUG_VOID_RETURN;
  }
  /* Link share last in used_table_share list */
  DBUG_PRINT("info", ("moving share to unused list"));
  DBUG_ASSERT(share->tdc.next == 0);
  share->tdc.prev= end_of_unused_share.tdc.prev;
  *end_of_unused_share.tdc.prev= share;
  end_of_unused_share.tdc.prev= &share->tdc.next;
  share->tdc.next= &end_of_unused_share;
  mysql_mutex_unlock(&share->tdc.LOCK_table_share);
  mysql_mutex_unlock(&LOCK_unused_shares);

  /* Delete the least used share to preserve LRU order. */
  tdc_purge(false);
  DBUG_VOID_RETURN;
}


static TABLE_SHARE *tdc_delete_share(const char *db, const char *table_name)
{
  TABLE_SHARE *share;
  DBUG_ENTER("tdc_delete_share");

  while ((share= tdc_lock_share(db, table_name)))
  {
    share->tdc.ref_count++;
    if (share->tdc.ref_count > 1)
    {
      tdc_unlock_share(share);
      DBUG_RETURN(share);
    }
    tdc_unlock_share(share);

    mysql_mutex_lock(&LOCK_unused_shares);
    if (share->tdc.prev)
    {
      *share->tdc.prev= share->tdc.next;
      share->tdc.next->tdc.prev= share->tdc.prev;
      /* Concurrent thread may start using share again, reset prev and next. */
      share->tdc.prev= 0;
      share->tdc.next= 0;
    } 
    mysql_mutex_unlock(&LOCK_unused_shares);

    if (!tdc_delete_share_from_hash(share))
      break;
  }
  DBUG_RETURN(0);
}


/**
   Remove all or some (depending on parameter) instances of TABLE and
   TABLE_SHARE from the table definition cache.

   @param  thd          Thread context
   @param  remove_type  Type of removal:
                        TDC_RT_REMOVE_ALL     - remove all TABLE instances and
                                                TABLE_SHARE instance. There
                                                should be no used TABLE objects
                                                and caller should have exclusive
                                                metadata lock on the table.
                        TDC_RT_REMOVE_NOT_OWN - remove all TABLE instances
                                                except those that belong to
                                                this thread. There should be
                                                no TABLE objects used by other
                                                threads and caller should have
                                                exclusive metadata lock on the
                                                table.
                        TDC_RT_REMOVE_UNUSED  - remove all unused TABLE
                                                instances (if there are no
                                                used instances will also
                                                remove TABLE_SHARE).
                        TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE -
                                                remove all TABLE instances
                                                except those that belong to
                                                this thread, but don't mark
                                                TABLE_SHARE as old. There
                                                should be no TABLE objects
                                                used by other threads and
                                                caller should have exclusive
                                                metadata lock on the table.
   @param  db           Name of database
   @param  table_name   Name of table
   @param  kill_delayed_threads     If TRUE, kill INSERT DELAYED threads

   @note It assumes that table instances are already not used by any
   (other) thread (this should be achieved by using meta-data locks).
*/

bool tdc_remove_table(THD *thd, enum_tdc_remove_table_type remove_type,
                      const char *db, const char *table_name,
                      bool kill_delayed_threads)
{
  TABLE *table;
  TABLE_SHARE *share;
  bool found= false;
  DBUG_ENTER("tdc_remove_table");
  DBUG_PRINT("enter",("name: %s  remove_type: %d", table_name, remove_type));

  DBUG_ASSERT(remove_type == TDC_RT_REMOVE_UNUSED ||
              thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                             MDL_EXCLUSIVE));

  if ((share= tdc_delete_share(db, table_name)))
  {
    I_P_List <TABLE, TABLE_share> purge_tables;
    purge_tables.empty();

    mysql_mutex_lock(&LOCK_open);
    if (kill_delayed_threads)
      kill_delayed_threads_for_table(share);

    TABLE_SHARE::TABLE_list::Iterator it(share->tdc.free_tables);
#ifndef DBUG_OFF
    if (remove_type == TDC_RT_REMOVE_ALL)
      DBUG_ASSERT(share->tdc.used_tables.is_empty());
    else if (remove_type == TDC_RT_REMOVE_NOT_OWN)
    {
      TABLE_SHARE::TABLE_list::Iterator it2(share->tdc.used_tables);
      while ((table= it2++))
        DBUG_ASSERT(table->in_use == thd);
    }
#endif
    /*
      Set share's version to zero in order to ensure that it gets
      automatically deleted once it is no longer referenced.

      Note that code in TABLE_SHARE::wait_for_old_version() assumes that
      incrementing of refresh_version is followed by purge of unused table
      shares.
    */
    if (remove_type != TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE)
      share->version= 0;

    while ((table= it++))
    {
      tc_remove_table(table);
      purge_tables.push_front(table);
    }
    mysql_rwlock_rdlock(&LOCK_flush);
    mysql_mutex_unlock(&LOCK_open);

    while ((table= purge_tables.pop_front()))
      intern_close_table(table);
    mysql_rwlock_unlock(&LOCK_flush);

    check_unused(thd);
    tdc_release_share(share);

    /* Wait for concurrent threads to free unused objects. */
    mysql_rwlock_wrlock(&LOCK_flush);
    mysql_rwlock_unlock(&LOCK_flush);

    found= true;
  }
  DBUG_ASSERT(found || remove_type != TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE);
  DBUG_RETURN(found);
}


/**
  Check if table's share is being removed from the table definition
  cache and, if yes, wait until the flush is complete.

  @param thd             Thread context.
  @param table_list      Table which share should be checked.
  @param timeout         Timeout for waiting.
  @param deadlock_weight Weight of this wait for deadlock detector.

  @retval 0       Success. Share is up to date or has been flushed.
  @retval 1       Error (OOM, was killed, the wait resulted
                  in a deadlock or timeout). Reported.
*/

int tdc_wait_for_old_version(THD *thd, const char *db, const char *table_name,
                             ulong wait_timeout, uint deadlock_weight)
{
  TABLE_SHARE *share;
  int res= FALSE;

  if ((share= tdc_lock_share(db, table_name)))
  {
    if (share->has_old_version())
    {
      struct timespec abstime;
      set_timespec(abstime, wait_timeout);
      res= share->wait_for_old_version(thd, &abstime, deadlock_weight);
    }
    else
      tdc_unlock_share(share);
  }
  return res;
}


ulong tdc_refresh_version(void)
{
  my_atomic_rwlock_rdlock(&LOCK_tdc_atomics);
  ulong v= my_atomic_load64(&tdc_version);
  my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);
  return v;
}


void tdc_increment_refresh_version(void)
{
  my_atomic_rwlock_wrlock(&LOCK_tdc_atomics);
#ifndef DBUG_OFF
  ulong v= my_atomic_add64(&tdc_version, 1);
#else
  my_atomic_add64(&tdc_version, 1);
#endif
  my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);
  DBUG_PRINT("tcache", ("incremented global refresh_version to: %lu", v));
}


/**
  Initialize table definition cache iterator.
*/

void TDC_iterator::init(void)
{
  DBUG_ENTER("TDC_iterator::init");
  idx= 0;
  mysql_rwlock_rdlock(&LOCK_tdc);
  DBUG_VOID_RETURN;
}


/**
  Deinitialize table definition cache iterator.
*/

void TDC_iterator::deinit(void)
{
  DBUG_ENTER("TDC_iterator::deinit");
  mysql_rwlock_unlock(&LOCK_tdc);
  DBUG_VOID_RETURN;
}


/**
  Get next TABLE_SHARE object from table definition cache.

  Object is protected against removal from table definition cache.

  @note Returned TABLE_SHARE is not guaranteed to be fully initialized:
  tdc_acquire_share() added new share, but didn't open it yet. If caller
  needs fully initializer share, it must lock table share mutex.
*/

TABLE_SHARE *TDC_iterator::next(void)
{
  TABLE_SHARE *share= 0;
  DBUG_ENTER("TDC_iterator::next");
  if (idx < tdc_hash.records)
  {
    share= (TABLE_SHARE*) my_hash_element(&tdc_hash, idx);
    idx++;
  }
  DBUG_RETURN(share);
}


/*
  Function to assign a new table map id to a table share.

  PARAMETERS

    share - Pointer to table share structure

  DESCRIPTION

    We are intentionally not checking that share->mutex is locked
    since this function should only be called when opening a table
    share and before it is entered into the table definition cache
    (meaning that it cannot be fetched by another thread, even
    accidentally).

  PRE-CONDITION(S)

    share is non-NULL
    last_table_id_lock initialized (tdc_inited)

  POST-CONDITION(S)

    share->table_map_id is given a value that with a high certainty is
    not used by any other table (the only case where a table id can be
    reused is on wrap-around, which means more than 4 billion table
    share opens have been executed while one table was open all the
    time).

    share->table_map_id is not ~0UL.
*/

void tdc_assign_new_table_id(TABLE_SHARE *share)
{
  ulong tid;
  DBUG_ENTER("assign_new_table_id");
  DBUG_ASSERT(share);
  DBUG_ASSERT(tdc_inited);

  /*
    There is one reserved number that cannot be used.  Remember to
    change this when 6-byte global table id's are introduced.
  */
  do
  {
    my_atomic_rwlock_wrlock(&LOCK_tdc_atomics);
    tid= my_atomic_add64(&last_table_id, 1);
    my_atomic_rwlock_wrunlock(&LOCK_tdc_atomics);
  } while (unlikely(tid == ~0UL));

  share->table_map_id= tid;
  DBUG_PRINT("info", ("table_id= %lu", share->table_map_id));
  DBUG_VOID_RETURN;
}
