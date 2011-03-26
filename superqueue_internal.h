#ifndef __LIBSUPERQUEUE_H_INTERNAL_H__
#define __LIBSUPERQUEUE_H_INTERNAL_H__

#include <stdio.h>
#include <pthread.h>

#include "superqueue.h"

#define LSQ_MAX_THREADPOOL_SIZE   32
#define LSQ_DEFAULT_STACKSIZE     1024 * 4 * 256  // 256 4k pages (1 MB)
#define LSQ_QUEUE_POLL_MSECS      10              // 100 times a second
#define LSQ_DEFAULT_SCRATCH_SIZE  1024 * 64       // 64k (CAUTION: THIS IS ON THE THREAD'S STACK!)
#define LSQ_MAX_GROUP_COUNT       256

#define LSQ_MAX_JOBS              2048            // Maximum # of jobs in any state
#define LSQ_MAX_JOBS_PER_QUEUE    LSQ_MAX_JOBS    // Maximum # of jobs in a single queue

#ifndef __THREAD__
#define __THREAD__ pthread_self()
#endif

#define LSQ_LOCSTR  "[%s:%d:%p:%s]: "
#define LSQ_LOCARG  __FILE__, __LINE__, __THREAD__, __FUNCTION__

#define LSQ_OUT(format, ...) \
  { \
    if(g_lsq_args.output_func) \
      g_lsq_args.output_func(LSQ_LOCSTR format, LSQ_LOCARG, __VA_ARGS__); \
    else \
      fprintf(stdout, LSQ_LOCSTR format, LSQ_LOCARG, __VA_ARGS__); \
  }

#if LSQ_DO_VERBOSE_OUTPUT
#define LSQ_VERBOSE_OUT(format, ...) LSQ_OUT(format, __VA_ARGS__)
#else
#define LSQ_VERBOSE_OUT(format, ...)
#endif


/** Internal structures **/
typedef enum _lsq_thread_state
{
  kRunning,
  kStopped,
} lsq_thread_state;

typedef enum _lsq_job_state
{
  kFree,
  kWaiting,
  kWorking,
  kFinished,
} lsq_job_state;

typedef struct _lsq_worker_thread_info
{
  pthread_t         id;
  lsq_thread_state  state;
  pthread_attr_t    attr;
} lsq_worker_thread_info;

typedef struct _lsq_job
{
  lsq_job_func    main;
  lsq_job_args    args;
  lsq_job_state   state;
  uint8_t         group;
  lsq_bool        autofree;
} lsq_job;

typedef struct _lsq_job_queue_ent
{
  lsq_job *                   job;
  struct _lsq_job_queue_ent * next; 
  struct _lsq_job_queue_ent * prev;
} lsq_job_queue_ent;

typedef struct _lsq_job_queue
{
  lsq_job_queue_ent * head;
  lsq_job_queue_ent * tail;
  size_t              size;
  pthread_mutex_t     mutex;
  pthread_cond_t      signal;
  lsq_job_queue_ent   entries[LSQ_MAX_JOBS_PER_QUEUE];
} lsq_job_queue;

typedef struct _lsq_job_group
{
  uint8_t           number;
  uint32_t          running_count;  
  pthread_mutex_t   mutex;
  pthread_cond_t    signal;
} lsq_job_group;

/** Internal functions **/

void _lsq_dump_state(void);
void _lsq_init_thread_info(lsq_worker_thread_info *);
void _lsq_start_thread(lsq_worker_thread_info *);
void _lsq_stop_thread(lsq_worker_thread_info *);

void * _lsq_worker_thread_main(void *);

lsq_job * _lsq_get_new_job(void);

/** Group management **/
void _lsq_group_init(lsq_job_group *, uint8_t);
void _lsq_group_destroy(lsq_job_group *);
void _lsq_group_lock(lsq_job_group *);
void _lsq_group_unlock(lsq_job_group *);
void _lsq_group_incr(lsq_job_group *);
void _lsq_group_decr(lsq_job_group *);
void _lsq_group_sync(lsq_job_group *);

/** linked list (queue) management **/
void _lsq_queue_init(lsq_job_queue *queue);
void _lsq_queue_destroy(lsq_job_queue *queue);
void _lsq_queue_add_tail(lsq_job_queue *queue, lsq_job *job);
void _lsq_queue_add_head(lsq_job_queue *queue, lsq_job *job);
lsq_job * _lsq_queue_pop_tail(lsq_job_queue *queue);
lsq_job * _lsq_queue_pop_head(lsq_job_queue *queue);
lsq_job_queue_ent * _lsq_find_open_queue_entry(lsq_job_queue * queue);
lsq_job_queue_ent * _lsq_new_queue_entry(lsq_job_queue * queue, lsq_job *job);
void _lsq_free_queue_entry(lsq_job_queue_ent * entry);
void _lsq_queue_remove_entry(lsq_job_queue *queue, lsq_job *job);
void _lsq_lock_queue(lsq_job_queue *queue);
void _lsq_unlock_queue(lsq_job_queue *queue);
void _lsq_queue_wait(lsq_job_queue *queue);
void _lsq_queue_signal_all(lsq_job_queue *queue);
void _lsq_queue_signal_one(lsq_job_queue *queue);



#endif
