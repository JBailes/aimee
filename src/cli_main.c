/* cli_main.c: pure client entry point -- all commands go through aimee-server */
#include "aimee.h"
#include "cli_client.h"
#include "cli_mcp_serve.h"
#include "commands.h"
#include "cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(void)
{
   fprintf(stderr,
           "Usage: aimee [--json] [--fields=FIELDS] [--profile=PROFILE] <command> [args...]\n"
           "\n"
           "Common shortcuts:\n"
           "  use <provider>      Set the default provider\n"
           "  provider [name]     Show or set the default provider\n"
           "  verify on|off       Enable or disable cross-verification\n"
           "\n"
           "Commands:\n"
           "  init             Initialize aimee database and config\n"
           "  setup            Provision workspace from manifest\n"
           "  wm               Working memory (session-scoped scratch)\n"
           "  workspace        Workspace management (add, remove)\n"
           "  index            Code indexing (overview, map, find, ...)\n"
           "  memory           Tiered memory management\n"
           "  rules            Rule management (list, generate, delete)\n"
           "  feedback         Record feedback (alias: +, -)\n"
           "  mode             Get/set session mode\n"
           "  agent            Sub-agent management and execution\n"
           "  delegate         Delegate a task to a sub-agent\n"
           "  verify           Cross-verify changes\n"
           "  webchat          Web chat + dashboard (HTTPS)\n"
           "  dashboard        Serve the dashboard UI\n"
           "  describe         Auto-describe projects\n"
           "  config           View and update configuration\n"
           "  version          Print version\n"
           "  help             Show help for a command\n"
           "\n"
           "Server is started automatically if not running.\n");
   exit(1);
}

static int print_shortcut_help(const char *target)
{
   if (strcmp(target, "use") == 0 || strcmp(target, "provider") == 0)
   {
      fprintf(stderr, "aimee use <provider>\n"
                      "aimee provider [name]\n"
                      "\n"
                      "Shortcuts for reading or updating config.provider.\n"
                      "Examples:\n"
                      "  aimee use claude\n"
                      "  aimee provider codex\n"
                      "  aimee provider\n");
      return 1;
   }
   if (strcmp(target, "verify") == 0)
   {
      fprintf(stderr, "aimee verify [on|off|enable|disable|config]\n"
                      "\n"
                      "Human shortcut aliases:\n"
                      "  aimee verify on   -> aimee verify enable\n"
                      "  aimee verify off  -> aimee verify disable\n");
      return 1;
   }
   return 0;
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

/* Read all of stdin into a buffer. Returns malloc'd string, caller frees. */
static char *read_stdin(void)
{
   size_t cap = 65536;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t len = 0;
   while (len < cap - 1)
   {
      ssize_t n = read(STDIN_FILENO, buf + len, cap - 1 - len);
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   buf[len] = '\0';
   return buf;
}

/* Streaming callback: print output events as they arrive */
static int forward_stream_cb(cJSON *event, void *userdata)
{
   (void)userdata;
   cJSON *jevt = cJSON_GetObjectItem(event, "event");
   if (!jevt || !cJSON_IsString(jevt))
      return 0;

   if (strcmp(jevt->valuestring, "output") == 0)
   {
      cJSON *jdata = cJSON_GetObjectItem(event, "data");
      if (jdata && cJSON_IsString(jdata))
         fputs(jdata->valuestring, stderr);
   }
   return 0;
}

/* Send a cli.forward request and print the output */
static int forward_command(const char *cmd, int argc, char **argv, int json_output,
                           const char *stdin_data)
{
   const char *sock = cli_ensure_server();
   if (!sock)
   {
      fprintf(stderr, "aimee: cannot start or connect to aimee-server\n");
      return 1;
   }

   cli_conn_t conn;
   if (cli_connect(&conn, sock) != 0)
   {
      fprintf(stderr, "aimee: cannot connect to aimee-server\n");
      return 1;
   }

   if (cli_authenticate(&conn) != 0)
   {
      fprintf(stderr, "aimee: authentication failed\n");
      cli_close(&conn);
      return 1;
   }

   /* Build request */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "cli.forward");
   cJSON_AddStringToObject(req, "command", cmd);

   cJSON *args = cJSON_CreateArray();
   for (int i = 0; i < argc; i++)
      cJSON_AddItemToArray(args, cJSON_CreateString(argv[i]));
   cJSON_AddItemToObject(req, "args", args);

   if (json_output)
      cJSON_AddTrueToObject(req, "json");

   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);

   if (stdin_data)
      cJSON_AddStringToObject(req, "stdin", stdin_data);

   /* Use streaming request so output from interactive commands (like OAuth
    * device flows) is displayed in real-time instead of being buffered. */
   cJSON *resp =
       cli_request_stream(&conn, req, 900000, forward_stream_cb, NULL); /* 15 min for OAuth flows */
   cJSON_Delete(req);
   cli_close(&conn);

   if (!resp)
   {
      fprintf(stderr, "aimee: no response from server\n");
      return 1;
   }

   /* Any remaining output in the final response (for backward compat) */
   cJSON *joutput = cJSON_GetObjectItemCaseSensitive(resp, "output");
   cJSON *jexit = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");

   if (cJSON_IsString(joutput) && joutput->valuestring[0])
   {
      fputs(joutput->valuestring, stdout);
      size_t len = strlen(joutput->valuestring);
      if (len > 0 && joutput->valuestring[len - 1] != '\n')
         putchar('\n');
   }

   int exit_code = cJSON_IsNumber(jexit) ? (int)jexit->valuedouble : 0;
   cJSON_Delete(resp);
   return exit_code;
}

/* Handle hooks specially -- use dedicated server methods for lower latency */
static int handle_hooks(int argc, char **argv, int json_output)
{
   if (argc < 1)
   {
      fprintf(stderr, "hooks requires 'pre' or 'post'\n");
      return 1;
   }

   const char *phase = argv[0];
   char *stdin_data = read_stdin();

   /* Parse tool_name and tool_input from stdin */
   cJSON *json = stdin_data ? cJSON_Parse(stdin_data) : NULL;
   const char *tool_name = "";
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
         char *serialized = cJSON_PrintUnformatted(ti);
         if (serialized)
            tool_input = serialized; /* freed when process exits */
      }
   }

   const char *sock = cli_ensure_server();
   if (!sock)
   {
      free(stdin_data);
      cJSON_Delete(json);
      return 1;
   }

   cli_conn_t conn;
   if (cli_connect(&conn, sock) != 0 || cli_authenticate(&conn) != 0)
   {
      cli_close(&conn);
      free(stdin_data);
      cJSON_Delete(json);
      return 1;
   }

   char cwd[4096];
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

   cJSON *resp = cli_request(&conn, req, 5000);
   cJSON_Delete(req);
   cli_close(&conn);
   free(stdin_data);
   cJSON_Delete(json);

   int exit_code = 1;
   if (resp)
   {
      cJSON *ec = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");
      if (cJSON_IsNumber(ec))
         exit_code = (int)ec->valuedouble;

      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);

      if (json_output)
      {
         cJSON *out = cJSON_CreateObject();
         cJSON_AddNumberToObject(out, "exit_code", exit_code);
         if (cJSON_IsString(msg) && msg->valuestring[0])
            cJSON_AddStringToObject(out, "message", msg->valuestring);
         char *s = cJSON_PrintUnformatted(out);
         if (s)
         {
            puts(s);
            free(s);
         }
         cJSON_Delete(out);
      }

      cJSON_Delete(resp);
   }

   return exit_code;
}

/* Launch a session: forward "launch" to server, print session context,
 * chdir to worktree, and exec the provider CLI. */
static int launch_session(int json_output, int debug)
{
   const char *sock = cli_ensure_server();
   if (!sock)
   {
      fprintf(stderr, "aimee: cannot start or connect to aimee-server\n");
      return 1;
   }

   cli_conn_t conn;
   if (cli_connect(&conn, sock) != 0)
   {
      fprintf(stderr, "aimee: cannot connect to aimee-server\n");
      return 1;
   }

   if (cli_authenticate(&conn) != 0)
   {
      fprintf(stderr, "aimee: authentication failed\n");
      cli_close(&conn);
      return 1;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "cli.forward");
   cJSON_AddStringToObject(req, "command", "launch");
   cJSON *args = cJSON_CreateArray();
   cJSON_AddItemToObject(req, "args", args);

   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);

   cJSON *resp = cli_request(&conn, req, 300000);
   cJSON_Delete(req);
   cli_close(&conn);

   if (!resp)
   {
      fprintf(stderr, "aimee: no response from server\n");
      return 1;
   }

   cJSON *joutput = cJSON_GetObjectItemCaseSensitive(resp, "output");
   cJSON *jexit = cJSON_GetObjectItemCaseSensitive(resp, "exit_code");
   int exit_code = cJSON_IsNumber(jexit) ? (int)jexit->valuedouble : 1;

   if (exit_code != 0)
   {
      if (cJSON_IsString(joutput) && joutput->valuestring[0])
         fputs(joutput->valuestring, stderr);
      cJSON_Delete(resp);
      return exit_code;
   }

   const char *output = cJSON_IsString(joutput) ? joutput->valuestring : "";

   launch_meta_t meta;
   if (parse_launch_meta(output, &meta))
   {
      /* Print session context only in debug mode */
      if (debug && meta.context_len > 0)
         fwrite(output, 1, meta.context_len, stdout);

      /* chdir to worktree if available */
      if (meta.worktree_cwd[0])
      {
         if (chdir(meta.worktree_cwd) == 0)
         {
            if (debug)
               fprintf(stderr, "aimee: session cwd: %s\n", meta.worktree_cwd);
         }
         else
            fprintf(stderr, "aimee: warning: could not chdir to worktree: %s\n", meta.worktree_cwd);
      }

      cJSON_Delete(resp);

      if (meta.builtin)
      {
         /* Built-in chat: forward to server */
         return forward_command("chat", 0, NULL, json_output, NULL);
      }

      /* Ensure AIMEE_SOCK is set so hooks route to the correct server */
      if (sock)
         setenv("AIMEE_SOCK", sock, 1);

      /* Exec provider CLI, with skip-permissions flag in autonomous mode */
      if (meta.autonomous)
      {
         const char *flag = NULL;
         if (strcmp(meta.provider, "claude") == 0)
            flag = "--dangerously-skip-permissions";
         else if (strcmp(meta.provider, "codex") == 0)
            flag = "--full-auto";

         if (flag)
         {
            fprintf(stderr, "aimee: autonomous mode — launching %s %s\n", meta.provider, flag);
            execlp(meta.provider, meta.provider, flag, NULL);
         }
         else
         {
            fprintf(stderr, "aimee: autonomous mode — no skip-permissions flag for '%s'\n",
                    meta.provider);
            execlp(meta.provider, meta.provider, NULL);
         }
      }
      else
      {
         execlp(meta.provider, meta.provider, NULL);
      }
      fprintf(stderr, "aimee: could not launch '%s': %s\n", meta.provider, strerror(errno));
      return 1;
   }

   /* Fallback: no launch marker found, just print output */
   if (output[0])
      fputs(output, stdout);

   cJSON_Delete(resp);
   return exit_code;
}

/* --- aimee clean [--force] --- */

/* Remove aimee hooks and MCP entries from a JSON settings file. */
static int clean_settings_file(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz <= 0 || sz >= (1L << 20))
   {
      fclose(fp);
      return 0;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return -1;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return 0;

   int changed = 0;

   /* Remove aimee entries from all hook events */
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   if (hooks && cJSON_IsObject(hooks))
   {
      cJSON *event = NULL;
      cJSON_ArrayForEach(event, hooks)
      {
         if (!cJSON_IsArray(event))
            continue;
         for (int i = cJSON_GetArraySize(event) - 1; i >= 0; i--)
         {
            cJSON *entry = cJSON_GetArrayItem(event, i);
            cJSON *hlist = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
            if (!hlist || !cJSON_IsArray(hlist))
               continue;
            cJSON *h = NULL;
            cJSON_ArrayForEach(h, hlist)
            {
               cJSON *c = cJSON_GetObjectItemCaseSensitive(h, "command");
               if (c && cJSON_IsString(c) && strstr(c->valuestring, "aimee"))
               {
                  cJSON_DeleteItemFromArray(event, i);
                  changed = 1;
                  break;
               }
            }
         }
      }
   }

   /* Remove aimee MCP server */
   cJSON *mcp = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (mcp && cJSON_HasObjectItem(mcp, "aimee"))
   {
      cJSON_DeleteItemFromObjectCaseSensitive(mcp, "aimee");
      changed = 1;
   }

   if (changed)
   {
      char *out = cJSON_Print(root);
      cJSON_Delete(root);
      if (!out)
         return -1;
      fp = fopen(path, "w");
      if (!fp)
      {
         free(out);
         return -1;
      }
      fputs(out, fp);
      fputc('\n', fp);
      fclose(fp);
      free(out);
      fprintf(stderr, "  cleaned %s\n", path);
   }
   else
   {
      cJSON_Delete(root);
   }
   return 0;
}

static int cli_clean(int argc, char **argv)
{
   int force = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0)
         force = 1;
   }

   const char *home = getenv("HOME");
   if (!home)
      home = "/tmp";

   char config_dir[MAX_PATH_LEN];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", home);

   if (!force)
   {
      fprintf(stderr,
              "This will remove all aimee data:\n"
              "  %s/          (config, database, keys, logs)\n\n"
              "And clean aimee hooks/MCP entries from:\n"
              "  ~/.claude/settings.json\n"
              "  ~/.gemini/settings.json\n"
              "  ~/.codex/hooks.json, ~/.codex/mcp-config.json\n"
              "  ~/.copilot/config.json, ~/.copilot/mcp-config.json\n\n"
              "Run with --force to proceed.\n",
              config_dir);
      return 0;
   }

   fprintf(stderr, "Cleaning aimee installation...\n");

   /* Remove aimee config directory */
   char cmd[MAX_PATH_LEN + 32];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", config_dir);
   (void)system(cmd);
   fprintf(stderr, "  removed %s\n", config_dir);

   /* Clean hooks and MCP entries from tool settings files */
   static const char *settings_files[] = {
       "%s/.claude/settings.json",  "%s/.gemini/settings.json", "%s/.codex/hooks.json",
       "%s/.codex/mcp-config.json", "%s/.copilot/config.json",  "%s/.copilot/mcp-config.json",
   };
   char path[MAX_PATH_LEN];
   for (size_t i = 0; i < sizeof(settings_files) / sizeof(settings_files[0]); i++)
   {
      snprintf(path, sizeof(path), settings_files[i], home);
      clean_settings_file(path);
   }

   fprintf(stderr, "Done. Run 'aimee init' to set up fresh.\n");
   return 0;
}

int main(int argc, char **argv)
{
   int json_output = 0;
   int debug = 0;
   int cmd_start = 1;

   /* See main.c: client integrations run only in cmd_init / cmd_setup. */

   /* Parse global flags */
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--json") == 0)
      {
         json_output = 1;
         cmd_start = i + 1;
      }
      else if (strcmp(argv[i], "--debug") == 0)
      {
         debug = 1;
         cmd_start = i + 1;
      }
      else if (strncmp(argv[i], "--fields=", 9) == 0 || strncmp(argv[i], "--profile=", 10) == 0)
      {
         cmd_start = i + 1;
      }
      else
         break;
   }

   if (cmd_start >= argc)
   {
      /* No subcommand: forward "launch" to server, parse metadata, exec provider */
      return launch_session(json_output, debug);
   }

   const char *cmd = argv[cmd_start];
   int sub_argc = argc - cmd_start - 1;
   char **sub_argv = argv + cmd_start + 1;
   rewrite_human_shortcuts(&cmd, &sub_argc, &sub_argv);

   /* Local commands (no server needed) */
   if (strcmp(cmd, "version") == 0)
   {
      printf("aimee %s\n", AIMEE_VERSION);
      return 0;
   }
   if (strcmp(cmd, "--help") == 0)
   {
      if (sub_argc > 0 && print_shortcut_help(sub_argv[0]))
         return 0;
      usage();
      return 0;
   }
   if (strcmp(cmd, "help") == 0)
   {
      if (sub_argc > 0 && print_shortcut_help(sub_argv[0]))
         return 0;
      return forward_command("help", sub_argc, sub_argv, json_output, NULL);
   }

   /* Clean: runs locally, no server needed */
   if (strcmp(cmd, "clean") == 0)
      return cli_clean(sub_argc, sub_argv);

   /* MCP stdio proxy (replaces standalone aimee-mcp binary) */
   if (strcmp(cmd, "mcp-serve") == 0)
      return cli_mcp_serve();

   /* Hooks: use dedicated server methods */
   if (strcmp(cmd, "hooks") == 0)
      return handle_hooks(sub_argc, sub_argv, json_output);

   /* Route through native server RPCs when possible (avoids cli.forward fork) */
   {
      const char *subcmd = sub_argc > 0 ? sub_argv[0] : NULL;
      int skip_rpc = 0;

      /* Exclude delegate status and --background delegates */
      if (strcmp(cmd, "delegate") == 0)
      {
         if (subcmd && strcmp(subcmd, "status") == 0)
            skip_rpc = 1;
         for (int i = 0; !skip_rpc && i < sub_argc; i++)
            if (strcmp(sub_argv[i], "--background") == 0)
               skip_rpc = 1;
      }

      if (!skip_rpc)
      {
         cli_rpc_route_t route;
         if (cli_rpc_lookup(cmd, subcmd, &route))
         {
            const char *sock = cli_ensure_server();
            if (sock)
            {
               int rc = cli_rpc_forward(sock, &route, json_output, NULL, NULL, sub_argc, sub_argv);
               if (rc >= 0)
                  return rc;
            }
         }
      }
   }

   /* Commands that need stdin forwarded */
   char *stdin_data = NULL;
   if (strcmp(cmd, "feedback") == 0 || strcmp(cmd, "+") == 0 || strcmp(cmd, "-") == 0)
   {
      stdin_data = read_stdin();
   }

   int rc = forward_command(cmd, sub_argc, sub_argv, json_output, stdin_data);
   free(stdin_data);
   return rc;
}
