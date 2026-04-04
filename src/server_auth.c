/* server_auth.c: authentication, capability tokens, and per-method capability checks */
#include "aimee.h"
#include "server.h"
#include "secret_store.h"
#include "log.h"
#include "platform_random.h"
#include "cJSON.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Rate limiting --- */

#define AUTH_MAX_TRACKED   32
#define AUTH_MAX_FAILURES  5
#define AUTH_WINDOW_SECS   60
#define AUTH_COOLDOWN_SECS 300

typedef struct
{
   uid_t uid;
   int failures;
   time_t first_failure;
   time_t cooldown_until;
} auth_rate_t;

static auth_rate_t rate_table[AUTH_MAX_TRACKED];
static int rate_count = 0;

static int rate_check(uid_t uid)
{
   time_t now = time(NULL);

   for (int i = 0; i < rate_count; i++)
   {
      if (rate_table[i].uid == uid)
      {
         if (now < rate_table[i].cooldown_until)
            return -1; /* Still in cooldown */

         /* Reset if outside window */
         if (now - rate_table[i].first_failure > AUTH_WINDOW_SECS)
         {
            rate_table[i].failures = 0;
            rate_table[i].cooldown_until = 0;
         }
         return 0;
      }
   }
   return 0;
}

static void rate_record_failure(uid_t uid)
{
   time_t now = time(NULL);

   for (int i = 0; i < rate_count; i++)
   {
      if (rate_table[i].uid == uid)
      {
         if (rate_table[i].failures == 0)
            rate_table[i].first_failure = now;
         rate_table[i].failures++;
         if (rate_table[i].failures >= AUTH_MAX_FAILURES)
            rate_table[i].cooldown_until = now + AUTH_COOLDOWN_SECS;
         return;
      }
   }

   /* New entry */
   if (rate_count < AUTH_MAX_TRACKED)
   {
      rate_table[rate_count].uid = uid;
      rate_table[rate_count].failures = 1;
      rate_table[rate_count].first_failure = now;
      rate_table[rate_count].cooldown_until = 0;
      rate_count++;
   }
}

/* --- Token management --- */

int server_load_token(server_ctx_t *ctx)
{
   /* Try to load from secret backend (keyring or file) */
   if (secret_load(SERVER_TOKEN_FILE, ctx->token, sizeof(ctx->token)) == 0 &&
       strlen(ctx->token) >= SERVER_TOKEN_LEN)
      return 0;

   /* Generate new token */
   unsigned char raw[32];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;

   for (size_t i = 0; i < sizeof(raw); i++)
      snprintf(ctx->token + i * 2, 3, "%02x", raw[i]);

   /* Store via secret backend */
   secret_store(SERVER_TOKEN_FILE, ctx->token);

   return 0;
}

/* --- Declarative method-to-capability registry --- */

/* Exact matches are checked first, then prefix matches (trailing '*').
 * Order within each group matters: first match wins for prefixes.
 * memory.store must appear before memory.* to get the correct capability. */
const method_policy_t method_registry[] = {
    /* No capability required */
    {"server.info", 0, "server information"},
    {"server.health", 0, "server health check"},
    {"auth", 0, "authenticate"},
    /* Hooks */
    {"hooks.pre", CAP_TOOL_EXECUTE, "pre-tool hook"},
    {"hooks.post", CAP_TOOL_EXECUTE, "post-tool hook"},
    /* Sessions (prefix) */
    {"session.*", CAP_SESSION_READ, "session operation"},
    /* Memory (exact before prefix) */
    {"memory.store", CAP_MEMORY_WRITE, "store memory"},
    {"memory.*", CAP_MEMORY_READ, "memory operation"},
    /* Index (prefix) */
    {"index.*", CAP_INDEX_READ, "index operation"},
    /* Rules (prefix) */
    {"rules.*", CAP_RULES_READ, "rules operation"},
    /* Working memory (prefix) */
    {"wm.*", CAP_SESSION_READ, "working memory operation"},
    {"attempt.*", CAP_SESSION_READ, "attempt log operation"},
    /* Dashboard (prefix) */
    {"dashboard.*", CAP_DASHBOARD_READ, "dashboard operation"},
    /* Workspace */
    {"workspace.context", CAP_INDEX_READ, "workspace context"},
    /* Compute */
    {"tool.execute", CAP_TOOL_EXECUTE, "execute tool"},
    {"delegate", CAP_DELEGATE, "delegate task"},
    {"delegate.reply", CAP_DELEGATE, "delegate reply"},
    {"chat.send_stream", CAP_CHAT, "chat stream"},
    {"cli.forward", CAP_TOOL_EXECUTE, "CLI forwarding"},
    /* Sentinel */
    {NULL, 0, NULL}};

const int method_registry_count =
    (int)(sizeof(method_registry) / sizeof(method_registry[0])) - 1; /* exclude sentinel */

/* --- Capability check --- */

uint32_t server_capability_for_method(const char *method)
{
   /* Pass 1: exact matches */
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *pat = method_registry[i].method;
      /* Skip prefix patterns */
      size_t plen = strlen(pat);
      if (plen > 0 && pat[plen - 1] == '*')
         continue;
      if (strcmp(method, pat) == 0)
         return method_registry[i].required_caps;
   }

   /* Pass 2: prefix matches (patterns ending with '*') */
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *pat = method_registry[i].method;
      size_t plen = strlen(pat);
      if (plen > 0 && pat[plen - 1] == '*')
      {
         if (strncmp(method, pat, plen - 1) == 0)
            return method_registry[i].required_caps;
      }
   }

   return CAPS_ALL; /* Unknown method: require all caps (deny-by-default) */
}

/* Return the policy entry for a method, or NULL if not registered */
const method_policy_t *server_policy_for_method(const char *method)
{
   /* Pass 1: exact matches */
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *pat = method_registry[i].method;
      size_t plen = strlen(pat);
      if (plen > 0 && pat[plen - 1] == '*')
         continue;
      if (strcmp(method, pat) == 0)
         return &method_registry[i];
   }

   /* Pass 2: prefix matches */
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *pat = method_registry[i].method;
      size_t plen = strlen(pat);
      if (plen > 0 && pat[plen - 1] == '*')
      {
         if (strncmp(method, pat, plen - 1) == 0)
            return &method_registry[i];
      }
   }

   return NULL;
}

/* --- Auth handler --- */

int handle_auth(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   /* Rate limit check */
   if (rate_check(conn->peer_uid) != 0)
   {
      audit_log("auth_rate_limited", "uid=%u cooldown=%ds", (unsigned)conn->peer_uid,
                AUTH_COOLDOWN_SECS);
      return server_send_error(conn, "too many failed auth attempts, try later", NULL);
   }

   cJSON *jtoken = cJSON_GetObjectItemCaseSensitive(req, "token");
   if (!cJSON_IsString(jtoken))
      return server_send_error(conn, "missing token", NULL);

   /* Constant-time comparison to prevent timing attacks */
   const char *provided = jtoken->valuestring;
   const char *expected = ctx->token;
   size_t plen = strlen(provided);
   size_t elen = strlen(expected);
   size_t len = plen > elen ? plen : elen;

   unsigned char diff = (plen != elen) ? 1 : 0;
   for (size_t i = 0; i < len; i++)
   {
      unsigned char a = i < plen ? (unsigned char)provided[i] : 0;
      unsigned char b = i < elen ? (unsigned char)expected[i] : 0;
      diff |= a ^ b;
   }

   if (diff != 0)
   {
      rate_record_failure(conn->peer_uid);
      audit_log("auth_fail", "uid=%u reason=invalid_token", (unsigned)conn->peer_uid);
      return server_send_error(conn, "invalid token", NULL);
   }

   /* Upgrade to authenticated (least-privilege, not full admin) */
   conn->capabilities = CAPS_AUTHENTICATED;

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "trust_level", "local_attested");
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
