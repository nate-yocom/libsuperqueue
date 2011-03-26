#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // sleep()
#include <math.h>
#include <time.h>
#include <string.h>
#include <openssl/rsa.h>

/**
 * Sample/test app for libsuperqueue.. mostly just hackin space for non-lib components
 * until things are flushed out enough to go into a proper app/examples dir etc.
 **/

#include "superqueue.h"

// A job that just does work, output is irrelevant
void sample_job_forgettable(lsq_job_args *args)
{
  int x = 0;
  float f = 3.f;

  // Same fake work so we take some time
  for(x = 0; x < 1000000; ++x)
    f = f + sin(x) + (rand() % 91233);
}

// A job that does work, with output we care about (pub/priv key)
void sample_job_outputs_stuff(lsq_job_args *args)
{
  unsigned char * output = (unsigned char *) args->output_buffer.memory;
  int encoded_len = 0;
  RSA * rsa_key = RSA_generate_key(2048, 65537, NULL, NULL);
 
  memset(args->output_buffer.memory, 0, args->output_buffer.size);
  if(rsa_key)
  {
    // Make sure it'll fit...
    encoded_len = i2d_RSAPublicKey(rsa_key, NULL);
    //encoded_len += i2d_RSAPrivateKey(rsa_key, NULL);

    memcpy(output, &encoded_len, 4);
    output += 4;

    if(encoded_len <= args->output_buffer.size)
    {
      i2d_RSAPublicKey(rsa_key, &output);
      //i2d_RSAPrivateKey(rsa_key, &output);
    }
  }
}

void output_encoded_key(void *memory)
{
  int len = 0, x = 0;
  unsigned char * output = ((unsigned char *) memory) + 4;
  memcpy(&len, memory, 4); 
  printf("Key[%d]: ", len);
  for(x = 0; x < len; ++x)
    printf("%02x", output[x]);
  printf("\n");
}

#define NUM_TRACKED_JOBS    256 
#define NUM_UNTRACKED_JOBS  512
#define NUM_GROUPS          4
#define DEFAULT_QUEUE_WIDTH 4

int main(int argc, char ** argv)
{
  lsq_job_handle tracked_handles[NUM_TRACKED_JOBS];
  uint8_t        groups[NUM_GROUPS];
  lsq_buffer     results;
  int x = 0;

  srand(time(NULL));

  lsq_init_args  init_args = lsq_default_init_args();
  init_args.queue_width = DEFAULT_QUEUE_WIDTH;

  // If we were given an arg, and its a number.. use it as our queue width
  if(argc > 1 && argv[1])
  {
    init_args.queue_width = atoi(argv[1]);
    if(init_args.queue_width <= 0)
      init_args.queue_width = DEFAULT_QUEUE_WIDTH;
  }

  lsq_initialize(init_args);

  for(x = 0; x < NUM_GROUPS; ++x)
  {
    groups[x] = (rand() % 255);
  }

  for(x = 0; x < NUM_UNTRACKED_JOBS / 2; ++x)
  {
    // Untracked jobs don't use output...
    lsq_queue_job(sample_job_forgettable, NULL, 0, NULL, 0, NULL, groups[(rand() % NUM_GROUPS)]);
  }

  for(x = 0; x < NUM_TRACKED_JOBS; ++x)
  {
    char * outputbuff = malloc(2048);
    lsq_queue_job(sample_job_outputs_stuff, NULL, 0, outputbuff, 2048, &tracked_handles[x], groups[(rand() % NUM_GROUPS)]);
  }

  for(x = 0; x < NUM_UNTRACKED_JOBS / 2; ++x)
  {
    // Untracked jobs don't use output...
    lsq_queue_job(sample_job_forgettable, NULL, 0, NULL, 0, NULL, groups[(rand() % NUM_GROUPS)]);
  }

  // Wait for all jobs to be done, one group at a time
  for(x = 0; x < NUM_GROUPS; ++x)
  {
    lsq_group_sync(groups[x]);    
  }

  // Output all tracked jobs, and free up their output buffers
  for(x = 0; x < NUM_TRACKED_JOBS; ++x)
  {
    results = lsq_get_job_output(tracked_handles[x]);
    // Uncomment if you want to see that the job really worked...
    //output_encoded_key(results.memory);
    lsq_free_job(tracked_handles[x]);
    tracked_handles[x] = NULL;
    free(results.memory);
  }

  // Shut it down...
  lsq_shutdown();
  return 0;
}
