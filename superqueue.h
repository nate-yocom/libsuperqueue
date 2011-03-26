#ifndef __LIBSUPERQUEUE_H_INCLUDED__
#define __LIBSUPERQUEUE_H_INCLUDED__

#ifdef __cplusplus
extern "C" 
{
#endif

#include <inttypes.h> // uint*_t types

#define LSQ_VERSION_STRING    "0.0.1"
#define LSQ_DO_VERBOSE_OUTPUT 0

typedef uint8_t   lsq_bool;
#define lsq_true  1
#define lsq_false 0

typedef enum _lsq_result_code
{
  kError = -1,
  kOK    = 0,
} lsq_result_code;

/** lsq_buffer's wrap the common (void *, size_t) pair **/
typedef struct _lsq_buffer
{
  void * memory;
  size_t size;
} lsq_buffer;

/** All jobs get a lsq_job_args struct as their only argument 
    when exectured.  Any or all of these buffers may be null! **/
typedef struct _lsq_job_args
{
  lsq_buffer input_buffer;
  lsq_buffer scratch_buffer;
  lsq_buffer output_buffer;
} lsq_job_args;

/** Opaque handle to a lsq job **/
typedef void * lsq_job_handle;

/** App supplied function types **/
typedef void   (*lsq_output_func)(const char *format, ...);   // LSQ writes debug output here...
typedef void   (*lsq_job_func)(lsq_job_args *);               // Actual worker function type

/** Initialize LSQ by calling lsq_initialize() with one of these **/
typedef struct _lsq_init_args
{
  uint8_t             queue_width;              // # of threads in the threadpool
  size_t              thread_stack_bytes;       // Stacksize of each thread
  size_t              thread_workbuffer_bytes;  // Scratch buffer provided to each job (per-thread)
  lsq_output_func     output_func;              // printf used if not provided
} lsq_init_args;

/** Get a default set of args **/
lsq_init_args   lsq_default_init_args(void);

/** Initialize LSQ! **/
lsq_result_code lsq_initialize(lsq_init_args init_args);

/** 
  Queueing jobs: 
    lsq_job_func - function that will be called on worker thread
    ibuffer/ibuffer_size - the input buffer and size thereof provided to the job_func in an lsq_job_args structure (0/0 acceptable)
    obuffer/obuffer_size - the output buffer for same (0/0 acceptable)
    job_handle - ** for storing the new job's handle.  If passed NULL, the job will *not* be kept after 
                 the job has been executed.  Instead it will be cleaned up and freed immediately.  If you want output,
                 pass a handle pointer!
    group_number - default group is 0, 0->255 inclusive are valid.  You sync on groups, so group jobs you care to sync together
**/
lsq_result_code lsq_queue_job(lsq_job_func, void * ibuffer, size_t ibuffer_size,
                                            void * obuffer, size_t obuffer_size,
                                            lsq_job_handle *job_handle, uint8_t group_number);

/** Job syncing, groups only - blocks until the pending job count in the group == 0 **/
void  lsq_group_sync(uint8_t group_number);

/** Queue and Job status, results, and destruction */
size_t          lsq_queued_job_count(void);         // How many jobs in the pending queue?
lsq_bool        lsq_job_complete(lsq_job_handle);   // Fast (just a flag check)
lsq_buffer      lsq_get_job_input(lsq_job_handle);  // Get a pointer to the input buffer the job used
lsq_buffer      lsq_get_job_output(lsq_job_handle); // Get a pointer to the output buffer the job wrote to 
lsq_result_code lsq_free_job(lsq_job_handle);  // Fails if job is not complete yet (for now)

/** Shutdown **/
lsq_result_code lsq_shutdown(void);


#ifdef __cplusplus
}
#endif
#endif // __LIBSUPERQUEUE_H_INCLUDED__
