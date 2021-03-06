#ifndef RPL_PARALLEL_H
#define RPL_PARALLEL_H

#include "log_event.h"


struct rpl_parallel;
struct rpl_parallel_entry;
struct rpl_parallel_thread_pool;

class Relay_log_info;
struct rpl_parallel_thread {
  bool delay_start;
  bool running;
  bool stop;
  mysql_mutex_t LOCK_rpl_thread;
  mysql_cond_t COND_rpl_thread;
  struct rpl_parallel_thread *next;             /* For free list. */
  struct rpl_parallel_thread_pool *pool;
  THD *thd;
  struct rpl_parallel_entry *current_entry;
  struct queued_event {
    queued_event *next;
    Log_event *ev;
    rpl_group_info *rgi;
    ulonglong future_event_relay_log_pos;
    char event_relay_log_name[FN_REFLEN];
    char future_event_master_log_name[FN_REFLEN];
    ulonglong event_relay_log_pos;
    my_off_t future_event_master_log_pos;
    size_t event_size;
  } *event_queue, *last_in_queue;
  uint64 queued_size;

  void enqueue(queued_event *qev)
  {
    if (last_in_queue)
      last_in_queue->next= qev;
    else
      event_queue= qev;
    last_in_queue= qev;
    queued_size+= qev->event_size;
  }

  void dequeue(queued_event *list)
  {
    queued_event *tmp;

    DBUG_ASSERT(list == event_queue);
    event_queue= last_in_queue= NULL;
    for (tmp= list; tmp; tmp= tmp->next)
      queued_size-= tmp->event_size;
  }
};


struct rpl_parallel_thread_pool {
  uint32 count;
  struct rpl_parallel_thread **threads;
  struct rpl_parallel_thread *free_list;
  mysql_mutex_t LOCK_rpl_thread_pool;
  mysql_cond_t COND_rpl_thread_pool;
  bool changing;
  bool inited;

  rpl_parallel_thread_pool();
  int init(uint32 size);
  void destroy();
  struct rpl_parallel_thread *get_thread(rpl_parallel_entry *entry);
};


struct rpl_parallel_entry {
  uint32 domain_id;
  uint32 last_server_id;
  uint64 last_seq_no;
  uint64 last_commit_id;
  bool active;
  /*
    Set when SQL thread is shutting down, and no more events can be processed,
    so worker threads must force abort any current transactions without
    waiting for event groups to complete.
  */
  bool force_abort;

  rpl_parallel_thread *rpl_thread;
  /*
    The sub_id of the last transaction to commit within this domain_id.
    Must be accessed under LOCK_parallel_entry protection.
  */
  uint64 last_committed_sub_id;
  mysql_mutex_t LOCK_parallel_entry;
  mysql_cond_t COND_parallel_entry;
  /*
    The sub_id of the last event group in this replication domain that was
    queued for execution by a worker thread.
  */
  uint64 current_sub_id;
  rpl_group_info *current_group_info;
  /*
    The sub_id of the last event group in the previous batch of group-committed
    transactions.

    When we spawn parallel worker threads for the next group-committed batch,
    they first need to wait for this sub_id to be committed before it is safe
    to start executing them.
  */
  uint64 prev_groupcommit_sub_id;
};
struct rpl_parallel {
  HASH domain_hash;
  rpl_parallel_entry *current;
  bool sql_thread_stopping;

  rpl_parallel();
  ~rpl_parallel();
  void reset();
  rpl_parallel_entry *find(uint32 domain_id);
  void wait_for_done();
  bool do_event(rpl_group_info *serial_rgi, Log_event *ev,
                ulonglong event_size);
};


extern struct rpl_parallel_thread_pool global_rpl_thread_pool;


extern int rpl_parallel_change_thread_count(rpl_parallel_thread_pool *pool,
                                            uint32 new_count,
                                            bool skip_check= false);

#endif  /* RPL_PARALLEL_H */
