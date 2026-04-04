/* mcp_tools.c: MCP tool definitions -- shared between proxy and legacy MCP server */
#include "cJSON.h"

static cJSON *build_tool(const char *name, const char *desc, cJSON *schema)
{
   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "name", name);
   cJSON_AddStringToObject(t, "description", desc);
   cJSON_AddItemToObject(t, "inputSchema", schema);
   return t;
}

cJSON *mcp_build_tools_list(void)
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
      cJSON *m = cJSON_AddObjectToObject(p, "mirror");
      cJSON_AddStringToObject(m, "type", "boolean");
      cJSON_AddStringToObject(m, "description",
                              "Push with --mirror: replaces ALL remote refs with local refs. "
                              "Deletes remote branches that don't exist locally. "
                              "DESTRUCTIVE — only use when explicitly requested.");
      cJSON_AddItemToArray(
          tools, build_tool("git_push",
                            "Push current branch to origin. Sets upstream on first push. "
                            "Blocked if project has verify steps and HEAD is not verified. "
                            "Use mirror=true to sync all refs (destructive). "
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
      cJSON_AddStringToObject(a, "description",
                              "One of: create, switch, list, delete, claim, orphan. "
                              "Use 'claim' to take ownership of an unowned branch. "
                              "Use 'orphan' to create a branch with no history.");
      cJSON *n = cJSON_AddObjectToObject(p, "name");
      cJSON_AddStringToObject(n, "type", "string");
      cJSON_AddStringToObject(n, "description", "Branch name (required for create/switch/delete)");
      cJSON *b = cJSON_AddObjectToObject(p, "base");
      cJSON_AddStringToObject(b, "type", "string");
      cJSON_AddStringToObject(b, "description", "Base ref for create (default: current HEAD)");
      cJSON *bf = cJSON_AddObjectToObject(p, "force");
      cJSON_AddStringToObject(bf, "type", "boolean");
      cJSON_AddStringToObject(bf, "description",
                              "Force delete with -D instead of -d (default false). "
                              "Only for delete action.");
      cJSON *br = cJSON_AddObjectToObject(p, "remote");
      cJSON_AddStringToObject(br, "type", "boolean");
      cJSON_AddStringToObject(br, "description",
                              "Also delete the remote branch (default false). "
                              "Only for delete action.");
      cJSON *req = cJSON_CreateArray();
      cJSON_AddItemToArray(req, cJSON_CreateString("action"));
      cJSON_AddItemToObject(s, "required", req);
      cJSON_AddItemToArray(tools,
                           build_tool("git_branch",
                                      "Create, switch, list, delete, claim, or orphan branches. "
                                      "Branches are owned by the creating session. "
                                      "Use 'claim' to take ownership of an unowned branch. "
                                      "Use 'orphan' to create a branch with no parent history.",
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
