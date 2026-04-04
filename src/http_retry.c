/* http_retry.c: exponential backoff with overflow-safe retries for API calls */
#include "aimee.h"
#include "agent_exec.h"
#include "http_retry.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const int RETRYABLE_STATUS[] = {
    408, /* Request Timeout */
    409, /* Conflict (temporary contention) */
    429, /* Too Many Requests (rate limit) */
    500, /* Internal Server Error */
    502, /* Bad Gateway */
    503, /* Service Unavailable */
    504, /* Gateway Timeout */
    0
};

int http_should_retry(int http_status)
{
   /* Network errors are always retryable */
   if (http_status < 0)
      return 1;

   for (int i = 0; RETRYABLE_STATUS[i] != 0; i++)
   {
      if (http_status == RETRYABLE_STATUS[i])
         return 1;
   }
   return 0;
}

int http_backoff_ms(int attempt, int base_ms, int max_ms)
{
   if (base_ms <= 0)
      base_ms = HTTP_RETRY_BASE_MS;
   if (max_ms <= 0)
      max_ms = HTTP_RETRY_MAX_MS;
   if (attempt < 0)
      attempt = 0;

   int delay = base_ms;
   for (int i = 0; i < attempt; i++)
   {
      /* Overflow-safe doubling: stop if we'd exceed max_ms */
      if (delay > max_ms / 2)
      {
         delay = max_ms;
         break;
      }
      delay *= 2;
   }

   if (delay > max_ms)
      delay = max_ms;

   return delay;
}

int http_retry_post(const char *url, const char *auth_header, const char *body,
                    char **response_buf, int timeout_ms, const char *extra_headers,
                    int max_attempts, int base_ms, int max_ms)
{
   if (max_attempts <= 0)
      max_attempts = 1; /* at least one attempt */

   int http_status = -1;

   for (int attempt = 0; attempt < max_attempts; attempt++)
   {
      /* Sleep before retry (not before first attempt) */
      if (attempt > 0)
      {
         int delay = http_backoff_ms(attempt - 1, base_ms, max_ms);
         fprintf(stderr, "http_retry: attempt %d/%d, retrying in %dms (status %d)...\n",
                 attempt + 1, max_attempts, delay, http_status);
         usleep((unsigned)(delay * 1000));
      }

      /* Free previous response if retrying */
      if (*response_buf)
      {
         free(*response_buf);
         *response_buf = NULL;
      }

      http_status = agent_http_post(url, auth_header, body, response_buf, timeout_ms,
                                    extra_headers);

      /* Success or non-retryable error: return immediately */
      if (!http_should_retry(http_status))
         return http_status;
   }

   /* Exhausted all retries */
   return http_status;
}
