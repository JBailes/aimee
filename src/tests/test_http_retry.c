/* test_http_retry.c: unit tests for HTTP retry with exponential backoff */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "http_retry.h"

/* --- http_should_retry tests --- */

static void test_retryable_status_codes(void)
{
   /* Network errors are always retryable */
   assert(http_should_retry(-1) == 1);
   assert(http_should_retry(-999) == 1);

   /* Retryable HTTP status codes */
   assert(http_should_retry(408) == 1); /* Request Timeout */
   assert(http_should_retry(409) == 1); /* Conflict */
   assert(http_should_retry(429) == 1); /* Too Many Requests */
   assert(http_should_retry(500) == 1); /* Internal Server Error */
   assert(http_should_retry(502) == 1); /* Bad Gateway */
   assert(http_should_retry(503) == 1); /* Service Unavailable */
   assert(http_should_retry(504) == 1); /* Gateway Timeout */

   printf("  retryable status codes: ok\n");
}

static void test_non_retryable_status_codes(void)
{
   /* Success is not retryable */
   assert(http_should_retry(200) == 0);
   assert(http_should_retry(201) == 0);

   /* Client errors (non-retryable) */
   assert(http_should_retry(400) == 0); /* Bad Request */
   assert(http_should_retry(401) == 0); /* Unauthorized */
   assert(http_should_retry(403) == 0); /* Forbidden */
   assert(http_should_retry(404) == 0); /* Not Found */
   assert(http_should_retry(422) == 0); /* Unprocessable Entity */

   /* Other server errors not in the retryable set */
   assert(http_should_retry(501) == 0); /* Not Implemented */

   /* Zero (no response) is not retryable */
   assert(http_should_retry(0) == 0);

   printf("  non-retryable status codes: ok\n");
}

/* --- http_backoff_ms tests --- */

static void test_backoff_basic(void)
{
   /* Attempt 0: base delay */
   assert(http_backoff_ms(0, 1000, 30000) == 1000);

   /* Attempt 1: 2x base */
   assert(http_backoff_ms(1, 1000, 30000) == 2000);

   /* Attempt 2: 4x base */
   assert(http_backoff_ms(2, 1000, 30000) == 4000);

   /* Attempt 3: 8x base */
   assert(http_backoff_ms(3, 1000, 30000) == 8000);

   /* Attempt 4: 16x base */
   assert(http_backoff_ms(4, 1000, 30000) == 16000);

   printf("  backoff basic doubling: ok\n");
}

static void test_backoff_clamped(void)
{
   /* Should not exceed max_ms */
   assert(http_backoff_ms(5, 1000, 30000) == 30000);
   assert(http_backoff_ms(10, 1000, 30000) == 30000);
   assert(http_backoff_ms(100, 1000, 30000) == 30000);

   /* Small max_ms */
   assert(http_backoff_ms(0, 1000, 500) == 500);
   assert(http_backoff_ms(3, 100, 500) == 500);

   printf("  backoff clamped to max: ok\n");
}

static void test_backoff_overflow_safe(void)
{
   /* Very high attempt count should not overflow */
   int result = http_backoff_ms(1000, 1000, 30000);
   assert(result == 30000);
   assert(result > 0); /* no negative overflow */

   /* Maximum int-safe attempt */
   result = http_backoff_ms(2147483647, 1000, 30000);
   assert(result == 30000);
   assert(result > 0);

   printf("  backoff overflow safety: ok\n");
}

static void test_backoff_edge_cases(void)
{
   /* Negative attempt treated as 0 */
   assert(http_backoff_ms(-1, 1000, 30000) == 1000);
   assert(http_backoff_ms(-100, 1000, 30000) == 1000);

   /* Zero/negative base_ms uses default */
   int result = http_backoff_ms(0, 0, 30000);
   assert(result == HTTP_RETRY_BASE_MS);

   /* Zero/negative max_ms uses default */
   result = http_backoff_ms(0, 1000, 0);
   assert(result == 1000); /* base < default max */

   printf("  backoff edge cases: ok\n");
}

static void test_backoff_small_values(void)
{
   /* Base of 1ms */
   assert(http_backoff_ms(0, 1, 100) == 1);
   assert(http_backoff_ms(1, 1, 100) == 2);
   assert(http_backoff_ms(5, 1, 100) == 32);
   assert(http_backoff_ms(7, 1, 100) == 100); /* clamped */

   printf("  backoff small values: ok\n");
}

int main(void)
{
   printf("test_http_retry:\n");

   test_retryable_status_codes();
   test_non_retryable_status_codes();
   test_backoff_basic();
   test_backoff_clamped();
   test_backoff_overflow_safe();
   test_backoff_edge_cases();
   test_backoff_small_values();

   printf("all http_retry tests passed.\n");
   return 0;
}
