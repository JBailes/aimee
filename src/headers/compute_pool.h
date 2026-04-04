#ifndef DEC_COMPUTE_POOL_H
#define DEC_COMPUTE_POOL_H 1

#include <pthread.h>
#include <stdint.h>

#define COMPUTE_POOL_SIZE  8
#define COMPUTE_QUEUE_SIZE 32

typedef struct
{
   void (*fn)(void *arg);
   void *arg;
   int64_t deadline_ms; /* monotonic ms deadline, 0 = no timeout */
} work_item_t;

typedef struct
{
   pthread_t threads[COMPUTE_POOL_SIZE];
   int thread_count;
   work_item_t queue[COMPUTE_QUEUE_SIZE];
   int queue_head;
   int queue_tail;
   int queue_count;
   pthread_mutex_t mutex;
   pthread_cond_t work_available;
   int shutdown;
} compute_pool_t;

/* Initialize pool with given number of worker threads. Returns 0 on success. */
int compute_pool_init(compute_pool_t *pool, int num_threads);

/* Submit work to the pool. Returns 0 on success, -1 if queue is full. */
int compute_pool_submit(compute_pool_t *pool, void (*fn)(void *), void *arg);

/* Shut down pool: signal all workers, wait for completion. */
void compute_pool_shutdown(compute_pool_t *pool);

#endif /* DEC_COMPUTE_POOL_H */
