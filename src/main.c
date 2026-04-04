/* main.c: entry point, command table, global flag parsing, usage */
#include "aimee.h"
#include "cli_client.h"
#include "workspace.h"
#include "cJSON.h"
#include "commands.h"
#include "agent_exec.h"
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* Command table defined in cmd_table.c */

/* --- Usage (auto-generated from command table) --- */

static void usage(void)
{
   fprintf(stderr, "Usage: aimee [--json] [--fields=FIELDS] [--profile=PROFILE] "
                   "<command> [args...]\n"
                   "\n"
                   "Common shortcuts:\n"
                   "  use <provider>      Set the default provider\n"
                   "  provider [name]     Show or set the default provider\n"
                   "  verify on|off       Enable or disable cross-verification\n");

   static const struct { cmd_tier_t tier; const char *label; } tiers[] = {
      {CMD_TIER_CORE,     "Core commands"},
      {CMD_TIER_ADVANCED, "Advanced commands"},
      {CMD_TIER_ADMIN,    "Admin commands"},
   };

   for (int t = 0; t < 3; t++)
   {
      fprintf(stderr, "\n%s:\n", tiers[t].label);
      for (int i = 0; commands[i].name != NULL; i++)
      {
         if (commands[i].tier != tiers[t].tier)
            continue;
         if (command_is_hidden_default(commands[i].name))
            continue;
         fprintf(stderr, "  %-16s %s\n", commands[i].name, commands[i].help);
      }
   }
   exit(1);
}

static void rewrite_human_shortcuts(const char **cmd, int *sub_argc, char ***sub_argv)
{
   static char *rewritten[4];

   if (strcmp(*cmd, "use") == 0)
   {
      rewritten[0] = (char *)((*sub_argc >= 1) ? "set" : "get");
      rewritten[1] = (char *)"provider";
      if (*sub_argc >= 1)
      {
         rewritten[2] = (*sub_argv)[0];
         *sub_argc = 3;
      }
      else
      {
         *sub_argc = 2;
      }
      *sub_argv = rewritten;
      *cmd = "config";
      return;
   }

   if (strcmp(*cmd, "provider") == 0)
   {
      rewritten[0] = (char *)((*sub_argc >= 1) ? "set" : "get");
      rewritten[1] = (char *)"provider";
      if (*sub_argc >= 1)
      {
         rewritten[2] = (*sub_argv)[0];
         *sub_argc = 3;
      }
      else
      {
         *sub_argc = 2;
      }
      *sub_argv = rewritten;
      *cmd = "config";
      return;
   }

   if (strcmp(*cmd, "verify") == 0 && *sub_argc >= 1)
   {
      if (strcmp((*sub_argv)[0], "on") == 0)
         (*sub_argv)[0] = (char *)"enable";
      else if (strcmp((*sub_argv)[0], "off") == 0)
         (*sub_argv)[0] = (char *)"disable";
   }
}

/* --- hooks via server --- */

static int cli_hooks_via_server(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("hooks requires 'pre' or 'post'");

   const char *phase = argv[0];

   /* Read JSON from stdin -- hook input is small (tool name + args) */
   char input[65536];
   size_t total = 0;
   while (total < sizeof(input) - 1)
   {
      ssize_t n = read(STDIN_FILENO, input + total, sizeof(input) - 1 - total);
      if (n <= 0)
         break;
      total += (size_t)n;
   }
   input[total] = '\0';

   /* Parse tool_name and tool_input */
   cJSON *json = cJSON_Parse(input);
   const char *tool_name = "";
   char *ti_heap = NULL;
   const char *tool_input = "{}";

   if (json)
   {
      cJSON *tn = cJSON_GetObjectItemCaseSensitive(json, "tool_name");
      if (cJSON_IsString(tn))
         tool_name = tn->valuestring;

      cJSON *ti = cJSON_GetObjectItemCaseSensitive(json, "tool_input");
      if (cJSON_IsString(ti))
         tool_input = ti->valuestring;
      else if (cJSON_IsObject(ti) || cJSON_IsArray(ti))
      {
         ti_heap = cJSON_PrintUnformatted(ti);
         tool_input = ti_heap;
      }
   }

   /* Build server request */
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      cwd[0] = '\0';

   const char *sid = getenv("CLAUDE_SESSION_ID");

   char method[32];
   snprintf(method, sizeof(method), "hooks.%s", phase);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddStringToObject(req, "tool_name", tool_name);
   cJSON_AddStringToObject(req, "tool_input", tool_input);
   cJSON_AddStringToObject(req, "cwd", cwd);
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);

   /* Send to server */
   cli_conn_t conn;
   if (cli_connect(&conn, NULL) != 0 || cli_authenticate(&conn) != 0)
   {
      /* Server went away or auth failed -- fall back */
      cli_close(&conn);
      cJSON_Delete(req);
      cJSON_Delete(json);
      return 1;
   }

   cJSON *resp = cli_request(&conn, req, CLIENT_DEFAULT_TIMEOUT_MS);
   cli_close(&conn);
   cJSON_Delete(req);

   int exit_code = 1;
   if (resp)
   {
      cJSON *ec = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");
      if (cJSON_IsNumber(ec))
         exit_code = (int)ec->valuedouble;

      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);

      /* JSON output mode */
      if (ctx->json_output)
      {
         cJSON *out = cJSON_CreateObject();
         cJSON_AddNumberToObject(out, "exit_code", exit_code);
         if (cJSON_IsString(msg) && msg->valuestring[0])
            cJSON_AddStringToObject(out, "message", msg->valuestring);
         emit_json_ctx(out, ctx->json_fields, ctx->response_profile);
      }

      cJSON_Delete(resp);
   }

   cJSON_Delete(json);
   if (ti_heap)
      free(ti_heap);
   return exit_code;
}

/* --- main --- */

int main(int argc, char **argv)
{
   /* Build application context */
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));

   /* Client integrations (MCP registration, plugin files) are ensured only
    * during cmd_init / cmd_setup, not on every invocation.  Running it here
    * caused a race: the non-atomic rewrite of ~/.claude/settings.json during
    * hook calls (session-start, hooks pre) could truncate the file while
    * Claude Code was reading it to discover MCP servers. */

   /* Parse global flags */
   int cmd_start = 1;
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
      {
         ctx.json_output = 1;
         cmd_start = i + 1;
      }
      else if (strncmp(argv[i], "--fields=", 9) == 0)
      {
         ctx.json_fields = argv[i] + 9;
         cmd_start = i + 1;
      }
      else if (strncmp(argv[i], "--profile=", 10) == 0)
      {
         ctx.response_profile = argv[i] + 10;
         cmd_start = i + 1;
      }
      else if (strcmp(argv[i], "--strict") == 0)
      {
         g_config_strict = 1;
         cmd_start = i + 1;
      }
      else
      {
         break;
      }
   }

   /* Check AIMEE_STRICT env var */
   if (!g_config_strict && getenv("AIMEE_STRICT") && strcmp(getenv("AIMEE_STRICT"), "1") == 0)
      g_config_strict = 1;

   if (cmd_start >= argc)
   {
      /* No subcommand: initialize session and launch provider CLI */

      /* 1. Ensure database and config exist */
      config_t cfg;
      if (config_load(&cfg) < 0)
         return 1;
      ctx.cfg = &cfg;

      /* Only persist config when the file doesn't exist yet (first run) */
      {
         struct stat cfg_st;
         if (stat(config_default_path(), &cfg_st) != 0)
            config_save(&cfg);
      }

      /* 2. Run session-start: prune stale sessions, set up state and worktrees */
      cmd_session_start(&ctx, 0, NULL);

      /* 3. Parallel worktree creation: spawn threads for all pending worktrees,
       *    then chdir to the appropriate one after they complete. */
      {
         char state_path[MAX_PATH_LEN];
         session_state_path(state_path, sizeof(state_path));
         session_state_t state;
         session_state_load(&state, state_path);
         worktree_gate_init(&state);

         /* Spawn one pthread per worktree that needs creation */
         pthread_t wt_threads[MAX_WORKTREES];
         worktree_thread_arg_t wt_args[MAX_WORKTREES];
         int thread_count = 0;

         for (int i = 0; i < state.worktree_count; i++)
         {
            if (state.worktrees[i].created == 0)
            {
               wt_args[thread_count].state = &state;
               wt_args[thread_count].ws_index = i;
               wt_args[thread_count].result = -1;
               if (pthread_create(&wt_threads[thread_count], NULL, worktree_thread_fn,
                                  &wt_args[thread_count]) == 0)
               {
                  thread_count++;
               }
               else
               {
                  /* Fallback: create synchronously if thread spawn fails */
                  worktree_ensure(&state.worktrees[i]);
               }
            }
         }

         /* Join all worktree threads — we need them ready before chdir */
         int all_ok = 1;
         for (int i = 0; i < thread_count; i++)
         {
            pthread_join(wt_threads[i], NULL);
            if (wt_args[i].result != 0)
               all_ok = 0;
         }

         /* Signal worktree readiness for any downstream gates */
         worktree_gate_signal(&state, all_ok ? 1 : -1);

         /* chdir to the worktree equivalent of cwd */
         if (state.worktree_count > 0)
         {
            char cwd[MAX_PATH_LEN];
            if (getcwd(cwd, sizeof(cwd)))
            {
               for (int i = 0; i < cfg.workspace_count; i++)
               {
                  size_t wlen = strlen(cfg.workspaces[i]);
                  if (strncmp(cwd, cfg.workspaces[i], wlen) == 0 &&
                      (cwd[wlen] == '/' || cwd[wlen] == '\0'))
                  {
                     const char *suffix = cwd + wlen;
                     const char *slash = strrchr(cfg.workspaces[i], '/');
                     const char *ws_name = slash ? slash + 1 : cfg.workspaces[i];

                     /* Find the matching worktree entry (already created by thread) */
                     const char *wt_path = NULL;
                     for (int j = 0; j < state.worktree_count; j++)
                     {
                        if (strcmp(state.worktrees[j].name, ws_name) == 0 &&
                            state.worktrees[j].created == 1)
                        {
                           wt_path = state.worktrees[j].path;
                           worktree_db_touch(wt_path);
                           state.dirty = 1;
                           break;
                        }
                     }

                     if (wt_path)
                     {
                        char target[MAX_PATH_LEN];
                        snprintf(target, sizeof(target), "%s%s", wt_path, suffix);
                        if (chdir(target) == 0)
                           fprintf(stderr, "aimee: session cwd: %s\n", target);
                        else
                           fprintf(stderr, "aimee: warning: could not chdir to worktree: %s\n",
                                   target);
                        session_state_save(&state, state_path);
                     }
                     else
                     {
                        fprintf(stderr, "aimee: error: failed to create worktree for '%s'\n",
                                ws_name);
                     }
                     break;
                  }
               }
            }
         }

         pthread_mutex_destroy(&state.wt_mutex);
         pthread_cond_destroy(&state.wt_cond);
      }

      /* 4. Determine provider */
      const char *provider = "claude";
      if (cfg.provider[0])
         provider = cfg.provider;

      /* 5. Launch provider CLI (or built-in chat) */
      setenv("CLAUDE_SESSION_ID", session_id(), 1);
      if (strcmp(provider, "openai") == 0 || strcmp(provider, "copilot") == 0 ||
          cfg.use_builtin_cli)
      {
         cmd_chat(&ctx, 0, NULL);
         return 0;
      }
      execlp(provider, provider, NULL);
      fprintf(stderr, "aimee: could not launch '%s': %s\n", provider, strerror(errno));
      return 1;
   }

   const char *cmd = argv[cmd_start];
   int sub_argc = argc - cmd_start - 1;
   char **sub_argv = argv + cmd_start + 1;
   rewrite_human_shortcuts(&cmd, &sub_argc, &sub_argv);

   /* Handle --help and help as special cases */
   if (strcmp(cmd, "--help") == 0)
   {
      if (sub_argc > 0)
      {
         /* aimee --help <command> => show help for that command */
         cmd_help(&ctx, sub_argc, sub_argv);
      }
      else
      {
         usage();
      }
      return 0;
   }

   /* Route hooks through server when available */
   if (strcmp(cmd, "hooks") == 0 && cli_server_available(NULL))
   {
      return cli_hooks_via_server(&ctx, sub_argc, sub_argv);
   }

   /* Route CLI subcommands through server RPCs for sub-10ms overhead */
   {
      const char *subcmd = sub_argc > 0 ? sub_argv[0] : NULL;

      /* Exclude delegate status — handled in-process */
      int skip_rpc = (strcmp(cmd, "delegate") == 0 && subcmd && strcmp(subcmd, "status") == 0);

      /* Exclude background delegates — need in-process fork */
      if (!skip_rpc && strcmp(cmd, "delegate") == 0)
      {
         for (int i = 0; i < sub_argc; i++)
         {
            if (strcmp(sub_argv[i], "--background") == 0)
            {
               skip_rpc = 1;
               break;
            }
         }
      }

      if (!skip_rpc)
      {
         cli_rpc_route_t route;
         if (cli_rpc_lookup(cmd, subcmd, &route))
         {
            const char *sock = cli_ensure_server();
            if (sock)
            {
               int rc = cli_rpc_forward(sock, &route, ctx.json_output, ctx.json_fields,
                                        ctx.response_profile, sub_argc, sub_argv);
               if (rc >= 0)
                  return rc;
               /* rc < 0: transport/protocol error, fall through to in-process */
            }
         }
      }
   }

   /* Look up command in the table */
   for (int i = 0; commands[i].name != NULL; i++)
   {
      if (strcmp(cmd, commands[i].name) == 0)
      {
         commands[i].handler(&ctx, sub_argc, sub_argv);
         return 0;
      }
   }

   fatal("unknown command: %s", cmd);
   return 0;
}

/* cmd_help is now in cmd_table.c */
