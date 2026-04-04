/* cmd_agent.c: agent subcommand CLI (agent list/test/run/add/remove/setup/token) */
#include "aimee.h"
#include "agent.h"
#include "agent_tunnel.h"
#include "commands.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* File-scope agent config, loaded once by cmd_agent before dispatch */
/* Non-static so cmd_agent_setup.c can reference it. */
agent_config_t s_agent_cfg;

/* --- agent subcommand handlers --- */

static void ag_list(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   (void)argc;
   (void)argv;
   agent_config_t *cfg = &s_agent_cfg;

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *ag = &cfg->agents[i];
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", ag->name);
         cJSON_AddStringToObject(obj, "endpoint", ag->endpoint);
         cJSON_AddStringToObject(obj, "model", ag->model);
         cJSON_AddStringToObject(obj, "auth_type", ag->auth_type);
         cJSON_AddStringToObject(obj, "provider", ag->provider);
         cJSON_AddNumberToObject(obj, "cost_tier", ag->cost_tier);
         cJSON_AddBoolToObject(obj, "enabled", ag->enabled);
         cJSON_AddBoolToObject(obj, "tools_enabled", ag->tools_enabled);
         cJSON_AddNumberToObject(obj, "max_turns", ag->max_turns);
         cJSON_AddNumberToObject(obj, "max_parallel", ag->max_parallel);
         cJSON *roles = cJSON_CreateArray();
         for (int j = 0; j < ag->role_count; j++)
            cJSON_AddItemToArray(roles, cJSON_CreateString(ag->roles[j]));
         cJSON_AddItemToObject(obj, "roles", roles);
         if (ag->exec_role_count > 0)
         {
            cJSON *er = cJSON_CreateArray();
            for (int j = 0; j < ag->exec_role_count; j++)
               cJSON_AddItemToArray(er, cJSON_CreateString(ag->exec_roles[j]));
            cJSON_AddItemToObject(obj, "exec_roles", er);
         }
         cJSON_AddItemToArray(arr, obj);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (cfg->agent_count == 0)
      {
         printf("No agents configured. Use 'aimee agent add' or "
                "edit %s\n",
                agent_config_path());
         return;
      }
      for (int i = 0; i < cfg->agent_count; i++)
      {
         agent_t *ag = &cfg->agents[i];
         printf("%-16s %-6s tier=%d model=%s endpoint=%s%s\n", ag->name, ag->enabled ? "ON" : "OFF",
                ag->cost_tier, ag->model, ag->endpoint, ag->tools_enabled ? " [tools]" : "");
      }
   }
}

static void ag_network(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   (void)argc;
   (void)argv;
   agent_network_t *nw = &s_agent_cfg.network;

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      if (nw->ssh_entry[0])
         cJSON_AddStringToObject(obj, "ssh_entry", nw->ssh_entry);
      if (nw->ssh_key[0])
         cJSON_AddStringToObject(obj, "ssh_key", nw->ssh_key);
      if (nw->host_count > 0)
      {
         cJSON *hosts = cJSON_CreateArray();
         for (int i = 0; i < nw->host_count; i++)
         {
            agent_net_host_t *h = &nw->hosts[i];
            cJSON *ho = cJSON_CreateObject();
            cJSON_AddStringToObject(ho, "name", h->name);
            cJSON_AddStringToObject(ho, "ip", h->ip);
            cJSON_AddStringToObject(ho, "user", h->user);
            if (h->port > 0)
               cJSON_AddNumberToObject(ho, "port", h->port);
            cJSON_AddStringToObject(ho, "desc", h->desc);
            cJSON_AddItemToArray(hosts, ho);
         }
         cJSON_AddItemToObject(obj, "hosts", hosts);
      }
      if (nw->network_count > 0)
      {
         cJSON *nets = cJSON_CreateArray();
         for (int i = 0; i < nw->network_count; i++)
         {
            agent_net_def_t *nd = &nw->networks[i];
            cJSON *no = cJSON_CreateObject();
            cJSON_AddStringToObject(no, "name", nd->name);
            cJSON_AddStringToObject(no, "cidr", nd->cidr);
            cJSON_AddStringToObject(no, "desc", nd->desc);
            cJSON_AddItemToArray(nets, no);
         }
         cJSON_AddItemToObject(obj, "networks", nets);
      }
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (!nw->ssh_entry[0])
      {
         printf("No network configured. Edit %s to add a \"network\" section.\n",
                agent_config_path());
         return;
      }
      printf("Entry point: %s\n", nw->ssh_entry);
      if (nw->ssh_key[0])
         printf("SSH key:     %s\n", nw->ssh_key);
      if (nw->host_count > 0)
      {
         printf("\nHosts:\n");
         for (int i = 0; i < nw->host_count; i++)
         {
            agent_net_host_t *h = &nw->hosts[i];
            if (h->port > 0)
               printf("  %-16s %s:%d  %-8s %s\n", h->name, h->ip, h->port, h->user, h->desc);
            else
               printf("  %-16s %-20s %-8s %s\n", h->name, h->ip, h->user, h->desc);
         }
      }
      if (nw->network_count > 0)
      {
         printf("\nNetworks:\n");
         for (int i = 0; i < nw->network_count; i++)
         {
            agent_net_def_t *nd = &nw->networks[i];
            printf("  %-16s %-20s %s\n", nd->name, nd->cidr, nd->desc);
         }
      }
   }
}

static void ag_test(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   if (argc < 1)
      fatal("usage: aimee agent test <name>");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);

   agent_http_init();
   sqlite3 *adb = ctx_db_open(ctx);
   if (!adb)
      fatal("cannot open database");
   agent_result_t result;
   int rc = agent_execute(adb, ag, NULL, "Respond with 'ok'.", 64, 0.0, &result);
   agent_http_cleanup();

   if (rc == 0)
   {
      printf("Agent '%s' responded: %s\n", ag->name, result.response ? result.response : "(empty)");
      printf("Latency: %dms, Tokens: %d/%d\n", result.latency_ms, result.prompt_tokens,
             result.completion_tokens);
   }
   else
   {
      fprintf(stderr, "Agent '%s' failed: %s\n", ag->name, result.error);
   }
   free(result.response);
   ctx_db_close(ctx);
}

static void ag_run(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   if (argc < 2)
      fatal("usage: aimee agent run <role> \"prompt\" [--system S]");

   opt_parsed_t opts;
   opt_parse(argc, argv, NULL, &opts);
   const char *role = opt_pos(&opts, 0);
   const char *prompt = opt_pos(&opts, 1);
   const char *sys_prompt = opt_get(&opts, "system");
   int max_tokens = opt_get_int(&opts, "max-tokens", 0);

   if (!role || !prompt)
      fatal("usage: aimee agent run <role> \"prompt\" [--system S]");

   agent_http_init();
   sqlite3 *adb = ctx_db_open(ctx);
   if (!adb)
      fatal("cannot open database");
   agent_result_t result;
   int rc = agent_run(adb, &s_agent_cfg, role, sys_prompt, prompt, max_tokens, &result);
   agent_http_cleanup();

   if (rc == 0)
   {
      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "agent", result.agent_name);
         cJSON_AddStringToObject(obj, "response", result.response ? result.response : "");
         cJSON_AddNumberToObject(obj, "prompt_tokens", result.prompt_tokens);
         cJSON_AddNumberToObject(obj, "completion_tokens", result.completion_tokens);
         cJSON_AddNumberToObject(obj, "latency_ms", result.latency_ms);
         emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      }
      else
      {
         printf("%s\n", result.response ? result.response : "");
      }
   }
   else
   {
      fatal("agent failed: %s", result.error);
   }
   free(result.response);
   ctx_db_close(ctx);
}

static void ag_parallel(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   if (argc < 1)
      fatal("usage: aimee agent parallel '<json tasks array>'\n"
            "  Each task: {\"role\":\"...\",\"prompt\":\"...\","
            "\"system\":\"...\",\"max_tokens\":N}");

   cJSON *tasks_json = cJSON_Parse(argv[0]);
   if (!tasks_json || !cJSON_IsArray(tasks_json))
      fatal("invalid JSON tasks array");

   int n = cJSON_GetArraySize(tasks_json);
   agent_task_t *tasks = calloc((size_t)n, sizeof(agent_task_t));
   agent_result_t *results = calloc((size_t)n, sizeof(agent_result_t));
   if (!tasks || !results)
   {
      free(tasks);
      free(results);
      cJSON_Delete(tasks_json);
      fatal("memory allocation failed");
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_GetArrayItem(tasks_json, i);
      cJSON *role = cJSON_GetObjectItem(t, "role");
      cJSON *prompt = cJSON_GetObjectItem(t, "prompt");
      cJSON *sys = cJSON_GetObjectItem(t, "system");
      cJSON *mt = cJSON_GetObjectItem(t, "max_tokens");
      tasks[i].role = (role && cJSON_IsString(role)) ? role->valuestring : "draft";
      tasks[i].user_prompt = (prompt && cJSON_IsString(prompt)) ? prompt->valuestring : "";
      tasks[i].system_prompt = (sys && cJSON_IsString(sys)) ? sys->valuestring : NULL;
      tasks[i].max_tokens = (mt && cJSON_IsNumber(mt)) ? mt->valueint : 0;
      tasks[i].temperature = 0.3;
   }

   agent_http_init();
   sqlite3 *adb = ctx_db_open(ctx);
   if (!adb)
      fatal("cannot open database");
   int ok = agent_run_parallel(adb, &s_agent_cfg, tasks, n, results);
   agent_http_cleanup();

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "agent", results[i].agent_name);
      cJSON_AddBoolToObject(obj, "success", results[i].success);
      if (results[i].response)
         cJSON_AddStringToObject(obj, "response", results[i].response);
      if (results[i].error[0])
         cJSON_AddStringToObject(obj, "error", results[i].error);
      cJSON_AddNumberToObject(obj, "latency_ms", results[i].latency_ms);
      cJSON_AddItemToArray(arr, obj);
      free(results[i].response);
   }

   if (ctx->json_output)
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   else
   {
      printf("%d/%d tasks completed\n", ok, n);
      cJSON_Delete(arr);
   }

   free(tasks);
   free(results);
   cJSON_Delete(tasks_json);
   ctx_db_close(ctx);
}

static void ag_stats(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   sqlite3 *adb = ctx_db_open(ctx);
   if (!adb)
      fatal("cannot open database");
   const char *name = (argc >= 1) ? argv[0] : NULL;
   agent_stats_t stats[MAX_AGENTS];
   int n = agent_get_stats(adb, name, stats, MAX_AGENTS);

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < n; i++)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", stats[i].name);
         cJSON_AddNumberToObject(obj, "total_calls", stats[i].total_calls);
         cJSON_AddNumberToObject(obj, "prompt_tokens", stats[i].total_prompt_tokens);
         cJSON_AddNumberToObject(obj, "completion_tokens", stats[i].total_completion_tokens);
         cJSON_AddNumberToObject(obj, "avg_latency_ms", stats[i].avg_latency_ms);
         cJSON_AddNumberToObject(obj, "success_rate", stats[i].success_rate);
         cJSON_AddItemToArray(arr, obj);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (n == 0)
      {
         printf("No agent calls recorded.\n");
      }
      else
      {
         for (int i = 0; i < n; i++)
         {
            printf("%-16s calls=%d tokens=%d/%d avg=%dms "
                   "success=%.0f%%\n",
                   stats[i].name, stats[i].total_calls, stats[i].total_prompt_tokens,
                   stats[i].total_completion_tokens, stats[i].avg_latency_ms,
                   stats[i].success_rate * 100);
         }
      }
   }
   ctx_db_close(ctx);
}

static void ag_add(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   agent_config_t *cfg = &s_agent_cfg;

   if (argc < 3)
      fatal("usage: aimee agent add <name> <endpoint> <model> "
            "[--key KEY] [--auth-cmd CMD] [--auth-type TYPE] "
            "[--provider openai|chatgpt] [--roles r1,r2,...] [--cost-tier N] "
            "[--tools-enabled] [--max-turns N] [--exec-roles r1,r2,...]");

   if (cfg->agent_count >= MAX_AGENTS)
      fatal("maximum number of agents reached");

   agent_t *ag = &cfg->agents[cfg->agent_count];
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, MAX_AGENT_NAME, "%s", argv[0]);
   snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", argv[1]);
   snprintf(ag->model, MAX_MODEL_LEN, "%s", argv[2]);
   snprintf(ag->auth_type, sizeof(ag->auth_type), "bearer");
   snprintf(ag->provider, sizeof(ag->provider), "openai");
   ag->max_tokens = AGENT_DEFAULT_MAX_TOKENS;
   ag->timeout_ms = AGENT_DEFAULT_TIMEOUT_MS;
   ag->max_turns = AGENT_DEFAULT_MAX_TURNS;
   ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;
   ag->enabled = 1;

   for (int i = 3; i < argc; i++)
   {
      if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
         agent_expand_env(argv[++i], ag->api_key, MAX_API_KEY_LEN);
      else if (strcmp(argv[i], "--auth-cmd") == 0 && i + 1 < argc)
         snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", argv[++i]);
      else if (strcmp(argv[i], "--auth-type") == 0 && i + 1 < argc)
         snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", argv[++i]);
      else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
         snprintf(ag->provider, sizeof(ag->provider), "%s", argv[++i]);
      else if (strcmp(argv[i], "--roles") == 0 && i + 1 < argc)
      {
         char *roles_str = argv[++i];
         char *tok = strtok(roles_str, ",");
         while (tok && ag->role_count < MAX_AGENT_ROLES)
         {
            snprintf(ag->roles[ag->role_count++], 32, "%s", tok);
            tok = strtok(NULL, ",");
         }
      }
      else if (strcmp(argv[i], "--cost-tier") == 0 && i + 1 < argc)
         ag->cost_tier = atoi(argv[++i]);
      else if (strcmp(argv[i], "--tools-enabled") == 0)
         ag->tools_enabled = 1;
      else if (strcmp(argv[i], "--max-turns") == 0 && i + 1 < argc)
         ag->max_turns = atoi(argv[++i]);
      else if (strcmp(argv[i], "--exec-roles") == 0 && i + 1 < argc)
      {
         char *er_str = argv[++i];
         char *tok = strtok(er_str, ",");
         while (tok && ag->exec_role_count < MAX_EXEC_ROLES)
         {
            snprintf(ag->exec_roles[ag->exec_role_count++], 32, "%s", tok);
            tok = strtok(NULL, ",");
         }
      }
   }

   /* Default roles if none specified */
   if (ag->role_count == 0)
   {
      snprintf(ag->roles[0], sizeof(ag->roles[0]), "summarize");
      snprintf(ag->roles[1], sizeof(ag->roles[1]), "format");
      snprintf(ag->roles[2], sizeof(ag->roles[2]), "draft");
      ag->role_count = 3;
   }

   cfg->agent_count++;

   /* Set as default if first agent */
   if (cfg->agent_count == 1)
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", ag->name);

   agent_save_config(cfg);
   printf("Agent '%s' added.\n", ag->name);
}

static void ag_remove(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   agent_config_t *cfg = &s_agent_cfg;

   if (argc < 1)
      fatal("usage: aimee agent remove <name>");
   int found = -1;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, argv[0]) == 0)
      {
         found = i;
         break;
      }
   }
   if (found < 0)
      fatal("agent '%s' not found", argv[0]);
   memmove(&cfg->agents[found], &cfg->agents[found + 1],
           (size_t)(cfg->agent_count - found - 1) * sizeof(agent_t));
   cfg->agent_count--;
   agent_save_config(cfg);
   printf("Agent '%s' removed.\n", argv[0]);
}

static void ag_enable(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   if (argc < 1)
      fatal("usage: aimee agent enable <name>");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);
   ag->enabled = 1;
   agent_save_config(&s_agent_cfg);
   printf("Agent '%s' enabled.\n", argv[0]);
}

static void ag_disable(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   (void)db;
   if (argc < 1)
      fatal("usage: aimee agent disable <name>");
   agent_t *ag = agent_find(&s_agent_cfg, argv[0]);
   if (!ag)
      fatal("agent '%s' not found", argv[0]);
   ag->enabled = 0;
   agent_save_config(&s_agent_cfg);
   printf("Agent '%s' disabled.\n", argv[0]);
}

/* --- key file helper --- */

/* Forward declarations for functions in cmd_agent_setup.c */
void ag_setup(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv);
void ag_token(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv);
void ag_tunnel(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv);

/* --- agent subcommand table --- */

static const subcmd_t agent_subcmds[] = {
    {"list", "List configured agents", ag_list},
    {"network", "Show network/host configuration", ag_network},
    {"tunnel", "Show tunnel configuration", ag_tunnel},
    {"test", "Test connectivity to an agent", ag_test},
    {"run", "Run a prompt on a specific agent", ag_run},
    {"parallel", "Run a prompt across multiple agents", ag_parallel},
    {"stats", "Show agent usage statistics", ag_stats},
    {"add", "Add a new agent", ag_add},
    {"remove", "Remove an agent", ag_remove},
    {"enable", "Enable a disabled agent", ag_enable},
    {"disable", "Disable an agent", ag_disable},
    {"setup", "Interactive agent setup wizard", ag_setup},
    {"token", "Refresh or show agent auth token", ag_token},
    {NULL, NULL, NULL},
};

const subcmd_t *get_agent_subcmds(void)
{
   return agent_subcmds;
}

/* --- cmd_agent --- */

void cmd_agent(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("agent", agent_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (agent_load_config(&s_agent_cfg) != 0)
      memset(&s_agent_cfg, 0, sizeof(s_agent_cfg));

   if (subcmd_dispatch(agent_subcmds, sub, ctx, NULL, argc, argv) != 0)
      fatal("unknown agent subcommand: %s", sub);
}
