/* cmd_agent_setup.c: provider setup wizards and token management */
#include "aimee.h"
#include "agent.h"
#include "agent_tunnel.h"
#include "commands.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- agent subcommand handlers --- */

/* Agent config reference (defined in cmd_agent.c, loaded before dispatch) */
extern agent_config_t s_agent_cfg;

static void write_key_file(const char *path, const char *content)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", config_default_dir());
   mkdir(dir, 0700);

   FILE *f = fopen(path, "w");
   if (!f)
      fatal("cannot write key file: %s", path);
   fputs(content, f);
   fputc('\n', f);
   fclose(f);
   chmod(path, 0600);
}

static char *read_file_contents(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 64 * 1024)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t nr = fread(buf, 1, (size_t)sz, f);
   buf[nr] = '\0';
   fclose(f);
   return buf;
}

/* --- agent setup: codex (API key) --- */

static void setup_codex(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== Codex Agent Setup (API Key) ===\n"
                   "\n"
                   "1. Visit: https://platform.openai.com/api-keys\n"
                   "2. Create a new API key (or copy an existing one)\n"
                   "3. Paste it below\n"
                   "\n");

   fprintf(stderr, "API key: ");
   char key_buf[MAX_API_KEY_LEN];
   if (!fgets(key_buf, sizeof(key_buf), stdin))
      fatal("failed to read API key");

   size_t klen = strlen(key_buf);
   while (klen > 0 &&
          (key_buf[klen - 1] == '\n' || key_buf[klen - 1] == '\r' || key_buf[klen - 1] == ' '))
      key_buf[--klen] = '\0';

   if (klen == 0)
      fatal("API key cannot be empty");

   if (strncmp(key_buf, "sk-", 3) != 0)
      fprintf(stderr, "Warning: key does not start with 'sk-', continuing anyway.\n");

   char key_path[MAX_PATH_LEN];
   snprintf(key_path, sizeof(key_path), "%s/codex.key", config_default_dir());
   write_key_file(key_path, key_buf);

   char auth_cmd[MAX_AUTH_CMD_LEN];
   snprintf(auth_cmd, sizeof(auth_cmd), "cat %s/codex.key", config_default_dir());

   agent_t *ag = agent_find(cfg, "codex");
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "codex");
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "https://api.openai.com/v1");
   snprintf(ag->model, MAX_MODEL_LEN, "gpt-4o");
   snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "oauth");
   snprintf(ag->provider, sizeof(ag->provider), "chatgpt");
   ag->api_key[0] = '\0';
   ag->cost_tier = 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   for (int i = 0; i < 6; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "codex");

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent 'codex' %s.\n", is_new ? "created" : "updated");
   fprintf(stderr, "Key saved to: %s\n", key_path);
   fprintf(stderr, "\nTest with: aimee agent test codex\n");
}

/* --- JWT helpers for extracting claims from id_token --- */

static int b64url_decode_char(char c)
{
   if (c >= 'A' && c <= 'Z')
      return c - 'A';
   if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
   if (c >= '0' && c <= '9')
      return c - '0' + 52;
   if (c == '-')
      return 62;
   if (c == '_')
      return 63;
   return -1;
}

static size_t b64url_decode(const char *in, size_t in_len, char *out, size_t out_len)
{
   size_t o = 0;
   unsigned int buf = 0;
   int bits = 0;
   for (size_t i = 0; i < in_len && o < out_len; i++)
   {
      int v = b64url_decode_char(in[i]);
      if (v < 0)
         continue;
      buf = (buf << 6) | (unsigned int)v;
      bits += 6;
      if (bits >= 8)
      {
         bits -= 8;
         out[o++] = (char)((buf >> bits) & 0xFF);
      }
   }
   return o;
}

/* Extract chatgpt_account_id from a JWT id_token.
 * Returns the value in buf, or empty string if not found. */
static void jwt_extract_account_id(const char *jwt, char *buf, size_t buf_len)
{
   buf[0] = '\0';
   if (!jwt)
      return;

   /* Find second segment (payload) between first and second dots */
   const char *p1 = strchr(jwt, '.');
   if (!p1)
      return;
   p1++;
   const char *p2 = strchr(p1, '.');
   size_t seg_len = p2 ? (size_t)(p2 - p1) : strlen(p1);

   char decoded[4096];
   size_t n = b64url_decode(p1, seg_len, decoded, sizeof(decoded) - 1);
   decoded[n] = '\0';

   cJSON *claims = cJSON_Parse(decoded);
   if (!claims)
      return;

   /* Try top-level first, then under https://api.openai.com/auth */
   cJSON *aid = cJSON_GetObjectItem(claims, "chatgpt_account_id");
   if (!aid || !cJSON_IsString(aid))
   {
      cJSON *auth_ns = cJSON_GetObjectItem(claims, "https://api.openai.com/auth");
      if (auth_ns && cJSON_IsObject(auth_ns))
         aid = cJSON_GetObjectItem(auth_ns, "chatgpt_account_id");
   }
   if (aid && cJSON_IsString(aid))
      snprintf(buf, buf_len, "%s", aid->valuestring);

   cJSON_Delete(claims);
}

/* --- agent setup: codex-oauth (device code flow) --- */

#define CODEX_OAUTH_CLIENT_ID  "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_USERCODE_URL     "https://auth.openai.com/api/accounts/deviceauth/usercode"
#define CODEX_DEVICETOKEN_URL  "https://auth.openai.com/api/accounts/deviceauth/token"
#define CODEX_TOKEN_URL        "https://auth.openai.com/oauth/token"
#define CODEX_VERIFY_URL       "https://auth.openai.com/codex/device"
#define CODEX_REDIRECT_URI     "https://auth.openai.com/deviceauth/callback"
#define CODEX_DEFAULT_INTERVAL 5
#define CODEX_DEFAULT_EXPIRES  900

static void setup_codex_oauth(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== Codex Agent Setup (ChatGPT Subscription OAuth) ===\n"
                   "\n"
                   "This will authenticate using your ChatGPT Plus/Pro/Enterprise subscription.\n"
                   "\n");

   agent_http_init();

   /* Step 1: Request user code via OpenAI device auth API */
   char *resp_body = NULL;
   char post_body[512];
   snprintf(post_body, sizeof(post_body), "{\"client_id\":\"%s\"}", CODEX_OAUTH_CLIENT_ID);

   int status = agent_http_post(CODEX_USERCODE_URL, NULL, post_body, &resp_body, 15000, NULL);
   if (status < 200 || status >= 300 || !resp_body)
   {
      free(resp_body);
      agent_http_cleanup();
      fatal("failed to request device code (HTTP %d)", status);
   }

   cJSON *dev = cJSON_Parse(resp_body);
   free(resp_body);
   if (!dev)
   {
      agent_http_cleanup();
      fatal("failed to parse device code response");
   }

   cJSON *j_device_auth_id = cJSON_GetObjectItem(dev, "device_auth_id");
   cJSON *j_user_code = cJSON_GetObjectItem(dev, "user_code");
   cJSON *j_interval = cJSON_GetObjectItem(dev, "interval");

   if (!j_device_auth_id || !cJSON_IsString(j_device_auth_id) || !j_user_code ||
       !cJSON_IsString(j_user_code))
   {
      cJSON_Delete(dev);
      agent_http_cleanup();
      fatal("device code response missing required fields");
   }

   const char *device_auth_id = j_device_auth_id->valuestring;
   const char *user_code = j_user_code->valuestring;
   int interval =
       (j_interval && cJSON_IsNumber(j_interval)) ? j_interval->valueint : CODEX_DEFAULT_INTERVAL;

   /* Step 2: Show user the code and URL */
   fprintf(stderr,
           "\n"
           "Visit this URL in your browser:\n"
           "\n"
           "    %s\n"
           "\n"
           "Enter the code: %s\n"
           "\n"
           "Waiting for authorization...\n",
           CODEX_VERIFY_URL, user_code);

   /* Step 3: Poll for authorization code */
   char poll_body[1024];
   snprintf(poll_body, sizeof(poll_body), "{\"device_auth_id\":\"%s\",\"user_code\":\"%s\"}",
            device_auth_id, user_code);

   int elapsed = 0;
   char *poll_resp = NULL;
   int poll_status = 0;
   int authorized = 0;
   char auth_code[1024] = {0};
   char code_verifier[256] = {0};

   while (elapsed < CODEX_DEFAULT_EXPIRES)
   {
      sleep((unsigned)interval);
      elapsed += interval;

      free(poll_resp);
      poll_resp = NULL;
      poll_status =
          agent_http_post(CODEX_DEVICETOKEN_URL, NULL, poll_body, &poll_resp, 15000, NULL);

      if (poll_status == 200 && poll_resp)
      {
         cJSON *pr = cJSON_Parse(poll_resp);
         if (pr)
         {
            cJSON *j_auth_code = cJSON_GetObjectItem(pr, "authorization_code");
            cJSON *j_verifier = cJSON_GetObjectItem(pr, "code_verifier");
            if (j_auth_code && cJSON_IsString(j_auth_code))
            {
               snprintf(auth_code, sizeof(auth_code), "%s", j_auth_code->valuestring);
               if (j_verifier && cJSON_IsString(j_verifier))
                  snprintf(code_verifier, sizeof(code_verifier), "%s", j_verifier->valuestring);
               authorized = 1;
            }
            cJSON_Delete(pr);
         }
         if (authorized)
            break;
      }

      /* 403/404 means still pending; 429 means rate-limited – retry both */
      if (poll_status == 403 || poll_status == 404 || poll_status == 429)
      {
         fprintf(stderr, ".");
         if (poll_status == 429)
            sleep((unsigned)interval); /* extra back-off on rate limit */
         continue;
      }

      /* Unexpected status */
      if (poll_status != 200)
      {
         fprintf(stderr, "\nUnexpected poll response (HTTP %d)\n", poll_status);
         break;
      }
   }

   cJSON_Delete(dev);
   free(poll_resp);

   if (!authorized || !auth_code[0])
   {
      agent_http_cleanup();
      fatal("authorization timed out or was denied");
   }

   fprintf(stderr, "\nAuthorized!\n");

   /* Step 4: Exchange authorization code for OAuth tokens (includes id_token) */
   char token_body[2048];
   snprintf(token_body, sizeof(token_body),
            "grant_type=authorization_code"
            "&code=%s"
            "&redirect_uri=%s"
            "&client_id=%s"
            "&code_verifier=%s",
            auth_code, CODEX_REDIRECT_URI, CODEX_OAUTH_CLIENT_ID, code_verifier);

   char *token_resp = NULL;
   int token_status = agent_http_post_form(CODEX_TOKEN_URL, token_body, &token_resp, 15000);

   if (token_status != 200 || !token_resp)
   {
      if (token_resp)
         fprintf(stderr, "aimee: token exchange response: %s\n", token_resp);
      free(token_resp);
      agent_http_cleanup();
      fatal("failed to exchange authorization code for token (HTTP %d)", token_status);
   }

   cJSON *tok = cJSON_Parse(token_resp);
   free(token_resp);
   if (!tok)
   {
      agent_http_cleanup();
      fatal("failed to parse token response");
   }

   cJSON *j_id_token = cJSON_GetObjectItem(tok, "id_token");
   cJSON *j_access_token = cJSON_GetObjectItem(tok, "access_token");
   cJSON *j_refresh = cJSON_GetObjectItem(tok, "refresh_token");
   cJSON *j_tok_expires = cJSON_GetObjectItem(tok, "expires_in");

   /* Step 5: Exchange id_token for an OpenAI API key (best-effort, like Codex CLI) */
   const char *final_token = NULL;
   int tok_expires = 3600;
   cJSON *exch = NULL;

   if (j_id_token && cJSON_IsString(j_id_token))
   {
      char exchange_body[4096];
      snprintf(exchange_body, sizeof(exchange_body),
               "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Atoken-exchange"
               "&client_id=%s"
               "&requested_token=openai-api-key"
               "&subject_token=%s"
               "&subject_token_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Atoken-type%%3Aid_token",
               CODEX_OAUTH_CLIENT_ID, j_id_token->valuestring);

      char *exchange_resp = NULL;
      int exchange_status =
          agent_http_post_form(CODEX_TOKEN_URL, exchange_body, &exchange_resp, 15000);

      if (exchange_status == 200 && exchange_resp)
      {
         exch = cJSON_Parse(exchange_resp);
         if (exch)
         {
            cJSON *j_api_key = cJSON_GetObjectItem(exch, "access_token");
            cJSON *j_api_expires = cJSON_GetObjectItem(exch, "expires_in");
            if (j_api_key && cJSON_IsString(j_api_key))
            {
               final_token = j_api_key->valuestring;
               if (j_api_expires && cJSON_IsNumber(j_api_expires))
                  tok_expires = j_api_expires->valueint;
            }
         }
      }
      if (!final_token)
      {
         fprintf(stderr, "aimee: API key exchange failed (HTTP %d)", exchange_status);
         if (exchange_resp)
            fprintf(stderr, ": %s", exchange_resp);
         fprintf(stderr, "\nFalling back to OAuth access token\n");
      }
      free(exchange_resp);
   }

   /* Fall back to the OAuth access_token if the API key exchange failed */
   if (!final_token)
   {
      if (!j_access_token || !cJSON_IsString(j_access_token))
      {
         cJSON_Delete(exch);
         cJSON_Delete(tok);
         agent_http_cleanup();
         fatal("no usable token: API key exchange failed and no access_token in OAuth response");
      }
      final_token = j_access_token->valuestring;
      if (j_tok_expires && cJSON_IsNumber(j_tok_expires))
         tok_expires = j_tok_expires->valueint;
   }

   /* Extract ChatGPT-Account-ID from id_token JWT before freeing token objects */
   char chatgpt_account_id[128] = {0};
   if (j_id_token && cJSON_IsString(j_id_token))
      jwt_extract_account_id(j_id_token->valuestring, chatgpt_account_id,
                             sizeof(chatgpt_account_id));

   /* Write auth JSON */
   cJSON *auth_json = cJSON_CreateObject();
   cJSON_AddStringToObject(auth_json, "access_token", final_token);
   if (j_refresh && cJSON_IsString(j_refresh))
      cJSON_AddStringToObject(auth_json, "refresh_token", j_refresh->valuestring);
   if (j_id_token && cJSON_IsString(j_id_token))
      cJSON_AddStringToObject(auth_json, "id_token", j_id_token->valuestring);
   cJSON_AddNumberToObject(auth_json, "expires_at", (double)(time(NULL) + tok_expires));
   cJSON_AddStringToObject(auth_json, "client_id", CODEX_OAUTH_CLIENT_ID);

   char *auth_str = cJSON_Print(auth_json);
   cJSON_Delete(auth_json);
   cJSON_Delete(exch);
   cJSON_Delete(tok);
   agent_http_cleanup();

   if (!auth_str)
      fatal("failed to serialize auth tokens");

   char auth_path[MAX_PATH_LEN];
   snprintf(auth_path, sizeof(auth_path), "%s/codex-auth.json", config_default_dir());
   write_key_file(auth_path, auth_str);
   free(auth_str);

   /* Step 6: Configure agent */
   char auth_cmd[MAX_AUTH_CMD_LEN];
   snprintf(auth_cmd, sizeof(auth_cmd), "aimee agent token codex");

   agent_t *ag = agent_find(cfg, "codex");
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "codex");
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "https://chatgpt.com/backend-api/codex");
   snprintf(ag->model, MAX_MODEL_LEN, "gpt-5.4");
   snprintf(ag->fallback_model, MAX_MODEL_LEN, "gpt-5.4-mini");
   snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "oauth");
   snprintf(ag->provider, sizeof(ag->provider), "chatgpt");
   ag->api_key[0] = '\0';
   ag->cost_tier = 0;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   /* Set ChatGPT-Account-ID header if extracted from id_token */
   ag->extra_headers[0] = '\0';
   if (chatgpt_account_id[0])
      snprintf(ag->extra_headers, sizeof(ag->extra_headers), "ChatGPT-Account-ID: %s",
               chatgpt_account_id);

   ag->role_count = 0;
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   for (int i = 0; i < 6; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "codex");

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent 'codex' %s (OAuth subscription).\n", is_new ? "created" : "updated");
   fprintf(stderr, "Tokens saved to: %s\n", auth_path);
   fprintf(stderr, "\nTest with: aimee agent test codex\n");
}

/* --- agent setup: claude (API key) --- */

static void setup_claude(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== Claude Agent Setup (API Key) ===\n"
                   "\n"
                   "1. Visit: https://console.anthropic.com/settings/keys\n"
                   "2. Create a new API key (or copy an existing one)\n"
                   "3. Paste it below\n"
                   "\n");

   fprintf(stderr, "API key: ");
   char key_buf[MAX_API_KEY_LEN];
   if (!fgets(key_buf, sizeof(key_buf), stdin))
      fatal("failed to read API key");

   size_t klen = strlen(key_buf);
   while (klen > 0 &&
          (key_buf[klen - 1] == '\n' || key_buf[klen - 1] == '\r' || key_buf[klen - 1] == ' '))
      key_buf[--klen] = '\0';

   if (klen == 0)
      fatal("API key cannot be empty");

   if (strncmp(key_buf, "sk-ant-", 7) != 0)
      fprintf(stderr, "Warning: key does not start with 'sk-ant-', continuing anyway.\n");

   char key_path[MAX_PATH_LEN];
   snprintf(key_path, sizeof(key_path), "%s/claude.key", config_default_dir());
   write_key_file(key_path, key_buf);

   char auth_cmd[MAX_AUTH_CMD_LEN];
   snprintf(auth_cmd, sizeof(auth_cmd), "cat %s/claude.key", config_default_dir());

   agent_t *ag = agent_find(cfg, "claude");
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "claude");
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "https://api.anthropic.com/v1");
   snprintf(ag->model, MAX_MODEL_LEN, "claude-sonnet-4-6");
   snprintf(ag->fallback_model, MAX_MODEL_LEN, "claude-haiku-4-5");
   snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "x-api-key");
   snprintf(ag->provider, sizeof(ag->provider), "anthropic");
   snprintf(ag->extra_headers, sizeof(ag->extra_headers), "anthropic-version: 2023-06-01");
   ag->api_key[0] = '\0';
   ag->cost_tier = 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   for (int i = 0; i < 6; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "claude");

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent 'claude' %s.\n", is_new ? "created" : "updated");
   fprintf(stderr, "Key saved to: %s\n", key_path);
   fprintf(stderr, "\nTest with: aimee agent test claude\n");
}

/* --- agent setup: gemini (API key) --- */

static void setup_gemini(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== Gemini Agent Setup (API Key) ===\n"
                   "\n"
                   "1. Visit: https://aistudio.google.com/apikey\n"
                   "2. Create a new API key (or copy an existing one)\n"
                   "3. Paste it below\n"
                   "\n");

   fprintf(stderr, "API key: ");
   char key_buf[MAX_API_KEY_LEN];
   if (!fgets(key_buf, sizeof(key_buf), stdin))
      fatal("failed to read API key");

   size_t klen = strlen(key_buf);
   while (klen > 0 &&
          (key_buf[klen - 1] == '\n' || key_buf[klen - 1] == '\r' || key_buf[klen - 1] == ' '))
      key_buf[--klen] = '\0';

   if (klen == 0)
      fatal("API key cannot be empty");

   char key_path[MAX_PATH_LEN];
   snprintf(key_path, sizeof(key_path), "%s/gemini.key", config_default_dir());
   write_key_file(key_path, key_buf);

   char auth_cmd[MAX_AUTH_CMD_LEN];
   snprintf(auth_cmd, sizeof(auth_cmd), "cat %s/gemini.key", config_default_dir());

   agent_t *ag = agent_find(cfg, "gemini");
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "gemini");
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN,
            "https://generativelanguage.googleapis.com/v1beta/openai");
   snprintf(ag->model, MAX_MODEL_LEN, "gemini-2.5-flash");
   snprintf(ag->fallback_model, MAX_MODEL_LEN, "gemini-2.5-flash-lite");
   snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "oauth");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->api_key[0] = '\0';
   ag->extra_headers[0] = '\0';
   ag->cost_tier = 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   for (int i = 0; i < 6; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "gemini");

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent 'gemini' %s.\n", is_new ? "created" : "updated");
   fprintf(stderr, "Key saved to: %s\n", key_path);
   fprintf(stderr, "\nTest with: aimee agent test gemini\n");
}

/* --- agent setup: gemini-oauth (Google subscription, device code flow) --- */

#define GOOGLE_DEVICE_AUTH_URL  "https://oauth2.googleapis.com/device/code"
#define GOOGLE_TOKEN_URL        "https://oauth2.googleapis.com/token"
#define GOOGLE_SCOPE            "https://www.googleapis.com/auth/generative-language"
#define GOOGLE_DEFAULT_INTERVAL 5
#define GOOGLE_DEFAULT_EXPIRES  900

static void setup_gemini_oauth(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== Gemini Agent Setup (Google Subscription OAuth) ===\n"
                   "\n"
                   "This will authenticate using your Google account.\n"
                   "You need a Google Cloud OAuth client ID with device flow enabled.\n"
                   "\n"
                   "Paste your OAuth client ID below.\n"
                   "\n");

   fprintf(stderr, "Client ID: ");
   char client_id[512];
   if (!fgets(client_id, sizeof(client_id), stdin))
      fatal("failed to read client ID");

   size_t clen = strlen(client_id);
   while (clen > 0 && (client_id[clen - 1] == '\n' || client_id[clen - 1] == '\r' ||
                       client_id[clen - 1] == ' '))
      client_id[--clen] = '\0';

   if (clen == 0)
      fatal("client ID cannot be empty");

   agent_http_init();

   /* Step 1: Request device code */
   char post_body[1024];
   snprintf(post_body, sizeof(post_body), "client_id=%s&scope=%s", client_id, GOOGLE_SCOPE);

   char *resp_body = NULL;
   int status = agent_http_post_form(GOOGLE_DEVICE_AUTH_URL, post_body, &resp_body, 15000);
   if (status < 200 || status >= 300 || !resp_body)
   {
      free(resp_body);
      agent_http_cleanup();
      fatal("failed to request device code (HTTP %d)", status);
   }

   cJSON *dev = cJSON_Parse(resp_body);
   free(resp_body);
   if (!dev)
   {
      agent_http_cleanup();
      fatal("failed to parse device code response");
   }

   cJSON *j_device_code = cJSON_GetObjectItem(dev, "device_code");
   cJSON *j_user_code = cJSON_GetObjectItem(dev, "user_code");
   cJSON *j_verify_url = cJSON_GetObjectItem(dev, "verification_url");
   cJSON *j_interval = cJSON_GetObjectItem(dev, "interval");

   if (!j_device_code || !cJSON_IsString(j_device_code) || !j_user_code ||
       !cJSON_IsString(j_user_code))
   {
      cJSON_Delete(dev);
      agent_http_cleanup();
      fatal("device code response missing required fields");
   }

   const char *device_code = j_device_code->valuestring;
   const char *user_code = j_user_code->valuestring;
   const char *verify_url = (j_verify_url && cJSON_IsString(j_verify_url))
                                ? j_verify_url->valuestring
                                : "https://www.google.com/device";
   int interval =
       (j_interval && cJSON_IsNumber(j_interval)) ? j_interval->valueint : GOOGLE_DEFAULT_INTERVAL;

   /* Step 2: Show user the code and URL */
   fprintf(stderr,
           "\n"
           "Visit this URL in your browser:\n"
           "\n"
           "    %s\n"
           "\n"
           "Enter the code: %s\n"
           "\n"
           "Waiting for authorization...\n",
           verify_url, user_code);

   /* Step 3: Poll for token */
   char poll_body[1024];
   snprintf(
       poll_body, sizeof(poll_body),
       "client_id=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3A"
       "device_code",
       client_id, device_code);

   int elapsed = 0;
   char *poll_resp = NULL;
   int poll_status = 0;
   int authorized = 0;
   char access_token[MAX_API_KEY_LEN] = {0};
   char refresh_token[MAX_API_KEY_LEN] = {0};
   int tok_expires = 3600;

   while (elapsed < GOOGLE_DEFAULT_EXPIRES)
   {
      sleep((unsigned)interval);
      elapsed += interval;

      free(poll_resp);
      poll_resp = NULL;
      poll_status = agent_http_post_form(GOOGLE_TOKEN_URL, poll_body, &poll_resp, 15000);

      if (poll_status == 200 && poll_resp)
      {
         cJSON *pr = cJSON_Parse(poll_resp);
         if (pr)
         {
            cJSON *j_access = cJSON_GetObjectItem(pr, "access_token");
            cJSON *j_refresh = cJSON_GetObjectItem(pr, "refresh_token");
            cJSON *j_exp = cJSON_GetObjectItem(pr, "expires_in");
            if (j_access && cJSON_IsString(j_access))
            {
               snprintf(access_token, sizeof(access_token), "%s", j_access->valuestring);
               if (j_refresh && cJSON_IsString(j_refresh))
                  snprintf(refresh_token, sizeof(refresh_token), "%s", j_refresh->valuestring);
               if (j_exp && cJSON_IsNumber(j_exp))
                  tok_expires = j_exp->valueint;
               authorized = 1;
            }
            cJSON_Delete(pr);
         }
         if (authorized)
            break;
      }

      /* 428 or error with "authorization_pending" means still waiting */
      if (poll_status == 428 || poll_status == 403)
      {
         fprintf(stderr, ".");
         continue;
      }

      if (poll_status != 200)
      {
         fprintf(stderr, "\nUnexpected poll response (HTTP %d)\n", poll_status);
         break;
      }
   }

   cJSON_Delete(dev);
   free(poll_resp);

   if (!authorized || !access_token[0])
   {
      agent_http_cleanup();
      fatal("authorization timed out or was denied");
   }

   fprintf(stderr, "\nAuthorized!\n");
   agent_http_cleanup();

   /* Write auth JSON */
   cJSON *auth_json = cJSON_CreateObject();
   cJSON_AddStringToObject(auth_json, "access_token", access_token);
   if (refresh_token[0])
      cJSON_AddStringToObject(auth_json, "refresh_token", refresh_token);
   cJSON_AddNumberToObject(auth_json, "expires_at", (double)(time(NULL) + tok_expires));
   cJSON_AddStringToObject(auth_json, "client_id", client_id);

   char *auth_str = cJSON_Print(auth_json);
   cJSON_Delete(auth_json);

   if (!auth_str)
      fatal("failed to serialize auth tokens");

   char auth_path[MAX_PATH_LEN];
   snprintf(auth_path, sizeof(auth_path), "%s/gemini-auth.json", config_default_dir());
   write_key_file(auth_path, auth_str);
   free(auth_str);

   /* Configure agent */
   agent_t *ag = agent_find(cfg, "gemini");
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "gemini");
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN,
            "https://generativelanguage.googleapis.com/v1beta/openai");
   snprintf(ag->model, MAX_MODEL_LEN, "gemini-2.5-flash");
   snprintf(ag->fallback_model, MAX_MODEL_LEN, "gemini-2.5-flash-lite");
   snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "aimee agent token gemini");
   snprintf(ag->auth_type, sizeof(ag->auth_type), "oauth");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->api_key[0] = '\0';
   ag->extra_headers[0] = '\0';
   ag->cost_tier = 0;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   for (int i = 0; i < 6; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "gemini");

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent 'gemini' %s (OAuth subscription).\n", is_new ? "created" : "updated");
   fprintf(stderr, "Tokens saved to: %s\n", auth_path);
   fprintf(stderr, "\nTest with: aimee agent test gemini\n");
}

/* --- agent setup: openai (generic OpenAI-compatible provider) --- */

static void read_line(const char *prompt, char *buf, size_t buf_len, int required)
{
   fprintf(stderr, "%s", prompt);
   if (!fgets(buf, (int)buf_len, stdin))
   {
      if (required)
         fatal("failed to read input");
      buf[0] = '\0';
      return;
   }
   size_t len = strlen(buf);
   while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' '))
      buf[--len] = '\0';
   if (required && len == 0)
      fatal("value cannot be empty");
}

static void setup_openai(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== OpenAI-Compatible Agent Setup ===\n"
                   "\n"
                   "Configure any provider with an OpenAI-compatible /chat/completions endpoint.\n"
                   "\n");

   char name_buf[MAX_AGENT_NAME];
   read_line("Agent name (e.g. groq, together, local): ", name_buf, sizeof(name_buf), 1);

   char endpoint_buf[MAX_ENDPOINT_LEN];
   read_line("Endpoint URL (e.g. https://api.groq.com/openai/v1): ", endpoint_buf,
             sizeof(endpoint_buf), 1);

   char model_buf[MAX_MODEL_LEN];
   read_line("Model name (e.g. llama-3.3-70b-versatile): ", model_buf, sizeof(model_buf), 1);

   char key_buf[MAX_API_KEY_LEN];
   read_line("API key (leave blank for no auth): ", key_buf, sizeof(key_buf), 0);

   char fallback_buf[MAX_MODEL_LEN];
   read_line("Fallback model (leave blank to skip): ", fallback_buf, sizeof(fallback_buf), 0);

   char roles_buf[256];
   read_line("Roles (comma-separated, default: code,review,explain,refactor,draft,execute): ",
             roles_buf, sizeof(roles_buf), 0);

   char tier_buf[16];
   read_line("Cost tier (0=free, 1=cheap, 2=expensive, default: 1): ", tier_buf, sizeof(tier_buf),
             0);

   /* Save key if provided */
   char auth_cmd[MAX_AUTH_CMD_LEN] = {0};
   int has_key = (key_buf[0] != '\0');
   if (has_key)
   {
      char key_path[MAX_PATH_LEN];
      snprintf(key_path, sizeof(key_path), "%s/%s.key", config_default_dir(), name_buf);
      write_key_file(key_path, key_buf);
      snprintf(auth_cmd, sizeof(auth_cmd), "cat %s/%s.key", config_default_dir(), name_buf);
   }

   agent_t *ag = agent_find(cfg, name_buf);
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "%s", name_buf);
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", endpoint_buf);
   snprintf(ag->model, MAX_MODEL_LEN, "%s", model_buf);
   if (fallback_buf[0])
      snprintf(ag->fallback_model, MAX_MODEL_LEN, "%s", fallback_buf);
   else
      ag->fallback_model[0] = '\0';

   if (has_key)
   {
      snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
      snprintf(ag->auth_type, sizeof(ag->auth_type), "oauth");
   }
   else
   {
      ag->auth_cmd[0] = '\0';
      snprintf(ag->auth_type, sizeof(ag->auth_type), "none");
   }

   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->api_key[0] = '\0';
   ag->extra_headers[0] = '\0';
   ag->cost_tier = (tier_buf[0] >= '0' && tier_buf[0] <= '9') ? atoi(tier_buf) : 1;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   /* Parse roles */
   ag->role_count = 0;
   if (roles_buf[0])
   {
      char roles_copy[256];
      snprintf(roles_copy, sizeof(roles_copy), "%s", roles_buf);
      char *tok = strtok(roles_copy, ",");
      while (tok && ag->role_count < MAX_AGENT_ROLES)
      {
         while (*tok == ' ')
            tok++;
         snprintf(ag->roles[ag->role_count++], 32, "%s", tok);
         tok = strtok(NULL, ",");
      }
   }
   else
   {
      const char *defaults[] = {"code", "review", "explain", "refactor", "draft", "execute"};
      for (int i = 0; i < 6; i++)
         snprintf(ag->roles[ag->role_count++], 32, "%s", defaults[i]);
   }

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", name_buf);

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent '%s' %s.\n", name_buf, is_new ? "created" : "updated");
   if (has_key)
      fprintf(stderr, "Key saved to: %s/%s.key\n", config_default_dir(), name_buf);
   fprintf(stderr, "\nTest with: aimee agent test %s\n", name_buf);
}

/* --- Copilot setup --- */

static void setup_copilot(app_ctx_t *ctx, agent_config_t *cfg)
{
   (void)ctx;

   fprintf(stderr, "\n"
                   "=== GitHub Copilot Agent Setup ===\n"
                   "\n"
                   "Uses the GitHub Copilot API (OpenAI-compatible).\n"
                   "Requires a GitHub token with Copilot access.\n"
                   "\n"
                   "Auth options:\n"
                   "  1. Run a command that prints a GitHub token (e.g. 'gh auth token')\n"
                   "  2. Paste a GitHub personal access token directly\n"
                   "\n");

   char auth_method[16];
   read_line("Auth method (command/token, default: command): ", auth_method, sizeof(auth_method),
             0);

   char auth_cmd[MAX_AUTH_CMD_LEN] = {0};
   char auth_type_str[16] = {0};

   if (auth_method[0] == 't' || auth_method[0] == 'T')
   {
      char key_buf[MAX_API_KEY_LEN];
      read_line("GitHub token: ", key_buf, sizeof(key_buf), 1);

      char key_path[MAX_PATH_LEN];
      snprintf(key_path, sizeof(key_path), "%s/copilot.key", config_default_dir());
      write_key_file(key_path, key_buf);
      snprintf(auth_cmd, sizeof(auth_cmd), "cat %s/copilot.key", config_default_dir());
      snprintf(auth_type_str, sizeof(auth_type_str), "bearer");
      fprintf(stderr, "Key saved to: %s\n", key_path);
   }
   else
   {
      char cmd_buf[MAX_AUTH_CMD_LEN];
      read_line("Auth command (default: gh auth token): ", cmd_buf, sizeof(cmd_buf), 0);
      if (cmd_buf[0])
         snprintf(auth_cmd, sizeof(auth_cmd), "%s", cmd_buf);
      else
         snprintf(auth_cmd, sizeof(auth_cmd), "gh auth token");
      snprintf(auth_type_str, sizeof(auth_type_str), "bearer");
   }

   char model_buf[MAX_MODEL_LEN];
   read_line("Model (default: gpt-4o): ", model_buf, sizeof(model_buf), 0);

   char fallback_buf[MAX_MODEL_LEN];
   read_line("Fallback model (default: gpt-4o-mini): ", fallback_buf, sizeof(fallback_buf), 0);

   agent_t *ag = agent_find(cfg, "copilot");
   int is_new = 0;
   if (!ag)
   {
      if (cfg->agent_count >= MAX_AGENTS)
         fatal("maximum number of agents reached");
      ag = &cfg->agents[cfg->agent_count];
      memset(ag, 0, sizeof(*ag));
      cfg->agent_count++;
      is_new = 1;
   }

   snprintf(ag->name, MAX_AGENT_NAME, "copilot");
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "https://api.githubcopilot.com");
   snprintf(ag->model, MAX_MODEL_LEN, "%s", model_buf[0] ? model_buf : "gpt-4o");
   snprintf(ag->fallback_model, MAX_MODEL_LEN, "%s",
            fallback_buf[0] ? fallback_buf : "gpt-4o-mini");
   snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", auth_cmd);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", auth_type_str);
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->api_key[0] = '\0';
   ag->extra_headers[0] = '\0';
   ag->cost_tier = 0;
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = 60000;
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->max_turns = -1; /* inherit from global config max_iterations_delegate */
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

   ag->role_count = 0;
   const char *roles[] = {"code", "review", "explain", "refactor", "draft", "execute"};
   for (int i = 0; i < 6; i++)
      snprintf(ag->roles[ag->role_count++], 32, "%s", roles[i]);

   if (cfg->agent_count == 1 || !cfg->default_agent[0])
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "copilot");

   agent_save_config(cfg);
   fprintf(stderr, "\nAgent 'copilot' %s.\n", is_new ? "created" : "updated");
   fprintf(stderr, "\nTest with: aimee agent test copilot\n");
}

/* --- agent setup dispatch --- */

typedef struct
{
   const char *name;
   void (*fn)(app_ctx_t *ctx, agent_config_t *cfg);
} setup_provider_t;

static const setup_provider_t setup_providers[] = {
    {"codex", setup_codex},   {"codex-oauth", setup_codex_oauth},
    {"claude", setup_claude}, {"copilot", setup_copilot},
    {"gemini", setup_gemini}, {"gemini-oauth", setup_gemini_oauth},
    {"openai", setup_openai}, {NULL, NULL},
};

void ag_setup(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   if (argc < 1)
   {
      fprintf(stderr, "usage: aimee agent setup <provider>\n");
      fprintf(stderr,
              "Providers: codex, codex-oauth, claude, copilot, gemini, gemini-oauth, openai\n");
      exit(1);
   }
   const char *provider = argv[0];
   for (int i = 0; setup_providers[i].name; i++)
   {
      if (strcmp(provider, setup_providers[i].name) == 0)
      {
         setup_providers[i].fn(ctx, &s_agent_cfg);
         return;
      }
   }
   fatal("unknown setup provider: %s "
         "(available: codex, codex-oauth, claude, copilot, gemini, gemini-oauth, openai)",
         provider);
}

/* --- agent token (print/refresh OAuth token) --- */

void ag_token(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;

   if (argc < 1)
      fatal("usage: aimee agent token <name>");

   const char *name = argv[0];

   int is_gemini = (strcmp(name, "gemini") == 0);
   int is_codex = (strcmp(name, "codex") == 0);

   if (!is_codex && !is_gemini)
      fatal("token refresh supported for 'codex' and 'gemini'");

   char auth_path[MAX_PATH_LEN];
   if (is_gemini)
      snprintf(auth_path, sizeof(auth_path), "%s/gemini-auth.json", config_default_dir());
   else
      snprintf(auth_path, sizeof(auth_path), "%s/codex-auth.json", config_default_dir());

   char *data = read_file_contents(auth_path);
   if (!data)
      fatal("no auth file found at %s (run: aimee agent setup %s)", auth_path,
            is_gemini ? "gemini-oauth" : "codex-oauth");

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      fatal("failed to parse %s", auth_path);

   cJSON *j_access = cJSON_GetObjectItem(root, "access_token");
   cJSON *j_refresh = cJSON_GetObjectItem(root, "refresh_token");
   cJSON *j_expires_at = cJSON_GetObjectItem(root, "expires_at");
   cJSON *j_client_id = cJSON_GetObjectItem(root, "client_id");

   if (!j_access || !cJSON_IsString(j_access))
   {
      cJSON_Delete(root);
      fatal("auth file missing access_token");
   }

   time_t now = time(NULL);
   double expires_at =
       (j_expires_at && cJSON_IsNumber(j_expires_at)) ? j_expires_at->valuedouble : 0;

   /* If token is still fresh (more than 5 minutes remaining), print it */
   if ((double)now + 300 < expires_at)
   {
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      return;
   }

   /* Token expired or expiring, try refresh */
   if (!j_refresh || !cJSON_IsString(j_refresh))
   {
      /* No refresh token, print what we have */
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      return;
   }

   const char *client_id = (j_client_id && cJSON_IsString(j_client_id)) ? j_client_id->valuestring
                                                                        : CODEX_OAUTH_CLIENT_ID;
   const char *token_url = is_gemini ? GOOGLE_TOKEN_URL : CODEX_TOKEN_URL;

   agent_http_init();

   char post_body[MAX_API_KEY_LEN + 256];
   snprintf(post_body, sizeof(post_body), "grant_type=refresh_token&client_id=%s&refresh_token=%s",
            client_id, j_refresh->valuestring);

   char *resp_body = NULL;
   int status = agent_http_post_form(token_url, post_body, &resp_body, 15000);

   if (status != 200 || !resp_body)
   {
      agent_http_cleanup();
      /* Fall back to current token */
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      free(resp_body);
      return;
   }

   cJSON *tok = cJSON_Parse(resp_body);
   free(resp_body);

   if (!tok)
   {
      agent_http_cleanup();
      printf("%s", j_access->valuestring);
      cJSON_Delete(root);
      return;
   }

   cJSON *new_id_token = cJSON_GetObjectItem(tok, "id_token");
   cJSON *new_access = cJSON_GetObjectItem(tok, "access_token");
   cJSON *new_refresh = cJSON_GetObjectItem(tok, "refresh_token");
   cJSON *new_tok_expires = cJSON_GetObjectItem(tok, "expires_in");

   /* Try to exchange id_token for API key (Codex-specific, best-effort) */
   const char *final_token = NULL;
   int new_exp_secs = 3600;
   cJSON *exch = NULL;

   if (!is_gemini && new_id_token && cJSON_IsString(new_id_token))
   {
      char exchange_body[4096];
      snprintf(exchange_body, sizeof(exchange_body),
               "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Atoken-exchange"
               "&client_id=%s"
               "&requested_token=openai-api-key"
               "&subject_token=%s"
               "&subject_token_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Atoken-type%%3Aid_token",
               client_id, new_id_token->valuestring);

      char *exch_resp = NULL;
      int exch_status = agent_http_post_form(CODEX_TOKEN_URL, exchange_body, &exch_resp, 15000);

      if (exch_status == 200 && exch_resp)
      {
         exch = cJSON_Parse(exch_resp);
         if (exch)
         {
            cJSON *new_api_key = cJSON_GetObjectItem(exch, "access_token");
            cJSON *new_expires = cJSON_GetObjectItem(exch, "expires_in");
            if (new_api_key && cJSON_IsString(new_api_key))
            {
               final_token = new_api_key->valuestring;
               if (new_expires && cJSON_IsNumber(new_expires))
                  new_exp_secs = new_expires->valueint;
            }
         }
      }
      if (!final_token)
      {
         fprintf(stderr, "aimee: API key exchange failed (HTTP %d)", exch_status);
         if (exch_resp)
            fprintf(stderr, ": %s", exch_resp);
         fprintf(stderr, "\n");
      }
      free(exch_resp);
   }

   agent_http_cleanup();

   /* Fall back to refreshed access_token, then to original token */
   if (!final_token && new_access && cJSON_IsString(new_access))
   {
      final_token = new_access->valuestring;
      if (new_tok_expires && cJSON_IsNumber(new_tok_expires))
         new_exp_secs = new_tok_expires->valueint;
   }
   if (!final_token)
   {
      printf("%s", j_access->valuestring);
      cJSON_Delete(exch);
      cJSON_Delete(tok);
      cJSON_Delete(root);
      return;
   }

   /* Update the auth file */
   cJSON *updated = cJSON_CreateObject();
   cJSON_AddStringToObject(updated, "access_token", final_token);
   if (new_refresh && cJSON_IsString(new_refresh))
      cJSON_AddStringToObject(updated, "refresh_token", new_refresh->valuestring);
   else if (j_refresh && cJSON_IsString(j_refresh))
      cJSON_AddStringToObject(updated, "refresh_token", j_refresh->valuestring);
   if (new_id_token && cJSON_IsString(new_id_token))
      cJSON_AddStringToObject(updated, "id_token", new_id_token->valuestring);
   cJSON_AddNumberToObject(updated, "expires_at", (double)(now + new_exp_secs));
   cJSON_AddStringToObject(updated, "client_id", client_id);

   /* Print token before freeing the cJSON objects that own the string */
   printf("%s", final_token);

   char *updated_str = cJSON_Print(updated);
   cJSON_Delete(updated);
   cJSON_Delete(exch);
   cJSON_Delete(tok);
   cJSON_Delete(root);

   if (updated_str)
   {
      write_key_file(auth_path, updated_str);
      free(updated_str);
   }
}

/* --- tunnel status --- */

void ag_tunnel(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   (void)argc;
   (void)argv;
   agent_tunnel_print_status(&s_agent_cfg.tunnel_mgr, ctx->json_output);
}
