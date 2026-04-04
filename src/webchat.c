/* webchat.c: unified HTTPS web chat + dashboard server with PAM auth and session cookies */
#include "aimee.h"
#include "cli_client.h"
#include "dashboard.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_tools.h"
#include "platform_random.h"
#include "commands.h"
#include "cJSON.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rsa.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Constants --- */

#define WC_DEFAULT_PORT    8080
#define WC_MAX_REQUEST     (64 * 1024)
#define WC_MAX_SESSIONS    16
#define WC_SESSION_TIMEOUT (4 * 3600) /* 4 hours */
#define WC_TOKEN_LEN       64         /* hex characters */
#define WC_MAX_TOOL_CALLS  16
#define WC_CHAT_TIMEOUT_MS 300000 /* 5 minutes */
#define WC_SSE_BUF_SIZE    (128 * 1024)
#define WC_MAX_ACL_ENTRIES 32

/* --- Provider types (mirrors cmd_chat.c) --- */

typedef enum
{
   WC_PROVIDER_OPENAI,
   WC_PROVIDER_ANTHROPIC,
   WC_PROVIDER_CLAUDE /* Forward to Claude CLI */
} wc_provider_t;

/* --- Tool call accumulator --- */

typedef struct
{
   char id[64];
   char name[64];
   char *arguments;
   size_t args_len;
   size_t args_cap;
} wc_tool_call_t;

/* --- Network ACL --- */

typedef struct
{
   uint32_t network; /* network address in host order */
   uint32_t mask;    /* netmask in host order */
} wc_acl_entry_t;

static wc_acl_entry_t acl_entries[WC_MAX_ACL_ENTRIES];
static int acl_count = 0;

/* --- Session --- */

typedef struct
{
   char token[WC_TOKEN_LEN + 1];
   char csrf_token[WC_TOKEN_LEN + 1];
   char username[64];
   time_t created;
   time_t last_access;
   int active;

   /* Chat state */
   wc_provider_t provider;
   cJSON *messages;
   cJSON *tools;
   char *system_prompt;
   char url[1024];
   char model[128];
   char auth_header[4128];
   char extra_headers[512];
   char claude_session_id[256]; /* For WC_PROVIDER_CLAUDE: resume session */

   pthread_mutex_t lock;
} wc_session_t;

static wc_session_t sessions[WC_MAX_SESSIONS];
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- Per-turn streaming context (writes to SSL instead of stdout) --- */

typedef struct
{
   SSL *ssl;
   wc_provider_t provider;
   char *sse_buf;
   size_t sse_len;
   char *content;
   size_t content_len;
   size_t content_cap;
   wc_tool_call_t tool_calls[WC_MAX_TOOL_CALLS];
   int tool_call_count;
   int finished;
   char finish_reason[32];
   char *error_buf;
   size_t error_buf_len;
   size_t error_buf_cap;
   int aborted;
} wc_turn_t;

/* --- Per-connection thread context --- */

typedef struct
{
   int client_fd;
   SSL *ssl;
   SSL_CTX *ssl_ctx;
} wc_conn_t;

/* --- Global state --- */

static SSL_CTX *g_ssl_ctx = NULL;
static int g_webchat_port = 0;
static char g_system_prompt_path[MAX_PATH_LEN] = {0};

/* --- Helpers --- */

static void wc_append(char **buf, size_t *len, size_t *cap, const char *data, size_t dlen)
{
   if (*len + dlen + 1 > *cap)
   {
      size_t new_cap = (*cap == 0) ? 1024 : *cap * 2;
      while (new_cap < *len + dlen + 1)
         new_cap *= 2;
      char *tmp = realloc(*buf, new_cap);
      if (!tmp)
         return;
      *buf = tmp;
      *cap = new_cap;
   }
   memcpy(*buf + *len, data, dlen);
   *len += dlen;
   (*buf)[*len] = '\0';
}

static void wc_tc_append(wc_tool_call_t *tc, const char *data, size_t dlen)
{
   wc_append(&tc->arguments, &tc->args_len, &tc->args_cap, data, dlen);
}

/* --- JSON string escaping for SSE data --- */

static char *json_escape(const char *s)
{
   if (!s)
      return strdup("");
   size_t len = strlen(s);
   size_t cap = len * 2 + 3;
   char *out = malloc(cap);
   if (!out)
      return strdup("");
   size_t oi = 0;
   for (size_t i = 0; i < len && oi < cap - 6; i++)
   {
      switch (s[i])
      {
      case '"':
         out[oi++] = '\\';
         out[oi++] = '"';
         break;
      case '\\':
         out[oi++] = '\\';
         out[oi++] = '\\';
         break;
      case '\n':
         out[oi++] = '\\';
         out[oi++] = 'n';
         break;
      case '\r':
         out[oi++] = '\\';
         out[oi++] = 'r';
         break;
      case '\t':
         out[oi++] = '\\';
         out[oi++] = 't';
         break;
      default:
         if ((unsigned char)s[i] < 0x20)
         {
            oi += (size_t)snprintf(out + oi, cap - oi, "\\u%04x", (unsigned char)s[i]);
         }
         else
         {
            out[oi++] = s[i];
         }
      }
   }
   out[oi] = '\0';
   return out;
}

/* --- SSL write helper --- */

static int ssl_write_all(SSL *ssl, const char *data, size_t len)
{
   size_t written = 0;
   while (written < len)
   {
      int n = SSL_write(ssl, data + written, (int)(len - written));
      if (n <= 0)
         return -1;
      written += (size_t)n;
   }
   return 0;
}

static int ssl_printf(SSL *ssl, const char *fmt, ...)
{
   char buf[8192];
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (n <= 0)
      return -1;
   /* Clamp to actual buffer size to prevent OOB read on truncation */
   if ((size_t)n >= sizeof(buf))
      n = (int)(sizeof(buf) - 1);
   return ssl_write_all(ssl, buf, (size_t)n);
}

/* --- Send SSE event to browser --- */

static int sse_send(SSL *ssl, const char *event, const char *data)
{
   char buf[WC_MAX_REQUEST];
   int n = snprintf(buf, sizeof(buf), "event: %s\ndata: %s\n\n", event, data);
   if (n <= 0 || (size_t)n >= sizeof(buf))
      return -1;
   return ssl_write_all(ssl, buf, (size_t)n);
}

/* === TLS CERTIFICATE GENERATION === */

static void ensure_tls_dir(char *dir, size_t len)
{
   snprintf(dir, len, "%s/tls", config_default_dir());
   mkdir(dir, 0700);
}

static int generate_self_signed_cert(const char *cert_path, const char *key_path)
{
   EVP_PKEY *pkey = NULL;
   EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
   if (!pctx)
      return -1;
   if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 4096) <= 0 ||
       EVP_PKEY_keygen(pctx, &pkey) <= 0)
   {
      EVP_PKEY_CTX_free(pctx);
      return -1;
   }
   EVP_PKEY_CTX_free(pctx);

   X509 *x509 = X509_new();
   if (!x509)
   {
      EVP_PKEY_free(pkey);
      return -1;
   }

   ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
   X509_gmtime_adj(X509_getm_notBefore(x509), 0);
   X509_gmtime_adj(X509_getm_notAfter(x509), 3650L * 86400L); /* 10 years */

   X509_set_pubkey(x509, pkey);

   X509_NAME *name = X509_get_subject_name(x509);
   X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"aimee", -1, -1, 0);
   X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"aimee", -1, -1, 0);
   X509_set_issuer_name(x509, name);

   /* Add SAN for localhost and 127.0.0.1 */
   X509V3_CTX v3ctx;
   X509V3_set_ctx_nodb(&v3ctx);
   X509V3_set_ctx(&v3ctx, x509, x509, NULL, NULL, 0);
   X509_EXTENSION *san_ext =
       X509V3_EXT_conf_nid(NULL, &v3ctx, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
   if (san_ext)
   {
      X509_add_ext(x509, san_ext, -1);
      X509_EXTENSION_free(san_ext);
   }

   X509_sign(x509, pkey, EVP_sha256());

   /* Write key */
   FILE *f = fopen(key_path, "w");
   if (!f)
   {
      X509_free(x509);
      EVP_PKEY_free(pkey);
      return -1;
   }
   PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL);
   fclose(f);
   chmod(key_path, 0600);

   /* Write cert */
   f = fopen(cert_path, "w");
   if (!f)
   {
      X509_free(x509);
      EVP_PKEY_free(pkey);
      return -1;
   }
   PEM_write_X509(f, x509);
   fclose(f);

   X509_free(x509);
   EVP_PKEY_free(pkey);
   return 0;
}

static SSL_CTX *webchat_tls_init(void)
{
   char tls_dir[MAX_PATH_LEN];
   ensure_tls_dir(tls_dir, sizeof(tls_dir));

   char cert_path[MAX_PATH_LEN], key_path[MAX_PATH_LEN];
   snprintf(cert_path, sizeof(cert_path), "%s/cert.pem", tls_dir);
   snprintf(key_path, sizeof(key_path), "%s/key.pem", tls_dir);

   /* Generate self-signed cert if not exists */
   struct stat st;
   if (stat(cert_path, &st) != 0 || stat(key_path, &st) != 0)
   {
      fprintf(stderr, "Generating self-signed TLS certificate...\n");
      if (generate_self_signed_cert(cert_path, key_path) < 0)
         fatal("failed to generate TLS certificate");
      fprintf(stderr, "Certificate saved to %s\n", tls_dir);
   }

   const SSL_METHOD *method = TLS_server_method();
   SSL_CTX *ctx = SSL_CTX_new(method);
   if (!ctx)
      fatal("SSL_CTX_new failed");

   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

   if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) <= 0)
      fatal("failed to load TLS certificate: %s", cert_path);
   if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) <= 0)
      fatal("failed to load TLS private key: %s", key_path);
   if (!SSL_CTX_check_private_key(ctx))
      fatal("TLS certificate and private key do not match");

   return ctx;
}

/* === NETWORK ACL === */

static void webchat_acl_init(void)
{
   acl_count = 0;

   /* Always allow loopback */
   acl_entries[acl_count].network = ntohl(inet_addr("127.0.0.0"));
   acl_entries[acl_count].mask = ntohl(inet_addr("255.0.0.0"));
   acl_count++;

   /* Auto-detect local subnets */
   struct ifaddrs *ifap = NULL;
   if (getifaddrs(&ifap) != 0)
      return;

   for (struct ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next)
   {
      if (!ifa->ifa_addr || !ifa->ifa_netmask)
         continue;
      if (!(ifa->ifa_flags & IFF_UP))
         continue;
      if (ifa->ifa_addr->sa_family != AF_INET)
         continue;

      struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
      struct sockaddr_in *mask = (struct sockaddr_in *)ifa->ifa_netmask;

      uint32_t ip = ntohl(addr->sin_addr.s_addr);
      uint32_t nm = ntohl(mask->sin_addr.s_addr);
      uint32_t net = ip & nm;

      /* Check for duplicate */
      int dup = 0;
      for (int i = 0; i < acl_count; i++)
      {
         if (acl_entries[i].network == net && acl_entries[i].mask == nm)
         {
            dup = 1;
            break;
         }
      }
      if (!dup && acl_count < WC_MAX_ACL_ENTRIES)
      {
         acl_entries[acl_count].network = net;
         acl_entries[acl_count].mask = nm;
         acl_count++;
      }
   }
   freeifaddrs(ifap);
}

static int webchat_acl_check(struct sockaddr_in *client_addr)
{
   uint32_t ip = ntohl(client_addr->sin_addr.s_addr);
   for (int i = 0; i < acl_count; i++)
   {
      if ((ip & acl_entries[i].mask) == acl_entries[i].network)
         return 1;
   }
   return 0;
}

/* === SESSION MANAGEMENT === */

static int generate_token(char *out, size_t len)
{
   unsigned char raw[32]; /* 256 bits */
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      return -1;
   for (size_t i = 0; i < sizeof(raw) && i * 2 + 1 < len; i++)
      snprintf(out + i * 2, 3, "%02x", raw[i]);
   out[len - 1] = '\0';
   return 0;
}

/* Build system prompt (same as cmd_chat.c) */
static char *wc_build_system_prompt(void)
{
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      snprintf(cwd, sizeof(cwd), ".");

   /* Load agent config to discover available delegate roles */
   agent_config_t acfg;
   memset(&acfg, 0, sizeof(acfg));
   agent_load_config(&acfg);

   char roles_buf[1024] = {0};
   int pos = 0;
   for (int i = 0; i < acfg.agent_count; i++)
   {
      if (!acfg.agents[i].enabled)
         continue;
      for (int j = 0; j < acfg.agents[i].role_count; j++)
      {
         if (pos > 0)
            pos += snprintf(roles_buf + pos, sizeof(roles_buf) - pos, ", ");
         pos += snprintf(roles_buf + pos, sizeof(roles_buf) - pos, "%s", acfg.agents[i].roles[j]);
      }
   }

   char *prompt = malloc(8192);
   if (!prompt)
      return NULL;

   snprintf(prompt, 8192,
            "You are aimee, an AI coding assistant. You are running on a web interface "
            "and forwarding through Claude Code.\n"
            "Working directory: %s\n\n"
            "# Delegation\n"
            "You can delegate work to sub-agents via the Bash tool. Do NOT use the "
            "Agent tool or RemoteTrigger for delegation — use aimee's delegate system:\n"
            "  aimee delegate <role> \"prompt\" [--tools] [--background]\n\n"
            "Available roles: %s\n\n"
            "Use --tools to give the delegate access to tools (bash, read_file, etc).\n"
            "Use --background to run asynchronously and get a task_id; poll with:\n"
            "  aimee delegate status <task_id>\n\n"
            "Delegates are ideal for offloading expensive or independent work "
            "(analysis, reviews, tests) in parallel while you continue.\n"
            "IMPORTANT: Always use 'aimee delegate' via Bash, never use the Agent "
            "tool or RemoteTrigger tool for delegation.\n\n"
            "# Work Queue\n"
            "You can coordinate with other aimee sessions via a shared work queue.\n"
            "  aimee work claim              # pick up next pending item\n"
            "  aimee work complete --result \"summary\"  # mark done\n"
            "  aimee work fail --result \"reason\"       # mark failed\n"
            "  aimee work list               # see all items\n"
            "When you claim an item, read its source to understand what to do.\n"
            "If the source is a proposal (e.g., \"proposal:foo.md\"), read the file,\n"
            "implement it, and create a PR. Then mark the item complete with the PR URL.\n",
            cwd, roles_buf[0] ? roles_buf : "code, review, explain, refactor");
   return prompt;
}

/* Write system prompt to a file for --append-system-prompt-file */
static void wc_write_system_prompt_file(void)
{
   snprintf(g_system_prompt_path, sizeof(g_system_prompt_path), "%s/webchat_system_prompt.txt",
            config_default_dir());

   char *prompt = wc_build_system_prompt();
   if (!prompt)
   {
      fprintf(stderr, "warning: failed to build system prompt\n");
      g_system_prompt_path[0] = '\0';
      return;
   }

   FILE *f = fopen(g_system_prompt_path, "w");
   if (f)
   {
      fputs(prompt, f);
      fclose(f);
      fprintf(stderr, "System prompt: %s\n", g_system_prompt_path);
   }
   else
   {
      fprintf(stderr, "warning: could not write %s\n", g_system_prompt_path);
      g_system_prompt_path[0] = '\0';
   }
   free(prompt);
}

/* Initialize chat state for a new session */
static void session_init_chat(wc_session_t *s)
{
   config_t cfg;
   config_load(&cfg);

   /* Detect provider */
   if (strcmp(cfg.provider, "claude") == 0)
   {
      s->provider = WC_PROVIDER_CLAUDE;
      s->claude_session_id[0] = '\0';
      s->messages = NULL;
      s->tools = NULL;
      s->system_prompt = NULL;
      return; /* Claude CLI handles everything */
   }
   else if (strcmp(cfg.provider, "gemini") == 0)
   {
      s->provider = WC_PROVIDER_OPENAI;
      if (strcmp(cfg.openai_endpoint, "https://api.openai.com/v1") == 0)
         snprintf(cfg.openai_endpoint, sizeof(cfg.openai_endpoint),
                  "https://generativelanguage.googleapis.com/v1beta/openai");
      if (strcmp(cfg.openai_model, "gpt-4o") == 0)
         snprintf(cfg.openai_model, sizeof(cfg.openai_model), "gemini-2.5-pro");
   }
   else if (strcmp(cfg.provider, "codex") == 0)
   {
      s->provider = WC_PROVIDER_OPENAI;
      if (strcmp(cfg.openai_model, "gpt-4o") == 0)
         snprintf(cfg.openai_model, sizeof(cfg.openai_model), "o3");
   }
   else
   {
      s->provider = WC_PROVIDER_OPENAI;
   }

   snprintf(s->model, sizeof(s->model), "%s", cfg.openai_model);

   if (s->provider == WC_PROVIDER_ANTHROPIC)
      snprintf(s->url, sizeof(s->url), "%s/messages", cfg.openai_endpoint);
   else
      snprintf(s->url, sizeof(s->url), "%s/chat/completions", cfg.openai_endpoint);

   /* Resolve auth */
   s->auth_header[0] = '\0';
   char key[4096] = {0};

   if (cfg.openai_key_cmd[0])
   {
      /* Use safe argv execution instead of popen() to prevent shell injection */
      const char *argv[] = {"/bin/sh", "-c", cfg.openai_key_cmd, NULL};
      if (!has_shell_metachar(cfg.openai_key_cmd) || strncmp(cfg.openai_key_cmd, "cat ", 4) == 0)
      {
         char *out = NULL;
         int rc = safe_exec_capture(argv, &out, sizeof(key) - 1);
         if (rc == 0 && out)
         {
            snprintf(key, sizeof(key), "%s", out);
            size_t n = strlen(key);
            while (n > 0 && (key[n - 1] == '\n' || key[n - 1] == '\r' || key[n - 1] == ' '))
               key[--n] = '\0';
         }
         free(out);
      }
      else
      {
         fprintf(stderr, "warning: openai_key_cmd contains shell metacharacters; "
                         "only 'cat <path>' style commands are supported\n");
      }
   }
   if (!key[0])
   {
      const char *env = NULL;
      if (s->provider == WC_PROVIDER_ANTHROPIC)
         env = getenv("ANTHROPIC_API_KEY");
      if (!env || !env[0])
         env = getenv("GEMINI_API_KEY");
      if (!env || !env[0])
         env = getenv("OPENAI_API_KEY");
      if (env && env[0])
         snprintf(key, sizeof(key), "%s", env);
   }
   if (key[0])
   {
      if (s->provider == WC_PROVIDER_ANTHROPIC)
         snprintf(s->auth_header, sizeof(s->auth_header), "x-api-key: %s", key);
      else
         snprintf(s->auth_header, sizeof(s->auth_header), "Authorization: Bearer %s", key);
   }

   /* Build tools */
   if (s->provider == WC_PROVIDER_ANTHROPIC)
      s->tools = build_tools_array_anthropic();
   else
      s->tools = build_tools_array();

   /* Init conversation */
   s->messages = cJSON_CreateArray();

   char *sys_prompt = wc_build_system_prompt();
   if (sys_prompt)
   {
      if (s->provider == WC_PROVIDER_ANTHROPIC)
      {
         s->system_prompt = sys_prompt;
      }
      else
      {
         cJSON *sys_msg = cJSON_CreateObject();
         cJSON_AddStringToObject(sys_msg, "role", "system");
         cJSON_AddStringToObject(sys_msg, "content", sys_prompt);
         cJSON_AddItemToArray(s->messages, sys_msg);
         free(sys_prompt);
      }
   }
}

static wc_session_t *session_create(const char *username)
{
   pthread_mutex_lock(&sessions_mutex);

   /* Find free slot */
   wc_session_t *s = NULL;
   for (int i = 0; i < WC_MAX_SESSIONS; i++)
   {
      if (!sessions[i].active)
      {
         s = &sessions[i];
         break;
      }
   }
   if (!s)
   {
      /* Evict oldest session */
      time_t oldest = time(NULL);
      int oldest_idx = 0;
      for (int i = 0; i < WC_MAX_SESSIONS; i++)
      {
         if (sessions[i].last_access < oldest)
         {
            oldest = sessions[i].last_access;
            oldest_idx = i;
         }
      }
      s = &sessions[oldest_idx];
      /* Mark inactive first, then acquire session lock to ensure no thread is using it */
      s->active = 0;
      pthread_mutex_lock(&s->lock);
      cJSON_Delete(s->messages);
      cJSON_Delete(s->tools);
      free(s->system_prompt);
      s->messages = NULL;
      s->tools = NULL;
      s->system_prompt = NULL;
      pthread_mutex_unlock(&s->lock);
      pthread_mutex_destroy(&s->lock);
   }

   memset(s, 0, sizeof(*s));
   generate_token(s->token, sizeof(s->token));
   generate_token(s->csrf_token, sizeof(s->csrf_token));
   snprintf(s->username, sizeof(s->username), "%s", username);
   s->created = time(NULL);
   s->last_access = s->created;
   s->active = 1;
   pthread_mutex_init(&s->lock, NULL);

   /* Initialize chat state */
   session_init_chat(s);

   pthread_mutex_unlock(&sessions_mutex);
   return s;
}

static wc_session_t *session_lookup(const char *token)
{
   if (!token || !token[0])
      return NULL;

   pthread_mutex_lock(&sessions_mutex);
   wc_session_t *found = NULL;
   time_t now = time(NULL);

   for (int i = 0; i < WC_MAX_SESSIONS; i++)
   {
      if (sessions[i].active && strcmp(sessions[i].token, token) == 0)
      {
         if (now - sessions[i].last_access > WC_SESSION_TIMEOUT)
         {
            /* Expired */
            sessions[i].active = 0;
            cJSON_Delete(sessions[i].messages);
            cJSON_Delete(sessions[i].tools);
            free(sessions[i].system_prompt);
            sessions[i].messages = NULL;
            sessions[i].tools = NULL;
            sessions[i].system_prompt = NULL;
            break;
         }
         sessions[i].last_access = now;
         found = &sessions[i];
         break;
      }
   }
   pthread_mutex_unlock(&sessions_mutex);
   return found;
}

static void session_reap(void)
{
   time_t now = time(NULL);
   pthread_mutex_lock(&sessions_mutex);
   for (int i = 0; i < WC_MAX_SESSIONS; i++)
   {
      if (sessions[i].active && (now - sessions[i].last_access > WC_SESSION_TIMEOUT))
      {
         sessions[i].active = 0;
         cJSON_Delete(sessions[i].messages);
         cJSON_Delete(sessions[i].tools);
         free(sessions[i].system_prompt);
         sessions[i].messages = NULL;
         sessions[i].tools = NULL;
         sessions[i].system_prompt = NULL;
      }
   }
   pthread_mutex_unlock(&sessions_mutex);
}

/* === HTTP REQUEST PARSING === */

static const char *extract_cookie(const char *headers, const char *name, char *value, size_t vlen)
{
   char needle[128];
   snprintf(needle, sizeof(needle), "%s=", name);

   const char *cookie = strstr(headers, "Cookie: ");
   if (!cookie)
      cookie = strstr(headers, "cookie: ");
   if (!cookie)
      return NULL;

   cookie += 8;
   const char *pos = strstr(cookie, needle);
   if (!pos)
      return NULL;

   pos += strlen(needle);
   size_t i = 0;
   while (*pos && *pos != ';' && *pos != '\r' && *pos != '\n' && i < vlen - 1)
      value[i++] = *pos++;
   value[i] = '\0';
   return value;
}

/* URL-decode in place */
static void url_decode(char *s)
{
   char *src = s, *dst = s;
   while (*src)
   {
      if (*src == '%' && src[1] && src[2])
      {
         char hex[3] = {src[1], src[2], 0};
         char ch = (char)strtol(hex, NULL, 16);
         if (ch)
         {
            *dst++ = ch;
            src += 3;
            continue;
         }
      }
      else if (*src == '+')
      {
         *dst++ = ' ';
         src++;
         continue;
      }
      *dst++ = *src++;
   }
   *dst = '\0';
}

/* Parse form-encoded body: extract a field value */
static int parse_form_field(const char *body, const char *field, char *out, size_t out_len)
{
   char needle[128];
   snprintf(needle, sizeof(needle), "%s=", field);

   const char *pos = strstr(body, needle);
   if (!pos)
      return -1;

   /* Make sure it's at start or after & */
   if (pos != body && *(pos - 1) != '&')
   {
      /* Search again */
      while ((pos = strstr(pos + 1, needle)) != NULL)
      {
         if (pos == body || *(pos - 1) == '&')
            break;
      }
      if (!pos)
         return -1;
   }

   pos += strlen(needle);
   size_t i = 0;
   while (*pos && *pos != '&' && i < out_len - 1)
      out[i++] = *pos++;
   out[i] = '\0';
   url_decode(out);
   return 0;
}

/* Read full HTTP request including body (based on Content-Length) */
static ssize_t read_request(SSL *ssl, char *buf, size_t buf_size)
{
   ssize_t total = 0;

   /* Read headers first */
   while ((size_t)total < buf_size - 1)
   {
      int n = SSL_read(ssl, buf + total, (int)(buf_size - 1 - (size_t)total));
      if (n <= 0)
         break;
      total += n;
      buf[total] = '\0';
      if (strstr(buf, "\r\n\r\n"))
         break;
   }
   if (total <= 0)
      return -1;

   /* Check for Content-Length and read body */
   const char *cl = strstr(buf, "Content-Length: ");
   if (!cl)
      cl = strstr(buf, "content-length: ");
   if (cl)
   {
      int content_len = atoi(cl + 16);
      if (content_len > 0 && (size_t)content_len < buf_size - (size_t)total - 1)
      {
         char *body_start = strstr(buf, "\r\n\r\n");
         if (body_start)
         {
            body_start += 4;
            ssize_t body_have = total - (body_start - buf);
            ssize_t body_need = content_len - body_have;
            while (body_need > 0 && (size_t)total < buf_size - 1)
            {
               int n = SSL_read(ssl, buf + total, (int)body_need);
               if (n <= 0)
                  break;
               total += n;
               body_need -= n;
            }
            buf[total] = '\0';
         }
      }
   }
   return total;
}

/* === SSE PARSING (mirrors cmd_chat.c but writes to SSL) === */

static void wc_process_sse_openai(wc_turn_t *turn, const char *line, size_t len)
{
   if (len == 0 || line[0] == ':')
      return;
   if (len < 6 || strncmp(line, "data: ", 6) != 0)
      return;

   const char *json_str = line + 6;
   if (strncmp(json_str, "[DONE]", 6) == 0)
   {
      turn->finished = 1;
      return;
   }

   cJSON *root = cJSON_Parse(json_str);
   if (!root)
      return;

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
   {
      cJSON *err = cJSON_GetObjectItem(root, "error");
      if (err)
      {
         cJSON *msg = cJSON_GetObjectItem(err, "message");
         if (msg && cJSON_IsString(msg))
            wc_append(&turn->error_buf, &turn->error_buf_len, &turn->error_buf_cap,
                      msg->valuestring, strlen(msg->valuestring));
      }
      cJSON_Delete(root);
      return;
   }

   cJSON *choice = cJSON_GetArrayItem(choices, 0);
   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   if (finish && cJSON_IsString(finish))
      snprintf(turn->finish_reason, sizeof(turn->finish_reason), "%s", finish->valuestring);

   cJSON *delta = cJSON_GetObjectItem(choice, "delta");
   if (!delta)
   {
      cJSON_Delete(root);
      return;
   }

   /* Text content */
   cJSON *content = cJSON_GetObjectItem(delta, "content");
   if (content && cJSON_IsString(content) && content->valuestring[0])
   {
      const char *text = content->valuestring;
      size_t tlen = strlen(text);
      wc_append(&turn->content, &turn->content_len, &turn->content_cap, text, tlen);

      /* Send SSE text event to browser */
      char *escaped = json_escape(text);
      char evt[WC_MAX_REQUEST];
      snprintf(evt, sizeof(evt), "{\"content\":\"%s\"}", escaped);
      sse_send(turn->ssl, "text", evt);
      free(escaped);
   }

   /* Tool call deltas */
   cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
   if (tool_calls && cJSON_IsArray(tool_calls))
   {
      int n = cJSON_GetArraySize(tool_calls);
      for (int i = 0; i < n; i++)
      {
         cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
         cJSON *idx_j = cJSON_GetObjectItem(tc, "index");
         int idx = (idx_j && cJSON_IsNumber(idx_j)) ? idx_j->valueint : 0;
         if (idx >= WC_MAX_TOOL_CALLS)
            continue;
         if (idx >= turn->tool_call_count)
            turn->tool_call_count = idx + 1;

         wc_tool_call_t *call = &turn->tool_calls[idx];
         cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
         if (tc_id && cJSON_IsString(tc_id) && !call->id[0])
            snprintf(call->id, sizeof(call->id), "%s", tc_id->valuestring);

         cJSON *fn = cJSON_GetObjectItem(tc, "function");
         if (fn)
         {
            cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
            if (fn_name && cJSON_IsString(fn_name) && !call->name[0])
               snprintf(call->name, sizeof(call->name), "%s", fn_name->valuestring);
            cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
            if (fn_args && cJSON_IsString(fn_args) && fn_args->valuestring[0])
               wc_tc_append(call, fn_args->valuestring, strlen(fn_args->valuestring));
         }
      }
   }

   cJSON_Delete(root);
}

static void wc_process_sse_anthropic(wc_turn_t *turn, const char *line, size_t len)
{
   if (len == 0 || line[0] == ':')
      return;
   if (len > 6 && strncmp(line, "event:", 6) == 0)
      return;
   if (len < 6 || strncmp(line, "data: ", 6) != 0)
      return;

   const char *json_str = line + 6;
   cJSON *root = cJSON_Parse(json_str);
   if (!root)
      return;

   cJSON *type_j = cJSON_GetObjectItem(root, "type");
   const char *t = (type_j && cJSON_IsString(type_j)) ? type_j->valuestring : "";

   if (strcmp(t, "content_block_start") == 0)
   {
      cJSON *block = cJSON_GetObjectItem(root, "content_block");
      if (block)
      {
         cJSON *btype = cJSON_GetObjectItem(block, "type");
         if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0)
         {
            int idx = turn->tool_call_count;
            if (idx < WC_MAX_TOOL_CALLS)
            {
               wc_tool_call_t *tc = &turn->tool_calls[idx];
               cJSON *id = cJSON_GetObjectItem(block, "id");
               if (id && cJSON_IsString(id))
                  snprintf(tc->id, sizeof(tc->id), "%s", id->valuestring);
               cJSON *nm = cJSON_GetObjectItem(block, "name");
               if (nm && cJSON_IsString(nm))
                  snprintf(tc->name, sizeof(tc->name), "%s", nm->valuestring);
               turn->tool_call_count = idx + 1;
            }
         }
      }
   }
   else if (strcmp(t, "content_block_delta") == 0)
   {
      cJSON *delta = cJSON_GetObjectItem(root, "delta");
      if (delta)
      {
         cJSON *dtype = cJSON_GetObjectItem(delta, "type");
         const char *dt = (dtype && cJSON_IsString(dtype)) ? dtype->valuestring : "";

         if (strcmp(dt, "text_delta") == 0)
         {
            cJSON *text = cJSON_GetObjectItem(delta, "text");
            if (text && cJSON_IsString(text) && text->valuestring[0])
            {
               const char *s = text->valuestring;
               size_t slen = strlen(s);
               wc_append(&turn->content, &turn->content_len, &turn->content_cap, s, slen);

               char *escaped = json_escape(s);
               char evt[WC_MAX_REQUEST];
               snprintf(evt, sizeof(evt), "{\"content\":\"%s\"}", escaped);
               sse_send(turn->ssl, "text", evt);
               free(escaped);
            }
         }
         else if (strcmp(dt, "input_json_delta") == 0)
         {
            cJSON *pj = cJSON_GetObjectItem(delta, "partial_json");
            if (pj && cJSON_IsString(pj) && pj->valuestring[0])
            {
               int tc_idx = turn->tool_call_count - 1;
               if (tc_idx >= 0 && tc_idx < WC_MAX_TOOL_CALLS)
                  wc_tc_append(&turn->tool_calls[tc_idx], pj->valuestring, strlen(pj->valuestring));
            }
         }
      }
   }
   else if (strcmp(t, "message_delta") == 0)
   {
      cJSON *delta = cJSON_GetObjectItem(root, "delta");
      if (delta)
      {
         cJSON *sr = cJSON_GetObjectItem(delta, "stop_reason");
         if (sr && cJSON_IsString(sr))
            snprintf(turn->finish_reason, sizeof(turn->finish_reason), "%s", sr->valuestring);
      }
   }
   else if (strcmp(t, "message_stop") == 0)
   {
      turn->finished = 1;
   }
   else if (strcmp(t, "error") == 0)
   {
      cJSON *err = cJSON_GetObjectItem(root, "error");
      if (err)
      {
         cJSON *msg = cJSON_GetObjectItem(err, "message");
         if (msg && cJSON_IsString(msg))
            wc_append(&turn->error_buf, &turn->error_buf_len, &turn->error_buf_cap,
                      msg->valuestring, strlen(msg->valuestring));
      }
   }

   cJSON_Delete(root);
}

/* Stream callback: called per chunk from AI provider */
static int wc_stream_cb(const char *data, size_t len, void *userdata)
{
   wc_turn_t *turn = (wc_turn_t *)userdata;
   if (turn->aborted)
      return 1;

   if (turn->sse_len + len + 1 > WC_SSE_BUF_SIZE)
      turn->sse_len = 0;

   memcpy(turn->sse_buf + turn->sse_len, data, len);
   turn->sse_len += len;
   turn->sse_buf[turn->sse_len] = '\0';

   char *buf = turn->sse_buf;
   char *nl;
   while ((nl = strchr(buf, '\n')) != NULL)
   {
      size_t line_len = (size_t)(nl - buf);
      if (line_len > 0 && buf[line_len - 1] == '\r')
         line_len--;

      /* Null-terminate the line temporarily */
      char saved = buf[line_len];
      buf[line_len] = '\0';

      if (turn->provider == WC_PROVIDER_ANTHROPIC)
         wc_process_sse_anthropic(turn, buf, line_len);
      else
         wc_process_sse_openai(turn, buf, line_len);

      buf[line_len] = saved;
      buf = nl + 1;
   }

   size_t remaining = turn->sse_len - (size_t)(buf - turn->sse_buf);
   if (remaining > 0 && buf != turn->sse_buf)
      memmove(turn->sse_buf, buf, remaining);
   turn->sse_len = remaining;

   return 0;
}

/* Reset turn state */
static void wc_turn_reset(wc_turn_t *turn)
{
   turn->sse_len = 0;
   turn->finished = 0;
   turn->finish_reason[0] = '\0';

   free(turn->content);
   turn->content = NULL;
   turn->content_len = 0;
   turn->content_cap = 0;

   free(turn->error_buf);
   turn->error_buf = NULL;
   turn->error_buf_len = 0;
   turn->error_buf_cap = 0;

   for (int i = 0; i < turn->tool_call_count; i++)
      free(turn->tool_calls[i].arguments);
   memset(turn->tool_calls, 0, sizeof(turn->tool_calls));
   turn->tool_call_count = 0;
}

/* === CHAT TURN EXECUTION === */

static int wc_chat_send(wc_session_t *s, wc_turn_t *turn)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", s->model);
   cJSON_AddBoolToObject(req, "stream", 1);

   if (s->provider == WC_PROVIDER_ANTHROPIC)
   {
      cJSON_AddNumberToObject(req, "max_tokens", 8192);
      if (s->system_prompt && s->system_prompt[0])
         cJSON_AddStringToObject(req, "system", s->system_prompt);
   }

   cJSON_AddItemReferenceToObject(req, "messages", s->messages);

   if (s->tools && cJSON_GetArraySize(s->tools) > 0)
      cJSON_AddItemReferenceToObject(req, "tools", s->tools);

   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   int rc = agent_http_post_stream(s->url, s->auth_header, body, wc_stream_cb, turn,
                                   WC_CHAT_TIMEOUT_MS, s->extra_headers);
   free(body);
   return rc;
}

/* Build assistant tool-calls message for OpenAI history */
static cJSON *wc_build_tc_msg_openai(wc_turn_t *turn)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON_AddNullToObject(msg, "content");

   cJSON *tcs = cJSON_AddArrayToObject(msg, "tool_calls");
   for (int i = 0; i < turn->tool_call_count; i++)
   {
      wc_tool_call_t *tc = &turn->tool_calls[i];
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "id", tc->id);
      cJSON_AddStringToObject(item, "type", "function");
      cJSON *fn = cJSON_AddObjectToObject(item, "function");
      cJSON_AddStringToObject(fn, "name", tc->name);
      cJSON_AddStringToObject(fn, "arguments", tc->arguments ? tc->arguments : "{}");
      cJSON_AddItemToArray(tcs, item);
   }
   return msg;
}

/* Build assistant tool-calls message for Anthropic history */
static cJSON *wc_build_tc_msg_anthropic(wc_turn_t *turn)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON *content_arr = cJSON_AddArrayToObject(msg, "content");

   if (turn->content && turn->content_len > 0)
   {
      cJSON *tb = cJSON_CreateObject();
      cJSON_AddStringToObject(tb, "type", "text");
      cJSON_AddStringToObject(tb, "text", turn->content);
      cJSON_AddItemToArray(content_arr, tb);
   }

   for (int i = 0; i < turn->tool_call_count; i++)
   {
      wc_tool_call_t *tc = &turn->tool_calls[i];
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "tool_use");
      cJSON_AddStringToObject(block, "id", tc->id);
      cJSON_AddStringToObject(block, "name", tc->name);
      cJSON *input = cJSON_Parse(tc->arguments ? tc->arguments : "{}");
      if (!input)
         input = cJSON_CreateObject();
      cJSON_AddItemToObject(block, "input", input);
      cJSON_AddItemToArray(content_arr, block);
   }
   return msg;
}

/* Execute tool calls, sending events to browser, updating conversation history */
static int wc_execute_tools(wc_session_t *s, wc_turn_t *turn)
{
   /* Add assistant message with tool calls to history */
   cJSON *assist_msg;
   if (s->provider == WC_PROVIDER_ANTHROPIC)
      assist_msg = wc_build_tc_msg_anthropic(turn);
   else
      assist_msg = wc_build_tc_msg_openai(turn);
   cJSON_AddItemToArray(s->messages, assist_msg);

   /* For Anthropic, tool results go in a single user message */
   cJSON *anth_user_msg = NULL;
   cJSON *anth_results = NULL;
   if (s->provider == WC_PROVIDER_ANTHROPIC)
   {
      anth_user_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(anth_user_msg, "role", "user");
      anth_results = cJSON_AddArrayToObject(anth_user_msg, "content");
   }

   for (int i = 0; i < turn->tool_call_count; i++)
   {
      wc_tool_call_t *tc = &turn->tool_calls[i];

      /* Send tool_start event */
      char *esc_name = json_escape(tc->name);
      char *esc_args = json_escape(tc->arguments ? tc->arguments : "{}");
      char evt[WC_MAX_REQUEST];
      snprintf(evt, sizeof(evt), "{\"name\":\"%s\",\"args\":\"%s\"}", esc_name, esc_args);
      sse_send(turn->ssl, "tool_start", evt);
      free(esc_name);
      free(esc_args);

      /* Execute the tool */
      char *result =
          dispatch_tool_call(tc->name, tc->arguments ? tc->arguments : "{}", WC_CHAT_TIMEOUT_MS);

      /* Send tool_result event */
      char *esc_result = json_escape(result ? result : "error: tool execution failed");
      /* Result can be large, allocate dynamically */
      size_t evt_len = strlen(esc_result) + strlen(tc->name) + 64;
      char *evt_buf = malloc(evt_len);
      if (evt_buf)
      {
         snprintf(evt_buf, evt_len, "{\"name\":\"%s\",\"result\":\"%s\"}", tc->name, esc_result);
         sse_send(turn->ssl, "tool_result", evt_buf);
         free(evt_buf);
      }
      free(esc_result);

      /* Add tool result to conversation */
      if (s->provider == WC_PROVIDER_ANTHROPIC)
      {
         cJSON *tr = cJSON_CreateObject();
         cJSON_AddStringToObject(tr, "type", "tool_result");
         cJSON_AddStringToObject(tr, "tool_use_id", tc->id);
         cJSON_AddStringToObject(tr, "content", result ? result : "error: tool execution failed");
         cJSON_AddItemToArray(anth_results, tr);
      }
      else
      {
         cJSON *tool_msg = cJSON_CreateObject();
         cJSON_AddStringToObject(tool_msg, "role", "tool");
         cJSON_AddStringToObject(tool_msg, "tool_call_id", tc->id);
         cJSON_AddStringToObject(tool_msg, "content",
                                 result ? result : "error: tool execution failed");
         cJSON_AddItemToArray(s->messages, tool_msg);
      }

      free(result);

      if (turn->aborted)
      {
         if (anth_user_msg)
            cJSON_AddItemToArray(s->messages, anth_user_msg);
         return -1;
      }
   }

   if (anth_user_msg)
      cJSON_AddItemToArray(s->messages, anth_user_msg);
   return 0;
}

/* Add assistant text to history */
static void wc_add_assistant_text(wc_session_t *s, wc_turn_t *turn)
{
   cJSON *assist = cJSON_CreateObject();
   cJSON_AddStringToObject(assist, "role", "assistant");

   if (s->provider == WC_PROVIDER_ANTHROPIC)
   {
      cJSON *ca = cJSON_AddArrayToObject(assist, "content");
      cJSON *tb = cJSON_CreateObject();
      cJSON_AddStringToObject(tb, "type", "text");
      cJSON_AddStringToObject(tb, "text", turn->content);
      cJSON_AddItemToArray(ca, tb);
   }
   else
   {
      cJSON_AddStringToObject(assist, "content", turn->content);
   }

   cJSON_AddItemToArray(s->messages, assist);
}

/* === CLAUDE CLI FORWARDING === */

static void send_sse_headers(SSL *ssl);

#define WC_CLAUDE_LINE_MAX (256 * 1024)

static void wc_chat_via_claude(wc_session_t *s, SSL *ssl, int client_fd, const char *message)
{
   send_sse_headers(ssl);
   int flag = 1;
   setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

   /* Connect to aimee-server */
   cli_conn_t conn;
   if (cli_connect(&conn, NULL) != 0 || cli_authenticate(&conn) != 0)
   {
      cli_close(&conn);
      sse_send(ssl, "error", "{\"message\":\"cannot connect to aimee-server\"}");
      sse_send(ssl, "done", "{}");
      return;
   }

   /* Build chat.send_stream request */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "chat.send_stream");
   cJSON_AddStringToObject(req, "message", message);
   if (s->claude_session_id[0])
      cJSON_AddStringToObject(req, "claude_session_id", s->claude_session_id);

   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!req_str)
   {
      cli_close(&conn);
      sse_send(ssl, "error", "{\"message\":\"out of memory\"}");
      sse_send(ssl, "done", "{}");
      return;
   }

   /* Send request */
   size_t rlen = strlen(req_str);
   size_t written = 0;
   while (written < rlen)
   {
      ssize_t w = write(conn.fd, req_str + written, rlen - written);
      if (w <= 0)
         break;
      written += (size_t)w;
   }
   if (write(conn.fd, "\n", 1) < 0)
   { /* ignore */
   }
   free(req_str);

   /* Read streaming events and forward as SSE */
   char buf[65536] = {0};
   size_t buf_len = 0;

   for (;;)
   {
      /* Check for complete lines in buffer */
      char *nl = memchr(buf, '\n', buf_len);
      if (nl)
      {
         size_t line_len = (size_t)(nl - buf);
         buf[line_len] = '\0';

         cJSON *evt = cJSON_Parse(buf);
         if (evt)
         {
            cJSON *jev = cJSON_GetObjectItem(evt, "event");
            const char *ev = (jev && cJSON_IsString(jev)) ? jev->valuestring : "";

            if (strcmp(ev, "text") == 0)
            {
               cJSON *jc = cJSON_GetObjectItem(evt, "content");
               if (jc && cJSON_IsString(jc) && jc->valuestring[0])
               {
                  char *escaped = json_escape(jc->valuestring);
                  size_t elen = strlen(escaped) + 32;
                  char *data = malloc(elen);
                  if (data)
                  {
                     snprintf(data, elen, "{\"content\":\"%s\"}", escaped);
                     sse_send(ssl, "text", data);
                     free(data);
                  }
                  free(escaped);
               }
            }
            else if (strcmp(ev, "turn_start") == 0)
               sse_send(ssl, "turn_start", "{}");
            else if (strcmp(ev, "turn_end") == 0)
               sse_send(ssl, "turn_end", "{}");
            else if (strcmp(ev, "session") == 0)
            {
               cJSON *jid = cJSON_GetObjectItem(evt, "id");
               if (jid && cJSON_IsString(jid))
               {
                  snprintf(s->claude_session_id, sizeof(s->claude_session_id), "%s",
                           jid->valuestring);
                  char *esc = json_escape(jid->valuestring);
                  char data[320];
                  snprintf(data, sizeof(data), "{\"id\":\"%s\"}", esc);
                  sse_send(ssl, "session", data);
                  free(esc);
               }
            }
            else if (strcmp(ev, "done") == 0)
            {
               cJSON_Delete(evt);
               break;
            }
            else if (strcmp(ev, "error") == 0)
            {
               cJSON *jm = cJSON_GetObjectItem(evt, "message");
               char data[512];
               snprintf(data, sizeof(data), "{\"message\":\"%s\"}",
                        (jm && cJSON_IsString(jm)) ? jm->valuestring : "server error");
               sse_send(ssl, "error", data);
            }

            cJSON_Delete(evt);
         }

         /* Shift remaining data */
         size_t remain = buf_len - line_len - 1;
         if (remain > 0)
            memmove(buf, nl + 1, remain);
         buf_len = remain;
         continue;
      }

      /* Read more data */
      if (buf_len >= sizeof(buf) - 1)
      {
         buf_len = 0; /* Drop oversized line */
         continue;
      }

      struct pollfd pfd = {.fd = conn.fd, .events = POLLIN};
      int rc = poll(&pfd, 1, WC_CHAT_TIMEOUT_MS);
      if (rc <= 0)
         break;

      ssize_t n = read(conn.fd, buf + buf_len, sizeof(buf) - 1 - buf_len);
      if (n <= 0)
         break;
      buf_len += (size_t)n;
   }

   cli_close(&conn);
   sse_send(ssl, "done", "{}");
}

/* === EMBEDDED HTML === */

static const char *login_html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>aimee - login</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui;background:#111;color:#eee;display:flex;"
    "justify-content:center;align-items:center;min-height:100vh}"
    ".card{background:#1a1a2e;border-radius:12px;padding:32px;width:360px;"
    "border:1px solid #333}"
    "h1{font-size:22px;color:#8cf;margin-bottom:24px;text-align:center}"
    "label{display:block;font-size:13px;color:#888;margin-bottom:4px}"
    "input{width:100%;padding:10px 12px;background:#111;color:#eee;border:1px solid #444;"
    "border-radius:6px;font-size:14px;margin-bottom:16px}"
    "input:focus{outline:none;border-color:#8cf}"
    "button{width:100%;padding:10px;background:#234;color:#8cf;border:1px solid #456;"
    "border-radius:6px;font-size:14px;cursor:pointer}"
    "button:hover{background:#345}"
    ".error{color:#f44;font-size:13px;margin-bottom:12px;text-align:center}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>aimee</h1>"
    "<div id='err' class='error'></div>"
    "<form method='POST' action='/login'>"
    "<label>Username</label>"
    "<input name='username' type='text' autocomplete='username' autofocus required>"
    "<label>Password</label>"
    "<input name='password' type='password' autocomplete='current-password' required>"
    "<button type='submit'>Sign in</button>"
    "</form></div>"
    "<script>"
    "if(location.search.includes('error=1'))"
    "document.getElementById('err').textContent='Invalid credentials';"
    "</script></body></html>";

static const char *chat_html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>aimee - chat</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui;background:#111;color:#eee;height:100vh;display:flex;"
    "flex-direction:column}"
    "nav{background:#1a1a2e;padding:10px 20px;display:flex;align-items:center;gap:16px;"
    "border-bottom:1px solid #333}"
    "nav a{color:#8cf;text-decoration:none;font-size:14px}"
    "nav a:hover{text-decoration:underline}"
    "nav a.active{font-weight:bold;color:#fff}"
    "nav .title{font-size:16px;font-weight:bold;color:#8cf;margin-right:auto}"
    "#messages{flex:1;overflow-y:auto;padding:16px;display:flex;flex-direction:column;gap:12px}"
    ".msg{max-width:80%;padding:10px 14px;border-radius:10px;font-size:14px;"
    "line-height:1.5;white-space:pre-wrap;word-wrap:break-word}"
    ".msg.user{align-self:flex-end;background:#1a3a5c;color:#cdf;border-bottom-right-radius:2px}"
    ".msg.assistant{align-self:flex-start;background:#1a1a2e;color:#eee;"
    "border-bottom-left-radius:2px}"
    ".msg.assistant pre{background:#0a0a1a;padding:8px 10px;border-radius:4px;"
    "overflow-x:auto;margin:6px 0;font-size:13px}"
    ".msg.assistant code{background:#0a0a1a;padding:1px 4px;border-radius:3px;font-size:13px}"
    ".tool{margin:4px 0;padding:8px 10px;background:#0d1117;border-left:3px solid #456;"
    "border-radius:4px;font-size:12px;color:#888}"
    ".tool summary{cursor:pointer;color:#8cf;font-size:12px}"
    ".tool pre{margin-top:6px;font-size:11px;color:#aaa;max-height:200px;overflow-y:auto;"
    "white-space:pre-wrap}"
    "#input-area{padding:12px 16px;background:#1a1a2e;border-top:1px solid #333;"
    "display:flex;gap:8px}"
    "#input-area textarea{flex:1;padding:10px;background:#111;color:#eee;border:1px solid #444;"
    "border-radius:6px;font-size:14px;resize:none;font-family:system-ui;min-height:44px;"
    "max-height:200px}"
    "#input-area textarea:focus{outline:none;border-color:#8cf}"
    "#input-area button{padding:10px 20px;background:#234;color:#8cf;border:1px solid #456;"
    "border-radius:6px;cursor:pointer;font-size:14px;white-space:nowrap}"
    "#input-area button:hover{background:#345}"
    "#input-area button:disabled{opacity:0.5;cursor:not-allowed}"
    ".typing{color:#666;font-style:italic;font-size:13px;padding:4px 14px}"
    ".working{display:flex;align-items:center;gap:8px;padding:8px 14px;color:#8cf;"
    "font-size:13px}"
    ".working .dots{display:inline-flex;gap:4px}"
    ".working .dots span{width:6px;height:6px;border-radius:50%;background:#8cf;"
    "animation:pulse 1.4s infinite ease-in-out}"
    ".working .dots span:nth-child(2){animation-delay:0.2s}"
    ".working .dots span:nth-child(3){animation-delay:0.4s}"
    "@keyframes pulse{0%,80%,100%{opacity:0.2;transform:scale(0.8)}"
    "40%{opacity:1;transform:scale(1)}}"
    "#tab-bar{display:flex;background:#151525;border-bottom:1px solid #333;padding:0 8px;"
    "align-items:stretch;gap:0;overflow-x:auto;flex-shrink:0}"
    ".tab{padding:8px 16px;font-size:13px;color:#888;cursor:pointer;border:none;"
    "background:transparent;display:flex;align-items:center;gap:6px;white-space:nowrap;"
    "border-bottom:2px solid transparent;min-width:0}"
    ".tab:hover{color:#ccc;background:#1a1a2e}"
    ".tab.active{color:#8cf;border-bottom-color:#8cf;background:#1a1a2e}"
    ".tab .close{font-size:11px;color:#666;cursor:pointer;padding:2px 4px;border-radius:3px}"
    ".tab .close:hover{color:#f88;background:#333}"
    ".tab-title{max-width:120px;overflow:hidden;text-overflow:ellipsis}"
    "#new-tab{padding:8px 12px;font-size:16px;color:#666;cursor:pointer;border:none;"
    "background:transparent;line-height:1}"
    "#new-tab:hover{color:#8cf}"
    "</style></head><body>"
    "<nav>"
    "<span class='title'>aimee</span>"
    "<a href='/chat' class='active'>Chat</a>"
    "<a href='/dashboard'>Dashboard</a>"
    "<a href='/logout'>Logout</a>"
    "</nav>"
    "<div id='tab-bar'>"
    "<button id='new-tab' onclick='newTab()' title='New tab'>+</button>"
    "</div>"
    "<div id='messages'></div>"
    "<div id='input-area'>"
    "<textarea id='input' placeholder='Type a message... (Shift+Enter for newline)'"
    " rows='1'></textarea>"
    "<button id='send' onclick='sendMessage()'>Send</button>"
    "</div>"
    "<script>"
    "const msgs=document.getElementById('messages');"
    "const input=document.getElementById('input');"
    "const sendBtn=document.getElementById('send');"
    "let sending=false;"
    ""
    /* --- Tab management --- */
    "let tabs=[];"
    "let activeIdx=0;"
    ""
    "function saveTabs(){"
    "localStorage.setItem('aimee_chat_tabs',JSON.stringify(tabs));}"
    ""
    "function loadTabs(){"
    "try{tabs=JSON.parse(localStorage.getItem('aimee_chat_tabs')||'[]');"
    "}catch(e){tabs=[];}"
    "if(!tabs.length)tabs=[{title:'Chat',messages:[],sid:''}];"
    "activeIdx=Math.min(activeIdx,tabs.length-1);"
    "renderTabs();renderMessages();}"
    ""
    "function renderTabs(){"
    "const bar=document.getElementById('tab-bar');"
    "const btn=document.getElementById('new-tab');"
    "bar.querySelectorAll('.tab').forEach(function(t){t.remove();});"
    "tabs.forEach(function(t,i){"
    "const el=document.createElement('div');"
    "el.className='tab'+(i===activeIdx?' active':'');"
    "el.innerHTML='<span class=\"tab-title\">'+escHtml(t.title)+'</span>'"
    "+(tabs.length>1?'<span class=\"close\" "
    "onclick=\"event.stopPropagation();closeTab('+i+')\">x</span>':'');"
    "el.onclick=function(){switchTab(i);};"
    "bar.insertBefore(el,btn);});}"
    ""
    "function switchTab(i){"
    "if(i===activeIdx)return;"
    "saveCurrentTab();"
    "activeIdx=i;"
    "renderTabs();renderMessages();}"
    ""
    "function newTab(){"
    "saveCurrentTab();"
    "tabs.push({title:'Chat '+(tabs.length+1),messages:[],sid:''});"
    "activeIdx=tabs.length-1;"
    "saveTabs();renderTabs();renderMessages();input.focus();}"
    ""
    "function closeTab(i){"
    "if(tabs.length<=1)return;"
    "tabs.splice(i,1);"
    "if(activeIdx>=tabs.length)activeIdx=tabs.length-1;"
    "saveTabs();renderTabs();renderMessages();}"
    ""
    "function saveCurrentTab(){"
    "if(!tabs[activeIdx])return;"
    "let items=[];"
    "msgs.querySelectorAll('.msg').forEach(function(el){"
    "items.push({role:el.classList.contains('user')?'user':'assistant',"
    "text:el.getAttribute('data-raw')||''});});"
    "tabs[activeIdx].messages=items;"
    "saveTabs();}"
    ""
    "function renderMessages(){"
    "msgs.innerHTML='';"
    "if(!tabs[activeIdx])return;"
    "tabs[activeIdx].messages.forEach(function(m){"
    "if(!m.text)return;"
    "addMsg(m.role,m.text);});"
    "if(tabs[activeIdx].messages.length)msgs.scrollTop=msgs.scrollHeight;}"
    ""
    "function saveHistory(){saveCurrentTab();}"
    ""
    /* --- Init --- */
    "async function initSession(){"
    "try{"
    "let r=await fetch('/api/chat/session');"
    "let s=await r.json();"
    "if(s.csrf)window._csrf=s.csrf;"
    "}catch(e){}}"
    "initSession();"
    "loadTabs();"
    ""
    "input.addEventListener('keydown',function(e){"
    "if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendMessage();}});"
    ""
    "input.addEventListener('input',function(){"
    "this.style.height='auto';"
    "this.style.height=Math.min(this.scrollHeight,200)+'px';});"
    ""
    "function escHtml(s){"
    "return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
    ""
    "function renderMd(text){"
    "let h=escHtml(text);"
    "h=h.replace(/```([\\s\\S]*?)```/g,function(m,c){return '<pre>'+c+'</pre>';});"
    "h=h.replace(/`([^`]+)`/g,'<code>$1</code>');"
    "h=h.replace(/\\*\\*([^*]+)\\*\\*/g,'<strong>$1</strong>');"
    "return h;}"
    ""
    "function addMsg(role,content){"
    "const d=document.createElement('div');"
    "d.className='msg '+role;"
    "d.setAttribute('data-raw',content);"
    "d.innerHTML=renderMd(content);"
    "msgs.appendChild(d);"
    "msgs.scrollTop=msgs.scrollHeight;"
    "return d;}"
    ""
    "function addTool(name,args,result){"
    "const d=document.createElement('details');"
    "d.className='tool';"
    "d.innerHTML='<summary>Tool: '+escHtml(name)+'</summary>'+"
    "'<pre>'+escHtml(args)+'</pre>'+"
    "(result?'<pre>'+escHtml(result)+'</pre>':'');"
    "msgs.appendChild(d);"
    "msgs.scrollTop=msgs.scrollHeight;"
    "return d;}"
    ""
    "async function sendMessage(){"
    "if(sending)return;"
    "const text=input.value.trim();"
    "if(!text)return;"
    "input.value='';"
    "input.style.height='auto';"
    "sending=true;"
    "sendBtn.disabled=true;"
    ""
    /* Auto-title from first message */
    "if(tabs[activeIdx]&&tabs[activeIdx].messages.length===0){"
    "tabs[activeIdx].title=text.substring(0,30)+(text.length>30?'...':'');"
    "renderTabs();}"
    ""
    "addMsg('user',text);"
    "saveHistory();"
    ""
    "function showWorking(){"
    "let w=document.querySelector('.working');"
    "if(!w){w=document.createElement('div');"
    "w.className='working';"
    "w.innerHTML='<div class=\"dots\"><span></span><span></span><span></span></div> Working';"
    "msgs.appendChild(w);msgs.scrollTop=msgs.scrollHeight;}"
    "return w;}"
    "function hideWorking(){"
    "let w=document.querySelector('.working');if(w)w.remove();}"
    ""
    "showWorking();"
    "let assistDiv=null;"
    "let fullText='';"
    ""
    "try{"
    "const resp=await fetch('/api/chat/send',{"
    "method:'POST',"
    "headers:{'Content-Type':'application/json','X-CSRF-Token':window._csrf||''},"
    "body:JSON.stringify({message:text,"
    "claude_session_id:(tabs[activeIdx]&&tabs[activeIdx].sid)||''})});"
    ""
    "if(resp.status===401){window.location='/login';return;}"
    ""
    "const reader=resp.body.getReader();"
    "const decoder=new TextDecoder();"
    "let buf='';"
    ""
    "while(true){"
    "const{done,value}=await reader.read();"
    "if(done)break;"
    "buf+=decoder.decode(value,{stream:true});"
    ""
    "let lines=buf.split('\\n');"
    "buf=lines.pop();"
    ""
    "let evtType=null;"
    "for(let line of lines){"
    "if(line.startsWith('event: ')){evtType=line.slice(7).trim();}"
    "else if(line.startsWith('data: ')&&evtType){"
    "try{"
    "const data=JSON.parse(line.slice(6));"
    "if(evtType==='turn_start'){"
    "hideWorking();"
    "assistDiv=addMsg('assistant','');"
    "fullText='';}"
    "else if(evtType==='text'){"
    "if(!assistDiv){hideWorking();assistDiv=addMsg('assistant','');fullText='';}"
    "fullText+=data.content||'';"
    "assistDiv.setAttribute('data-raw',fullText);"
    "assistDiv.innerHTML=renderMd(fullText);"
    "msgs.scrollTop=msgs.scrollHeight;}"
    "else if(evtType==='turn_end'){"
    "saveHistory();"
    "assistDiv=null;"
    "showWorking();}"
    "else if(evtType==='error'){"
    "hideWorking();"
    "if(!assistDiv){assistDiv=addMsg('assistant','');fullText='';}"
    "fullText+='\\n\\n**Error:** '+(data.message||'Unknown error');"
    "assistDiv.setAttribute('data-raw',fullText);"
    "assistDiv.innerHTML=renderMd(fullText);}"
    "else if(evtType==='session'){"
    "if(tabs[activeIdx])tabs[activeIdx].sid=data.id||'';"
    "saveTabs();}"
    "else if(evtType==='done'){"
    "hideWorking();"
    "saveHistory();}"
    "}catch(e){}"
    "evtType=null;}}"
    "}"
    "}catch(e){"
    "hideWorking();"
    "if(!assistDiv){assistDiv=addMsg('assistant','');fullText='';}"
    "fullText+='\\n\\n**Connection error:** '+e.message;"
    "assistDiv.setAttribute('data-raw',fullText);"
    "assistDiv.innerHTML=renderMd(fullText);"
    "saveHistory();}"
    "finally{"
    "sending=false;"
    "sendBtn.disabled=false;"
    "input.focus();}}"
    ""
    "document.getElementById('input').focus();"
    "document.querySelectorAll('a[href=\"/logout\"]').forEach(function(a){"
    "a.addEventListener('click',function(){"
    "localStorage.removeItem('aimee_chat_tabs');});});"
    "</script></body></html>";

static const char *dashboard_page_html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>aimee - dashboard</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui;background:#111;color:#eee;height:100vh;display:flex;"
    "flex-direction:column;overflow:hidden}"
    "nav{background:#1a1a2e;padding:10px 20px;display:flex;align-items:center;gap:16px;"
    "border-bottom:1px solid #333;flex-shrink:0}"
    "nav a{color:#8cf;text-decoration:none;font-size:14px}"
    "nav a:hover{text-decoration:underline}"
    "nav a.active{font-weight:bold;color:#fff}"
    "nav .title{font-size:16px;font-weight:bold;color:#8cf;margin-right:auto}"
    ".dashboard{flex:1;display:grid;"
    "grid-template-columns:1fr 1fr 1fr;grid-template-rows:1fr 1fr;gap:0;overflow:hidden}"
    ".win{display:flex;flex-direction:column;border:1px solid #333;overflow:hidden;"
    "min-height:0}"
    ".win-head{background:#1a1a2e;padding:6px 12px;font-size:13px;font-weight:bold;"
    "color:#adf;border-bottom:1px solid #333;display:flex;align-items:center;"
    "flex-shrink:0;gap:8px}"
    ".win-head .tag{font-size:10px;color:#888;font-weight:normal}"
    ".win-body{flex:1;overflow-y:auto;padding:8px;font-size:12px}"
    "table{width:100%;border-collapse:collapse}"
    "th{text-align:left;padding:4px 6px;border-bottom:1px solid #444;color:#888;"
    "font-size:11px;position:sticky;top:0;background:#111}"
    "td{padding:4px 6px;border-bottom:1px solid #1a1a1a}"
    ".badge{display:inline-block;padding:1px 6px;border-radius:3px;font-size:10px}"
    ".badge.success{background:#143;color:#4f4}"
    ".badge.error{background:#411;color:#f44}"
    ".badge.running{background:#431;color:#fa0}"
    ".num{text-align:right;font-variant-numeric:tabular-nums}"
    ".log-agent{color:#8cf}.log-decision{color:#fa0}"
    ".log-detail{color:#888;max-width:300px;overflow:hidden;text-overflow:ellipsis;"
    "white-space:nowrap}"
    "#refresh{padding:4px 10px;background:#234;color:#8cf;border:1px solid #456;"
    "border-radius:3px;cursor:pointer;font-size:11px}"
    "</style></head><body>"
    "<nav>"
    "<span class='title'>aimee</span>"
    "<a href='/chat'>Chat</a>"
    "<a href='/dashboard' class='active'>Dashboard</a>"
    "<a href='/logout'>Logout</a>"
    "<button id='refresh' onclick='load()'>Refresh</button>"
    "</nav>"
    "<div class='dashboard'>"
    "<div class='win'><div class='win-head'>Delegations<span class='tag' "
    "id='del-count'></span></div>"
    "<div class='win-body' id='delegations'></div></div>"
    "<div class='win'><div class='win-head'>Metrics</div>"
    "<div class='win-body' id='metrics'></div></div>"
    "<div class='win'><div class='win-head'>Execution Plans</div>"
    "<div class='win-body' id='plans'></div></div>"
    "<div class='win'><div class='win-head'>Traces</div>"
    "<div class='win-body' id='traces'></div></div>"
    "<div class='win'><div class='win-head'>Memory</div>"
    "<div class='win-body' id='memory'></div></div>"
    "<div class='win'><div class='win-head'>Logs<span class='tag' id='log-count'></span></div>"
    "<div class='win-body' id='logs'></div></div>"
    "</div>"
    "<script>"
    "function e(s){if(s==null)return'';return String(s).replace(/&/g,'&amp;')"
    ".replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\\\"/g,'&quot;')}"
    "async function load(){"
    "try{"
    "let[d,m,t,s,p,l]=await Promise.all(["
    "fetch('/api/delegations').then(r=>r.json()),"
    "fetch('/api/metrics').then(r=>r.json()),"
    "fetch('/api/traces').then(r=>r.json()),"
    "fetch('/api/memory-stats').then(r=>r.json()),"
    "fetch('/api/plans').then(r=>r.json()),"
    "fetch('/api/logs').then(r=>r.json())]);"
    "document.getElementById('del-count').textContent='('+d.length+')';"
    "document.getElementById('delegations').innerHTML='<table><tr><th>Agent</th><th>Role</th>"
    "<th>Status</th><th>Turns</th><th>Tools</th><th>Latency</th></tr>'"
    "+d.map(r=>'<tr><td>'+e(r.agent)+'</td><td>'+e(r.role)+'</td><td><span class=\"badge '+"
    "(r.success?'success':'error')+'\">'"
    "+(r.success?'OK':'ERR')+'</span></td><td class=\"num\">'+r.turns+'</td>"
    "<td class=\"num\">'+r.tool_calls+'</td>"
    "<td class=\"num\">'+r.latency_ms+'ms</td></tr>').join('')+'</table>';"
    "document.getElementById('metrics').innerHTML='<table><tr><th>Role</th><th>Total</th>"
    "<th>OK</th><th>Avg Lat</th><th>Tokens</th></tr>'"
    "+m.map(r=>'<tr><td>'+e(r.role)+'</td><td class=\"num\">'+r.total+'</td>"
    "<td class=\"num\">'+r.successes+'</td><td class=\"num\">'+r.avg_latency_ms+'ms</td>"
    "<td class=\"num\">'+r.tokens+'</td></tr>').join('')+'</table>';"
    "document.getElementById('traces').innerHTML='<table><tr><th>Turn</th><th>Tool</th>"
    "<th>Dir</th></tr>'"
    "+t.map(r=>'<tr><td class=\"num\">'+r.turn+'</td><td>'"
    "+e(r.tool_name||'--')+'</td><td>'+e(r.direction)+'</td></tr>').join('')+'</table>';"
    "document.getElementById('memory').innerHTML='<table><tr><th>Tier</th><th>Kind</th>"
    "<th>Count</th></tr>'"
    "+s.map(r=>'<tr><td>'+e(r.tier)+'</td><td>'+e(r.kind)+'</td>"
    "<td class=\"num\">'+r.count+'</td></tr>').join('')+'</table>';"
    "document.getElementById('plans').innerHTML='<table><tr><th>ID</th><th>Agent</th>"
    "<th>Status</th><th>Steps</th><th>Task</th></tr>'"
    "+p.map(r=>'<tr><td class=\"num\">'+r.id+'</td><td>'+e(r.agent)+'</td>"
    "<td><span class=\"badge "
    "'+(r.status==\"done\"?'success':r.status==\"failed\"?'error':'running')"
    "+'\">'+e(r.status)+'</span></td><td class=\"num\">'+r.done_steps+'/'+r.total_steps+'</td>"
    "<td>'+e(r.task.substring(0,60))+'</td></tr>').join('')+'</table>';"
    "document.getElementById('log-count').textContent='('+l.length+')';"
    "document.getElementById('logs').innerHTML='<table><tr><th>Time</th><th>Source</th>"
    "<th>Who</th><th>What</th><th>Detail</th></tr>'"
    "+l.map(r=>'<tr><td>'+e(r.timestamp)+'</td>"
    "<td><span class=\"'+(r.source==='agent'?'log-agent':'log-decision')+'\">"
    "'+e(r.source)+'</span></td>"
    "<td>'+e(r.who)+'</td><td>'+e(r.what)+'</td>"
    "<td class=\"log-detail\" title=\"'+e(r.detail)+'\">'+e(r.detail)+'</td></tr>').join('')"
    "+'</table>';"
    "}catch(err){console.error('load failed',err);}}"
    "load();setInterval(load,15000);"
    "</script></body></html>";

/* === HTTP RESPONSE HELPERS === */

static void send_html(SSL *ssl, int status, const char *body)
{
   ssl_printf(ssl,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: text/html; charset=utf-8\r\n"
              "Content-Length: %d\r\n"
              "Connection: close\r\n\r\n",
              status, status == 200 ? "OK" : (status == 302 ? "Found" : "Unauthorized"),
              (int)strlen(body));
   ssl_write_all(ssl, body, strlen(body));
}

static void send_json(SSL *ssl, const char *json)
{
   ssl_printf(ssl,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %d\r\n"
              "Connection: close\r\n\r\n",
              (int)strlen(json));
   ssl_write_all(ssl, json, strlen(json));
}

static void send_redirect(SSL *ssl, const char *location, const char *set_cookie)
{
   if (set_cookie)
   {
      ssl_printf(ssl,
                 "HTTP/1.1 302 Found\r\n"
                 "Location: %s\r\n"
                 "Set-Cookie: %s\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n\r\n",
                 location, set_cookie);
   }
   else
   {
      ssl_printf(ssl,
                 "HTTP/1.1 302 Found\r\n"
                 "Location: %s\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n\r\n",
                 location);
   }
}

static void send_sse_headers(SSL *ssl)
{
   ssl_printf(ssl, "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/event-stream\r\n"
                   "Cache-Control: no-cache\r\n"
                   "Connection: keep-alive\r\n"
                   "X-Accel-Buffering: no\r\n\r\n");
}

/* --- CSRF validation --- */

static int validate_csrf(const char *request_buf, wc_session_t *session)
{
   /* Check X-CSRF-Token header */
   const char *hdr = strstr(request_buf, "X-CSRF-Token: ");
   if (!hdr)
      hdr = strstr(request_buf, "x-csrf-token: ");
   if (!hdr)
      return 0;
   hdr += 14;
   /* Compare token up to CRLF */
   size_t tlen = strlen(session->csrf_token);
   if (strncmp(hdr, session->csrf_token, tlen) == 0 &&
       (hdr[tlen] == '\r' || hdr[tlen] == '\n' || hdr[tlen] == '\0'))
      return 1;
   return 0;
}

/* === REQUEST HANDLER === */

static void handle_request(SSL *ssl, int client_fd)
{
   char buf[WC_MAX_REQUEST];
   ssize_t total = read_request(ssl, buf, sizeof(buf));
   if (total <= 0)
      return;

   /* Parse method and path */
   char method[16] = {0}, path[512] = {0};
   sscanf(buf, "%15s %511s", method, path);

   /* Strip query string for routing */
   char *qmark = strchr(path, '?');
   char query[512] = {0};
   if (qmark)
   {
      snprintf(query, sizeof(query), "%s", qmark + 1);
      *qmark = '\0';
   }

   /* URL-decode path */
   url_decode(path);

   /* Reject path traversal */
   if (strstr(path, ".."))
   {
      send_html(ssl, 400, "Bad Request");
      return;
   }

   /* --- Public routes (no auth required) --- */

   if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0)
   {
      send_redirect(ssl, "/chat", NULL);
      return;
   }

   if (strcmp(path, "/login") == 0)
   {
      if (strcmp(method, "GET") == 0)
      {
         send_html(ssl, 200, login_html);
         return;
      }
      if (strcmp(method, "POST") == 0)
      {
         char *body = strstr(buf, "\r\n\r\n");
         if (!body)
         {
            send_html(ssl, 400, "Bad Request");
            return;
         }
         body += 4;

         char username[64] = {0}, password[256] = {0};
         parse_form_field(body, "username", username, sizeof(username));
         parse_form_field(body, "password", password, sizeof(password));

#ifdef __linux__
         if (!username[0] || !password[0] || !pam_check_credentials(username, password))
         {
            send_redirect(ssl, "/login?error=1", NULL);
            return;
         }
#else
         /* PAM not available: reject all logins rather than allowing unauthenticated access */
         (void)password;
         if (!username[0])
         {
            send_redirect(ssl, "/login?error=1", NULL);
            return;
         }
         send_html(ssl, 500, "Authentication not available on this platform (requires Linux/PAM)");
         return;
#endif

         wc_session_t *s = session_create(username);
         char cookie[256];
         snprintf(cookie, sizeof(cookie),
                  "aimee_session=%s; Path=/; HttpOnly; SameSite=Strict; Secure", s->token);
         send_redirect(ssl, "/chat", cookie);
         return;
      }
      send_html(ssl, 405, "Method Not Allowed");
      return;
   }

   /* --- All other routes require authentication --- */

   char token[WC_TOKEN_LEN + 1] = {0};
   extract_cookie(buf, "aimee_session", token, sizeof(token));
   wc_session_t *session = session_lookup(token);

   if (!session)
   {
      send_redirect(ssl, "/login", NULL);
      return;
   }

   /* Logout */
   if (strcmp(path, "/logout") == 0)
   {
      pthread_mutex_lock(&sessions_mutex);
      session->active = 0;
      cJSON_Delete(session->messages);
      cJSON_Delete(session->tools);
      free(session->system_prompt);
      session->messages = NULL;
      session->tools = NULL;
      session->system_prompt = NULL;
      pthread_mutex_unlock(&sessions_mutex);

      send_redirect(ssl, "/login",
                    "aimee_session=; Path=/; HttpOnly; SameSite=Strict; Secure; Max-Age=0");
      return;
   }

   /* Chat session state (for client-side history sync) */
   if (strcmp(path, "/api/chat/session") == 0 && strcmp(method, "GET") == 0)
   {
      /* If server lost state (restart) but client has it in query param, restore */
      if (!session->claude_session_id[0] && query[0])
      {
         char csid[256] = {0};
         const char *p = strstr(query, "sid=");
         if (p)
         {
            p += 4;
            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len < sizeof(csid))
            {
               memcpy(csid, p, len);
               snprintf(session->claude_session_id, sizeof(session->claude_session_id), "%s", csid);
            }
         }
      }
      char json[512];
      snprintf(json, sizeof(json), "{\"session_id\":\"%s\",\"csrf\":\"%s\"}",
               session->claude_session_id, session->csrf_token);
      send_json(ssl, json);
      return;
   }

   /* Chat page */
   if (strcmp(path, "/chat") == 0 && strcmp(method, "GET") == 0)
   {
      send_html(ssl, 200, chat_html);
      return;
   }

   /* Dashboard page */
   if (strcmp(path, "/dashboard") == 0 && strcmp(method, "GET") == 0)
   {
      send_html(ssl, 200, dashboard_page_html);
      return;
   }

   /* Chat clear */
   if (strcmp(path, "/api/chat/clear") == 0 && strcmp(method, "POST") == 0)
   {
      if (!validate_csrf(buf, session))
      {
         send_json(ssl, "{\"error\":\"CSRF token invalid\"}");
         return;
      }
      pthread_mutex_lock(&session->lock);
      cJSON_Delete(session->messages);
      session->messages = cJSON_CreateArray();

      /* Re-init system prompt */
      char *sys = wc_build_system_prompt();
      if (sys)
      {
         if (session->provider == WC_PROVIDER_ANTHROPIC)
         {
            free(session->system_prompt);
            session->system_prompt = sys;
         }
         else
         {
            cJSON *sm = cJSON_CreateObject();
            cJSON_AddStringToObject(sm, "role", "system");
            cJSON_AddStringToObject(sm, "content", sys);
            cJSON_AddItemToArray(session->messages, sm);
            free(sys);
         }
      }
      pthread_mutex_unlock(&session->lock);

      send_json(ssl, "{\"ok\":true}");
      return;
   }

   /* Chat send (POST returns SSE stream) */
   if (strcmp(path, "/api/chat/send") == 0 && strcmp(method, "POST") == 0)
   {
      if (!validate_csrf(buf, session))
      {
         send_json(ssl, "{\"error\":\"CSRF token invalid\"}");
         return;
      }
      char *body = strstr(buf, "\r\n\r\n");
      if (!body)
      {
         send_json(ssl, "{\"error\":\"no body\"}");
         return;
      }
      body += 4;

      cJSON *req = cJSON_Parse(body);
      if (!req)
      {
         send_json(ssl, "{\"error\":\"invalid json\"}");
         return;
      }

      cJSON *msg_j = cJSON_GetObjectItem(req, "message");
      if (!msg_j || !cJSON_IsString(msg_j) || !msg_j->valuestring[0])
      {
         cJSON_Delete(req);
         send_json(ssl, "{\"error\":\"empty message\"}");
         return;
      }

      /* Lock session for the duration of the AI turn */
      pthread_mutex_lock(&session->lock);

      /* Claude CLI mode: forward to claude process */
      if (session->provider == WC_PROVIDER_CLAUDE)
      {
         /* Restore claude session from client if server lost it (restart) */
         if (!session->claude_session_id[0])
         {
            cJSON *csid = cJSON_GetObjectItem(req, "claude_session_id");
            if (csid && cJSON_IsString(csid) && csid->valuestring[0])
               snprintf(session->claude_session_id, sizeof(session->claude_session_id), "%s",
                        csid->valuestring);
         }
         char *msg_copy = safe_strdup(msg_j->valuestring);
         cJSON_Delete(req);
         wc_chat_via_claude(session, ssl, client_fd, msg_copy);
         free(msg_copy);
         pthread_mutex_unlock(&session->lock);
         return;
      }

      if (!session->auth_header[0])
      {
         pthread_mutex_unlock(&session->lock);
         cJSON_Delete(req);
         send_json(ssl, "{\"error\":\"no API key configured\"}");
         return;
      }

      /* Add user message to history */
      cJSON *user_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(user_msg, "role", "user");
      cJSON_AddStringToObject(user_msg, "content", msg_j->valuestring);
      cJSON_AddItemToArray(session->messages, user_msg);
      cJSON_Delete(req);

      /* Start SSE response */
      send_sse_headers(ssl);

      /* Set TCP_NODELAY for low-latency streaming */
      int flag = 1;
      setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

      /* Allocate turn state */
      wc_turn_t turn;
      memset(&turn, 0, sizeof(turn));
      turn.ssl = ssl;
      turn.provider = session->provider;
      turn.sse_buf = malloc(WC_SSE_BUF_SIZE);
      if (!turn.sse_buf)
      {
         sse_send(ssl, "error", "{\"message\":\"out of memory\"}");
         sse_send(ssl, "done", "{}");
         pthread_mutex_unlock(&session->lock);
         return;
      }

      /* Multi-turn loop (may iterate for tool calls) */
      for (;;)
      {
         wc_turn_reset(&turn);
         int http_rc = wc_chat_send(session, &turn);

         if (turn.aborted || http_rc < 0)
         {
            if (turn.content && turn.content_len > 0)
               wc_add_assistant_text(session, &turn);
            sse_send(ssl, "error", "{\"message\":\"network error\"}");
            break;
         }

         if (http_rc >= 400)
         {
            char err_evt[1024];
            char *esc = json_escape(turn.error_buf ? turn.error_buf : "API error");
            snprintf(err_evt, sizeof(err_evt), "{\"message\":\"%s\"}", esc);
            sse_send(ssl, "error", err_evt);
            free(esc);
            break;
         }

         /* Check for tool calls */
         int is_tool_call = 0;
         if (session->provider == WC_PROVIDER_ANTHROPIC)
            is_tool_call =
                (turn.tool_call_count > 0 && strcmp(turn.finish_reason, "tool_use") == 0);
         else
            is_tool_call =
                (turn.tool_call_count > 0 && strcmp(turn.finish_reason, "tool_calls") == 0);

         if (is_tool_call)
         {
            if (wc_execute_tools(session, &turn) < 0)
            {
               sse_send(ssl, "error", "{\"message\":\"tool execution interrupted\"}");
               break;
            }
            continue; /* Next AI turn */
         }

         /* Normal text response */
         if (turn.content && turn.content_len > 0)
            wc_add_assistant_text(session, &turn);
         break;
      }

      sse_send(ssl, "done", "{}");
      wc_turn_reset(&turn);
      free(turn.sse_buf);
      pthread_mutex_unlock(&session->lock);
      return;
   }

   /* Dashboard API endpoints */
   if (strncmp(path, "/api/", 5) == 0 && strcmp(method, "GET") == 0)
   {
      sqlite3 *db = db_open(NULL);
      if (!db)
      {
         send_json(ssl, "[]");
         return;
      }

      char *json = NULL;
      if (strcmp(path, "/api/delegations") == 0)
         json = api_delegations(db);
      else if (strcmp(path, "/api/metrics") == 0)
         json = api_metrics(db);
      else if (strcmp(path, "/api/traces") == 0)
         json = api_traces(db);
      else if (strcmp(path, "/api/memory-stats") == 0)
         json = api_memory_stats(db);
      else if (strcmp(path, "/api/plans") == 0)
         json = api_plans(db);
      else if (strcmp(path, "/api/logs") == 0)
         json = api_logs(db);

      if (json)
      {
         send_json(ssl, json);
         free(json);
      }
      else
      {
         send_html(ssl, 404, "Not Found");
      }

      sqlite3_close(db);
      return;
   }

   send_html(ssl, 404, "Not Found");
}

/* === CONNECTION THREAD === */

static void http_redirect(int fd)
{
   /* Read the full HTTP request (drain it) */
   char raw[2048] = {0};
   recv(fd, raw, sizeof(raw) - 1, 0);

   /* Use fixed localhost redirect to prevent Host header injection */
   char redirect[512];
   snprintf(redirect, sizeof(redirect),
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: https://127.0.0.1:%d/\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n",
            g_webchat_port);
   send(fd, redirect, strlen(redirect), 0);
}

static void *connection_thread(void *arg)
{
   wc_conn_t *conn = (wc_conn_t *)arg;

   /* Peek at first byte to detect plain HTTP vs TLS.
    * TLS records start with 0x16 (handshake); HTTP starts with ASCII. */
   unsigned char first = 0;
   ssize_t n = recv(conn->client_fd, &first, 1, MSG_PEEK);
   if (n <= 0)
   {
      close(conn->client_fd);
      free(conn);
      return NULL;
   }

   if (first != 0x16)
   {
      /* Not a TLS handshake — likely plain HTTP, send redirect */
      http_redirect(conn->client_fd);
      close(conn->client_fd);
      free(conn);
      return NULL;
   }

   SSL *ssl = SSL_new(g_ssl_ctx);
   if (!ssl)
   {
      close(conn->client_fd);
      free(conn);
      return NULL;
   }

   SSL_set_fd(ssl, conn->client_fd);

   if (SSL_accept(ssl) <= 0)
   {
      SSL_free(ssl);
      close(conn->client_fd);
      free(conn);
      return NULL;
   }

   handle_request(ssl, conn->client_fd);

   SSL_shutdown(ssl);
   SSL_free(ssl);
   close(conn->client_fd);
   free(conn);
   return NULL;
}

/* === MAIN SERVER LOOP === */

void webchat_serve(int port)
{
   if (port <= 0)
      port = WC_DEFAULT_PORT;
   g_webchat_port = port;

   /* Initialize HTTP (for outbound AI API calls) */
   agent_http_init();

   /* Initialize TLS */
   g_ssl_ctx = webchat_tls_init();

   /* Initialize network ACL */
   webchat_acl_init();

   /* Write system prompt file for Claude CLI */
   wc_write_system_prompt_file();

   /* Print allowed subnets */
   fprintf(stderr, "Allowed networks:\n");
   for (int i = 0; i < acl_count; i++)
   {
      struct in_addr net, mask;
      net.s_addr = htonl(acl_entries[i].network);
      mask.s_addr = htonl(acl_entries[i].mask);
      char net_str[INET_ADDRSTRLEN], mask_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &net, net_str, sizeof(net_str));
      inet_ntop(AF_INET, &mask, mask_str, sizeof(mask_str));
      fprintf(stderr, "  %s/%s\n", net_str, mask_str);
   }

   /* Create listening socket */
   int srv = socket(AF_INET, SOCK_STREAM, 0);
   if (srv < 0)
      fatal("socket failed");

   int opt = 1;
   setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

   struct sockaddr_in addr;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = htons((uint16_t)port);

   if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0)
      fatal("bind failed on port %d", port);

   if (listen(srv, 16) < 0)
      fatal("listen failed");

   fprintf(stderr, "aimee webchat: https://127.0.0.1:%d\n", port);
   fflush(stderr);

   time_t last_reap = time(NULL);

   while (1)
   {
      struct sockaddr_in client_addr;
      socklen_t client_len = sizeof(client_addr);
      int client = accept(srv, (struct sockaddr *)&client_addr, &client_len);
      if (client < 0)
         continue;

      /* ACL check */
      if (!webchat_acl_check(&client_addr))
      {
         close(client);
         continue;
      }

      /* Reap expired sessions periodically */
      time_t now = time(NULL);
      if (now - last_reap > 60)
      {
         session_reap();
         last_reap = now;
      }

      /* Spawn thread */
      wc_conn_t *conn = malloc(sizeof(wc_conn_t));
      if (!conn)
      {
         close(client);
         continue;
      }
      conn->client_fd = client;

      pthread_t tid;
      if (pthread_create(&tid, NULL, connection_thread, conn) != 0)
      {
         close(client);
         free(conn);
         continue;
      }
      pthread_detach(tid);
   }
}

/* === BACKGROUND SERVER === */

static int g_webchat_started = 0;

static void *webchat_thread(void *arg)
{
   int port = *(int *)arg;
   free(arg);
   webchat_serve(port);
   return NULL;
}

void webchat_serve_background(int port)
{
   if (g_webchat_started)
      return;
   g_webchat_started = 1;

   int *port_arg = malloc(sizeof(int));
   if (!port_arg)
      return;
   *port_arg = port;

   pthread_t tid;
   if (pthread_create(&tid, NULL, webchat_thread, port_arg) != 0)
   {
      free(port_arg);
      fprintf(stderr, "aimee: failed to start webchat background thread\n");
      return;
   }
   pthread_detach(tid);
}
