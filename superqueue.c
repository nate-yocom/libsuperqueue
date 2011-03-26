#include <string.h>     // memcpy
#include <sys/time.h>   // gettimeofday

#include "superqueue.h"
#include "superqueue_internal.h"

static lsq_init_args          g_lsq_args = { 0 };
static lsq_worker_thread_info g_lsq_workers[LSQ_MAX_THREADPOOL_SIZE];
static lsq_job_queue          g_lsq_ready_queue;   
static lsq_job_group          g_lsq_groups[LSQ_MAX_GROUP_COUNT];
static lsq_job                g_lsq_jobs[LSQ_MAX_JOBS];
static pthread_mutex_t        g_lsq_jobs_mutex = PTHREAD_MUTEX_INITIALIZER;

lsq_init_args lsq_default_init_args(void)
{
  lsq_init_args args = { 0 };
  args.queue_width = LSQ_MAX_THREADPOOL_SIZE;
  args.thread_stack_bytes = LSQ_DEFAULT_STACKSIZE;
  args.thread_workbuffer_bytes = LSQ_DEFAULT_SCRATCH_SIZE;
  return args;
}

lsq_result_code lsq_initialize(lsq_init_args init_args)
{
  int x = 0;

  // Copy our args
  memcpy(&g_lsq_args, &init_args, sizeof(lsq_init_args));

  // Fixup any overage
  if(g_lsq_args.queue_width > LSQ_MAX_THREADPOOL_SIZE)
    g_lsq_args.queue_width = LSQ_MAX_THREADPOOL_SIZE;
  
  // Init our groups
  for(x = 0; x < LSQ_MAX_GROUP_COUNT; ++x)
  {
    _lsq_group_init(&g_lsq_groups[x], x);  
  }

  // Init our queues
  _lsq_queue_init(&g_lsq_ready_queue);

  // Set all our jobs to free to start
  for(x = 0; x < LSQ_MAX_JOBS; ++x)
  {
    g_lsq_jobs[x].state = kFree;
  }

  // Dump our starting state/mem usage etc...
  _lsq_dump_state();

  // Initialize thread attributes for each thread, and start'em up
  for(x = 0; x < LSQ_MAX_THREADPOOL_SIZE; ++x)
  {
    lsq_worker_thread_info * info = &g_lsq_workers[x]; 
    _lsq_init_thread_info(info);
    if(x < g_lsq_args.queue_width)
    {
      _lsq_start_thread(info);
     }
  }

  return kOK;
}

lsq_result_code lsq_shutdown()
{
  int x = 0;

  for(x = 0; x < g_lsq_args.queue_width; ++x)
  {
    lsq_worker_thread_info * info = &g_lsq_workers[x];
    _lsq_stop_thread(info);
  }

  _lsq_queue_destroy(&g_lsq_ready_queue);

  // Cleanup our groups
  for(x = 0; x < LSQ_MAX_GROUP_COUNT; ++x)
  {
    _lsq_group_destroy(&g_lsq_groups[x]);
  }

  return kOK;
}

void _lsq_init_thread_info(lsq_worker_thread_info * info)
{
  info->id = 0;
  info->state = kRunning;
  pthread_attr_init(&info->attr);
  pthread_attr_setdetachstate(&info->attr, PTHREAD_CREATE_JOINABLE);
  pthread_attr_setstacksize(&info->attr, g_lsq_args.thread_stack_bytes);
}

void _lsq_dump_state(void)
{
  size_t total_mem = 0;
  time_t curr_time = time(NULL);

  LSQ_OUT("LSQ %s: \n", LSQ_VERSION_STRING);

  LSQ_OUT(" Initialized on %s with:\n", ctime(&curr_time));
  LSQ_OUT("  lsq_init_args.queue_width              = %d\n",  g_lsq_args.queue_width);
  LSQ_OUT("  lsq_init_args.thread_stack_bytes       = %ld\n", g_lsq_args.thread_stack_bytes);
  LSQ_OUT("  lsq_init_args.thread_workbuffer_bytes  = %ld\n", g_lsq_args.thread_workbuffer_bytes);
  LSQ_OUT("  lsq_init_args.output_func              = %p\n",  g_lsq_args.output_func);

  LSQ_OUT(" Compiled on %s %s with:\n", __DATE__, __TIME__);
  LSQ_OUT("  LSQ_DO_VERBOSE_OUTPUT      = %d\n", LSQ_DO_VERBOSE_OUTPUT);
  LSQ_OUT("  LSQ_MAX_THREADPOOL_SIZE    = %d\n", LSQ_MAX_THREADPOOL_SIZE);
  LSQ_OUT("  LSQ_DEFAULT_STACKSIZE      = %d\n", LSQ_DEFAULT_STACKSIZE);
  LSQ_OUT("  LSQ_QUEUE_POLL_MSECS       = %d\n", LSQ_QUEUE_POLL_MSECS);
  LSQ_OUT("  LSQ_DEFAULT_SCRATCH_SIZE   = %d\n", LSQ_DEFAULT_SCRATCH_SIZE);
  LSQ_OUT("  LSQ_MAX_GROUP_COUNT        = %d\n", LSQ_MAX_GROUP_COUNT);
  LSQ_OUT("  LSQ_MAX_JOBS               = %d\n", LSQ_MAX_JOBS);
  LSQ_OUT("  LSQ_MAX_JOBS_PER_QUEUE     = %d\n", LSQ_MAX_JOBS_PER_QUEUE);

  total_mem += sizeof(g_lsq_args);
  total_mem += sizeof(g_lsq_workers);
  total_mem += sizeof(g_lsq_ready_queue);
  total_mem += sizeof(g_lsq_groups);
  total_mem += sizeof(g_lsq_jobs);
  total_mem += sizeof(g_lsq_jobs_mutex);

  LSQ_OUT(" Total LSQ memory usage:      = %ld (bytes)\n", total_mem);
}

void _lsq_start_thread(lsq_worker_thread_info * info)
{
  int retval = pthread_create(&info->id, &info->attr, _lsq_worker_thread_main, (void *) info);
  if(retval != 0)
  {
    LSQ_OUT("pthread_create failed: %d\n", retval);
  }
}

void _lsq_stop_thread(lsq_worker_thread_info * info)
{
  info->state = kStopped;
  _lsq_queue_signal_all(&g_lsq_ready_queue);  // Wake all threads
  int retval = pthread_join(info->id, NULL);
  if(retval != 0)
  {
    LSQ_OUT("pthread_join failed: %d\n", retval);
  }

  pthread_attr_destroy(&info->attr);
}

void * _lsq_worker_thread_main(void * tinfo)
{
  unsigned char * thread_scratch_buffer[g_lsq_args.thread_workbuffer_bytes];
  size_t          thread_scratch_buffer_size = g_lsq_args.thread_workbuffer_bytes;

  lsq_worker_thread_info * info = (lsq_worker_thread_info *) tinfo;
  LSQ_VERBOSE_OUT("Worker thread started, info at: %p State: %d\n", info, info->state);
  
  while(info->state == kRunning)
  {
    lsq_job * job = _lsq_queue_pop_tail(&g_lsq_ready_queue);
    if(job)
    {
      LSQ_VERBOSE_OUT("Worker thread taking job: %p function: %p group: %d\n", job, job->main, job->group);
      job->state = kWorking;
      job->args.scratch_buffer.memory = thread_scratch_buffer;
      job->args.scratch_buffer.size   = thread_scratch_buffer_size;
      job->main(&job->args);
      job->args.scratch_buffer.memory = NULL;
      job->args.scratch_buffer.size   = 0;
      job->state = kFinished;
      _lsq_group_decr(&g_lsq_groups[job->group]);

      // Autofree jobs get cleaned up immediately and in context, TBD: batch'em?
      if(job->autofree == lsq_true)
      {
        lsq_free_job(job);
      }

      LSQ_VERBOSE_OUT("Worker thread job done: %p function: %p group: %d\n", job, job->main, job->group);
    }
    else
    {
      _lsq_queue_wait(&g_lsq_ready_queue);
    }
  }

  LSQ_VERBOSE_OUT("Worker thread exiting, state: %d\n", info->state); 
  pthread_exit(NULL);
}

lsq_job * _lsq_get_new_job(void)
{
  int x = 0;
  lsq_job * new_job = 0;

  // Find first free job
  pthread_mutex_lock(&g_lsq_jobs_mutex);
  for(x = 0; x < LSQ_MAX_JOBS && new_job == 0; ++x)
  {
    if(g_lsq_jobs[x].state == kFree)
    {
      new_job = &g_lsq_jobs[x];
      new_job->state = kWaiting;
    }
  }
  pthread_mutex_unlock(&g_lsq_jobs_mutex);

  return new_job;
}

lsq_result_code lsq_queue_job(lsq_job_func main, void * ibuffer, size_t ibuffer_size,
                                                 void * obuffer, size_t obuffer_size,
                                                 lsq_job_handle *job_handle, uint8_t group_number)
{
  lsq_job * new_job = _lsq_get_new_job();
  
  if(new_job == 0)
  {
    LSQ_OUT("Unable to find a free job for %p\n", main);
    return kError;
  }

  new_job->main = main;
  new_job->group = group_number;
  new_job->args.scratch_buffer.memory = NULL;
  new_job->args.scratch_buffer.size   = 0;
  new_job->args.input_buffer.memory   = ibuffer;
  new_job->args.input_buffer.size     = ibuffer_size;
  new_job->args.output_buffer.memory  = obuffer;
  new_job->args.output_buffer.size    = obuffer_size;
  new_job->autofree = lsq_false;

  // No handle means caller doesn't care, we'll clean this guy up when he finishes
  if(job_handle == NULL)
    new_job->autofree = lsq_true;
  else
    *job_handle = (void *) new_job;

  _lsq_group_incr(&g_lsq_groups[group_number]);

  _lsq_queue_add_head(&g_lsq_ready_queue, new_job);
  _lsq_queue_signal_one(&g_lsq_ready_queue);
  return kOK;
}

lsq_bool   lsq_job_complete(lsq_job_handle jobh)
{
  lsq_job * job = (lsq_job *)jobh;
  if(job->state == kFinished)
    return lsq_true;

  return lsq_false;
}

lsq_buffer lsq_get_job_input(lsq_job_handle jobh)
{
  lsq_job * job = (lsq_job *)jobh;
  return job->args.input_buffer;
}

lsq_buffer lsq_get_job_output(lsq_job_handle jobh)
{
  lsq_job * job = (lsq_job *)jobh;
  return job->args.output_buffer;
}

lsq_result_code lsq_free_job(lsq_job_handle jobh)
{
  lsq_job * job = (lsq_job *)jobh;
  if(lsq_job_complete(jobh) == lsq_false)
    return kError;

  job->state = kFree;
  return kOK;
}

size_t lsq_queued_job_count(void)
{
  return g_lsq_ready_queue.size;
}

void _lsq_group_init(lsq_job_group *group, uint8_t number)
{
  group->number = number;
  group->running_count = 0;
  pthread_mutex_init(&group->mutex, NULL);
  pthread_cond_init(&group->signal, NULL);
}

void _lsq_group_destroy(lsq_job_group *group)
{
  group->running_count = 0;
  pthread_mutex_destroy(&group->mutex);
  pthread_cond_destroy(&group->signal);
}

void _lsq_group_lock(lsq_job_group *group)
{
  pthread_mutex_lock(&group->mutex);
}

void _lsq_group_unlock(lsq_job_group *group)
{
  pthread_mutex_unlock(&group->mutex);
}

void _lsq_group_incr(lsq_job_group *group)
{
  _lsq_group_lock(group);
  LSQ_VERBOSE_OUT("Group %d count %d->%d\n", group->number, group->running_count, 
                                             group->running_count + 1);
  group->running_count++;
  pthread_cond_broadcast(&group->signal);
  _lsq_group_unlock(group);
}

void _lsq_group_decr(lsq_job_group *group)
{
  _lsq_group_lock(group);
  LSQ_VERBOSE_OUT("Group %d count %d->%d\n", group->number, group->running_count, 
                                             group->running_count - 1);
  group->running_count--;
  pthread_cond_broadcast(&group->signal);
  _lsq_group_unlock(group);
}

void lsq_group_sync(uint8_t group_number)
{
  _lsq_group_sync(&g_lsq_groups[group_number]);
}

void _lsq_group_sync(lsq_job_group *group)
{
  struct timeval tv;
  struct timespec ts;

  while(group->running_count > 0)
  {
    _lsq_group_lock(group);

    gettimeofday(&tv, NULL);
    tv.tv_usec += LSQ_QUEUE_POLL_MSECS * 1000;  // Delay in Milliseconds converted to Microseconds
    ts.tv_sec  = tv.tv_sec;
    ts.tv_nsec = tv.tv_usec * 1000;
    pthread_cond_timedwait(&group->signal, &group->mutex, &ts);

    _lsq_group_unlock(group);
  }
}

/** Queue (linked list) management **/
void _lsq_queue_init(lsq_job_queue *queue)
{
  int x = 0;

  queue->head = NULL;
  queue->tail = NULL;
  queue->size = 0;
  pthread_mutex_init(&queue->mutex, NULL);
  pthread_cond_init(&queue->signal, NULL);

  for(x = 0; x < LSQ_MAX_JOBS_PER_QUEUE; ++x)
  {
    queue->entries[x].next = NULL;
    queue->entries[x].prev = NULL;
    queue->entries[x].job = NULL;
  }
}

void _lsq_queue_destroy(lsq_job_queue *queue)
{
  while(_lsq_queue_pop_head(queue) != NULL);
  pthread_mutex_destroy(&queue->mutex);
  pthread_cond_destroy(&queue->signal);
}

void _lsq_queue_add_tail(lsq_job_queue *queue, lsq_job *job)
{
  _lsq_lock_queue(queue);

  lsq_job_queue_ent * new_ent = _lsq_new_queue_entry(queue, job);

  // First entry?
  if(queue->head == NULL && queue->tail == NULL)
  {
    queue->head = new_ent;
    queue->tail = new_ent;
  }
  else
  {
    queue->tail->next = new_ent;
    new_ent->prev = queue->tail;
    queue->tail = new_ent;
  }

  queue->size++;

  _lsq_unlock_queue(queue);
}

void _lsq_queue_add_head(lsq_job_queue *queue, lsq_job *job)
{
  _lsq_lock_queue(queue);

  lsq_job_queue_ent * new_ent = _lsq_new_queue_entry(queue, job);
  
  // First entry?
  if(queue->head == NULL && queue->tail == NULL)
  {
    queue->head = new_ent;
    queue->tail = new_ent;
  }
  else
  {
    queue->head->prev = new_ent;
    new_ent->next = queue->head;
    queue->head = new_ent;
  }
  
  queue->size++;

  _lsq_unlock_queue(queue);
}

lsq_job * _lsq_queue_pop_tail(lsq_job_queue *queue)
{
  _lsq_lock_queue(queue);

  if(queue->tail == NULL)
  {
    _lsq_unlock_queue(queue);
    return NULL;
  }

  lsq_job * job = queue->tail->job;

  // Only one entry?
  if(queue->tail == queue->head && queue->tail)
  {
    _lsq_free_queue_entry(queue->tail);
    queue->tail = NULL;
    queue->head = NULL;
  }
  else if(queue->tail)
  {
    lsq_job_queue_ent * old_tail = queue->tail;
    if(queue->tail->prev)
      queue->tail->prev->next = NULL;
    queue->tail = queue->tail->prev;
    _lsq_free_queue_entry(old_tail);
  }

  queue->size--;

  _lsq_unlock_queue(queue);
  return job;
}

lsq_job * _lsq_queue_pop_head(lsq_job_queue *queue)
{
  _lsq_lock_queue(queue);

  if(queue->head == NULL)
  {
    _lsq_unlock_queue(queue);
    return NULL;
  }

  lsq_job * job = queue->head->job;

  // Only one entry
  if(queue->head == queue->tail && queue->head)
  {
    _lsq_free_queue_entry(queue->head);
    queue->head = NULL;
    queue->tail = NULL;
  }
  else if(queue->head)
  {
    lsq_job_queue_ent * old_head = queue->head;
    if(queue->head->next)
      queue->head->next->prev = NULL;
    queue->head = queue->head->next;
    _lsq_free_queue_entry(old_head);
  }

  queue->size--;

  _lsq_unlock_queue(queue);
  return job;
}

lsq_job_queue_ent * _lsq_find_open_queue_entry(lsq_job_queue * queue)
{
  int x = 0;

  // No need to lock, anyone calling this should have already done so!
  for(x = 0; x < LSQ_MAX_JOBS_PER_QUEUE; ++x)
  {
    if(queue->entries[x].job == NULL)
    {
      return &queue->entries[x]; 
    }
  }

  return NULL;
}

lsq_job_queue_ent * _lsq_new_queue_entry(lsq_job_queue * queue, lsq_job * job)
{
  lsq_job_queue_ent * new_ent = _lsq_find_open_queue_entry(queue);
  if(!new_ent) 
  {
    LSQ_OUT("Unable to find room for a new queue entry in %p for %p", queue, job);
    return NULL;
  }

  new_ent->next = NULL;
  new_ent->prev = NULL;
  new_ent->job = job;
  return new_ent;
}

void _lsq_free_queue_entry(lsq_job_queue_ent * entry)
{
  entry->next = NULL;
  entry->prev = NULL;
  entry->job = NULL;
}

void _lsq_lock_queue(lsq_job_queue *queue)
{
  pthread_mutex_lock(&queue->mutex);
}

void _lsq_unlock_queue(lsq_job_queue *queue)
{
  pthread_mutex_unlock(&queue->mutex);
}

void _lsq_queue_wait(lsq_job_queue *queue)
{
  struct timeval tv;
  struct timespec ts;
  gettimeofday(&tv, NULL);

  tv.tv_usec += LSQ_QUEUE_POLL_MSECS * 1000;  // Delay in Milliseconds converted to Microseconds
  ts.tv_sec  = tv.tv_sec;
  ts.tv_nsec = tv.tv_usec * 1000;
 
  pthread_mutex_lock(&queue->mutex);
  pthread_cond_timedwait(&queue->signal, &queue->mutex, &ts);
  pthread_mutex_unlock(&queue->mutex);
}

void _lsq_queue_signal_all(lsq_job_queue *queue)
{
  _lsq_lock_queue(queue);
  pthread_cond_broadcast(&queue->signal);
  _lsq_unlock_queue(queue);
}

void _lsq_queue_signal_one(lsq_job_queue *queue)
{
  _lsq_lock_queue(queue);
  pthread_cond_signal(&queue->signal);
  _lsq_unlock_queue(queue);
}

