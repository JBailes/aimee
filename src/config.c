/* config.c: app configuration loading/saving (~/.config/aimee/config.json) */
#include "aimee.h"
#include "platform_random.h"
#include "cJSON.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Session identity --- */

static __thread char g_session_override[64];

const char *session_id(void)
{
   static char id[64];
   if (g_session_override[0])
      return g_session_override;
   if (id[0])
      return id;

   /* All processes in an agent session share a parent PID (the agent process).
    * Use a ppid-keyed file so hooks, MCP server, and delegates share one
    * aimee session ID without relying on agent-specific env vars. */
   {
      char path[512];
      const char *home = getenv("HOME");
      if (home)
      {
         snprintf(path, sizeof(path), "%s/.config/aimee/session-ppid-%d", home, (int)getppid());
         FILE *fp = fopen(path, "r");
         if (fp)
         {
            if (fgets(id, sizeof(id), fp))
            {
               size_t len = strlen(id);
               while (len > 0 && (id[len - 1] == '\n' || id[len - 1] == '\r' || id[len - 1] == ' '))
                  id[--len] = '\0';
               if (id[0])
               {
                  fclose(fp);
                  return id;
               }
            }
            fclose(fp);
         }
      }
   }

   /* Generate new aimee session ID and persist atomically for sibling processes */
   unsigned char buf[16];
   if (platform_random_bytes(buf, sizeof(buf)) != 0)
      memset(buf, 0, sizeof(buf));
   snprintf(id, sizeof(id), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10],
            buf[11], buf[12], buf[13], buf[14], buf[15]);

   {
      char path[512];
      const char *home = getenv("HOME");
      if (home)
      {
         snprintf(path, sizeof(path), "%s/.config/aimee/session-ppid-%d", home, (int)getppid());
         int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
         if (fd >= 0)
         {
            (void)write(fd, id, strlen(id));
            close(fd);
         }
         else
         {
            /* Another process won the race — read their ID */
            FILE *fp = fopen(path, "r");
            if (fp)
            {
               char tmp[64] = "";
               if (fgets(tmp, sizeof(tmp), fp))
               {
                  size_t len = strlen(tmp);
                  while (len > 0 &&
                         (tmp[len - 1] == '\n' || tmp[len - 1] == '\r' || tmp[len - 1] == ' '))
                     tmp[--len] = '\0';
                  if (tmp[0])
                     snprintf(id, sizeof(id), "%s", tmp);
               }
               fclose(fp);
            }
         }
      }
   }

   return id;
}

void session_id_set_override(const char *sid)
{
   if (!sid || !sid[0])
   {
      g_session_override[0] = '\0';
      return;
   }
   snprintf(g_session_override, sizeof(g_session_override), "%s", sid);
}

void session_id_clear_override(void)
{
   g_session_override[0] = '\0';
}

/* --- Thread-local DB pointer for MCP handlers --- */

static __thread sqlite3 *g_mcp_db;

void mcp_db_set(sqlite3 *db)
{
   g_mcp_db = db;
}

sqlite3 *mcp_db_get(void)
{
   return g_mcp_db;
}

void mcp_db_clear(void)
{
   g_mcp_db = NULL;
}

void session_state_path(char *buf, size_t len)
{
   snprintf(buf, len, "%s/session-%s.state", config_output_dir(), session_id());
}

/* --- Path helpers --- */

const char *config_default_dir(void)
{
   static char dir[MAX_PATH_LEN];
   if (dir[0])
      return dir;

   const char *home = getenv("HOME");
   if (!home)
      home = "/tmp";
   snprintf(dir, sizeof(dir), "%s/.config/aimee", home);
   return dir;
}

const char *config_default_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   snprintf(path, sizeof(path), "%s/config.json", config_default_dir());
   return path;
}

const char *config_output_dir(void)
{
   return config_default_dir();
}

/* --- Load (with mtime cache) --- */

static config_t g_config_cache;
static time_t g_config_mtime;
static int g_config_cached;

/* Strict mode: errors instead of warnings, exit non-zero on validation failure */
int g_config_strict;

/* --- Config schema --- */

static const config_schema_entry_t config_schema[] = {
    {"db_path", SCHEMA_STRING, 0},         {"guardrail_mode", SCHEMA_STRING, 0},
    {"provider", SCHEMA_STRING, 0},        {"use_builtin_cli", SCHEMA_BOOL, 0},
    {"openai_endpoint", SCHEMA_STRING, 0}, {"openai_model", SCHEMA_STRING, 0},
    {"openai_key_cmd", SCHEMA_STRING, 0},  {"embedding_command", SCHEMA_STRING, 0},
    {"workspace_root", SCHEMA_STRING, 0},  {"workspaces", SCHEMA_ARRAY, 0},
    {"autonomous", SCHEMA_BOOL, 0},        {"cross_verify", SCHEMA_OBJECT, 0},
    {"retry", SCHEMA_OBJECT, 0},           {NULL, 0, 0}};

static const char *schema_type_name(schema_type_t t)
{
   switch (t)
   {
   case SCHEMA_STRING:
      return "string";
   case SCHEMA_INT:
      return "integer";
   case SCHEMA_BOOL:
      return "boolean";
   case SCHEMA_ARRAY:
      return "array";
   case SCHEMA_OBJECT:
      return "object";
   }
   return "unknown";
}

static const char *cjson_type_name(const cJSON *item)
{
   if (cJSON_IsString(item))
      return "string";
   if (cJSON_IsNumber(item))
      return "integer";
   if (cJSON_IsBool(item))
      return "boolean";
   if (cJSON_IsArray(item))
      return "array";
   if (cJSON_IsObject(item))
      return "object";
   if (cJSON_IsNull(item))
      return "null";
   return "unknown";
}

static int schema_type_matches(schema_type_t expected, const cJSON *item)
{
   switch (expected)
   {
   case SCHEMA_STRING:
      return cJSON_IsString(item);
   case SCHEMA_INT:
      return cJSON_IsNumber(item);
   case SCHEMA_BOOL:
      return cJSON_IsBool(item);
   case SCHEMA_ARRAY:
      return cJSON_IsArray(item);
   case SCHEMA_OBJECT:
      return cJSON_IsObject(item);
   }
   return 0;
}

static int config_validate(const cJSON *root)
{
   int issues = 0;
   const char *level = g_config_strict ? "error" : "warning";

   /* Check each key in the config against the schema */
   const cJSON *item;
   cJSON_ArrayForEach(item, root)
   {
      const config_schema_entry_t *found = NULL;
      for (const config_schema_entry_t *s = config_schema; s->key; s++)
      {
         if (strcmp(s->key, item->string) == 0)
         {
            found = s;
            break;
         }
      }

      if (!found)
      {
         fprintf(stderr, "aimee: config %s: unknown key \"%s\"\n", level, item->string);
         issues++;
         continue;
      }

      if (!schema_type_matches(found->type, item))
      {
         fprintf(stderr, "aimee: config %s: \"%s\" expected %s, got %s\n", level, item->string,
                 schema_type_name(found->type), cjson_type_name(item));
         issues++;
      }
   }

   /* Check required keys */
   for (const config_schema_entry_t *s = config_schema; s->key; s++)
   {
      if (s->required && !cJSON_GetObjectItemCaseSensitive(root, s->key))
      {
         fprintf(stderr, "aimee: config %s: missing required key \"%s\"\n", level, s->key);
         issues++;
      }
   }

   return issues;
}

int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   /* Defaults */
   snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", db_default_path());
   snprintf(cfg->guardrail_mode, sizeof(cfg->guardrail_mode), "%s", MODE_APPROVE);
   snprintf(cfg->provider, sizeof(cfg->provider), "claude");
   cfg->use_builtin_cli = 0;
   snprintf(cfg->openai_endpoint, sizeof(cfg->openai_endpoint), "https://api.openai.com/v1");
   snprintf(cfg->openai_model, sizeof(cfg->openai_model), "gpt-4o");
   cfg->openai_key_cmd[0] = '\0';
   cfg->workspace_root[0] = '\0';
   cfg->workspace_count = 0;

   const char *path = config_default_path();

   /* Return cached config if mtime unchanged and caching enabled */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      struct stat st;
      if (stat(path, &st) == 0 && g_config_cached && st.st_mtime == g_config_mtime)
      {
         memcpy(cfg, &g_config_cache, sizeof(*cfg));
         return 0;
      }
   }

   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0; /* defaults are fine */

   fseek(fp, 0, SEEK_END);
   long len = ftell(fp);
   fseek(fp, 0, SEEK_SET);

   if (len <= 0 || len > MAX_FILE_SIZE)
   {
      fclose(fp);
      return 0;
   }

   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(fp);
      return -1;
   }

   size_t nread = fread(buf, 1, (size_t)len, fp);
   fclose(fp);
   buf[nread] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return 0;

   /* Validate config against schema */
   int issues = config_validate(root);
   if (issues > 0 && g_config_strict)
   {
      fprintf(stderr, "aimee: strict mode: %d config validation error(s), aborting\n", issues);
      cJSON_Delete(root);
      return -1;
   }

   cJSON *item;

   item = cJSON_GetObjectItemCaseSensitive(root, "db_path");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->db_path, sizeof(cfg->db_path), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "guardrail_mode");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->guardrail_mode, sizeof(cfg->guardrail_mode), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "provider");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->provider, sizeof(cfg->provider), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "openai_endpoint");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->openai_endpoint, sizeof(cfg->openai_endpoint), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "openai_model");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->openai_model, sizeof(cfg->openai_model), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "openai_key_cmd");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->openai_key_cmd, sizeof(cfg->openai_key_cmd), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "use_builtin_cli");
   if (cJSON_IsBool(item))
      cfg->use_builtin_cli = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "autonomous");
   if (cJSON_IsBool(item))
      cfg->autonomous = cJSON_IsTrue(item);

   item = cJSON_GetObjectItemCaseSensitive(root, "embedding_command");
   if (cJSON_IsString(item) && item->valuestring[0])
      snprintf(cfg->embedding_command, sizeof(cfg->embedding_command), "%s", item->valuestring);

   item = cJSON_GetObjectItemCaseSensitive(root, "workspace_root");
   if (cJSON_IsString(item) && item->valuestring[0])
   {
      /* Expand ~ to $HOME */
      if (item->valuestring[0] == '~' &&
          (item->valuestring[1] == '/' || item->valuestring[1] == '\0'))
      {
         const char *home = getenv("HOME");
         if (!home)
            home = "/tmp";
         snprintf(cfg->workspace_root, sizeof(cfg->workspace_root), "%s%s", home,
                  item->valuestring + 1);
      }
      else
      {
         snprintf(cfg->workspace_root, sizeof(cfg->workspace_root), "%s", item->valuestring);
      }
   }

   cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "workspaces");
   if (cJSON_IsArray(ws))
   {
      int i = 0;
      cJSON *el;
      cJSON_ArrayForEach(el, ws)
      {
         if (i >= 64)
            break;
         if (cJSON_IsString(el) && el->valuestring[0])
         {
            /* Resolve relative paths against workspace_root, falling back to CWD */
            if (el->valuestring[0] != '/')
            {
               const char *base = cfg->workspace_root[0] ? cfg->workspace_root : NULL;
               char cwd_buf[MAX_PATH_LEN];
               if (!base)
               {
                  if (getcwd(cwd_buf, sizeof(cwd_buf)))
                     base = cwd_buf;
                  else
                     base = "/tmp";
               }
               if (strcmp(el->valuestring, ".") == 0)
                  snprintf(cfg->workspaces[i], MAX_PATH_LEN, "%s", base);
               else
                  snprintf(cfg->workspaces[i], MAX_PATH_LEN, "%s/%s", base, el->valuestring);
            }
            else
               snprintf(cfg->workspaces[i], MAX_PATH_LEN, "%s", el->valuestring);
            i++;
         }
      }
      cfg->workspace_count = i;
   }

   /* Cross-verification */
   cJSON *cv = cJSON_GetObjectItemCaseSensitive(root, "cross_verify");
   if (cJSON_IsObject(cv))
   {
      item = cJSON_GetObjectItemCaseSensitive(cv, "enabled");
      if (cJSON_IsBool(item))
         cfg->cross_verify = cJSON_IsTrue(item);

      item = cJSON_GetObjectItemCaseSensitive(cv, "verify_cmd");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->verify_cmd, sizeof(cfg->verify_cmd), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(cv, "role");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->verify_role, sizeof(cfg->verify_role), "%s", item->valuestring);

      item = cJSON_GetObjectItemCaseSensitive(cv, "prompt");
      if (cJSON_IsString(item) && item->valuestring[0])
         snprintf(cfg->verify_prompt, sizeof(cfg->verify_prompt), "%s", item->valuestring);
   }

   /* API retry settings */
   cJSON *retry = cJSON_GetObjectItemCaseSensitive(root, "retry");
   if (cJSON_IsObject(retry))
   {
      item = cJSON_GetObjectItemCaseSensitive(retry, "max_attempts");
      if (cJSON_IsNumber(item))
         cfg->retry_max_attempts = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(retry, "base_ms");
      if (cJSON_IsNumber(item))
         cfg->retry_base_ms = (int)item->valuedouble;

      item = cJSON_GetObjectItemCaseSensitive(retry, "max_ms");
      if (cJSON_IsNumber(item))
         cfg->retry_max_ms = (int)item->valuedouble;
   }

   cJSON_Delete(root);

   /* Update mtime cache */
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         memcpy(&g_config_cache, cfg, sizeof(g_config_cache));
         g_config_mtime = st.st_mtime;
         g_config_cached = 1;
      }
   }

   return 0;
}

/* --- Save --- */

static void ensure_config_dir(void)
{
   const char *dir = config_default_dir();
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", dir);

   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(tmp, 0700);
         *p = '/';
      }
   }
   mkdir(tmp, 0700);
}

int config_save(const config_t *cfg)
{
   ensure_config_dir();

   cJSON *root = cJSON_CreateObject();
   if (!root)
      return -1;

   cJSON_AddStringToObject(root, "db_path", cfg->db_path);
   cJSON_AddStringToObject(root, "guardrail_mode", cfg->guardrail_mode);
   cJSON_AddStringToObject(root, "provider", cfg->provider);

   if (cfg->use_builtin_cli)
      cJSON_AddTrueToObject(root, "use_builtin_cli");
   if (cfg->autonomous)
      cJSON_AddTrueToObject(root, "autonomous");

   if (cfg->openai_endpoint[0])
      cJSON_AddStringToObject(root, "openai_endpoint", cfg->openai_endpoint);
   if (cfg->openai_model[0])
      cJSON_AddStringToObject(root, "openai_model", cfg->openai_model);
   if (cfg->openai_key_cmd[0])
      cJSON_AddStringToObject(root, "openai_key_cmd", cfg->openai_key_cmd);
   if (cfg->embedding_command[0])
      cJSON_AddStringToObject(root, "embedding_command", cfg->embedding_command);

   /* workspaces: always store as absolute paths */
   cJSON *ws = cJSON_AddArrayToObject(root, "workspaces");
   for (int i = 0; i < cfg->workspace_count; i++)
      cJSON_AddItemToArray(ws, cJSON_CreateString(cfg->workspaces[i]));

   /* Cross-verification */
   if (cfg->cross_verify || cfg->verify_cmd[0] || cfg->verify_role[0])
   {
      cJSON *cv = cJSON_AddObjectToObject(root, "cross_verify");
      cJSON_AddBoolToObject(cv, "enabled", cfg->cross_verify);
      if (cfg->verify_cmd[0])
         cJSON_AddStringToObject(cv, "verify_cmd", cfg->verify_cmd);
      if (cfg->verify_role[0])
         cJSON_AddStringToObject(cv, "role", cfg->verify_role);
      if (cfg->verify_prompt[0])
         cJSON_AddStringToObject(cv, "prompt", cfg->verify_prompt);
   }

   /* API retry settings (only save if non-default) */
   if (cfg->retry_max_attempts || cfg->retry_base_ms || cfg->retry_max_ms)
   {
      cJSON *retry = cJSON_AddObjectToObject(root, "retry");
      if (cfg->retry_max_attempts)
         cJSON_AddNumberToObject(retry, "max_attempts", cfg->retry_max_attempts);
      if (cfg->retry_base_ms)
         cJSON_AddNumberToObject(retry, "base_ms", cfg->retry_base_ms);
      if (cfg->retry_max_ms)
         cJSON_AddNumberToObject(retry, "max_ms", cfg->retry_max_ms);
   }

   char *json_str = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json_str)
      return -1;

   const char *path = config_default_path();

   /* Atomic write: write to temp file, then rename */
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
   FILE *fp = fopen(tmp, "w");
   if (!fp)
   {
      free(json_str);
      return -1;
   }

   fputs(json_str, fp);
   fputc('\n', fp);
   fclose(fp);
   free(json_str);

   /* Restrict permissions (may reference sensitive config) */
   chmod(tmp, 0600);

   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

/* --- Guardrail mode --- */

const char *config_guardrail_mode(const config_t *cfg)
{
   if (cfg->guardrail_mode[0])
      return cfg->guardrail_mode;
   return MODE_APPROVE;
}

/* --- Conversation directories --- */

int config_conversation_dirs(const config_t *cfg, char dirs[][MAX_PATH_LEN], int max_dirs)
{
   const char *home = getenv("HOME");
   if (!home)
      home = "/tmp";

   const char *provider = cfg->provider[0] ? cfg->provider : "claude";
   int count = 0;

   if (strcmp(provider, "claude") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.claude/projects", home);
         count++;
      }
   }
   else if (strcmp(provider, "gemini") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.gemini/tmp", home);
         count++;
      }
   }
   else if (strcmp(provider, "codex") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.codex/sessions", home);
         count++;
      }
   }
   else if (strcmp(provider, "copilot") == 0)
   {
      if (count < max_dirs)
      {
         snprintf(dirs[count], MAX_PATH_LEN, "%s/.copilot", home);
         count++;
      }
   }

   return count;
}

/* --- Provider path --- */

const char *config_provider_path(const config_t *cfg)
{
   static char path[MAX_PATH_LEN];
   const char *home = getenv("HOME");
   if (!home)
      home = "/tmp";

   const char *provider = cfg->provider[0] ? cfg->provider : "claude";

   if (strcmp(provider, "claude") == 0)
      snprintf(path, sizeof(path), "%s/.claude/settings.json", home);
   else if (strcmp(provider, "gemini") == 0)
      snprintf(path, sizeof(path), "%s/.gemini/settings.json", home);
   else if (strcmp(provider, "codex") == 0)
      snprintf(path, sizeof(path), "%s/.codex/config.json", home);
   else if (strcmp(provider, "copilot") == 0)
      snprintf(path, sizeof(path), "%s/.copilot/config.json", home);
   else
      snprintf(path, sizeof(path), "%s/.%s/settings.json", home, provider);

   return path;
}
