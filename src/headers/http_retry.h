#ifndef DEC_HTTP_RETRY_H
#define DEC_HTTP_RETRY_H 1

/* http_retry: exponential backoff with overflow-safe retries for LLM API calls.
 *
 * Retryable status codes: 408, 409, 429, 500, 502, 503, 504.
 * Non-retryable (fail immediately): 400, 401, 403, 404, and all other 4xx.
 * Network errors (http_status < 0) are always retryable.
 */

/* Default retry parameters */
#define HTTP_RETRY_MAX_ATTEMPTS  3
#define HTTP_RETRY_BASE_MS       1000
#define HTTP_RETRY_MAX_MS        30000

/* Returns 1 if the HTTP status code is retryable, 0 otherwise.
 * Network errors (status < 0) are always retryable. */
int http_should_retry(int http_status);

/* Compute backoff delay in milliseconds for the given attempt (0-based).
 * Uses exponential backoff (base_ms * 2^attempt) clamped to max_ms.
 * Overflow-safe: will not exceed max_ms regardless of attempt count. */
int http_backoff_ms(int attempt, int base_ms, int max_ms);

/* Retry wrapper around agent_http_post().
 * Retries up to max_attempts times on retryable status codes.
 * Sleeps with exponential backoff between retries.
 * Logs retry attempts at INFO level to stderr.
 * Returns the final HTTP status code, or -1 on network error.
 * On success or non-retryable error, *response_buf holds the response body. */
int http_retry_post(const char *url, const char *auth_header, const char *body,
                    char **response_buf, int timeout_ms, const char *extra_headers,
                    int max_attempts, int base_ms, int max_ms);

#endif /* DEC_HTTP_RETRY_H */
