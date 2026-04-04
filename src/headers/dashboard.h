#ifndef DEC_DASHBOARD_H
#define DEC_DASHBOARD_H 1

#include <sqlite3.h>

#define CORS_ORIGIN_LEN 256

/* Start the local dashboard HTTP server. Blocks indefinitely.
 * Port defaults to 9200 if <= 0. */
void dashboard_serve(int port);

/* Start the unified web chat + dashboard HTTPS server. Blocks indefinitely.
 * Port defaults to 8080 if <= 0. */
void webchat_serve(int port);

/* Start the webchat server on a background thread. Returns immediately.
 * Port defaults to 8080 if <= 0. Safe to call once; subsequent calls are no-ops. */
void webchat_serve_background(int port);

/* CORS origin management */
int dashboard_cors_add(const char *origin);
int dashboard_cors_remove(const char *origin);
int dashboard_cors_list(char origins[][CORS_ORIGIN_LEN], int max);

/* Dashboard JSON API handlers (return malloc'd JSON strings, caller frees) */
char *api_delegations(sqlite3 *db);
char *api_metrics(sqlite3 *db);
char *api_traces(sqlite3 *db);
char *api_memory_stats(sqlite3 *db);
char *api_plans(sqlite3 *db);
char *api_logs(sqlite3 *db);
char *api_bench_results(void);

/* PAM credential check (Linux only). Returns 1 on success, 0 on failure. */
int pam_check_credentials(const char *user, const char *password);

/* Base64 decode (minimal, for Authorization header). Returns decoded length. */
int base64_decode(const char *in, char *out, size_t out_len);

#endif /* DEC_DASHBOARD_H */
