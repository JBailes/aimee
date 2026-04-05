/* server_mcp.c: handle mcp.call -- dispatches MCP tool calls within the server */
#include "server.h"
#include "aimee.h"
#include "memory.h"
#include "index.h"
#include "working_memory.h"
#include "mcp_git.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Forward declarations from agent_config.c */
int agent_load_config(agent_config_t *cfg);

/* Forward declarations from server_compute.c */
int handle_delegate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);
int handle_delegate_reply(server_ctx_t *ctx, server_conn_t *conn, cJSON *req);

/* --- Helpers --- */

static cJSON *text_content(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static int send_mcp_result(server_conn_t *conn, cJSON *content)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "content", content);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Tool handlers --- */

static cJSON *tool_search_memory(sqlite3 *db, cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   if (!cJSON_IsString(jq))
      return text_content("error: missing 'query' parameter");

   memory_t facts[20];
   int count = memory_find_facts(db, jq->valuestring, 20, facts, 20);

   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos += snprintf(buf + pos, sizeof(buf) - pos, "No facts found for '%s'", jq->valuestring);
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "Found %d fact(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "- **%s** [%s/%s]: %s\n", facts[i].key,
                         facts[i].tier, facts[i].kind, facts[i].content);
   }
   return text_content(buf);
}

static cJSON *tool_list_facts(sqlite3 *db)
{
   memory_t facts[64];
   int count = memory_list(db, TIER_L2, KIND_FACT, 64, facts, 64);

   char buf[8192];
   int pos = 0;
   if (count == 0)
      pos += snprintf(buf + pos, sizeof(buf) - pos, "No L2 facts stored.");
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "%d fact(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "- **%s**: %s\n", facts[i].key,
                         facts[i].content);
   }
   return text_content(buf);
}

static cJSON *tool_get_host(cJSON *args)
{
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(args, "name");
   if (!cJSON_IsString(jn))
      return text_content("error: missing 'name' parameter");

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || !cfg.network.ssh_entry[0])
      return text_content("error: no network configuration found");

   for (int i = 0; i < cfg.network.host_count; i++)
   {
      agent_net_host_t *h = &cfg.network.hosts[i];
      if (strcasecmp(h->name, jn->valuestring) == 0)
      {
         char buf[512];
         if (h->port > 0)
            snprintf(buf, sizeof(buf), "Host: %s\nIP: %s\nPort: %d\nUser: %s\nDescription: %s",
                     h->name, h->ip, h->port, h->user, h->desc);
         else
            snprintf(buf, sizeof(buf), "Host: %s\nIP: %s\nUser: %s\nDescription: %s", h->name,
                     h->ip, h->user, h->desc);
         return text_content(buf);
      }
   }

   char buf[256];
   snprintf(buf, sizeof(buf), "Host '%s' not found. Use list_hosts to see all available hosts.",
            jn->valuestring);
   return text_content(buf);
}

static cJSON *tool_list_hosts(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || !cfg.network.ssh_entry[0])
      return text_content("No network configuration found.");

   char buf[16384];
   int pos = 0;
   agent_network_t *nw = &cfg.network;

   pos += snprintf(buf + pos, sizeof(buf) - pos, "SSH Entry: %s\n\n", nw->ssh_entry);

   if (nw->network_count > 0)
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "Networks:\n");
      for (int i = 0; i < nw->network_count; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-16s %-20s %s\n", nw->networks[i].name,
                         nw->networks[i].cidr, nw->networks[i].desc);
      pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
   }

   if (nw->host_count > 0)
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "Hosts (%d):\n", nw->host_count);
      for (int i = 0; i < nw->host_count && pos < (int)sizeof(buf) - 256; i++)
      {
         agent_net_host_t *h = &nw->hosts[i];
         if (h->port > 0)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-20s %s:%d  %-8s %s\n", h->name,
                            h->ip, h->port, h->user, h->desc);
         else
            pos += snprintf(buf + pos, sizeof(buf) - pos, "  %-20s %-20s %-8s %s\n", h->name, h->ip,
                            h->user, h->desc);
      }
   }

   return text_content(buf);
}

static cJSON *tool_find_symbol(sqlite3 *db, cJSON *args)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "identifier");
   if (!cJSON_IsString(jid))
      return text_content("error: missing 'identifier' parameter");

   term_hit_t hits[20];
   int count = index_find(db, jid->valuestring, hits, 20);

   char buf[4096];
   int pos = 0;
   if (count == 0)
      pos += snprintf(buf + pos, sizeof(buf) - pos, "No symbol found for '%s'", jid->valuestring);
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "Found %d match(es) for '%s':\n\n", count,
                      jid->valuestring);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "- %s:%d [%s] in project '%s'\n",
                         hits[i].file_path, hits[i].line, hits[i].kind, hits[i].project);
   }
   return text_content(buf);
}

static cJSON *tool_preview_blast_radius(sqlite3 *db, cJSON *args)
{
   cJSON *jproj = cJSON_GetObjectItemCaseSensitive(args, "project");
   cJSON *jpaths = cJSON_GetObjectItemCaseSensitive(args, "paths");
   if (!cJSON_IsString(jproj) || !cJSON_IsArray(jpaths))
      return text_content("error: missing 'project' or 'paths' parameter");

   int cnt = cJSON_GetArraySize(jpaths);
   if (cnt < 1 || cnt > 100)
      return text_content("error: paths must contain 1-100 entries");

   char *paths[100];
   for (int i = 0; i < cnt; i++)
   {
      cJSON *item = cJSON_GetArrayItem(jpaths, i);
      paths[i] = cJSON_IsString(item) ? item->valuestring : "";
   }

   char *json = index_blast_radius_preview(db, jproj->valuestring, paths, cnt);
   cJSON *content = text_content(json);
   free(json);
   return content;
}

static cJSON *tool_record_attempt(sqlite3 *db, cJSON *args)
{
   cJSON *jap = cJSON_GetObjectItemCaseSensitive(args, "approach");
   cJSON *joc = cJSON_GetObjectItemCaseSensitive(args, "outcome");
   if (!cJSON_IsString(jap) || !cJSON_IsString(joc))
      return text_content("error: missing 'approach' or 'outcome' parameter");

   cJSON *jtc = cJSON_GetObjectItemCaseSensitive(args, "task_context");
   cJSON *jls = cJSON_GetObjectItemCaseSensitive(args, "lesson");

   cJSON *val = cJSON_CreateObject();
   cJSON_AddStringToObject(val, "task_context", cJSON_IsString(jtc) ? jtc->valuestring : "");
   cJSON_AddStringToObject(val, "approach", jap->valuestring);
   cJSON_AddStringToObject(val, "outcome", joc->valuestring);
   cJSON_AddStringToObject(val, "lesson", cJSON_IsString(jls) ? jls->valuestring : "");

   char *json_val = cJSON_PrintUnformatted(val);
   cJSON_Delete(val);
   if (!json_val)
      return text_content("error: failed to serialize attempt");

   char key[64];
   static int attempt_counter = 0;
   snprintf(key, sizeof(key), "attempt:%d", ++attempt_counter);

   const char *sid = session_id();
   int rc = wm_set(db, sid, key, json_val, "attempt", 14400);
   free(json_val);

   char buf[128];
   if (rc == 0)
      snprintf(buf, sizeof(buf), "Recorded attempt as %s", key);
   else
      snprintf(buf, sizeof(buf), "error: failed to store attempt");
   return text_content(buf);
}

static cJSON *tool_list_attempts(sqlite3 *db, cJSON *args)
{
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(args, "filter");
   const char *filter = cJSON_IsString(jf) ? jf->valuestring : NULL;

   const char *sid = session_id();
   wm_entry_t entries[WM_MAX_RESULTS];
   int count = wm_list(db, sid, "attempt", entries, WM_MAX_RESULTS);

   char buf[8192];
   int pos = 0;

   if (count == 0)
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "No attempts recorded this session.");
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "Previous attempts (%d total):\n\n",
                      count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++)
      {
         cJSON *v = cJSON_Parse(entries[i].value);
         if (!v)
            continue;

         cJSON *jtc = cJSON_GetObjectItemCaseSensitive(v, "task_context");
         cJSON *jap = cJSON_GetObjectItemCaseSensitive(v, "approach");
         cJSON *joc = cJSON_GetObjectItemCaseSensitive(v, "outcome");
         cJSON *jls = cJSON_GetObjectItemCaseSensitive(v, "lesson");

         const char *tc = cJSON_IsString(jtc) ? jtc->valuestring : "";
         const char *ap = cJSON_IsString(jap) ? jap->valuestring : "";
         const char *oc = cJSON_IsString(joc) ? joc->valuestring : "";
         const char *ls = cJSON_IsString(jls) ? jls->valuestring : "";

         if (filter && filter[0] && !strstr(tc, filter) && !strstr(ap, filter))
         {
            cJSON_Delete(v);
            continue;
         }

         pos +=
             snprintf(buf + pos, sizeof(buf) - (size_t)pos,
                      "- Context: %s\n  Tried: %s\n  Result: %s\n  Lesson: %s\n\n", tc, ap, oc, ls);
         cJSON_Delete(v);
      }
   }

   return text_content(buf);
}

/* --- Git tool dispatch --- */

static cJSON *dispatch_git_tool(const char *tool, cJSON *args, const char *sid, sqlite3 *db)
{
   char git_old_cwd[MAX_PATH_LEN] = {0};

   /* Reset worktree flag for each dispatch — it's per-call, not sticky */
   mcp_git_set_worktree(0);

   /* Set session ID override so mcp_chdir_git_root reads the correct git-cwd file */
   if (sid && sid[0])
      session_id_set_override(sid);

   /* Expose db to git handlers for branch ownership checks */
   if (db)
      mcp_db_set(db);

   int did_chdir = mcp_chdir_git_root(git_old_cwd, sizeof(git_old_cwd), args);

   /* If we're in a workspace that has a sibling worktree, chdir to it.
    * Uses the worktree mappings stored in session state.
    *
    * Skip worktree redirect for operations that must run in the actual
    * project root: git_verify (runs build/test/lint commands relative to
    * the project), and git_branch (manages branches in the main checkout). */
   int skip_worktree = (strcmp(tool, "git_verify") == 0 || strcmp(tool, "git_branch") == 0);

   if (sid && sid[0] && !skip_worktree)
   {
      char state_path[MAX_PATH_LEN];
      session_state_path(state_path, sizeof(state_path));
      session_state_t state;
      session_state_load(&state, state_path);

      if (state.worktree_count > 0)
      {
         char cwd[MAX_PATH_LEN];
         if (getcwd(cwd, sizeof(cwd)))
         {
            const char *wt = worktree_for_cwd(&state, cwd);
            if (wt)
            {
               fprintf(stderr, "aimee: mcp git: redirecting from %s to worktree %s\n", cwd, wt);
               if (chdir(wt) == 0)
               {
                  did_chdir = 1;
                  mcp_git_set_worktree(1);
               }
            }
         }
      }
   }

   cJSON *content = NULL;

   if (strcmp(tool, "git_status") == 0)
      content = handle_git_status(args);
   else if (strcmp(tool, "git_commit") == 0)
      content = handle_git_commit(args);
   else if (strcmp(tool, "git_push") == 0)
      content = handle_git_push(args);
   else if (strcmp(tool, "git_branch") == 0)
      content = handle_git_branch(args);
   else if (strcmp(tool, "git_log") == 0)
      content = handle_git_log(args);
   else if (strcmp(tool, "git_diff_summary") == 0)
      content = handle_git_diff_summary(args);
   else if (strcmp(tool, "git_pr") == 0)
      content = handle_git_pr(args);
   else if (strcmp(tool, "git_verify") == 0)
      content = handle_git_verify(args);
   else if (strcmp(tool, "git_pull") == 0)
      content = handle_git_pull(args);
   else if (strcmp(tool, "git_clone") == 0)
      content = handle_git_clone(args);
   else if (strcmp(tool, "git_stash") == 0)
      content = handle_git_stash(args);
   else if (strcmp(tool, "git_tag") == 0)
      content = handle_git_tag(args);
   else if (strcmp(tool, "git_fetch") == 0)
      content = handle_git_fetch(args);
   else if (strcmp(tool, "git_reset") == 0)
      content = handle_git_reset(args);
   else if (strcmp(tool, "git_restore") == 0)
      content = handle_git_restore(args);

   if (did_chdir && git_old_cwd[0])
      chdir(git_old_cwd);

   mcp_db_clear();

   if (sid && sid[0])
      session_id_clear_override();

   return content;
}

/* --- Main dispatch: mcp.call --- */

int handle_mcp_call(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jtool = cJSON_GetObjectItemCaseSensitive(req, "tool");
   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(req, "arguments");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   const char *sid = (jsid && cJSON_IsString(jsid)) ? jsid->valuestring : NULL;

   if (!cJSON_IsString(jtool))
      return server_send_error(conn, "missing 'tool' parameter", NULL);

   if (!jargs)
      jargs = cJSON_CreateObject();

   const char *tool = jtool->valuestring;
   cJSON *content = NULL;

   /* Delegate: forward to existing async handler */
   if (strcmp(tool, "delegate") == 0)
   {
      /* Reformat as server delegate request */
      cJSON *dreq = cJSON_CreateObject();
      cJSON_AddStringToObject(dreq, "method", "delegate");
      cJSON *jr = cJSON_GetObjectItemCaseSensitive(jargs, "role");
      cJSON *jp = cJSON_GetObjectItemCaseSensitive(jargs, "prompt");
      if (cJSON_IsString(jr))
         cJSON_AddStringToObject(dreq, "role", jr->valuestring);
      if (cJSON_IsString(jp))
         cJSON_AddStringToObject(dreq, "prompt", jp->valuestring);
      int rc = handle_delegate(ctx, conn, dreq);
      cJSON_Delete(dreq);
      return rc;
   }

   /* Delegate reply: forward to existing handler */
   if (strcmp(tool, "delegate_reply") == 0)
   {
      cJSON *dreq = cJSON_CreateObject();
      cJSON_AddStringToObject(dreq, "method", "delegate.reply");
      cJSON *jid = cJSON_GetObjectItemCaseSensitive(jargs, "delegation_id");
      cJSON *jc = cJSON_GetObjectItemCaseSensitive(jargs, "content");
      if (cJSON_IsString(jid))
         cJSON_AddStringToObject(dreq, "delegation_id", jid->valuestring);
      if (cJSON_IsString(jc))
         cJSON_AddStringToObject(dreq, "content", jc->valuestring);
      int rc = handle_delegate_reply(ctx, conn, dreq);
      cJSON_Delete(dreq);
      return rc;
   }

   /* Non-git tools */
   if (strcmp(tool, "search_memory") == 0)
      content = tool_search_memory(conn->db, jargs);
   else if (strcmp(tool, "list_facts") == 0)
      content = tool_list_facts(conn->db);
   else if (strcmp(tool, "get_host") == 0)
      content = tool_get_host(jargs);
   else if (strcmp(tool, "list_hosts") == 0)
      content = tool_list_hosts();
   else if (strcmp(tool, "find_symbol") == 0)
      content = tool_find_symbol(conn->db, jargs);
   else if (strcmp(tool, "preview_blast_radius") == 0)
      content = tool_preview_blast_radius(conn->db, jargs);
   else if (strcmp(tool, "record_attempt") == 0)
      content = tool_record_attempt(conn->db, jargs);
   else if (strcmp(tool, "list_attempts") == 0)
      content = tool_list_attempts(conn->db, jargs);
   /* Git tools */
   else if (strncmp(tool, "git_", 4) == 0)
      content = dispatch_git_tool(tool, jargs, sid, conn->db);

   if (!content)
   {
      char errmsg[256];
      snprintf(errmsg, sizeof(errmsg), "unknown MCP tool: %s", tool);
      return server_send_error(conn, errmsg, NULL);
   }

   return send_mcp_result(conn, content);
}
