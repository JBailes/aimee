/* compute_pool.c: bounded thread pool for long-running server operations */
#include "compute_pool.h"
#include <string.h>

static void *worker_thread(void *arg)
{
   compute_pool_t *pool = (compute_pool_t *)arg;

   for (;;)
   {
      pthread_mutex_lock(&pool->mutex);

      /* Wait for work or shutdown */
      while (pool->queue_count == 0 && !pool->shutdown)
         pthread_cond_wait(&pool->work_available, &pool->mutex);

      if (pool->shutdown && pool->queue_count == 0)
      {
         pthread_mutex_unlock(&pool->mutex);
         break;
      }

      /* Dequeue work */
      work_item_t item = pool->queue[pool->queue_head];
      pool->queue_head = (pool->queue_head + 1) % COMPUTE_QUEUE_SIZE;
      pool->queue_count--;

      pthread_mutex_unlock(&pool->mutex);

      /* Execute */
      if (item.fn)
         item.fn(item.arg);
   }

   return NULL;
}

int compute_pool_init(compute_pool_t *pool, int num_threads)
{
   memset(pool, 0, sizeof(*pool));

   if (num_threads > COMPUTE_POOL_SIZE)
      num_threads = COMPUTE_POOL_SIZE;
   if (num_threads < 1)
      num_threads = 1;

   pthread_mutex_init(&pool->mutex, NULL);
   pthread_cond_init(&pool->work_available, NULL);

   for (int i = 0; i < num_threads; i++)
   {
      if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
         break;
      pool->thread_count++;
   }

   return pool->thread_count > 0 ? 0 : -1;
}

int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg)
{
   pthread_mutex_lock(&pool->mutex);

   if (pool->queue_count >= COMPUTE_QUEUE_SIZE)
   {
      pthread_mutex_unlock(&pool->mutex);
      return -1;
   }

   pool->queue[pool->queue_tail].fn = fn;
   pool->queue[pool->queue_tail].arg = arg;
   pool->queue_tail = (pool->queue_tail + 1) % COMPUTE_QUEUE_SIZE;
   pool->queue_count++;

   pthread_cond_signal(&pool->work_available);
   pthread_mutex_unlock(&pool->mutex);
   return 0;
}

void compute_pool_shutdown(compute_pool_t *pool)
{
   pthread_mutex_lock(&pool->mutex);
   pool->shutdown = 1;
   pthread_cond_broadcast(&pool->work_available);
   pthread_mutex_unlock(&pool->mutex);

   for (int i = 0; i < pool->thread_count; i++)
      pthread_join(pool->threads[i], NULL);

   pthread_mutex_destroy(&pool->mutex);
   pthread_cond_destroy(&pool->work_available);
}
