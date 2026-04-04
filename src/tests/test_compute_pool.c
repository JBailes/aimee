#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "compute_pool.h"

/* --- Test helpers --- */

static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t counter_cond = PTHREAD_COND_INITIALIZER;
static int counter = 0;

typedef struct
{
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   int ready;
   int release;
} gate_t;

static void increment_counter(void *arg)
{
   int *val = (int *)arg;
   pthread_mutex_lock(&counter_mutex);
   counter += *val;
   pthread_cond_broadcast(&counter_cond);
   pthread_mutex_unlock(&counter_mutex);
}

static void set_flag(void *arg)
{
   int *flag = (int *)arg;
   *flag = 1;
}

static void sleep_task(void *arg)
{
   int *ms = (int *)arg;
   usleep((unsigned)(*ms) * 1000);
}

static void gated_task(void *arg)
{
   gate_t *gate = (gate_t *)arg;
   pthread_mutex_lock(&gate->mutex);
   gate->ready++;
   pthread_cond_broadcast(&gate->cond);
   while (!gate->release)
      pthread_cond_wait(&gate->cond, &gate->mutex);
   pthread_mutex_unlock(&gate->mutex);
}

int main(void)
{
   printf("compute_pool: ");

   /* --- Init and shutdown --- */
   {
      compute_pool_t pool;
      int rc = compute_pool_init(&pool, 2);
      assert(rc == 0);
      assert(pool.thread_count == 2);
      compute_pool_shutdown(&pool);
   }

   /* --- Submit and execute single task --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 2);

      int flag = 0;
      int rc = compute_pool_submit(&pool, set_flag, &flag);
      assert(rc == 0);

      pthread_mutex_lock(&counter_mutex);
      for (int i = 0; i < 100 && !flag; i++)
      {
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_nsec += 10000000;
         if (ts.tv_nsec >= 1000000000)
         {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
         }
         pthread_cond_timedwait(&counter_cond, &counter_mutex, &ts);
      }
      pthread_mutex_unlock(&counter_mutex);
      assert(flag == 1);

      compute_pool_shutdown(&pool);
   }

   /* --- Submit multiple tasks, all execute --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 4);

      counter = 0;
      int vals[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      for (int i = 0; i < 8; i++)
         compute_pool_submit(&pool, increment_counter, &vals[i]);

      pthread_mutex_lock(&counter_mutex);
      while (counter != 36)
         pthread_cond_wait(&counter_cond, &counter_mutex);
      pthread_mutex_unlock(&counter_mutex);
      assert(counter == 36); /* 1+2+3+4+5+6+7+8 */

      compute_pool_shutdown(&pool);
   }

   /* --- Concurrent execution (tasks run in parallel, not serial) --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 4);

      gate_t gate = {
          .mutex = PTHREAD_MUTEX_INITIALIZER,
          .cond = PTHREAD_COND_INITIALIZER,
      };

      for (int i = 0; i < 4; i++)
         compute_pool_submit(&pool, gated_task, &gate);

      pthread_mutex_lock(&gate.mutex);
      while (gate.ready < 4)
         pthread_cond_wait(&gate.cond, &gate.mutex);
      gate.release = 1;
      pthread_cond_broadcast(&gate.cond);
      pthread_mutex_unlock(&gate.mutex);

      compute_pool_shutdown(&pool);
      assert(gate.ready == 4);
   }

   /* --- Queue overflow returns -1 --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 1);

      int sleep_ms = 200;
      /* Fill the queue (COMPUTE_QUEUE_SIZE = 32) */
      int submitted = 0;
      int failed = 0;
      for (int i = 0; i < 40; i++)
      {
         int rc = compute_pool_submit(&pool, sleep_task, &sleep_ms);
         if (rc == 0)
            submitted++;
         else
         {
            failed = 1;
            break;
         }
      }
      assert(failed == 1);
      assert(submitted >= COMPUTE_QUEUE_SIZE);

      compute_pool_shutdown(&pool);
   }

   /* --- Thread count clamping --- */
   {
      compute_pool_t pool;
      /* Request more than max */
      int rc = compute_pool_init(&pool, 100);
      assert(rc == 0);
      assert(pool.thread_count <= COMPUTE_POOL_SIZE);
      compute_pool_shutdown(&pool);

      /* Request zero -- should clamp to 1 */
      rc = compute_pool_init(&pool, 0);
      assert(rc == 0);
      assert(pool.thread_count >= 1);
      compute_pool_shutdown(&pool);
   }

   /* --- Graceful shutdown drains queue --- */
   {
      compute_pool_t pool;
      compute_pool_init(&pool, 2);

      counter = 0;
      int vals[4] = {10, 20, 30, 40};
      for (int i = 0; i < 4; i++)
         compute_pool_submit(&pool, increment_counter, &vals[i]);

      /* Shutdown should wait for all tasks to complete */
      compute_pool_shutdown(&pool);
      assert(counter == 100); /* 10+20+30+40 */
   }

   printf("all tests passed\n");
   return 0;
}
