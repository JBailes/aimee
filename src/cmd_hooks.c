/* cmd_hooks.c: session lifecycle CLI (hooks, session-start, wrapup, session context builder) */
#include "aimee.h"
#include "agent_config.h"
#include "agent_coord.h"
#include "agent_eval.h"
#include "trace_analysis.h"
#include "workspace.h"
#include "commands.h"
#include "cJSON.h"
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* --- cmd_hooks --- */

void cmd_hooks(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("hooks requires 'pre' or 'post'");

   const char *phase = argv[0];
   argc--;
   argv++;

   config_t cfg;
   config_load(&cfg);

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

   cJSON *json = cJSON_Parse(input);

   const char *tool_name = "";
   char *tool_input_heap = NULL;
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
         tool_input_heap = cJSON_PrintUnformatted(ti);
         tool_input = tool_input_heap;
      }
   }

   ctx->db = db_open_fast(cfg.db_path);
   if (!ctx->db)
      fatal("cannot open database");
   sqlite3 *db = ctx->db;

   if (strcmp(phase, "pre") == 0)
   {
      /* Load session state */
      char state_path[MAX_PATH_LEN];
      session_state_path(state_path, sizeof(state_path));
      session_state_t state;
      session_state_load(&state, state_path);

      char cwd[MAX_PATH_LEN];
      if (!getcwd(cwd, sizeof(cwd)))
         cwd[0] = '\0';

      /* Write CWD to session-scoped tracking file so MCP server follows session CWD */
      if (cwd[0])
      {
         char cwd_path[MAX_PATH_LEN];
         snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(), session_id());
         FILE *fp = fopen(cwd_path, "w");
         if (fp)
         {
            fputs(cwd, fp);
            fclose(fp);
         }
      }

      char msg[1024] = "";
      int rc = pre_tool_check(db, tool_name, tool_input, &state, config_guardrail_mode(&cfg), cwd,
                              msg, sizeof(msg));

      session_state_save(&state, state_path);

      /* Auto-delegation detection (#2):
       * If the tool is a shell tool and the command looks like a remote operation
       * that a sub-agent could handle, suggest delegation on stderr.
       * Check tool_input as raw string (contains the command JSON). */
      if (rc == 0 && is_shell_tool(tool_name) && tool_input && tool_input[0])
      {
         const char *role = NULL;
         if (strstr(tool_input, "ssh ") &&
             (strstr(tool_input, "deploy") || strstr(tool_input, "192.168") ||
              strstr(tool_input, "10.0.") || strstr(tool_input, "10.1.")))
            role = "deploy";
         else if (strstr(tool_input, "curl ") && strstr(tool_input, "/health"))
            role = "validate";
         else if (strstr(tool_input, "systemctl restart") || strstr(tool_input, "systemctl stop"))
            role = "deploy";

         if (role)
         {
            agent_config_t acfg;
            if (agent_load_config(&acfg) == 0)
            {
               int has_tools = 0;
               for (int i = 0; i < acfg.agent_count; i++)
               {
                  if (acfg.agents[i].tools_enabled && agent_has_role(&acfg.agents[i], role))
                     has_tools = 1;
               }
               if (has_tools)
               {
                  fprintf(stderr,
                          "aimee: this looks like a %s task that a sub-agent could handle. "
                          "Consider: aimee delegate %s \"<task>\"\n",
                          role, role);
               }
            }
         }
      }

      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddNumberToObject(j, "exit_code", rc);
         if (msg[0])
            cJSON_AddStringToObject(j, "message", msg);
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }

      ctx_db_close(ctx);
      cJSON_Delete(json);
      exit(rc);
   }
   else if (strcmp(phase, "post") == 0)
   {
      /* Update CWD tracking (catches worktree CWD changes via PostToolUse) */
      {
         char cwd[MAX_PATH_LEN];
         if (getcwd(cwd, sizeof(cwd)) && cwd[0])
         {
            char cwd_path[MAX_PATH_LEN];
            snprintf(cwd_path, sizeof(cwd_path), "%s/git-cwd-%s", config_output_dir(),
                     session_id());
            FILE *fp = fopen(cwd_path, "w");
            if (fp)
            {
               fputs(cwd, fp);
               fclose(fp);
            }
         }
      }

      post_tool_update(db, tool_name, tool_input);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("hooks requires 'pre' or 'post', got: %s", phase);
   }

   ctx_db_close(ctx);
   cJSON_Delete(json);
   if (tool_input_heap)
      free(tool_input_heap);
}

/* --- cmd_session_start --- */

/* Build the aimee capabilities reference text. Caller owns the returned string. */
char *build_capabilities_text(void)
{
   size_t cap = 4096;
   char *buf = malloc(cap);
   size_t pos = 0;

   pos += (size_t)snprintf(buf + pos, cap - pos,
                           "# aimee Commands\n"
                           "Use `aimee <command>` for all operations.\n"
                           "Common shortcuts: `aimee use <provider>`, `aimee provider [name]`, "
                           "`aimee verify on|off`.\n\n");

   static const struct
   {
      cmd_tier_t tier;
      const char *label;
   } tiers[] = {
       {CMD_TIER_CORE, "Core"},
       {CMD_TIER_ADVANCED, "Advanced"},
       {CMD_TIER_ADMIN, "Admin"},
   };

   for (int t = 0; t < 3 && pos < cap - 256; t++)
   {
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n## %s\n", tiers[t].label);
      for (int i = 0; commands[i].name != NULL && pos < cap - 256; i++)
      {
         if (commands[i].tier != tiers[t].tier)
            continue;
         if (command_is_hidden_default(commands[i].name) ||
             strcmp(commands[i].name, "version") == 0)
            continue;
         pos += (size_t)snprintf(buf + pos, cap - pos, "- `aimee %s` -- %s\n", commands[i].name,
                                 commands[i].help);
      }
   }
   pos += (size_t)snprintf(buf + pos, cap - pos, "\n");

   struct
   {
      const char *parent;
      const subcmd_t *(*getter)(void);
   } sub_tables[] = {
       {"memory", get_memory_subcmds},
       {"index", get_index_subcmds},
       {"wm", get_wm_subcmds},
       {"agent", get_agent_subcmds},
   };

   for (int t = 0; t < 4 && pos < cap - 256; t++)
   {
      const subcmd_t *subs = sub_tables[t].getter();
      pos += (size_t)snprintf(buf + pos, cap - pos, "## aimee %s\n", sub_tables[t].parent);
      for (int j = 0; subs[j].name && pos < cap - 128; j++)
      {
         if (subs[j].help)
            pos += (size_t)snprintf(buf + pos, cap - pos, "- `%s` -- %s\n", subs[j].name,
                                    subs[j].help);
         else
            pos += (size_t)snprintf(buf + pos, cap - pos, "- `%s`\n", subs[j].name);
      }
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
   }

   pos += (size_t)snprintf(
       buf + pos, cap - pos,
       "## Usage Notes\n"
       "- Use `aimee git status`, `aimee git log`, `aimee git diff` for compact git info.\n"
       "- Use `aimee git pr <list|view|create>` for GitHub PR/issue operations.\n"
       "- Use `aimee delegate <role> \"prompt\"` for deploy, validate, test, "
       "diagnose, execute tasks when sub-agents are configured.\n"
       "- Record feedback with `aimee + \"what went well\"` or `aimee - \"what went wrong\"`.\n"
       "- Add `--json` to any command for machine-readable output.\n\n");

   buf[pos] = '\0';
   return buf;
}

/* Build condensed session context from rules, CLAUDE.md, key facts, and
 * delegation hints. Caller owns the returned string (may be empty). */
static char *build_session_context(sqlite3 *db)
{
   size_t cap = 32768;
   char *buf = malloc(cap);
   size_t pos = 0;

   /* Tool preference rules go FIRST for primacy bias — the LLM is more
    * likely to follow instructions near the top of the context window. */
   pos += (size_t)snprintf(
       buf + pos, cap - pos,
       "# Tool Preferences (IMPORTANT)\n"
       "- ALWAYS run `aimee index overview` or `aimee index find <symbol>` to locate "
       "projects and code BEFORE using Grep, Glob, find, or the Agent tool.\n"
       "- ALWAYS check `aimee memory search <terms>` for prior context before "
       "researching topics from scratch.\n"
       "- Do NOT use the Agent tool with subagent_type=Explore or general-purpose "
       "for codebase discovery. Use aimee index instead.\n\n");

   pos += (size_t)snprintf(buf + pos, cap - pos, "# Rules\n\n");

   /* Aimee rules (from feedback/learning) */
   char *rules = rules_generate(db);
   if (rules && rules[0] && strcmp(rules, "# Rules\n\n") != 0 &&
       strcmp(rules, "No rules configured.") != 0)
   {
      const char *body = rules;
      if (strncmp(body, "# Rules\n", 8) == 0)
         body += 8;
      while (*body == '\n')
         body++;
      if (*body)
         pos += (size_t)snprintf(buf + pos, cap - pos, "%s\n", body);
   }
   free(rules);

   /* Key infrastructure facts (top 5 most-used) */
   {
      static const char *sql = "SELECT key, content FROM memories"
                               " WHERE tier = 'L2' AND kind = 'fact'"
                               " ORDER BY use_count DESC, confidence DESC LIMIT 15";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         int has_facts = 0;
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 600)
         {
            if (!has_facts)
            {
               pos += (size_t)snprintf(buf + pos, cap - pos, "# Key Facts\n");
               has_facts = 1;
            }
            const char *key = (const char *)sqlite3_column_text(stmt, 0);
            const char *val = (const char *)sqlite3_column_text(stmt, 1);
            pos += (size_t)snprintf(buf + pos, cap - pos, "- %s: %.300s\n", key ? key : "",
                                    val ? val : "");
         }
         if (has_facts)
            pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Network summary */
   {
      agent_config_t net_cfg;
      if (agent_load_config(&net_cfg) == 0 && net_cfg.network.ssh_entry[0])
      {
         agent_network_t *nw = &net_cfg.network;
         pos += (size_t)snprintf(buf + pos, cap - pos, "# Network\nEntry: %s\n", nw->ssh_entry);
         if (nw->host_count > 0)
         {
            pos += (size_t)snprintf(buf + pos, cap - pos, "Hosts (%d): ", nw->host_count);
            int show = nw->host_count < 5 ? nw->host_count : 5;
            for (int i = 0; i < show && pos < cap - 128; i++)
               pos += (size_t)snprintf(buf + pos, cap - pos, "%s%s (%s)", i > 0 ? ", " : "",
                                       nw->hosts[i].name, nw->hosts[i].ip);
            if (nw->host_count > show)
               pos += (size_t)snprintf(buf + pos, cap - pos, " +%d more", nw->host_count - show);
            pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
         }
         if (nw->network_count > 0)
         {
            for (int i = 0; i < nw->network_count && pos < cap - 128; i++)
               pos +=
                   (size_t)snprintf(buf + pos, cap - pos, "- %s: %s (%s)\n", nw->networks[i].name,
                                    nw->networks[i].cidr, nw->networks[i].desc);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos,
                                 "Run `aimee agent network` or `aimee --json agent network` "
                                 "for full host details.\n\n");
      }
   }

   /* Workspace-scoped project context */
   {
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)))
      {
         config_t app_cfg;
         config_load(&app_cfg);

         const char *project_name = NULL;
         for (int i = 0; i < app_cfg.workspace_count; i++)
         {
            size_t wlen = strlen(app_cfg.workspaces[i]);
            if (strncmp(cwd, app_cfg.workspaces[i], wlen) == 0 &&
                (cwd[wlen] == '/' || cwd[wlen] == '\0'))
            {
               const char *slash = strrchr(app_cfg.workspaces[i], '/');
               project_name = slash ? slash + 1 : app_cfg.workspaces[i];
               break;
            }
         }

         if (project_name && pos < cap - 512)
         {
            int has_section = 0;

            /* Project-relevant memories via workspace tag JOIN.
             * Prioritizes workflow rules, then by tier and usage.
             * Falls back to LIKE matching for untagged memories. */
            static const char *mem_sql =
                "SELECT m.key, m.content, m.kind FROM memories m"
                " JOIN memory_workspaces mw ON mw.memory_id = m.id"
                " WHERE mw.workspace = ?"
                " AND m.tier IN ('L1', 'L2', 'L3')"
                " ORDER BY"
                "   CASE m.kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,"
                "   CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,"
                "   m.use_count DESC"
                " LIMIT 10";
            sqlite3_stmt *mstmt = db_prepare(db, mem_sql);
            if (mstmt)
            {
               sqlite3_bind_text(mstmt, 1, project_name, -1, SQLITE_TRANSIENT);

               while (sqlite3_step(mstmt) == SQLITE_ROW && pos < cap - 512)
               {
                  if (!has_section)
                  {
                     pos += (size_t)snprintf(buf + pos, cap - pos, "# Project Context (%s)\n",
                                             project_name);
                     has_section = 1;
                  }
                  const char *val = (const char *)sqlite3_column_text(mstmt, 1);
                  const char *key = (const char *)sqlite3_column_text(mstmt, 0);
                  const char *kind = (const char *)sqlite3_column_text(mstmt, 2);
                  const char *text = (val && val[0]) ? val : (key ? key : "");
                  if (kind && kind[0])
                     pos += (size_t)snprintf(buf + pos, cap - pos, "- [%s] %.300s\n", kind, text);
                  else
                     pos += (size_t)snprintf(buf + pos, cap - pos, "- %.300s\n", text);
               }
               sqlite3_reset(mstmt);
            }

            /* Fallback: LIKE matching for memories not yet workspace-tagged */
            if (!has_section)
            {
               char like_pattern[256];
               snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", project_name);

               static const char *like_sql =
                   "SELECT m.key, m.content, m.kind FROM memories m"
                   " WHERE m.tier IN ('L1', 'L2', 'L3')"
                   " AND m.id NOT IN (SELECT memory_id FROM memory_workspaces)"
                   " AND (m.key LIKE ? OR m.content LIKE ?)"
                   " ORDER BY"
                   "   CASE m.kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,"
                   "   CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,"
                   "   m.use_count DESC LIMIT 5";
               sqlite3_stmt *lstmt = db_prepare(db, like_sql);
               if (lstmt)
               {
                  sqlite3_bind_text(lstmt, 1, like_pattern, -1, SQLITE_TRANSIENT);
                  sqlite3_bind_text(lstmt, 2, like_pattern, -1, SQLITE_TRANSIENT);

                  while (sqlite3_step(lstmt) == SQLITE_ROW && pos < cap - 512)
                  {
                     if (!has_section)
                     {
                        pos += (size_t)snprintf(buf + pos, cap - pos, "# Project Context (%s)\n",
                                                project_name);
                        has_section = 1;
                     }
                     const char *val = (const char *)sqlite3_column_text(lstmt, 1);
                     const char *key = (const char *)sqlite3_column_text(lstmt, 0);
                     const char *kind = (const char *)sqlite3_column_text(lstmt, 2);
                     const char *text = (val && val[0]) ? val : (key ? key : "");
                     if (kind && kind[0])
                        pos +=
                            (size_t)snprintf(buf + pos, cap - pos, "- [%s] %.300s\n", kind, text);
                     else
                        pos += (size_t)snprintf(buf + pos, cap - pos, "- %.300s\n", text);
                  }
                  sqlite3_reset(lstmt);
               }
            }

            /* Index stats for this project */
            static const char *stats_sql =
                "SELECT"
                " (SELECT COUNT(*) FROM files f"
                "  JOIN projects p ON p.id = f.project_id WHERE p.name = ?),"
                " (SELECT COUNT(*) FROM terms t"
                "  JOIN files f ON f.id = t.file_id"
                "  JOIN projects p ON p.id = f.project_id"
                "  WHERE p.name = ? AND t.kind = 'definition')";
            sqlite3_stmt *sstmt = db_prepare(db, stats_sql);
            if (sstmt)
            {
               sqlite3_bind_text(sstmt, 1, project_name, -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(sstmt, 2, project_name, -1, SQLITE_TRANSIENT);
               if (sqlite3_step(sstmt) == SQLITE_ROW)
               {
                  int file_count = sqlite3_column_int(sstmt, 0);
                  int def_count = sqlite3_column_int(sstmt, 1);
                  if (file_count > 0 || def_count > 0)
                  {
                     if (!has_section)
                     {
                        pos += (size_t)snprintf(buf + pos, cap - pos, "# Project Context (%s)\n",
                                                project_name);
                        has_section = 1;
                     }
                     pos += (size_t)snprintf(buf + pos, cap - pos,
                                             "- %d files indexed, %d definitions\n", file_count,
                                             def_count);
                  }
               }
               sqlite3_reset(sstmt);
            }

            if (has_section)
               pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
         }
      }
   }

   /* Shared context: cross-cutting memories tagged with _shared workspace */
   {
      static const char *shared_sql =
          "SELECT m.key, m.content, m.kind FROM memories m"
          " JOIN memory_workspaces mw ON mw.memory_id = m.id"
          " WHERE mw.workspace = '_shared'"
          " AND m.tier IN ('L1', 'L2', 'L3')"
          " ORDER BY"
          "   CASE m.kind WHEN 'workflow' THEN 0 WHEN 'decision' THEN 1 ELSE 2 END,"
          "   CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 ELSE 2 END,"
          "   m.use_count DESC"
          " LIMIT 5";
      sqlite3_stmt *shstmt = db_prepare(db, shared_sql);
      if (shstmt)
      {
         int has_shared = 0;
         while (sqlite3_step(shstmt) == SQLITE_ROW && pos < cap - 512)
         {
            if (!has_shared)
            {
               pos += (size_t)snprintf(buf + pos, cap - pos, "# Shared Context\n");
               has_shared = 1;
            }
            const char *val = (const char *)sqlite3_column_text(shstmt, 1);
            const char *key = (const char *)sqlite3_column_text(shstmt, 0);
            const char *kind = (const char *)sqlite3_column_text(shstmt, 2);
            const char *text = (val && val[0]) ? val : (key ? key : "");
            if (kind && kind[0])
               pos += (size_t)snprintf(buf + pos, cap - pos, "- [%s] %.300s\n", kind, text);
            else
               pos += (size_t)snprintf(buf + pos, cap - pos, "- %.300s\n", text);
         }
         if (has_shared)
            pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
         sqlite3_reset(shstmt);
      }
   }

   /* Sub-agent delegation (IMPORTANT) */
   {
      agent_config_t acfg;
      if (agent_load_config(&acfg) == 0 && acfg.agent_count > 0)
      {
         int has_tools = 0;
         for (int i = 0; i < acfg.agent_count; i++)
         {
            if (acfg.agents[i].tools_enabled)
            {
               has_tools = 1;
               break;
            }
         }
         if (has_tools)
         {
            pos += (size_t)snprintf(
                buf + pos, cap - pos,
                "# Delegation (IMPORTANT)\n"
                "ALWAYS use `aimee delegate <role> \"prompt\"` for multi-file tasks, "
                "infrastructure work, deployments, service checks, and parallel work.\n"
                "Do NOT use Claude's Agent tool or direct SSH — use aimee delegates instead.\n"
                "Sub-agents have SSH access to all homelab hosts and full tool execution.\n"
                "Roles: code, review, deploy, validate, test, diagnose, execute, refactor.\n"
                "Options: --tools (tool use), --background (async), --retry N, --verify CMD\n"
                "Examples:\n"
                "  aimee delegate execute \"Check nginx status on wol-web\"\n"
                "  aimee delegate code --tools \"Add error handling to src/memory.c\"\n"
                "  aimee delegate review \"Review PR #85 for security issues\"\n\n");
         }
      }
   }

   /* Recent delegation outcomes */
   {
      static const char *del_sql = "SELECT role, agent_name, success, turns, tool_calls,"
                                   " COALESCE(error, ''), created_at"
                                   " FROM agent_log ORDER BY id DESC LIMIT 5";
      sqlite3_stmt *dstmt = db_prepare(db, del_sql);
      if (dstmt)
      {
         int has_del = 0;
         sqlite3_reset(dstmt);
         while (sqlite3_step(dstmt) == SQLITE_ROW && pos < cap - 256)
         {
            if (!has_del)
            {
               pos += (size_t)snprintf(buf + pos, cap - pos, "# Recent Delegations\n");
               has_del = 1;
            }
            const char *drole = (const char *)sqlite3_column_text(dstmt, 0);
            const char *dagent = (const char *)sqlite3_column_text(dstmt, 1);
            int dsuccess = sqlite3_column_int(dstmt, 2);
            int dturns = sqlite3_column_int(dstmt, 3);
            int dtools = sqlite3_column_int(dstmt, 4);
            const char *derr = (const char *)sqlite3_column_text(dstmt, 5);
            if (dsuccess)
               pos += (size_t)snprintf(buf + pos, cap - pos,
                                       "- [%s] via %s: OK (%d turns, %d tools)\n",
                                       drole ? drole : "?", dagent ? dagent : "?", dturns, dtools);
            else
               pos += (size_t)snprintf(buf + pos, cap - pos, "- [%s] via %s: FAILED (%.80s)\n",
                                       drole ? drole : "?", dagent ? dagent : "?",
                                       derr ? derr : "unknown");
         }
         if (has_del)
            pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Workspace project descriptions and style guides */
   {
      config_t ws_cfg;
      config_load(&ws_cfg);
      char *ws_ctx = workspace_build_context_from_config(db, &ws_cfg);
      if (ws_ctx && ws_ctx[0])
      {
         size_t ws_len = strlen(ws_ctx);
         if (pos + ws_len < cap)
         {
            memcpy(buf + pos, ws_ctx, ws_len);
            pos += ws_len;
         }
      }
      free(ws_ctx);
   }

   /* Work queue summary */
   if (pos + 256 < cap)
   {
      int wlen = work_queue_summary(db, buf + pos, cap - pos);
      if (wlen > 0)
      {
         pos += (size_t)wlen;
         if (pos < cap)
            buf[pos++] = '\n';
      }
   }

   /* aimee capabilities reference */
   {
      char *caps = build_capabilities_text();
      if (caps)
      {
         size_t clen = strlen(caps);
         if (pos + clen < cap)
         {
            memcpy(buf + pos, caps, clen);
            pos += clen;
         }
         free(caps);
      }
   }

   buf[pos] = '\0';
   return buf;
}

/* Remove worktrees for a single stale session directory. */
static void remove_stale_worktrees(const config_t *cfg, const char *wt_dir, const char *sid)
{
   char session_wt_dir[MAX_PATH_LEN];
   snprintf(session_wt_dir, sizeof(session_wt_dir), "%s/%s", wt_dir, sid);

   DIR *sd = opendir(session_wt_dir);
   if (sd)
   {
      struct dirent *sub;
      while ((sub = readdir(sd)) != NULL)
      {
         if (sub->d_name[0] == '.')
            continue;
         char sub_path[MAX_PATH_LEN];
         snprintf(sub_path, sizeof(sub_path), "%s/%s", session_wt_dir, sub->d_name);

         for (int i = 0; i < cfg->workspace_count; i++)
         {
            const char *slash = strrchr(cfg->workspaces[i], '/');
            const char *ws_name = slash ? slash + 1 : cfg->workspaces[i];
            if (strcmp(ws_name, sub->d_name) == 0)
            {
               char *exec_out = NULL;
               const char *rm_argv[] = {"git",    "-C",      cfg->workspaces[i], "worktree",
                                        "remove", "--force", sub_path,           NULL};
               safe_exec_capture(rm_argv, &exec_out, 1024);
               free(exec_out);

               char short_id[12];
               snprintf(short_id, sizeof(short_id), "%.8s", sid);
               char branch_name[64];
               snprintf(branch_name, sizeof(branch_name), "aimee/session/%s", short_id);
               const char *br_argv[] = {"git",       "-C", cfg->workspaces[i], "branch", "-d",
                                        branch_name, NULL};
               exec_out = NULL;
               safe_exec_capture(br_argv, &exec_out, 1024);
               free(exec_out);
               break;
            }
         }
      }
      closedir(sd);
   }

   char *exec_out = NULL;
   const char *rm_argv[] = {"rm", "-rf", session_wt_dir, NULL};
   safe_exec_capture(rm_argv, &exec_out, 256);
   free(exec_out);
}

/* Prune stale sessions: fold their memories, run maintenance, clean up worktrees
 * and state files. This replaces the explicit wrapup command for ended sessions. */
static void prune_stale_sessions(sqlite3 *db, const config_t *cfg)
{
   char wt_dir[MAX_PATH_LEN];
   snprintf(wt_dir, sizeof(wt_dir), "%s/worktrees", config_output_dir());

   const char *config_dir = config_output_dir();
   time_t now = time(NULL);
   int did_maintenance = 0;

   /* Scan for stale session state files */
   DIR *d = opendir(config_dir);
   if (d)
   {
      struct dirent *ent;
      while ((ent = readdir(d)) != NULL)
      {
         /* Match session-*.state files */
         if (strncmp(ent->d_name, "session-", 8) != 0)
            continue;
         const char *dot = strstr(ent->d_name, ".state");
         if (!dot || dot[6] != '\0')
            continue;

         /* Extract session ID from filename */
         char stale_sid[64];
         size_t id_len = (size_t)(dot - (ent->d_name + 8));
         if (id_len == 0 || id_len >= sizeof(stale_sid))
            continue;
         memcpy(stale_sid, ent->d_name + 8, id_len);
         stale_sid[id_len] = '\0';

         /* Skip our own session */
         if (strcmp(stale_sid, session_id()) == 0)
            continue;

         /* Check staleness */
         char state_file[MAX_PATH_LEN];
         snprintf(state_file, sizeof(state_file), "%s/%s", config_dir, ent->d_name);
         struct stat st;
         if (stat(state_file, &st) != 0)
            continue;
         if (difftime(now, st.st_mtime) < 604800)
            continue; /* still fresh (7 days) */

         /* Fold this session's L0 memories into L1 */
         memory_fold_session(db, stale_sid);
         did_maintenance = 1;

         /* Release any work queue items claimed by this stale session */
         {
            const char *release_sql =
                "UPDATE work_queue SET status = 'pending', claimed_by = NULL, claimed_at = NULL "
                "WHERE claimed_by = ? AND status = 'claimed'";
            sqlite3_stmt *rs = db_prepare(db, release_sql);
            if (rs)
            {
               sqlite3_bind_text(rs, 1, stale_sid, -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(rs, "prune_stale_sessions");
               sqlite3_reset(rs);
            }
         }

         /* Remove worktrees for this session */
         remove_stale_worktrees(cfg, wt_dir, stale_sid);

         /* Delete the stale state file */
         unlink(state_file);
      }
      closedir(d);
   }

   /* Also check for orphaned worktree dirs with no state file */
   const char *current_sid = session_id();
   DIR *wd = opendir(wt_dir);
   if (wd)
   {
      struct dirent *ent;
      while ((ent = readdir(wd)) != NULL)
      {
         if (ent->d_name[0] == '.')
            continue;
         /* Skip our own session — state file may not exist yet during startup */
         if (strcmp(ent->d_name, current_sid) == 0)
            continue;
         char state_file[MAX_PATH_LEN];
         snprintf(state_file, sizeof(state_file), "%s/session-%s.state", config_dir, ent->d_name);
         struct stat st;
         if (stat(state_file, &st) != 0)
         {
            /* No state file, remove orphaned worktrees */
            remove_stale_worktrees(cfg, wt_dir, ent->d_name);
         }
      }
      closedir(wd);
   }

   /* Run global maintenance if any sessions were pruned */
   if (did_maintenance)
   {
      /* Scan conversations */
      char dirs[8][MAX_PATH_LEN];
      int dir_count = config_conversation_dirs(cfg, dirs, 8);
      memory_scan_conversations(db, dirs, dir_count);

      /* Expire session directives */
      directive_expire_session(db);

      /* Promote/demote/expire */
      int promoted = 0, demoted = 0, expired = 0;
      memory_run_maintenance(db, &promoted, &demoted, &expired);

      /* Extract anti-patterns */
      anti_pattern_extract_from_feedback(db);
      anti_pattern_extract_from_failures(db);

      /* Learn style */
      memory_learn_style(db);

      /* Compact old windows */
      int sc = 0, fc = 0;
      memory_compact_windows(db, &sc, &fc);

      /* Regenerate rules */
      char *md = rules_generate(db);
      free(md);
   }

   /* Clean up expired server_sessions (older than 7 days) and their worktrees */
   {
      /* First, find expired session IDs so we can clean up their worktrees */
      const char *exp_sql = "SELECT id FROM server_sessions "
                            "WHERE created_at <= datetime('now', '-7 days')";
      sqlite3_stmt *exp_stmt = NULL;
      if (sqlite3_prepare_v2(db, exp_sql, -1, &exp_stmt, NULL) == SQLITE_OK)
      {
         while (sqlite3_step(exp_stmt) == SQLITE_ROW)
         {
            const char *exp_sid = (const char *)sqlite3_column_text(exp_stmt, 0);
            if (exp_sid && exp_sid[0])
               remove_stale_worktrees(cfg, wt_dir, exp_sid);
         }
         sqlite3_finalize(exp_stmt);
      }

      /* Delete expired rows */
      const char *del_sql = "DELETE FROM server_sessions "
                            "WHERE created_at <= datetime('now', '-7 days')";
      sqlite3_stmt *del_stmt = NULL;
      if (sqlite3_prepare_v2(db, del_sql, -1, &del_stmt, NULL) == SQLITE_OK)
      {
         DB_STEP_LOG(del_stmt, "cmd_hooks");
         sqlite3_finalize(del_stmt);
      }
   }
}

void cmd_session_start(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argv;
   (void)argc;

   config_t cfg_buf;
   config_t *cfgp;
   if (ctx->cfg)
   {
      cfgp = ctx->cfg;
   }
   else
   {
      config_load(&cfg_buf);
      cfgp = &cfg_buf;
   }
   config_t cfg = *cfgp;
   ctx->db = db_open(cfg.db_path);
   if (!ctx->db)
      fatal("cannot open database");
   sqlite3 *db = ctx->db;

   /* Corruption detection: run quick_check once per session. If corruption is
    * found, attempt automatic recovery from the most recent valid backup. */
   if (db_quick_check(db) != 0)
   {
      fprintf(stderr, "aimee: database corruption detected\n");
      ctx_db_close(ctx);
      if (db_recover(cfg.db_path, 0) == 0)
      {
         /* Re-open after recovery (migrations will bring schema up to date) */
         ctx->db = db_open(cfg.db_path);
         if (!ctx->db)
            fatal("cannot open database after recovery");
         db = ctx->db;
         fprintf(stderr, "aimee: database recovered successfully\n");
      }
      else
      {
         fatal("database corrupted, run: aimee db recover --force");
      }
   }

   /* Build session state: compute sibling worktree paths for workspaces. */
   char state_path[MAX_PATH_LEN];
   session_state_path(state_path, sizeof(state_path));
   session_state_t state;
   memset(&state, 0, sizeof(state));
   snprintf(state.guardrail_mode, sizeof(state.guardrail_mode), "%s", config_guardrail_mode(&cfg));

   /* Register sibling worktrees for configured workspaces and CWD's git repo. */
   const char *sid = session_id();

   for (int i = 0; i < cfg.workspace_count && state.worktree_count < MAX_WORKTREES; i++)
   {
      struct stat ws_st;
      if (stat(cfg.workspaces[i], &ws_st) != 0 || !S_ISDIR(ws_st.st_mode))
      {
         fprintf(stderr, "aimee: warning: workspace '%s' does not exist, skipping worktree\n",
                 cfg.workspaces[i]);
         continue;
      }

      char gr[MAX_PATH_LEN];
      if (git_repo_root(cfg.workspaces[i], gr, sizeof(gr)) == 0)
      {
         /* Deduplicate by git_root */
         int dup = 0;
         for (int j = 0; j < state.worktree_count; j++)
         {
            if (strcmp(state.worktrees[j].git_root, gr) == 0)
            {
               dup = 1;
               break;
            }
         }
         if (!dup)
         {
            worktree_mapping_t *m = &state.worktrees[state.worktree_count];
            snprintf(m->git_root, sizeof(m->git_root), "%s", gr);
            worktree_sibling_path(gr, sid, m->worktree_path, sizeof(m->worktree_path));
            state.worktree_count++;
         }
      }
   }

   /* Auto-detect: if CWD is inside a git repo and not already covered, register it. */
   {
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)))
      {
         char gr[MAX_PATH_LEN];
         if (git_repo_root(cwd, gr, sizeof(gr)) == 0)
         {
            int dup = 0;
            for (int j = 0; j < state.worktree_count; j++)
            {
               if (strcmp(state.worktrees[j].git_root, gr) == 0)
               {
                  dup = 1;
                  break;
               }
            }
            if (!dup && state.worktree_count < MAX_WORKTREES)
            {
               worktree_mapping_t *m = &state.worktrees[state.worktree_count];
               snprintf(m->git_root, sizeof(m->git_root), "%s", gr);
               worktree_sibling_path(gr, sid, m->worktree_path, sizeof(m->worktree_path));
               state.worktree_count++;
            }
         }
      }
   }

   /* --- Session changelog: load previous HEADs, compute current HEADs --- */
   char heads_path[MAX_PATH_LEN];
   snprintf(heads_path, sizeof(heads_path), "%s/main_heads.json", config_output_dir());

   /* Load previous main heads */
   cJSON *prev_heads = NULL;
   {
      FILE *hf = fopen(heads_path, "r");
      if (hf)
      {
         fseek(hf, 0, SEEK_END);
         long hlen = ftell(hf);
         fseek(hf, 0, SEEK_SET);
         if (hlen > 0 && hlen < 65536)
         {
            char *hbuf = malloc(hlen + 1);
            if (hbuf)
            {
               size_t nr = fread(hbuf, 1, hlen, hf);
               hbuf[nr] = '\0';
               prev_heads = cJSON_Parse(hbuf);
               free(hbuf);
            }
         }
         fclose(hf);
      }
   }

   /* For each workspace, get current main HEAD and build changelog */
   char changelog_buf[WM_MAX_VALUE_LEN];
   int cl_off = 0;
   cJSON *new_heads = cJSON_CreateObject();
   int has_changelog = 0;

   /* Track projects whose main branch changed for background re-indexing */
   char changed_projects[MAX_WORKTREES][128];
   char changed_roots[MAX_WORKTREES][MAX_PATH_LEN];
   int changed_count = 0;

   for (int i = 0; i < state.worktree_count; i++)
   {
      const char *ws_root = state.worktrees[i].git_root;
      if (!ws_root[0])
         continue;

      /* Derive project name from git root */
      const char *proj_slash = strrchr(ws_root, '/');
      const char *proj_name = proj_slash ? proj_slash + 1 : ws_root;

      /* Get current HEAD of default branch */
      char head_cmd_buf[MAX_PATH_LEN + 64];
      snprintf(head_cmd_buf, sizeof(head_cmd_buf), "%s", ws_root);
      const char *rev_argv[] = {"git", "-C", head_cmd_buf, "rev-parse", "HEAD", NULL};
      char *head_out = NULL;
      int rc = safe_exec_capture(rev_argv, &head_out, 256);
      if (rc != 0 || !head_out)
      {
         free(head_out);
         continue;
      }

      /* Trim newline */
      size_t hlen = strlen(head_out);
      while (hlen > 0 && (head_out[hlen - 1] == '\n' || head_out[hlen - 1] == '\r'))
         head_out[--hlen] = '\0';

      if (hlen == 0)
      {
         free(head_out);
         continue;
      }

      cJSON_AddStringToObject(new_heads, proj_name, head_out);

      /* Check for previous HEAD */
      cJSON *ph = prev_heads ? cJSON_GetObjectItem(prev_heads, proj_name) : NULL;
      if (ph && cJSON_IsString(ph) && ph->valuestring[0] && strcmp(ph->valuestring, head_out) != 0)
      {
         /* Record this project as changed for background re-indexing */
         if (changed_count < MAX_WORKTREES)
         {
            snprintf(changed_projects[changed_count], sizeof(changed_projects[0]), "%s",
                     proj_name);
            snprintf(changed_roots[changed_count], sizeof(changed_roots[0]), "%s", ws_root);
            changed_count++;
         }

         /* Compute changelog: git log --oneline prev..current */
         char prev_range[192];
         snprintf(prev_range, sizeof(prev_range), "%s..%s", ph->valuestring, head_out);
         const char *log_argv[] = {"git",         "-C",  ws_root,    "log", "--oneline",
                                   "--no-merges", "-20", prev_range, NULL};
         char *log_out = NULL;
         rc = safe_exec_capture(log_argv, &log_out, 4096);

         /* Compute file diff stats */
         const char *stat_argv[] = {"git",      "-C", ws_root, "diff", "--stat", "--stat-width=60",
                                    prev_range, NULL};
         char *stat_out = NULL;
         int src = safe_exec_capture(stat_argv, &stat_out, 4096);

         /* Count commits */
         const char *count_argv[] = {"git", "-C", ws_root, "rev-list", "--count", prev_range, NULL};
         char *count_out = NULL;
         safe_exec_capture(count_argv, &count_out, 64);
         int commit_count = count_out ? atoi(count_out) : 0;

         if (rc == 0 && log_out && log_out[0])
         {
            has_changelog = 1;
            int n = snprintf(changelog_buf + cl_off, sizeof(changelog_buf) - cl_off,
                             "## %s (%d new commit%s)\n", proj_name, commit_count,
                             commit_count == 1 ? "" : "s");
            if (n > 0)
               cl_off += n;

            n = snprintf(changelog_buf + cl_off, sizeof(changelog_buf) - cl_off, "%s", log_out);
            if (n > 0)
               cl_off += n;

            if (src == 0 && stat_out && stat_out[0])
            {
               n = snprintf(changelog_buf + cl_off, sizeof(changelog_buf) - cl_off, "\n%s",
                            stat_out);
               if (n > 0)
                  cl_off += n;
            }
         }

         free(log_out);
         free(stat_out);
         free(count_out);
      }
      free(head_out);
   }

   /* Save current heads for next session */
   {
      char *hj = cJSON_PrintUnformatted(new_heads);
      if (hj)
      {
         FILE *hf = fopen(heads_path, "w");
         if (hf)
         {
            fputs(hj, hf);
            fclose(hf);
         }
         free(hj);
      }
   }
   cJSON_Delete(new_heads);
   if (prev_heads)
      cJSON_Delete(prev_heads);

   /* Store changelog in working memory */
   if (has_changelog)
   {
      changelog_buf[sizeof(changelog_buf) - 1] = '\0';
      wm_set(db, sid, "session_changelog", changelog_buf, "system", 0);
   }

   /* Write state file */
   state.dirty = 1;
   session_state_force_save(&state, state_path);

   /* Ensure .mcp.json exists in the CWD (worktree root) so MCP-capable
    * clients discover aimee's MCP server even in ephemeral worktrees. */
   {
      char mcp_cwd[MAX_PATH_LEN];
      if (getcwd(mcp_cwd, sizeof(mcp_cwd)))
         ensure_mcp_json(mcp_cwd);
      /* Also place in each worktree path */
      for (int i = 0; i < state.worktree_count; i++)
      {
         if (state.worktrees[i].worktree_path[0])
            ensure_mcp_json(state.worktrees[i].worktree_path);
      }
   }

   char *brief = build_session_context(db);

   /* Output: for hooks, print to stdout (text mode) */
   if (brief[0])
      printf("%s", brief);

   /* Output changelog */
   if (has_changelog)
   {
      printf("# Changes Since Last Session\n%s\n", changelog_buf);
   }

   /* NOTE: Working Directories output is deferred to cmd_launch() so that
    * only successfully-created worktrees are listed. Printing here would
    * advertise paths that may not exist yet (created==0 means deferred). */

   free(brief);

   /* Background janitor: run worktree GC if last run was >6 hours ago.
    * Uses a timestamp file to throttle. */
   {
      char janitor_ts[MAX_PATH_LEN];
      snprintf(janitor_ts, sizeof(janitor_ts), "%s/janitor.ts", config_output_dir());
      int should_run = 0;
      struct stat jst;
      if (stat(janitor_ts, &jst) != 0)
         should_run = 1; /* never run before */
      else if (difftime(time(NULL), jst.st_mtime) > 6 * 3600)
         should_run = 1;

      if (should_run)
      {
         /* Touch the timestamp file first to prevent concurrent runs */
         FILE *tf = fopen(janitor_ts, "w");
         if (tf)
            fclose(tf);

         /* Worktree GC is no longer needed — sibling worktrees are visible
          * to users and cleaned up on session end via worktree_cleanup(). */
      }
   }

   /* Background re-indexer: re-scan projects whose main branch changed,
    * throttled to at most once per hour per project. */
   if (changed_count > 0)
   {
      /* Filter to projects not indexed within the last hour */
      char idx_names[MAX_WORKTREES][128];
      char idx_roots[MAX_WORKTREES][MAX_PATH_LEN];
      int idx_count = 0;

      for (int i = 0; i < changed_count; i++)
      {
         char ts_path[MAX_PATH_LEN];
         snprintf(ts_path, sizeof(ts_path), "%s/index-%s.ts", config_output_dir(),
                  changed_projects[i]);
         struct stat ist;
         int throttled = 0;
         if (stat(ts_path, &ist) == 0 && difftime(time(NULL), ist.st_mtime) < 3600)
            throttled = 1;

         if (!throttled)
         {
            /* Touch throttle file immediately */
            FILE *tf = fopen(ts_path, "w");
            if (tf)
               fclose(tf);

            snprintf(idx_names[idx_count], sizeof(idx_names[0]), "%s", changed_projects[i]);
            snprintf(idx_roots[idx_count], sizeof(idx_roots[0]), "%s", changed_roots[i]);
            idx_count++;
         }
      }

      if (idx_count > 0)
      {
         pid_t idx_pid = fork();
         if (idx_pid == 0)
         {
            config_t idx_cfg;
            config_load(&idx_cfg);
            sqlite3 *idx_db = db_open_fast(idx_cfg.db_path);
            if (idx_db)
            {
               for (int i = 0; i < idx_count; i++)
                  index_scan_project(idx_db, idx_names[i], idx_roots[i], 0);
               db_stmt_cache_clear();
               db_close(idx_db);
            }
            _exit(0);
         }
         if (idx_pid > 0)
            waitpid(idx_pid, NULL, WNOHANG);
      }
   }

   ctx_db_close(ctx);
}

/* --- cmd_launch --- */

void cmd_launch(app_ctx_t *ctx, int argc, char **argv)
{
   /* Run session-start (prints context to stdout) */
   cmd_session_start(ctx, argc, argv);

   /* Now emit a JSON metadata line with launch info for the client.
    * The client needs: provider, and the worktree path to chdir to. */
   config_t cfg_local;
   config_t *cfg_ptr = ctx->cfg;
   if (!cfg_ptr)
   {
      config_load(&cfg_local);
      cfg_ptr = &cfg_local;
   }
   config_t cfg = *cfg_ptr;

   /* Determine provider */
   const char *provider = "claude";
   if (cfg.provider[0])
      provider = cfg.provider;

   int use_builtin =
       cfg.use_builtin_cli || strcmp(provider, "openai") == 0 || strcmp(provider, "copilot") == 0;

   /* Determine worktree target for cwd */
   char state_path[MAX_PATH_LEN];
   session_state_path(state_path, sizeof(state_path));
   session_state_t state;
   session_state_load(&state, state_path);

   /* Eagerly create sibling worktrees for all registered repos */
   {
      const char *launch_sid = session_id();
      for (int i = 0; i < state.worktree_count; i++)
         worktree_create_sibling(state.worktrees[i].git_root, launch_sid);
   }

   /* Output worktree directory mapping */
   if (state.worktree_count > 0)
   {
      printf("# Working Directories\n"
             "This session uses isolated worktrees. Use these paths for all reads/writes:\n");
      for (int i = 0; i < state.worktree_count; i++)
         printf("- %s -> %s\n", state.worktrees[i].git_root, state.worktrees[i].worktree_path);
      printf("\n");
   }

   char target_dir[MAX_PATH_LEN] = "";
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)) && state.worktree_count > 0)
   {
      /* If CWD is inside a tracked git root, compute the equivalent worktree path */
      const char *wt = worktree_for_cwd(&state, cwd);
      if (wt)
      {
         /* Compute the relative suffix within the git root */
         for (int i = 0; i < state.worktree_count; i++)
         {
            size_t rlen = strlen(state.worktrees[i].git_root);
            if (strncmp(cwd, state.worktrees[i].git_root, rlen) == 0 &&
                (cwd[rlen] == '/' || cwd[rlen] == '\0'))
            {
               const char *suffix = cwd + rlen;
               snprintf(target_dir, sizeof(target_dir), "%s%s",
                        state.worktrees[i].worktree_path, suffix);
               state.dirty = 1;
               session_state_save(&state, state_path);
               break;
            }
         }
      }
   }

   /* Check if this session has a changelog in working memory */
   int launch_has_changelog = 0;
   {
      sqlite3 *ldb = db_open(cfg.db_path);
      if (ldb)
      {
         wm_entry_t wm_entry;
         if (wm_get(ldb, session_id(), "session_changelog", &wm_entry) == 0)
            launch_has_changelog = 1;
         sqlite3_close(ldb);
      }
   }

   /* Emit launch metadata as a JSON line with a known prefix */
   cJSON *meta = cJSON_CreateObject();
   cJSON_AddStringToObject(meta, "provider", provider);
   cJSON_AddBoolToObject(meta, "builtin", use_builtin);
   if (cfg.autonomous)
      cJSON_AddBoolToObject(meta, "autonomous", 1);
   if (target_dir[0])
      cJSON_AddStringToObject(meta, "worktree_cwd", target_dir);
   if (launch_has_changelog)
      cJSON_AddBoolToObject(meta, "has_changelog", 1);
   char *json_str = cJSON_PrintUnformatted(meta);
   if (json_str)
   {
      printf("__LAUNCH__%s\n", json_str);
      free(json_str);
   }
   cJSON_Delete(meta);
}

/* --- cmd_wrapup --- */

/* Clean up worktrees for a session. Warns if unpushed commits exist. */
static void cleanup_worktrees(const session_state_t *state, const config_t *cfg, const char *sid)
{
   (void)cfg;
   if (state->worktree_count == 0)
      return;

   for (int i = 0; i < state->worktree_count; i++)
      worktree_cleanup(state->worktrees[i].git_root, sid);
}

void cmd_wrapup(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   config_t cfg;
   config_load(&cfg);

   const char *sid = session_id();

   /* Load session state for worktree cleanup */
   char state_path[MAX_PATH_LEN];
   session_state_path(state_path, sizeof(state_path));
   session_state_t state;
   session_state_load(&state, state_path);

   /* Clean up worktrees */
   cleanup_worktrees(&state, &cfg, sid);

   /* Run eval-to-behavior feedback loop */
   sqlite3 *db = ctx_db_open(ctx);
   if (db)
   {
      /* Extract anti-patterns from feedback and failures */
      anti_pattern_extract_from_feedback(db);
      anti_pattern_extract_from_failures(db);

      /* Escalate anti-patterns with 5+ hits to hard directives */
      int escalated = anti_pattern_escalate(db, 5);

      /* Adjust rule weights based on recent eval results */
      int adjustments = eval_feedback_loop(db);

      /* Decay stale rule weights */
      int decayed = rules_decay(db);

      /* Learn style preferences from feedback */
      memory_learn_style(db);

      /* Record session outcome based on recent agent_log entries */
      {
         static const char *outcome_sql = "SELECT"
                                          " COALESCE(SUM(success), 0),"
                                          " COUNT(*)"
                                          " FROM agent_log"
                                          " WHERE session_id = ?";
         sqlite3_stmt *os = db_prepare(db, outcome_sql);
         if (os)
         {
            sqlite3_bind_text(os, 1, sid, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(os) == SQLITE_ROW)
            {
               int successes = sqlite3_column_int(os, 0);
               int total = sqlite3_column_int(os, 1);
               const char *outcome = "unknown";
               if (total > 0)
                  outcome = (successes == total) ? "success"
                            : (successes > 0)    ? "partial"
                                                 : "failure";
               memory_record_outcome(db, sid, outcome);
            }
            sqlite3_reset(os);
         }
      }
      /* Mine execution traces for recurring patterns */
      trace_mine(db);

      if (!ctx->json_output && (escalated > 0 || adjustments > 0))
         fprintf(stderr, "Feedback loop: %d rule adjustments, %d anti-pattern escalations.\n",
                 adjustments, escalated);
      if (!ctx->json_output && (escalated > 0 || adjustments > 0 || decayed > 0))
         fprintf(stderr,
                 "Feedback loop: %d rule adjustments, %d anti-pattern escalations, %d rules "
                 "decayed.\n",
                 adjustments, escalated, decayed);

      ctx_db_close(ctx);
   }

   /* Delete session state file */
   unlink(state_path);

   /* Prune other stale sessions in background (non-blocking).
    * Done at exit so new session startup isn't impacted by old session cleanup. */
   pid_t prune_pid = fork();
   if (prune_pid == 0)
   {
      sqlite3 *prune_db = db_open_fast(cfg.db_path);
      if (prune_db)
      {
         prune_stale_sessions(prune_db, &cfg);
         db_close(prune_db);
      }
      _exit(0);
   }
   if (prune_pid > 0)
      waitpid(prune_pid, NULL, WNOHANG);

   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   else
      fprintf(stderr, "Session cleaned up.\n");
}
