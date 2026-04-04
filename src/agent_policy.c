/* agent_policy.c: validation, policy, trace, metrics, env, manifest, contract */
#include "aimee.h"
#include "agent.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Tool registry validation --- */

int tool_validate(sqlite3 *db, const char *tool_name, const char *args_json, char *err_out,
                  size_t err_len)
{
   if (!db || !tool_name || !args_json)
      return 0;

   static const char *sql = "SELECT input_schema, enabled FROM tool_registry WHERE name = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, tool_name, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_ROW)
      return 0;

   int enabled = sqlite3_column_int(stmt, 1);
   if (!enabled)
   {
      snprintf(err_out, err_len, "tool '%s' is disabled", tool_name);
      return -1;
   }

   const char *schema_str = (const char *)sqlite3_column_text(stmt, 0);
   if (!schema_str || !schema_str[0])
      return 0;

   cJSON *schema = cJSON_Parse(schema_str);
   if (!schema)
      return 0;

   cJSON *args = cJSON_Parse(args_json);
   if (!args)
   {
      snprintf(err_out, err_len, "invalid JSON arguments");
      cJSON_Delete(schema);
      return -1;
   }

   cJSON *required = cJSON_GetObjectItem(schema, "required");
   if (required && cJSON_IsArray(required))
   {
      int n = cJSON_GetArraySize(required);
      for (int i = 0; i < n; i++)
      {
         cJSON *req = cJSON_GetArrayItem(required, i);
         if (req && cJSON_IsString(req))
         {
            cJSON *field = cJSON_GetObjectItem(args, req->valuestring);
            if (!field)
            {
               snprintf(err_out, err_len, "missing required field '%s'", req->valuestring);
               cJSON_Delete(schema);
               cJSON_Delete(args);
               return -1;
            }
         }
      }
   }

   cJSON *props = cJSON_GetObjectItem(schema, "properties");
   if (props && cJSON_IsObject(props))
   {
      cJSON *prop = props->child;
      while (prop)
      {
         cJSON *field = cJSON_GetObjectItem(args, prop->string);
         if (field)
         {
            cJSON *type_spec = cJSON_GetObjectItem(prop, "type");
            if (type_spec && cJSON_IsString(type_spec))
            {
               const char *expected = type_spec->valuestring;
               int ok = 1;
               if (strcmp(expected, "string") == 0 && !cJSON_IsString(field))
                  ok = 0;
               else if (strcmp(expected, "integer") == 0 && !cJSON_IsNumber(field))
                  ok = 0;
               else if (strcmp(expected, "number") == 0 && !cJSON_IsNumber(field))
                  ok = 0;
               else if (strcmp(expected, "boolean") == 0 && !cJSON_IsBool(field))
                  ok = 0;
               if (!ok)
               {
                  snprintf(err_out, err_len, "field '%s' should be %s", prop->string, expected);
                  cJSON_Delete(schema);
                  cJSON_Delete(args);
                  return -1;
               }
            }
         }
         prop = prop->next;
      }
   }

   cJSON_Delete(schema);
   cJSON_Delete(args);
   return 0;
}

const char *tool_side_effect(sqlite3 *db, const char *tool_name)
{
   if (!db || !tool_name)
      return "read";

   static const char *sql = "SELECT side_effect FROM tool_registry WHERE name = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return "read";

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, tool_name, -1, SQLITE_TRANSIENT);

   static char buf[32];
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *val = (const char *)sqlite3_column_text(stmt, 0);
      snprintf(buf, sizeof(buf), "%s", val ? val : "read");
      return buf;
   }
   return "read";
}

/* --- Policy checking --- */

static char *g_policy_json = NULL;

int policy_load(void)
{
   const char *paths[] = {".aimee-policy.json", NULL};
   char global_path[MAX_PATH_LEN];
   snprintf(global_path, sizeof(global_path), "%s/policy.json", config_default_dir());
   paths[1] = global_path;

   for (int i = 0; i < 2; i++)
   {
      FILE *f = fopen(paths[i], "r");
      if (!f)
         continue;
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz > 0 && sz < 1024 * 1024)
      {
         free(g_policy_json);
         g_policy_json = malloc((size_t)sz + 1);
         if (!g_policy_json)
         {
            fclose(f);
            return -1;
         }
         size_t nread = fread(g_policy_json, 1, (size_t)sz, f);
         if (ferror(f) || (long)nread != sz)
         {
            free(g_policy_json);
            g_policy_json = NULL;
            fclose(f);
            return -1;
         }
         g_policy_json[nread] = '\0';
      }
      fclose(f);
      return 0;
   }
   return -1;
}

int policy_check_tool(const char *tool_name, const char *side_effect, const char *args_json,
                      char *reason_out, size_t reason_len)
{
   if (!g_policy_json)
   {
      policy_load();
      if (!g_policy_json)
         return 0;
   }

   cJSON *policy = cJSON_Parse(g_policy_json);
   if (!policy)
      return 0;

   if (strcmp(tool_name, "bash") == 0 && args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      cJSON *cmd = args ? cJSON_GetObjectItem(args, "command") : NULL;

      cJSON *forbidden = cJSON_GetObjectItem(policy, "forbidden_commands");
      if (forbidden && cJSON_IsArray(forbidden) && cmd && cJSON_IsString(cmd))
      {
         int n = cJSON_GetArraySize(forbidden);
         for (int i = 0; i < n; i++)
         {
            cJSON *pat = cJSON_GetArrayItem(forbidden, i);
            if (pat && cJSON_IsString(pat) && strstr(cmd->valuestring, pat->valuestring))
            {
               snprintf(reason_out, reason_len, "command matches forbidden pattern: %s",
                        pat->valuestring);
               cJSON_Delete(args);
               cJSON_Delete(policy);
               return -1;
            }
         }
      }
      cJSON_Delete(args);
   }

   /* Check tool_rules path-prefix restrictions (Feature 4) */
   cJSON *tool_rules = cJSON_GetObjectItem(policy, "tool_rules");
   if (tool_rules && cJSON_IsArray(tool_rules) && args_json)
   {
      /* Extract path from tool args */
      cJSON *args = cJSON_Parse(args_json);
      const char *target_path = NULL;
      if (args)
      {
         cJSON *p = cJSON_GetObjectItem(args, "path");
         if (p && cJSON_IsString(p))
            target_path = p->valuestring;
         else
         {
            cJSON *cmd = cJSON_GetObjectItem(args, "command");
            if (cmd && cJSON_IsString(cmd))
               target_path = cmd->valuestring;
         }
      }

      if (target_path)
      {
         int n = cJSON_GetArraySize(tool_rules);
         for (int i = 0; i < n; i++)
         {
            cJSON *rule = cJSON_GetArrayItem(tool_rules, i);
            if (!rule)
               continue;
            cJSON *prefix = cJSON_GetObjectItem(rule, "path_prefix");
            if (!prefix || !cJSON_IsString(prefix))
               continue;

            size_t plen = strlen(prefix->valuestring);
            if (strncmp(target_path, prefix->valuestring, plen) == 0 &&
                (target_path[plen] == '/' || target_path[plen] == '\0'))
            {
               /* Path matches this rule; check if tool is allowed */
               cJSON *allowed = cJSON_GetObjectItem(rule, "allowed_tools");
               if (allowed && cJSON_IsArray(allowed))
               {
                  int found = 0;
                  int an = cJSON_GetArraySize(allowed);
                  for (int j = 0; j < an; j++)
                  {
                     cJSON *at = cJSON_GetArrayItem(allowed, j);
                     if (at && cJSON_IsString(at) && strcmp(at->valuestring, tool_name) == 0)
                     {
                        found = 1;
                        break;
                     }
                  }
                  if (!found)
                  {
                     snprintf(reason_out, reason_len, "tool '%s' not allowed for path %s",
                              tool_name, prefix->valuestring);
                     cJSON_Delete(args);
                     cJSON_Delete(policy);
                     return -1;
                  }
               }
               break; /* First matching prefix wins */
            }
         }
      }
      cJSON_Delete(args);
   }

   cJSON *levels = cJSON_GetObjectItem(policy, "approval_levels");
   if (levels && side_effect)
   {
      cJSON *level = cJSON_GetObjectItem(levels, side_effect);
      if (level && cJSON_IsString(level) && strcmp(level->valuestring, "block") == 0)
      {
         snprintf(reason_out, reason_len, "policy blocks %s operations", side_effect);
         cJSON_Delete(policy);
         return -1;
      }
   }

   cJSON_Delete(policy);
   return 0;
}

/* --- Execution trace --- */

void agent_trace_log(sqlite3 *db, int plan_id, int turn, const char *direction, const char *content,
                     const char *tool_name, const char *tool_args, const char *tool_result,
                     const char *context_hash)
{
   if (!db)
      return;

   static const char *sql = "INSERT INTO execution_trace (plan_id, turn, direction, content,"
                            " tool_name, tool_args, tool_result, context_hash)"
                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_reset(stmt);
   if (plan_id > 0)
      sqlite3_bind_int(stmt, 1, plan_id);
   else
      sqlite3_bind_null(stmt, 1);
   sqlite3_bind_int(stmt, 2, turn);
   sqlite3_bind_text(stmt, 3, direction, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, content ? content : "", -1, SQLITE_TRANSIENT);
   if (tool_name)
      sqlite3_bind_text(stmt, 5, tool_name, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 5);
   if (tool_args)
      sqlite3_bind_text(stmt, 6, tool_args, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 6);
   if (tool_result)
      sqlite3_bind_text(stmt, 7, tool_result, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 7);
   if (context_hash)
      sqlite3_bind_text(stmt, 8, context_hash, -1, SQLITE_TRANSIENT);
   else
      sqlite3_bind_null(stmt, 8);
   DB_STEP_LOG(stmt, "agent_trace_log");
}

/* --- Confidence estimation --- */

int agent_estimate_confidence(const char *response_text)
{
   if (!response_text || !response_text[0])
      return 50;

   int confidence = 80;

   static const char *low_markers[] = {"I'm not sure", "not certain",   "unclear",      "I think",
                                       "might",        "possibly",      "I don't know", "uncertain",
                                       "may not",      "I cannot tell", "hard to say",  NULL};

   for (int i = 0; low_markers[i]; i++)
   {
      if (strcasestr(response_text, low_markers[i]))
      {
         confidence -= 15;
         break;
      }
   }

   static const char *high_markers[] = {"successfully", "completed",         "confirmed",
                                        "verified",     "all checks passed", NULL};

   for (int i = 0; high_markers[i]; i++)
   {
      if (strcasestr(response_text, high_markers[i]))
      {
         confidence += 10;
         break;
      }
   }

   if (confidence < 0)
      confidence = 0;
   if (confidence > 100)
      confidence = 100;

   return confidence;
}

/* --- Prometheus metrics --- */

static void prom_escape(const char *in, char *out, size_t out_len)
{
   size_t j = 0;
   for (size_t i = 0; in[i] && j < out_len - 1; i++)
   {
      if (in[i] == '\\' || in[i] == '"')
      {
         if (j + 2 >= out_len)
            break;
         out[j++] = '\\';
         out[j++] = in[i];
      }
      else if (in[i] == '\n')
      {
         if (j + 2 >= out_len)
            break;
         out[j++] = '\\';
         out[j++] = 'n';
      }
      else
      {
         out[j++] = in[i];
      }
   }
   out[j] = '\0';
}

void agent_write_metrics(sqlite3 *db)
{
   if (!db)
      return;

   const char *metrics_path = "/var/lib/prometheus/node-exporter/aimee.prom";
   FILE *f = fopen(metrics_path, "w");
   if (!f)
   {
      f = fopen("/tmp/aimee-metrics.prom", "w");
      if (!f)
         return;
      chmod("/tmp/aimee-metrics.prom", 0600);
   }

   static const char *sql = "SELECT agent_name, role,"
                            " COUNT(*) as total,"
                            " SUM(CASE WHEN success THEN 1 ELSE 0 END) as successes,"
                            " SUM(prompt_tokens) as ptok,"
                            " SUM(completion_tokens) as ctok,"
                            " AVG(latency_ms) as avg_lat,"
                            " SUM(tool_calls) as total_tools"
                            " FROM agent_log"
                            " GROUP BY agent_name, role";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
   {
      fclose(f);
      return;
   }

   fprintf(f, "# HELP aimee_delegations_total Total delegations by role and agent\n");
   fprintf(f, "# TYPE aimee_delegations_total counter\n");
   fprintf(f, "# HELP aimee_delegation_successes_total Successful delegations\n");
   fprintf(f, "# TYPE aimee_delegation_successes_total counter\n");
   fprintf(f, "# HELP aimee_delegation_latency_avg_ms Average delegation latency\n");
   fprintf(f, "# TYPE aimee_delegation_latency_avg_ms gauge\n");
   fprintf(f, "# HELP aimee_tokens_total Total tokens used\n");
   fprintf(f, "# TYPE aimee_tokens_total counter\n");
   fprintf(f, "# HELP aimee_tool_calls_total Total tool calls\n");
   fprintf(f, "# TYPE aimee_tool_calls_total counter\n");

   sqlite3_reset(stmt);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *agent = (const char *)sqlite3_column_text(stmt, 0);
      const char *role = (const char *)sqlite3_column_text(stmt, 1);
      int total = sqlite3_column_int(stmt, 2);
      int successes = sqlite3_column_int(stmt, 3);
      int ptok = sqlite3_column_int(stmt, 4);
      int ctok = sqlite3_column_int(stmt, 5);
      int avg_lat = sqlite3_column_int(stmt, 6);
      int total_tools = sqlite3_column_int(stmt, 7);

      if (!agent)
         agent = "unknown";
      if (!role)
         role = "unknown";

      char safe_agent[128], safe_role[128];
      prom_escape(agent, safe_agent, sizeof(safe_agent));
      prom_escape(role, safe_role, sizeof(safe_role));

      fprintf(f, "aimee_delegations_total{agent=\"%s\",role=\"%s\"} %d\n", safe_agent, safe_role,
              total);
      fprintf(f, "aimee_delegation_successes_total{agent=\"%s\",role=\"%s\"} %d\n", safe_agent,
              safe_role, successes);
      fprintf(f, "aimee_delegation_latency_avg_ms{agent=\"%s\",role=\"%s\"} %d\n", safe_agent,
              safe_role, avg_lat);
      fprintf(f, "aimee_tokens_total{agent=\"%s\",role=\"%s\",type=\"prompt\"} %d\n", safe_agent,
              safe_role, ptok);
      fprintf(f, "aimee_tokens_total{agent=\"%s\",role=\"%s\",type=\"completion\"} %d\n",
              safe_agent, safe_role, ctok);
      fprintf(f, "aimee_tool_calls_total{agent=\"%s\",role=\"%s\"} %d\n", safe_agent, safe_role,
              total_tools);
   }

   fclose(f);
}

/* --- Environment introspection --- */

static void env_set(sqlite3 *db, const char *key, const char *value)
{
   static const char *sql = "INSERT OR REPLACE INTO env_capabilities (key, value, detected_at)"
                            " VALUES (?, ?, datetime('now'))";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "env_set");
}

static int check_command(const char *cmd)
{
   const char *argv[] = {"which", cmd, NULL};
   char *output = NULL;
   int rc = safe_exec_capture(argv, &output, 256);
   free(output);
   return rc == 0;
}

void agent_introspect_env(sqlite3 *db)
{
   if (!db)
      return;

   /* Toolchains */
   static const char *tools[] = {"gcc",  "clang", "make",  "dotnet", "node",    "npm",    "python3",
                                 "pip3", "go",    "rustc", "java",   "mvn",     "gradle", "docker",
                                 "git",  "ssh",   "curl",  "jq",     "sqlite3", NULL};

   for (int i = 0; tools[i]; i++)
   {
      env_set(db, tools[i], check_command(tools[i]) ? "available" : "missing");
   }

   /* Package managers */
   static const char *pkg_mgrs[] = {"apt-get", "brew", "dnf", "pacman", "apk", NULL};
   for (int i = 0; pkg_mgrs[i]; i++)
   {
      if (check_command(pkg_mgrs[i]))
      {
         env_set(db, "package_manager", pkg_mgrs[i]);
         break;
      }
   }

   /* OS info — use safe argv execution instead of popen shell pipeline */
   {
      const char *argv[] = {"uname", "-s", "-r", "-m", NULL};
      char *out = NULL;
      if (safe_exec_capture(argv, &out, 256) == 0 && out)
      {
         size_t len = strlen(out);
         while (len > 0 && out[len - 1] == '\n')
            out[--len] = '\0';
         if (out[0])
            env_set(db, "os", out);
      }
      free(out);
   }

   /* Disk space — use safe argv execution */
   {
      const char *argv[] = {"df", "-h", "/", NULL};
      char *out = NULL;
      if (safe_exec_capture(argv, &out, 1024) == 0 && out)
      {
         /* Parse last line, 4th field (Available) */
         char *last_line = out;
         char *nl;
         while ((nl = strchr(last_line, '\n')) != NULL && nl[1])
            last_line = nl + 1;
         /* Skip whitespace-separated fields to get 4th */
         int field = 0;
         char *p = last_line;
         while (*p && field < 3)
         {
            while (*p && *p != ' ' && *p != '\t')
               p++;
            while (*p == ' ' || *p == '\t')
               p++;
            field++;
         }
         if (*p)
         {
            char avail[64] = {0};
            size_t ai = 0;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && ai < sizeof(avail) - 1)
               avail[ai++] = *p++;
            avail[ai] = '\0';
            if (avail[0])
               env_set(db, "disk_free", avail);
         }
      }
      free(out);
   }

   /* Memory — read from /proc/meminfo if available, fall back to safe exec */
   {
      FILE *f = fopen("/proc/meminfo", "r");
      if (f)
      {
         char line[128];
         while (fgets(line, sizeof(line), f))
         {
            if (strncmp(line, "MemTotal:", 9) == 0)
            {
               /* Parse "MemTotal:    XXXX kB" */
               char *p = line + 9;
               while (*p == ' ')
                  p++;
               long kb = strtol(p, NULL, 10);
               char mem_str[32];
               if (kb > 1048576)
                  snprintf(mem_str, sizeof(mem_str), "%.1fGi", (double)kb / 1048576.0);
               else
                  snprintf(mem_str, sizeof(mem_str), "%ldMi", kb / 1024);
               env_set(db, "total_memory", mem_str);
               break;
            }
         }
         fclose(f);
      }
      else
      {
         const char *argv[] = {"free", "-h", NULL};
         char *out = NULL;
         if (safe_exec_capture(argv, &out, 512) == 0 && out)
         {
            char *mem_line = strstr(out, "Mem:");
            if (mem_line)
            {
               char *p = mem_line + 4;
               while (*p == ' ' || *p == '\t')
                  p++;
               char total[32] = {0};
               size_t ti = 0;
               while (*p && *p != ' ' && *p != '\t' && *p != '\n' && ti < sizeof(total) - 1)
                  total[ti++] = *p++;
               total[ti] = '\0';
               if (total[0])
                  env_set(db, "total_memory", total);
            }
         }
         free(out);
      }
   }
}

/* --- Change manifests --- */

void agent_write_manifest(sqlite3 *db, const char *run_id, const agent_result_t *result,
                          const char *role)
{
   if (!db || !run_id || !result)
      return;

   char manifest_dir[MAX_PATH_LEN];
   snprintf(manifest_dir, sizeof(manifest_dir), "%s/manifests", config_default_dir());
   mkdir(manifest_dir, 0700);

   /* Build manifest from execution trace */
   cJSON *manifest = cJSON_CreateObject();
   cJSON_AddStringToObject(manifest, "run_id", run_id);
   cJSON_AddStringToObject(manifest, "agent", result->agent_name);
   cJSON_AddStringToObject(manifest, "role", role ? role : "");

   char now[32];
   now_utc(now, sizeof(now));
   cJSON_AddStringToObject(manifest, "timestamp", now);

   cJSON_AddNumberToObject(manifest, "confidence", result->confidence);
   cJSON_AddNumberToObject(manifest, "turns", result->turns);
   cJSON_AddNumberToObject(manifest, "tool_calls", result->tool_calls);
   cJSON_AddBoolToObject(manifest, "success", result->success);

   /* Collect commands from trace */
   cJSON *commands = cJSON_CreateArray();
   static const char *sql = "SELECT tool_name, tool_args, tool_result"
                            " FROM execution_trace"
                            " WHERE tool_name IS NOT NULL"
                            " ORDER BY id DESC LIMIT 50";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (stmt)
   {
      sqlite3_reset(stmt);
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         cJSON *entry = cJSON_CreateObject();
         const char *tn = (const char *)sqlite3_column_text(stmt, 0);
         const char *ta = (const char *)sqlite3_column_text(stmt, 1);
         cJSON_AddStringToObject(entry, "tool", tn ? tn : "");
         cJSON_AddStringToObject(entry, "args", ta ? ta : "");
         cJSON_AddItemToArray(commands, entry);
      }
   }
   cJSON_AddItemToObject(manifest, "tool_calls_detail", commands);

   char *json = cJSON_Print(manifest);
   cJSON_Delete(manifest);
   if (!json)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s.json", manifest_dir, run_id);
   FILE *fp = fopen(path, "w");
   if (fp)
   {
      fputs(json, fp);
      fputc('\n', fp);
      fclose(fp);
   }
   free(json);
}

/* --- Repo contract (.aimee/project.yaml) --- */

char *agent_load_project_contract(const char *project_root)
{
   char path[MAX_PATH_LEN];
   if (project_root && project_root[0])
      snprintf(path, sizeof(path), "%s/.aimee/project.yaml", project_root);
   else
      snprintf(path, sizeof(path), ".aimee/project.yaml");

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   /* Parse simple YAML key: value pairs */
   char name[128] = {0}, lang[64] = {0};
   char build_cmd[512] = {0}, test_cmd[512] = {0}, lint_cmd[512] = {0};
   char dod[1024] = {0};
   char risky[1024] = {0};
   int in_dod = 0, in_risky = 0;

   char line[512];
   while (fgets(line, sizeof(line), f))
   {
      /* Strip trailing newline */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      /* Check for list items under definition_of_done or risky_paths */
      if (in_dod && line[0] == ' ' && strstr(line, "- "))
      {
         char *item = strstr(line, "- ") + 2;
         size_t dlen = strlen(dod);
         snprintf(dod + dlen, sizeof(dod) - dlen, "  - %s\n", item);
         continue;
      }
      else if (in_risky && line[0] == ' ' && strstr(line, "- "))
      {
         char *item = strstr(line, "- ") + 2;
         size_t rlen = strlen(risky);
         snprintf(risky + rlen, sizeof(risky) - rlen, "  - %s\n", item);
         continue;
      }
      in_dod = 0;
      in_risky = 0;

      /* Parse key: value */
      if (strncmp(line, "name:", 5) == 0)
         snprintf(name, sizeof(name), "%s", line + 6);
      else if (strncmp(line, "language:", 9) == 0)
         snprintf(lang, sizeof(lang), "%s", line + 10);
      else if (strncmp(line, "build:", 6) == 0)
         snprintf(build_cmd, sizeof(build_cmd), "%s", line + 7);
      else if (strncmp(line, "test:", 5) == 0)
         snprintf(test_cmd, sizeof(test_cmd), "%s", line + 6);
      else if (strncmp(line, "lint:", 5) == 0)
         snprintf(lint_cmd, sizeof(lint_cmd), "%s", line + 6);
      else if (strncmp(line, "definition_of_done:", 19) == 0)
         in_dod = 1;
      else if (strncmp(line, "risky_paths:", 12) == 0)
         in_risky = 1;
   }
   fclose(f);

   /* Build structured context */
   size_t out_len = 2048;
   char *out = malloc(out_len);
   if (!out)
      return NULL;

   size_t pos = 0;
   pos += (size_t)snprintf(out + pos, out_len - pos, "# Project Contract\n");
   if (name[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Name: %s\n", name);
   if (lang[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Language: %s\n", lang);
   if (build_cmd[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Build: %s\n", build_cmd);
   if (test_cmd[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Test: %s\n", test_cmd);
   if (lint_cmd[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Lint: %s\n", lint_cmd);
   if (dod[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Definition of Done:\n%s", dod);
   if (risky[0])
      pos += (size_t)snprintf(out + pos, out_len - pos, "Risky Paths (require review):\n%s", risky);
   return out;
}

/* --- Tool result compression (#4) --- */

char *agent_compress_tool_result(const char *raw, size_t raw_len)
{
   if (!raw)
      return strdup("");

   if (raw_len <= AGENT_TOOL_OUTPUT_MAX)
      return strdup(raw);

   /* Keep the last AGENT_TOOL_OUTPUT_MAX bytes (tail is usually most useful) */
   const char *tail = raw + raw_len - AGENT_TOOL_OUTPUT_MAX + 64;
   size_t out_len = AGENT_TOOL_OUTPUT_MAX + 64;
   char *out = malloc(out_len + 1);
   if (!out)
   {
      size_t fb = raw_len > 256 ? 256 : raw_len;
      return strdup(raw + raw_len - fb); /* fallback */
   }

   int hdr = snprintf(out, out_len, "[truncated, showing last %d bytes]\n", AGENT_TOOL_OUTPUT_MAX);
   size_t remain = out_len - (size_t)hdr;
   size_t tail_len = raw_len - (size_t)(tail - raw);
   if (tail_len > remain)
      tail_len = remain;
   memcpy(out + hdr, tail, tail_len);
   out[hdr + (int)tail_len] = '\0';
   return out;
}
