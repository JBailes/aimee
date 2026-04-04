/* mcp_server.c: aimee MCP server -- stdio JSON-RPC 2.0 for Model Context Protocol */
#include "aimee.h"
#include "cJSON.h"
#include "memory.h"
#include "index.h"
#include "agent_types.h"
#include "working_memory.h"
#include "mcp_git.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

/* Forward declarations from agent_config.c */
int agent_load_config(agent_config_t *cfg);

/* mcp_chdir_git_root is now in mcp_git.c (shared with server_mcp.c) */

#define MCP_LINE_MAX         65536
#define MCP_PROTOCOL_VERSION "2024-11-05"

/* Mutex for stdout writes -- the main loop and background delegate threads
 * both need to write JSON-RPC responses to stdout. */
static pthread_mutex_t g_stdout_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- Helpers --- */

static cJSON *mcp_text_content(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

static void mcp_send(cJSON *msg)
{
   char *s = cJSON_PrintUnformatted(msg);
   if (s)
   {
      pthread_mutex_lock(&g_stdout_mutex);
      fprintf(stdout, "%s\n", s);
      fflush(stdout);
      pthread_mutex_unlock(&g_stdout_mutex);
      free(s);
   }
   cJSON_Delete(msg);
}

static void mcp_respond(cJSON *id, cJSON *result)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
   cJSON_AddItemToObject(resp, "result", result);
   mcp_send(resp);
}

static void mcp_error(cJSON *id, int code, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   if (id)
      cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
   else
      cJSON_AddNullToObject(resp, "id");
   cJSON *err = cJSON_CreateObject();
   cJSON_AddNumberToObject(err, "code", code);
   cJSON_AddStringToObject(err, "message", message);
   cJSON_AddItemToObject(resp, "error", err);
   mcp_send(resp);
}

/* --- Tool definitions --- */

static cJSON *build_tool(const char *name, const char *desc, cJSON *schema)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON_AddItemToObject(t, "inputSchema", schema);
   return t;
}

static cJSON *build_tools_list(void)
{
   cJSON *tools = cJSON_CreateArray();

   /* search_memory */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *q = cJSON_AddObjectToObject(p, "query");
      cJSON_AddStringToObject(q, "type", "string");
      cJSON_AddStringToObject(q, "description", "Search terms to find matching facts and memories");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("query"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("search_memory",
                            "Search aimee's knowledge base for stored facts and memories. "
                            "Returns matching L2/L3 facts by keyword.",
                            s));
   }

   /* list_facts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("list_facts",
                            "List all stored facts in aimee's long-term memory (L2 tier).", s));
   }

   /* get_host */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Hostname to look up (e.g. 'proxmox', 'wol-web')");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("name"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("get_host",
                                      "Look up a specific host by name from the network inventory. "
                                      "Returns IP, port, user, and description.",
                                      s));
   }

   /* list_hosts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("list_hosts",
                            "List all hosts and networks in the infrastructure inventory.", s));
   }

   /* find_symbol */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *id = cJSON_AddObjectToObject(p, "identifier");
      cJSON_AddStringToObject(id, "type", "string");
      cJSON_AddStringToObject(id, "description",
                              "Symbol name to find (function, class, variable, etc.)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("identifier"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("find_symbol",
                                      "Find a code symbol (function, class, variable) across all "
                                      "indexed projects. Returns file path, line number, and kind.",
                                      s));
   }

   /* delegate */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "role");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description",
                              "Delegation role. Common values: code, review, explain, "
                              "refactor, draft, summarize, deploy, validate, test, "
                              "diagnose, execute");
      cJSON *pr = cJSON_AddObjectToObject(p, "prompt");
      cJSON_AddStringToObject(pr, "type", "string");
      cJSON_AddStringToObject(pr, "description", "Task description for the sub-agent");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("role"));
      cJSON_AddItemToArray(req, cJSON_CreateString("prompt"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("delegate",
                                      "Delegate a task to an aimee sub-agent. The sub-agent has "
                                      "SSH access to all homelab hosts and full tool execution.",
                                      s));
   }

   /* preview_blast_radius */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pj = cJSON_AddObjectToObject(p, "project");
      cJSON_AddStringToObject(pj, "type", "string");
      cJSON_AddStringToObject(pj, "description", "Project name from aimee index");
      cJSON *pp = cJSON_AddObjectToObject(p, "paths");
      cJSON_AddStringToObject(pp, "type", "array");
      cJSON *pi = cJSON_CreateObject();
      cJSON_AddStringToObject(pi, "type", "string");
      cJSON_AddItemToObject(pp, "items", pi);
      cJSON_AddStringToObject(pp, "description", "File paths to preview blast radius for");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("project"));
      cJSON_AddItemToArray(req, cJSON_CreateString("paths"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("preview_blast_radius",
                            "Preview the blast radius of proposed file changes before starting "
                            "work. Returns affected files, severity, and warnings.",
                            s));
   }

   /* delegate_reply */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *did = cJSON_AddObjectToObject(p, "delegation_id");
      cJSON_AddStringToObject(did, "type", "string");
      cJSON_AddStringToObject(did, "description", "ID of the delegation to reply to");
      cJSON *ct = cJSON_AddObjectToObject(p, "content");
      cJSON_AddStringToObject(ct, "type", "string");
      cJSON_AddStringToObject(ct, "description", "Reply content for the delegate");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("delegation_id"));
      cJSON_AddItemToArray(req, cJSON_CreateString("content"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("delegate_reply",
                                             "Reply to a delegate that has requested input. "
                                             "The delegate will receive this as the response "
                                             "to its request_input call.",
                                             s));
   }

   /* record_attempt */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *tc = cJSON_AddObjectToObject(p, "task_context");
      cJSON_AddStringToObject(tc, "type", "string");
      cJSON_AddStringToObject(tc, "description", "Brief description of what was being attempted");
      cJSON *ap = cJSON_AddObjectToObject(p, "approach");
      cJSON_AddStringToObject(ap, "type", "string");
      cJSON_AddStringToObject(ap, "description", "What was tried");
      cJSON *oc = cJSON_AddObjectToObject(p, "outcome");
      cJSON_AddStringToObject(oc, "type", "string");
      cJSON_AddStringToObject(oc, "description",
                              "What happened (error message, test failure, etc.)");
      cJSON *ls = cJSON_AddObjectToObject(p, "lesson");
      cJSON_AddStringToObject(ls, "type", "string");
      cJSON_AddStringToObject(ls, "description", "What to avoid or do differently");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("approach"));
      cJSON_AddItemToArray(req, cJSON_CreateString("outcome"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools, build_tool("record_attempt",
                                             "Record a failed approach so that delegates can avoid "
                                             "repeating the same mistake. Stored per-session.",
                                             s));
   }

   /* list_attempts */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *f = cJSON_AddObjectToObject(p, "filter");
      cJSON_AddStringToObject(f, "type", "string");
      cJSON_AddStringToObject(f, "description",
                              "Optional keyword to filter attempts by task_context or approach");
      cJSON_AddItemToArray(tools, build_tool("list_attempts",
                                             "List previously recorded failed approaches for the "
                                             "current session. Helps avoid repeating mistakes.",
                                             s));
   }

   /* --- Git tools --- */

   /* git_status */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools,
          build_tool("git_status",
                     "Get compact working tree status: branch, staged/modified/untracked counts "
                     "and file lists. Use instead of running 'git status' via Bash.",
                     s));
   }

   /* git_commit */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description", "Commit message");
      cJSON *f = cJSON_AddObjectToObject(p, "files");
      cJSON_AddStringToObject(f, "type", "array");
      cJSON *fi = cJSON_CreateObject();
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON_AddItemToObject(f, "items", fi);
      cJSON_AddStringToObject(
          f, "description",
          "Files to stage. If omitted, stages all modified tracked files. "
          "Sensitive files (.env, credentials, keys) are automatically skipped.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("message"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("git_commit",
                            "Stage files and commit. Returns commit hash and one-line diffstat. "
                            "Use instead of running 'git add' + 'git commit' via Bash.",
                            s));
   }

   /* git_push */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *f = cJSON_AddObjectToObject(p, "force");
      cJSON_AddStringToObject(f, "type", "boolean");
      cJSON_AddStringToObject(f, "description",
                              "Use --force-with-lease (default false). Never uses --force.");
      cJSON *sv = cJSON_AddObjectToObject(p, "skip_verify");
      cJSON_AddStringToObject(sv, "type", "boolean");
      cJSON_AddStringToObject(sv, "description",
                              "Skip verification gate (default false). "
                              "Only use when explicitly requested.");
      cJSON_AddItemToArray(
          tools, build_tool("git_push",
                            "Push current branch to origin. Sets upstream on first push. "
                            "Blocked if project has verify steps and HEAD is not verified. "
                            "Use instead of 'git push' via Bash.",
                            s));
   }

   /* git_verify */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON_AddObjectToObject(s, "properties");
      cJSON_AddItemToArray(
          tools, build_tool("git_verify",
                            "Run project verification steps (build, test, lint, etc.) defined "
                            "in .aimee/project.yaml. All steps must pass before git_push is "
                            "allowed. Returns pass/fail per step with timing.",
                            s));
   }

   /* git_branch */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description", "One of: create, switch, list, delete");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Branch name (required for create/switch/delete)");
      cJSON *b = cJSON_AddObjectToObject(p, "base");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Base ref for create (default: current HEAD)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("action"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("git_branch",
                                      "Create, switch, list, or delete branches. "
                                      "Use instead of 'git branch' / 'git checkout -b' via Bash.",
                                      s));
   }

   /* git_log */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *c = cJSON_AddObjectToObject(p, "count");
      cJSON_AddStringToObject(c, "type", "integer");
      cJSON_AddStringToObject(c, "description", "Number of commits (default 10, max 50)");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description",
                              "Ref or range (e.g. 'main..HEAD'). Default: current branch");
      cJSON *ds = cJSON_AddObjectToObject(p, "diff_stat");
      cJSON_AddStringToObject(ds, "type", "boolean");
      cJSON_AddStringToObject(ds, "description", "Include diffstat per commit (default false)");
      cJSON_AddItemToArray(tools,
                           build_tool("git_log",
                                      "Compact commit log with short hash and relative date. "
                                      "Use instead of 'git log' via Bash.",
                                      s));
   }

   /* git_diff_summary */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description",
                              "Compare against this ref (default: unstaged changes vs HEAD)");
      cJSON *so = cJSON_AddObjectToObject(p, "stat_only");
      cJSON_AddStringToObject(so, "type", "boolean");
      cJSON_AddStringToObject(so, "description",
                              "Only show file-level stats (default true). "
                              "Set false for per-file change summaries.");
      cJSON *f = cJSON_AddObjectToObject(p, "files");
      cJSON_AddStringToObject(f, "type", "array");
      cJSON *fi = cJSON_CreateObject();
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON_AddItemToObject(f, "items", fi);
      cJSON_AddStringToObject(f, "description", "Limit diff to these files");
      cJSON_AddItemToArray(
          tools, build_tool("git_diff_summary",
                            "Compact diff summary: file stats or compressed change descriptions. "
                            "Use instead of 'git diff' via Bash.",
                            s));
   }

   /* git_pr */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description", "One of: create, view, list, merge_status");
      cJSON *t = cJSON_AddObjectToObject(p, "title");
      cJSON_AddStringToObject(t, "type", "string");
      cJSON_AddStringToObject(t, "description", "PR title (for create)");
      cJSON *bd = cJSON_AddObjectToObject(p, "body");
      cJSON_AddStringToObject(bd, "type", "string");
      cJSON_AddStringToObject(bd, "description", "PR body (for create)");
      cJSON *n = cJSON_AddObjectToObject(p, "number");
      cJSON_AddStringToObject(n, "type", "integer");
      cJSON_AddStringToObject(n, "description", "PR number (for view/merge_status)");
      cJSON *b = cJSON_AddObjectToObject(p, "base");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Base branch for create (default: main)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("action"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("git_pr",
                            "Create, view, list PRs, or check merge status. "
                            "Use instead of 'gh pr' via Bash. Essential for checking if a PR "
                            "is merged before pushing.",
                            s));
   }

   /* git_pull */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "rebase");
      cJSON_AddStringToObject(r, "type", "boolean");
      cJSON_AddStringToObject(r, "description", "Use --rebase instead of merge (default false)");
      cJSON_AddItemToArray(tools,
                           build_tool("git_pull",
                                      "Pull changes from remote. Returns summary of what changed. "
                                      "Use instead of 'git pull' via Bash.",
                                      s));
   }

   /* git_clone */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *u = cJSON_AddObjectToObject(p, "url");
      cJSON_AddStringToObject(u, "type", "string");
      cJSON_AddStringToObject(u, "description", "Repository URL to clone");
      cJSON *pa = cJSON_AddObjectToObject(p, "path");
      cJSON_AddStringToObject(pa, "type", "string");
      cJSON_AddStringToObject(pa, "description", "Local path to clone into (optional)");
      cJSON *b = cJSON_AddObjectToObject(p, "branch");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Branch to checkout (optional)");
      cJSON *d = cJSON_AddObjectToObject(p, "depth");
      cJSON_AddStringToObject(d, "type", "integer");
      cJSON_AddStringToObject(d, "description", "Shallow clone depth (optional)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("url"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools,
          build_tool("git_clone", "Clone a repository. Use instead of 'git clone' via Bash.", s));
   }

   /* git_stash */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description",
                              "One of: push, pop, apply, list, drop (default: push)");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description", "Stash message (for push)");
      cJSON *idx = cJSON_AddObjectToObject(p, "index");
      cJSON_AddStringToObject(idx, "type", "integer");
      cJSON_AddStringToObject(idx, "description", "Stash index for apply/drop (default: 0)");
      cJSON_AddItemToArray(tools, build_tool("git_stash",
                                             "Stash or restore uncommitted changes. "
                                             "Use instead of 'git stash' via Bash.",
                                             s));
   }

   /* git_tag */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *a = cJSON_AddObjectToObject(p, "action");
      cJSON_AddStringToObject(a, "type", "string");
      cJSON_AddStringToObject(a, "description", "One of: create, list, delete (default: list)");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Tag name (required for create/delete)");
      cJSON *m = cJSON_AddObjectToObject(p, "message");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description",
                              "Tag message for annotated tag (optional, for create)");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Ref to tag (default: HEAD)");
      cJSON_AddItemToArray(
          tools, build_tool("git_tag",
                            "Create, list, or delete tags. Use instead of 'git tag' via Bash.", s));
   }

   /* git_fetch */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *pr = cJSON_AddObjectToObject(p, "prune");
      cJSON_AddStringToObject(pr, "type", "boolean");
      cJSON_AddStringToObject(pr, "description",
                              "Prune remote-tracking refs that no longer exist (default false)");
      cJSON *r = cJSON_AddObjectToObject(p, "remote");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Remote name (default: origin)");
      cJSON_AddItemToArray(
          tools,
          build_tool("git_fetch",
                     "Fetch from remote without merging. Use instead of 'git fetch' via Bash.", s));
   }

   /* git_reset */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *r = cJSON_AddObjectToObject(p, "ref");
      cJSON_AddStringToObject(r, "type", "string");
      cJSON_AddStringToObject(r, "description", "Target ref (default: HEAD~1)");
      cJSON *m = cJSON_AddObjectToObject(p, "mode");
      cJSON_AddStringToObject(m, "type", "string");
      cJSON_AddStringToObject(m, "description",
                              "Reset mode: soft (keep staged), mixed (unstage, default), "
                              "or hard (discard all changes)");
      cJSON_AddItemToArray(tools,
                           build_tool("git_reset",
                                      "Reset HEAD to a ref. Use instead of 'git reset' via Bash. "
                                      "Be cautious with --hard as it discards changes.",
                                      s));
   }

   /* git_restore */
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "type", "object");
      cJSON *p = cJSON_AddObjectToObject(s, "properties");
      cJSON *f = cJSON_AddObjectToObject(p, "files");
      cJSON_AddStringToObject(f, "type", "array");
      cJSON *fi = cJSON_CreateObject();
      cJSON_AddStringToObject(fi, "type", "string");
      cJSON_AddItemToObject(f, "items", fi);
      cJSON_AddStringToObject(f, "description", "Files to restore");
      cJSON *st = cJSON_AddObjectToObject(p, "staged");
      cJSON_AddStringToObject(st, "type", "boolean");
      cJSON_AddStringToObject(st, "description",
                              "Unstage files (restore --staged). Default false.");
      cJSON *src = cJSON_AddObjectToObject(p, "source");
      cJSON_AddStringToObject(src, "type", "string");
      cJSON_AddStringToObject(src, "description",
                              "Restore from this ref instead of index (e.g. HEAD~1)");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("files"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(
          tools, build_tool("git_restore",
                            "Restore files to a previous state or unstage them. "
                            "Use instead of 'git restore' / 'git checkout -- file' via Bash.",
                            s));
   }

   return tools;
}

/* --- Tool handlers --- */

static cJSON *handle_search_memory(sqlite3 *db, cJSON *args)
{
   cJSON *jq = cJSON_GetObjectItemCaseSensitive(args, "query");
   if (!cJSON_IsString(jq))
      return mcp_text_content("error: missing 'query' parameter");

   memory_t facts[20];
   int count = memory_find_facts(db, jq->valuestring, 20, facts, 20);

   char buf[8192];
   int pos = 0;
   if (count == 0)
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "No facts found for '%s'", jq->valuestring);
   }
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "Found %d fact(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "- **%s** [%s/%s]: %s\n", facts[i].key,
                         facts[i].tier, facts[i].kind, facts[i].content);
   }
   return mcp_text_content(buf);
}

static cJSON *handle_list_facts(sqlite3 *db)
{
   memory_t facts[64];
   int count = memory_list(db, TIER_L2, KIND_FACT, 64, facts, 64);

   char buf[8192];
   int pos = 0;
   if (count == 0)
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "No L2 facts stored.");
   }
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "%d fact(s):\n\n", count);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 512; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "- **%s**: %s\n", facts[i].key,
                         facts[i].content);
   }
   return mcp_text_content(buf);
}

static cJSON *handle_get_host(cJSON *args)
{
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(args, "name");
   if (!cJSON_IsString(jn))
      return mcp_text_content("error: missing 'name' parameter");

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || !cfg.network.ssh_entry[0])
      return mcp_text_content("error: no network configuration found");

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
         return mcp_text_content(buf);
      }
   }

   char buf[256];
   snprintf(buf, sizeof(buf), "Host '%s' not found. Use list_hosts to see all available hosts.",
            jn->valuestring);
   return mcp_text_content(buf);
}

static cJSON *handle_list_hosts(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || !cfg.network.ssh_entry[0])
      return mcp_text_content("No network configuration found.");

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

   return mcp_text_content(buf);
}

static cJSON *handle_find_symbol(sqlite3 *db, cJSON *args)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "identifier");
   if (!cJSON_IsString(jid))
      return mcp_text_content("error: missing 'identifier' parameter");

   term_hit_t hits[20];
   int count = index_find(db, jid->valuestring, hits, 20);

   char buf[4096];
   int pos = 0;
   if (count == 0)
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "No symbol found for '%s'", jid->valuestring);
   }
   else
   {
      pos += snprintf(buf + pos, sizeof(buf) - pos, "Found %d match(es) for '%s':\n\n", count,
                      jid->valuestring);
      for (int i = 0; i < count && pos < (int)sizeof(buf) - 256; i++)
         pos += snprintf(buf + pos, sizeof(buf) - pos, "- %s:%d [%s] in project '%s'\n",
                         hits[i].file_path, hits[i].line, hits[i].kind, hits[i].project);
   }
   return mcp_text_content(buf);
}

static cJSON *handle_delegate_reply_mcp(cJSON *args)
{
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(args, "delegation_id");
   cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(args, "content");
   if (!cJSON_IsString(jid) || !cJSON_IsString(jcontent))
      return mcp_text_content("error: missing 'delegation_id' or 'content' parameter");

   /* Use the CLI to forward the reply to the server */
   char *esc_id = shell_escape(jid->valuestring);
   char *esc_content = shell_escape(jcontent->valuestring);
   char cmd[4096];
   snprintf(cmd, sizeof(cmd), "aimee --json delegate reply --id '%s' --content '%s' 2>&1", esc_id,
            esc_content);
   free(esc_id);
   free(esc_content);

   FILE *fp = popen(cmd, "r");
   if (!fp)
      return mcp_text_content("error: failed to send reply");

   char output[1024];
   size_t total = 0;
   size_t nread;
   while ((nread = fread(output + total, 1, sizeof(output) - total - 1, fp)) > 0)
      total += nread;
   output[total] = '\0';
   pclose(fp);

   return mcp_text_content(total > 0 ? output : "Reply sent.");
}

/* Async delegate: runs popen in a background thread so the MCP stdio loop
 * is not blocked.  Sends the JSON-RPC response via mcp_send when done. */

typedef struct
{
   char cmd[4096];
   char role[64];
   cJSON *mcp_id; /* deep copy of the request id */
} delegate_thread_ctx_t;

static void *delegate_thread_fn(void *arg)
{
   delegate_thread_ctx_t *ctx = (delegate_thread_ctx_t *)arg;

   FILE *fp = popen(ctx->cmd, "r");
   if (!fp)
   {
      cJSON *content = mcp_text_content("error: failed to execute delegate command");
      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "content", content);
      mcp_respond(ctx->mcp_id, result);
      cJSON_Delete(ctx->mcp_id);
      free(ctx);
      return NULL;
   }

   char *output = malloc(65536);
   if (!output)
   {
      pclose(fp);
      cJSON *content = mcp_text_content("error: out of memory");
      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "content", content);
      mcp_respond(ctx->mcp_id, result);
      cJSON_Delete(ctx->mcp_id);
      free(ctx);
      return NULL;
   }

   size_t total = 0;
   size_t nread;
   while ((nread = fread(output + total, 1, 65536 - total - 1, fp)) > 0)
      total += nread;
   output[total] = '\0';

   int status = pclose(fp);

   char *msg = malloc(total + 256);
   if (msg)
   {
      if (status == 0)
         snprintf(msg, total + 256, "Delegation [%s] succeeded:\n\n%s", ctx->role, output);
      else
         snprintf(msg, total + 256, "Delegation [%s] failed (exit %d):\n\n%s", ctx->role,
                  WEXITSTATUS(status), output);
   }

   cJSON *content = mcp_text_content(msg ? msg : output);
   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "content", content);
   mcp_respond(ctx->mcp_id, result);

   free(output);
   free(msg);
   cJSON_Delete(ctx->mcp_id);
   free(ctx);
   return NULL;
}

/* Launch delegate in background thread. Returns 0 on success (response will be
 * sent asynchronously), or -1 if we should fall back to synchronous error. */
static int handle_delegate_async(cJSON *args, cJSON *mcp_id)
{
   cJSON *jr = cJSON_GetObjectItemCaseSensitive(args, "role");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(args, "prompt");
   if (!cJSON_IsString(jr) || !cJSON_IsString(jp))
      return -1; /* caller sends error */

   char *esc_role = shell_escape(jr->valuestring);
   char *esc_prompt = shell_escape(jp->valuestring);

   delegate_thread_ctx_t *ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
   {
      free(esc_role);
      free(esc_prompt);
      return -1;
   }

   snprintf(ctx->cmd, sizeof(ctx->cmd), "aimee --json delegate '%s' '%s' 2>&1", esc_role,
            esc_prompt);
   snprintf(ctx->role, sizeof(ctx->role), "%s", jr->valuestring);
   ctx->mcp_id = cJSON_Duplicate(mcp_id, 1);

   free(esc_role);
   free(esc_prompt);

   pthread_t tid;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   int rc = pthread_create(&tid, &attr, delegate_thread_fn, ctx);
   pthread_attr_destroy(&attr);

   if (rc != 0)
   {
      cJSON_Delete(ctx->mcp_id);
      free(ctx);
      return -1;
   }

   return 0; /* response will be sent by thread */
}

static cJSON *handle_record_attempt(sqlite3 *db, cJSON *args)
{
   cJSON *jap = cJSON_GetObjectItemCaseSensitive(args, "approach");
   cJSON *joc = cJSON_GetObjectItemCaseSensitive(args, "outcome");
   if (!cJSON_IsString(jap) || !cJSON_IsString(joc))
      return mcp_text_content("error: missing 'approach' or 'outcome' parameter");

   cJSON *jtc = cJSON_GetObjectItemCaseSensitive(args, "task_context");
   cJSON *jls = cJSON_GetObjectItemCaseSensitive(args, "lesson");

   /* Build structured JSON value */
   cJSON *val = cJSON_CreateObject();
   cJSON_AddStringToObject(val, "task_context", cJSON_IsString(jtc) ? jtc->valuestring : "");
   cJSON_AddStringToObject(val, "approach", jap->valuestring);
   cJSON_AddStringToObject(val, "outcome", joc->valuestring);
   cJSON_AddStringToObject(val, "lesson", cJSON_IsString(jls) ? jls->valuestring : "");

   char *json_val = cJSON_PrintUnformatted(val);
   cJSON_Delete(val);
   if (!json_val)
      return mcp_text_content("error: failed to serialize attempt");

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
   return mcp_text_content(buf);
}

static cJSON *handle_list_attempts(sqlite3 *db, cJSON *args)
{
   cJSON *jf = cJSON_GetObjectItemCaseSensitive(args, "filter");
   const char *filter = cJSON_IsString(jf) ? jf->valuestring : NULL;

   const char *sid = session_id();
   wm_entry_t entries[WM_MAX_RESULTS];
   int count = wm_list(db, sid, "attempt", entries, WM_MAX_RESULTS);

   char buf[8192];
   int pos = 0;

   if (count == 0)
   {
      pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "No attempts recorded this session.");
   }
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

         /* Apply keyword filter */
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

   return mcp_text_content(buf);
}

/* --- MCP dispatch --- */

static void handle_request(sqlite3 *db, cJSON *req)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");

   if (!cJSON_IsString(method))
   {
      if (id)
         mcp_error(id, -32600, "Invalid request: missing method");
      return;
   }

   const char *m = method->valuestring;

   /* Notifications (no id) — don't send response */
   if (!id)
      return;

   if (strcmp(m, "initialize") == 0)
   {
      cJSON *result = cJSON_CreateObject();
      cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);

      cJSON *caps = cJSON_CreateObject();
      cJSON *tools_cap = cJSON_CreateObject();
      cJSON_AddBoolToObject(tools_cap, "listChanged", 0);
      cJSON_AddItemToObject(caps, "tools", tools_cap);
      cJSON *res_cap = cJSON_CreateObject();
      cJSON_AddBoolToObject(res_cap, "subscribe", 0);
      cJSON_AddBoolToObject(res_cap, "listChanged", 0);
      cJSON_AddItemToObject(caps, "resources", res_cap);
      cJSON *prompts_cap = cJSON_CreateObject();
      cJSON_AddBoolToObject(prompts_cap, "listChanged", 0);
      cJSON_AddItemToObject(caps, "prompts", prompts_cap);
      cJSON_AddItemToObject(result, "capabilities", caps);

      cJSON *info = cJSON_CreateObject();
      cJSON_AddStringToObject(info, "name", "aimee");
      cJSON_AddStringToObject(info, "version", "0.2.0");
      cJSON_AddItemToObject(result, "serverInfo", info);

      mcp_respond(id, result);
   }
   else if (strcmp(m, "tools/list") == 0)
   {
      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "tools", build_tools_list());
      mcp_respond(id, result);
   }
   else if (strcmp(m, "tools/call") == 0)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
      cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
      cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

      if (!cJSON_IsString(name))
      {
         mcp_error(id, -32602, "Missing tool name");
         return;
      }

      if (!args)
         args = cJSON_CreateObject();

      cJSON *content = NULL;
      const char *tool = name->valuestring;

      if (strcmp(tool, "search_memory") == 0)
         content = handle_search_memory(db, args);
      else if (strcmp(tool, "list_facts") == 0)
         content = handle_list_facts(db);
      else if (strcmp(tool, "get_host") == 0)
         content = handle_get_host(args);
      else if (strcmp(tool, "list_hosts") == 0)
         content = handle_list_hosts();
      else if (strcmp(tool, "find_symbol") == 0)
         content = handle_find_symbol(db, args);
      else if (strcmp(tool, "delegate") == 0)
      {
         if (handle_delegate_async(args, id) == 0)
            return; /* response sent by background thread */
         content = mcp_text_content("error: missing 'role' or 'prompt' parameter");
      }
      else if (strcmp(tool, "preview_blast_radius") == 0)
      {
         cJSON *jproj = cJSON_GetObjectItemCaseSensitive(args, "project");
         cJSON *jpaths = cJSON_GetObjectItemCaseSensitive(args, "paths");
         if (cJSON_IsString(jproj) && cJSON_IsArray(jpaths))
         {
            int cnt = cJSON_GetArraySize(jpaths);
            if (cnt > 0 && cnt <= 100)
            {
               char *paths[100];
               for (int pi = 0; pi < cnt; pi++)
               {
                  cJSON *item = cJSON_GetArrayItem(jpaths, pi);
                  paths[pi] = cJSON_IsString(item) ? item->valuestring : "";
               }
               char *json = index_blast_radius_preview(db, jproj->valuestring, paths, cnt);
               content = cJSON_CreateArray();
               cJSON *block = cJSON_CreateObject();
               cJSON_AddStringToObject(block, "type", "text");
               cJSON_AddStringToObject(block, "text", json);
               cJSON_AddItemToArray(content, block);
               free(json);
            }
            else
               content = NULL;
         }
         else
            content = NULL;
      }
      else if (strcmp(tool, "record_attempt") == 0)
         content = handle_record_attempt(db, args);
      else if (strcmp(tool, "list_attempts") == 0)
         content = handle_list_attempts(db, args);
      else if (strcmp(tool, "delegate_reply") == 0)
         content = handle_delegate_reply_mcp(args);
      /* Git tools — chdir to project git root before dispatch */
      else if (strncmp(tool, "git_", 4) == 0)
      {
         char git_old_cwd[MAX_PATH_LEN] = {0};
         int did_chdir = mcp_chdir_git_root(git_old_cwd, sizeof(git_old_cwd), args);

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
            (void)chdir(git_old_cwd);
      }
      else
      {
         mcp_error(id, -32602, "Unknown tool");
         return;
      }

      if (!content)
      {
         mcp_error(id, -32602, "Invalid tool arguments");
         return;
      }
      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "content", content);
      mcp_respond(id, result);
   }
   else if (strcmp(m, "resources/list") == 0)
   {
      cJSON *result = cJSON_CreateObject();
      cJSON *resources = cJSON_CreateArray();

      const char *tiers[] = {"L0", "L1", "L2", "L3"};
      for (int i = 0; i < 4; i++)
      {
         cJSON *r = cJSON_CreateObject();
         char uri[64];
         snprintf(uri, sizeof(uri), "aimee://memories/%s", tiers[i]);
         cJSON_AddStringToObject(r, "uri", uri);
         char name[64];
         snprintf(name, sizeof(name), "Memories (%s)", tiers[i]);
         cJSON_AddStringToObject(r, "name", name);
         cJSON_AddStringToObject(r, "mimeType", "application/json");
         cJSON_AddItemToArray(resources, r);
      }

      cJSON *facts = cJSON_CreateObject();
      cJSON_AddStringToObject(facts, "uri", "aimee://facts");
      cJSON_AddStringToObject(facts, "name", "All stored facts");
      cJSON_AddStringToObject(facts, "mimeType", "application/json");
      cJSON_AddItemToArray(resources, facts);

      cJSON *cfg_res = cJSON_CreateObject();
      cJSON_AddStringToObject(cfg_res, "uri", "aimee://config");
      cJSON_AddStringToObject(cfg_res, "name", "Current configuration");
      cJSON_AddStringToObject(cfg_res, "mimeType", "application/json");
      cJSON_AddItemToArray(resources, cfg_res);

      cJSON_AddItemToObject(result, "resources", resources);
      mcp_respond(id, result);
   }
   else if (strcmp(m, "resources/read") == 0)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
      cJSON *juri = cJSON_GetObjectItemCaseSensitive(params, "uri");
      if (!cJSON_IsString(juri))
      {
         mcp_error(id, -32602, "Missing uri parameter");
         return;
      }
      const char *uri = juri->valuestring;
      cJSON *contents = cJSON_CreateArray();

      if (strncmp(uri, "aimee://memories/", 17) == 0)
      {
         const char *tier = uri + 17;
         memory_t mems[50];
         int count = memory_list(db, tier, NULL, 50, mems, 50);
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
         {
            cJSON *m2 = cJSON_CreateObject();
            cJSON_AddNumberToObject(m2, "id", mems[i].id);
            cJSON_AddStringToObject(m2, "key", mems[i].key);
            cJSON_AddStringToObject(m2, "kind", mems[i].kind);
            cJSON_AddStringToObject(m2, "content", mems[i].content);
            cJSON_AddNumberToObject(m2, "confidence", mems[i].confidence);
            cJSON_AddItemToArray(arr, m2);
         }
         char *json = cJSON_PrintUnformatted(arr);
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "uri", uri);
         cJSON_AddStringToObject(item, "mimeType", "application/json");
         cJSON_AddStringToObject(item, "text", json ? json : "[]");
         cJSON_AddItemToArray(contents, item);
         free(json);
         cJSON_Delete(arr);
      }
      else if (strcmp(uri, "aimee://facts") == 0)
      {
         memory_t mems[100];
         int count = memory_list(db, NULL, "fact", 100, mems, 100);
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
         {
            cJSON *m2 = cJSON_CreateObject();
            cJSON_AddStringToObject(m2, "key", mems[i].key);
            cJSON_AddStringToObject(m2, "content", mems[i].content);
            cJSON_AddStringToObject(m2, "tier", mems[i].tier);
            cJSON_AddItemToArray(arr, m2);
         }
         char *json = cJSON_PrintUnformatted(arr);
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "uri", uri);
         cJSON_AddStringToObject(item, "mimeType", "application/json");
         cJSON_AddStringToObject(item, "text", json ? json : "[]");
         cJSON_AddItemToArray(contents, item);
         free(json);
         cJSON_Delete(arr);
      }
      else if (strcmp(uri, "aimee://config") == 0)
      {
         config_t cfg;
         config_load(&cfg);
         cJSON *c = cJSON_CreateObject();
         cJSON_AddStringToObject(c, "provider", cfg.provider);
         cJSON_AddStringToObject(c, "guardrail_mode", cfg.guardrail_mode);
         cJSON_AddNumberToObject(c, "workspace_count", cfg.workspace_count);
         char *json = cJSON_PrintUnformatted(c);
         cJSON *item = cJSON_CreateObject();
         cJSON_AddStringToObject(item, "uri", uri);
         cJSON_AddStringToObject(item, "mimeType", "application/json");
         cJSON_AddStringToObject(item, "text", json ? json : "{}");
         cJSON_AddItemToArray(contents, item);
         free(json);
         cJSON_Delete(c);
      }
      else
      {
         mcp_error(id, -32602, "Unknown resource URI");
         cJSON_Delete(contents);
         return;
      }

      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "contents", contents);
      mcp_respond(id, result);
   }
   else if (strcmp(m, "prompts/list") == 0)
   {
      cJSON *result = cJSON_CreateObject();
      cJSON *prompts = cJSON_CreateArray();

      /* search-and-summarize */
      {
         cJSON *p = cJSON_CreateObject();
         cJSON_AddStringToObject(p, "name", "search-and-summarize");
         cJSON_AddStringToObject(p, "description", "Search memories and summarize results");
         cJSON *args = cJSON_CreateArray();
         cJSON *a1 = cJSON_CreateObject();
         cJSON_AddStringToObject(a1, "name", "query");
         cJSON_AddStringToObject(a1, "description", "Search terms");
         cJSON_AddBoolToObject(a1, "required", 1);
         cJSON_AddItemToArray(args, a1);
         cJSON_AddItemToObject(p, "arguments", args);
         cJSON_AddItemToArray(prompts, p);
      }

      /* delegate-task */
      {
         cJSON *p = cJSON_CreateObject();
         cJSON_AddStringToObject(p, "name", "delegate-task");
         cJSON_AddStringToObject(p, "description", "Delegate a task to a sub-agent");
         cJSON *args = cJSON_CreateArray();
         cJSON *a1 = cJSON_CreateObject();
         cJSON_AddStringToObject(a1, "name", "role");
         cJSON_AddStringToObject(a1, "description", "Agent role (execute, review, etc.)");
         cJSON_AddBoolToObject(a1, "required", 1);
         cJSON_AddItemToArray(args, a1);
         cJSON *a2 = cJSON_CreateObject();
         cJSON_AddStringToObject(a2, "name", "prompt");
         cJSON_AddStringToObject(a2, "description", "Task prompt for the delegate");
         cJSON_AddBoolToObject(a2, "required", 1);
         cJSON_AddItemToArray(args, a2);
         cJSON_AddItemToObject(p, "arguments", args);
         cJSON_AddItemToArray(prompts, p);
      }

      /* diagnose-issue */
      {
         cJSON *p = cJSON_CreateObject();
         cJSON_AddStringToObject(p, "name", "diagnose-issue");
         cJSON_AddStringToObject(p, "description", "Run a diagnostic workflow");
         cJSON *args = cJSON_CreateArray();
         cJSON *a1 = cJSON_CreateObject();
         cJSON_AddStringToObject(a1, "name", "description");
         cJSON_AddStringToObject(a1, "description", "Issue description");
         cJSON_AddBoolToObject(a1, "required", 1);
         cJSON_AddItemToArray(args, a1);
         cJSON_AddItemToObject(p, "arguments", args);
         cJSON_AddItemToArray(prompts, p);
      }

      cJSON_AddItemToObject(result, "prompts", prompts);
      mcp_respond(id, result);
   }
   else if (strcmp(m, "prompts/get") == 0)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
      cJSON *jname = cJSON_GetObjectItemCaseSensitive(params, "name");
      cJSON *jargs = cJSON_GetObjectItemCaseSensitive(params, "arguments");
      if (!cJSON_IsString(jname))
      {
         mcp_error(id, -32602, "Missing prompt name");
         return;
      }
      const char *pname = jname->valuestring;
      cJSON *result = cJSON_CreateObject();
      cJSON *messages = cJSON_CreateArray();

      if (strcmp(pname, "search-and-summarize") == 0)
      {
         const char *query = "";
         if (jargs)
         {
            cJSON *q = cJSON_GetObjectItem(jargs, "query");
            if (cJSON_IsString(q))
               query = q->valuestring;
         }
         char text[2048];
         snprintf(text, sizeof(text),
                  "Search aimee's memory for information about: %s\n\n"
                  "Then summarize the results concisely, highlighting the most "
                  "relevant facts and their confidence levels.",
                  query);
         cJSON *msg = cJSON_CreateObject();
         cJSON_AddStringToObject(msg, "role", "user");
         cJSON *content = cJSON_CreateObject();
         cJSON_AddStringToObject(content, "type", "text");
         cJSON_AddStringToObject(content, "text", text);
         cJSON_AddItemToObject(msg, "content", content);
         cJSON_AddItemToArray(messages, msg);
      }
      else if (strcmp(pname, "delegate-task") == 0)
      {
         const char *role = "execute";
         const char *prompt = "";
         if (jargs)
         {
            cJSON *r = cJSON_GetObjectItem(jargs, "role");
            cJSON *p = cJSON_GetObjectItem(jargs, "prompt");
            if (cJSON_IsString(r))
               role = r->valuestring;
            if (cJSON_IsString(p))
               prompt = p->valuestring;
         }
         char text[4096];
         snprintf(text, sizeof(text),
                  "Delegate the following task to a sub-agent with role '%s':\n\n%s\n\n"
                  "Use the delegate tool to execute this.",
                  role, prompt);
         cJSON *msg = cJSON_CreateObject();
         cJSON_AddStringToObject(msg, "role", "user");
         cJSON *content = cJSON_CreateObject();
         cJSON_AddStringToObject(content, "type", "text");
         cJSON_AddStringToObject(content, "text", text);
         cJSON_AddItemToObject(msg, "content", content);
         cJSON_AddItemToArray(messages, msg);
      }
      else if (strcmp(pname, "diagnose-issue") == 0)
      {
         const char *desc = "";
         if (jargs)
         {
            cJSON *d = cJSON_GetObjectItem(jargs, "description");
            if (cJSON_IsString(d))
               desc = d->valuestring;
         }
         char text[4096];
         snprintf(text, sizeof(text),
                  "Diagnose the following issue:\n\n%s\n\n"
                  "Steps:\n"
                  "1. Search memories for related known issues\n"
                  "2. Check relevant facts and recent context\n"
                  "3. Propose root causes and remediation steps",
                  desc);
         cJSON *msg = cJSON_CreateObject();
         cJSON_AddStringToObject(msg, "role", "user");
         cJSON *content = cJSON_CreateObject();
         cJSON_AddStringToObject(content, "type", "text");
         cJSON_AddStringToObject(content, "text", text);
         cJSON_AddItemToObject(msg, "content", content);
         cJSON_AddItemToArray(messages, msg);
      }
      else
      {
         mcp_error(id, -32602, "Unknown prompt");
         cJSON_Delete(messages);
         return;
      }

      cJSON_AddItemToObject(result, "messages", messages);
      mcp_respond(id, result);
   }
   else
   {
      mcp_error(id, -32601, "Method not found");
   }
}

/* --- Entry point --- */

int main(void)
{
   /* Unbuffered stdout for MCP protocol */
   setvbuf(stdout, NULL, _IONBF, 0);

   /* Open database */
   sqlite3 *db = db_open_fast(db_default_path());
   if (!db)
   {
      fprintf(stderr, "aimee: failed to open database\n");
      return 1;
   }

   /* Read JSON-RPC messages from stdin, one per line */
   char *line = malloc(MCP_LINE_MAX);
   if (!line)
   {
      db_close(db);
      return 1;
   }

   while (fgets(line, MCP_LINE_MAX, stdin))
   {
      /* Strip trailing newline */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      if (len == 0)
         continue;

      cJSON *req = cJSON_Parse(line);
      if (!req)
      {
         mcp_error(NULL, -32700, "Parse error");
         continue;
      }

      handle_request(db, req);
      cJSON_Delete(req);
   }

   free(line);
   db_close(db);
   return 0;
}
