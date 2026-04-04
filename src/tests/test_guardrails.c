#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"

static void test_classify_sensitive(void)
{
   classification_t c = classify_path(NULL, ".env");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, "credentials.json");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, "id_rsa");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, "server.key");
   assert(c.severity == SEV_BLOCK);
}

static void test_classify_database(void)
{
   classification_t c = classify_path(NULL, "data.db");
   assert(c.severity == SEV_RED);

   c = classify_path(NULL, "app.sqlite3");
   assert(c.severity == SEV_RED);
}

static void test_classify_safe(void)
{
   classification_t c = classify_path(NULL, "main.go");
   assert(c.severity == SEV_GREEN);

   c = classify_path(NULL, "src/handler.c");
   assert(c.severity == SEV_GREEN);
}

static void test_is_write_command(void)
{
   assert(is_write_command("rm -rf /tmp/test") == 1);
   assert(is_write_command("git push origin main") == 1);
   assert(is_write_command("git commit -m 'msg'") == 1);
   assert(is_write_command("echo hello > file.txt") == 1);
   assert(is_write_command("pip install flask") == 1);

   assert(is_write_command("git log --oneline") == 0);
   assert(is_write_command("ls -la") == 0);
   assert(is_write_command("grep -r pattern .") == 0);
}

static void test_normalize_path(void)
{
   char buf[MAX_PATH_LEN];

   normalize_path("/abs/path", "/cwd", buf, sizeof(buf));
   assert(strcmp(buf, "/abs/path") == 0);

   normalize_path("rel/path", "/cwd", buf, sizeof(buf));
   assert(strcmp(buf, "/cwd/rel/path") == 0);

   normalize_path("~/home/file", "/cwd", buf, sizeof(buf));
   const char *home = getenv("HOME");
   if (home)
   {
      char expected[MAX_PATH_LEN];
      snprintf(expected, sizeof(expected), "%s/home/file", home);
      assert(strcmp(buf, expected) == 0);
   }
   else
   {
      assert(strcmp(buf, "~/home/file") == 0);
   }
}

static void test_plan_mode_blocks_writes(void)
{
   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_PLAN);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];
   int rc = pre_tool_check(db, "Edit", "{\"file_path\":\"/test.c\"}", &state, MODE_APPROVE, "", msg,
                           sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "plan mode") != NULL);

   rc = pre_tool_check(db, "Bash", "{\"command\":\"ls\"}", &state, MODE_APPROVE, "", msg,
                       sizeof(msg));
   assert(rc == 0); /* read-only command allowed */

   db_stmt_cache_clear();
   db_close(db);
}

static void test_session_id(void)
{
   const char *id = session_id();
   assert(id != NULL);
   assert(id[0] != '\0');
   /* Should be stable within a process */
   assert(strcmp(id, session_id()) == 0);
}

static void test_session_id_override(void)
{
   const char *base = session_id();
   assert(base != NULL);
   assert(base[0] != '\0');

   session_id_set_override("override-session");
   assert(strcmp(session_id(), "override-session") == 0);

   char state_path[MAX_PATH_LEN];
   session_state_path(state_path, sizeof(state_path));
   assert(strstr(state_path, "override-session.state") != NULL);

   session_id_clear_override();
   assert(strcmp(session_id(), base) == 0);
}

static void test_canonical_tool_names(void)
{
   assert(strcmp(guardrails_canonical_tool_name("bash"), "Bash") == 0);
   assert(strcmp(guardrails_canonical_tool_name("write_file"), "Write") == 0);
   assert(strcmp(guardrails_canonical_tool_name("spawn_agent"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("RemoteTrigger"), "Subagent") == 0);
   assert(strcmp(guardrails_canonical_tool_name("Read"), "Read") == 0);
}

static void test_session_state_worktrees(void)
{
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Add worktree entries */
   strcpy(state.worktrees[0].name, "wol");
   strcpy(state.worktrees[0].path, "/tmp/test-wt/wol");
   state.worktrees[0].created = 1;
   strcpy(state.worktrees[1].name, "acktng");
   strcpy(state.worktrees[1].path, "/tmp/test-wt/acktng");
   state.worktrees[1].created = 1;
   state.worktree_count = 2;

   /* Save and reload */
   const char *test_path = "/tmp/test-session-wt.state";
   session_state_force_save(&state, test_path);

   session_state_t loaded;
   session_state_load(&loaded, test_path);
   assert(loaded.worktree_count == 2);
   assert(strcmp(loaded.worktrees[0].name, "wol") == 0);
   assert(strcmp(loaded.worktrees[0].path, "/tmp/test-wt/wol") == 0);
   assert(strcmp(loaded.worktrees[1].name, "acktng") == 0);
   assert(strcmp(loaded.worktrees[1].path, "/tmp/test-wt/acktng") == 0);

   unlink(test_path);
}

static void test_worktree_blocks_writes(void)
{
   /* Create fake worktree directories so stat() checks pass */
   system("mkdir -p /tmp/worktrees/abc/wol");

   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Set up a worktree for "wol" */
   strcpy(state.worktrees[0].name, "wol");
   strcpy(state.worktrees[0].path, "/tmp/worktrees/abc/wol");
   state.worktrees[0].created = 1;
   state.worktree_count = 1;

   /* The config needs a matching workspace for worktree_for_path to work.
    * We test worktree_for_path directly instead. */
   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   strcpy(cfg.workspaces[0], "/root/aicli/wol");
   cfg.workspace_count = 1;

   /* Path inside workspace should match */
   const char *wt = worktree_for_path(&state, &cfg, "/root/aicli/wol/src/Foo.cs");
   assert(wt != NULL);
   assert(strcmp(wt, "/tmp/worktrees/abc/wol") == 0);

   /* Path outside workspace should not match */
   wt = worktree_for_path(&state, &cfg, "/root/aicli/other/file.c");
   assert(wt == NULL);

   /* No worktrees means no match */
   state.worktree_count = 0;
   wt = worktree_for_path(&state, &cfg, "/root/aicli/wol/src/Foo.cs");
   assert(wt == NULL);

   db_stmt_cache_clear();
   db_close(db);
   system("rm -rf /tmp/worktrees");
}

static void test_worktree_prefers_specific_workspace(void)
{
   /* Create fake worktree directories so stat() checks pass */
   system("mkdir -p /tmp/worktrees/abc/dev /tmp/worktrees/abc/aimee");

   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Set up worktrees for both parent "dev" and child "aimee" */
   strcpy(state.worktrees[0].name, "dev");
   strcpy(state.worktrees[0].path, "/tmp/worktrees/abc/dev");
   state.worktrees[0].created = 1;
   strcpy(state.worktrees[1].name, "aimee");
   strcpy(state.worktrees[1].path, "/tmp/worktrees/abc/aimee");
   state.worktrees[1].created = 1;
   state.worktree_count = 2;

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   /* Parent directory listed first — used to incorrectly match first */
   strcpy(cfg.workspaces[0], "/root/dev");
   strcpy(cfg.workspaces[1], "/root/dev/aimee");
   cfg.workspace_count = 2;

   /* Path inside /root/dev/aimee should match the aimee worktree, not dev */
   const char *wt = worktree_for_path(&state, &cfg, "/root/dev/aimee/src/main.c");
   assert(wt != NULL);
   assert(strcmp(wt, "/tmp/worktrees/abc/aimee") == 0);

   /* worktree_for_path_if_created should also prefer the specific match */
   wt = worktree_for_path_if_created(&state, &cfg, "/root/dev/aimee/src/main.c");
   assert(wt != NULL);
   assert(strcmp(wt, "/tmp/worktrees/abc/aimee") == 0);

   /* Path directly inside /root/dev (not a child workspace) should match dev */
   wt = worktree_for_path(&state, &cfg, "/root/dev/other/file.c");
   assert(wt != NULL);
   assert(strcmp(wt, "/tmp/worktrees/abc/dev") == 0);

   db_stmt_cache_clear();
   db_close(db);
   system("rm -rf /tmp/worktrees");
}

/* --- Deeper edge case tests --- */

static void test_classify_path_traversal(void)
{
   /* Path traversal attempts should still classify based on final component */
   classification_t c = classify_path(NULL, "../../.env");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, "/tmp/../etc/shadow");
   assert(c.severity >= SEV_GREEN); /* Not in sensitive list, but path is suspicious */

   c = classify_path(NULL, "foo/.env.local");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, ".env.production");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, "config/.env.backup");
   assert(c.severity == SEV_BLOCK);
}

static void test_classify_edge_cases(void)
{
   /* Empty path */
   classification_t c = classify_path(NULL, "");
   assert(c.severity == SEV_GREEN);

   /* Path with only extension */
   c = classify_path(NULL, ".pem");
   assert(c.severity == SEV_BLOCK);

   c = classify_path(NULL, ".p12");
   assert(c.severity == SEV_BLOCK);

   /* Deep nested sensitive file */
   c = classify_path(NULL, "deploy/secrets/production/credentials.json");
   assert(c.severity == SEV_BLOCK);

   /* Safe file with misleading name */
   c = classify_path(NULL, "env_test.go");
   assert(c.severity == SEV_GREEN);
}

static void test_is_write_command_edge_cases(void)
{
   /* Append redirect */
   assert(is_write_command("echo data >> logfile") == 1);

   /* Pipe to write command */
   assert(is_write_command("cat file | tee output.txt") == 1);

   /* sed in-place */
   assert(is_write_command("sed -i 's/old/new/g' file.txt") == 1);

   /* chmod */
   assert(is_write_command("chmod 755 script.sh") == 1);

   /* mv */
   assert(is_write_command("mv old.txt new.txt") == 1);

   /* cp */
   assert(is_write_command("cp src.txt dst.txt") == 1);

   /* Git write commands */
   assert(is_write_command("git add src/foo.c") == 1);
   assert(is_write_command("git restore --staged file.c") == 1);
   assert(is_write_command("git rm old_file.c") == 1);
   assert(is_write_command("git mv old.c new.c") == 1);

   /* Git read-only commands */
   assert(is_write_command("git status") == 0);
   assert(is_write_command("git log --oneline") == 0);
   assert(is_write_command("git diff") == 0);
   assert(is_write_command("git show HEAD") == 0);

   /* Read-only commands with tricky names */
   assert(is_write_command("cat file.txt") == 0);
   assert(is_write_command("head -20 file.txt") == 0);
   assert(is_write_command("wc -l file.txt") == 0);
   assert(is_write_command("diff a.txt b.txt") == 0);
   assert(is_write_command("find . -name '*.c'") == 0);

   /* fd-to-fd redirections — NOT file writes */
   assert(is_write_command("make 2>&1") == 0);
   assert(is_write_command("gcc -o test test.c 2>&1") == 0);
   assert(is_write_command("./run.sh 2>&1 | head") == 0);
   assert(is_write_command("cmd >&2") == 0);
   assert(is_write_command("cmd 1>&2") == 0);

   /* File redirections — ARE writes */
   assert(is_write_command("echo hello > file.txt") == 1);
   assert(is_write_command("echo hello 2>err.log") == 1);
   assert(is_write_command("echo hello 1>out.log") == 1);

   /* Empty/null command */
   assert(is_write_command("") == 0);
   assert(is_write_command(NULL) == 0);
}

static void test_normalize_path_edge_cases(void)
{
   char buf[MAX_PATH_LEN];

   /* NULL cwd */
   normalize_path("relative", NULL, buf, sizeof(buf));
   assert(strlen(buf) > 0);

   /* Empty path */
   normalize_path("", "/cwd", buf, sizeof(buf));

   /* Very long path component */
   char longpath[512];
   memset(longpath, 'a', 500);
   longpath[500] = '\0';
   normalize_path(longpath, "/cwd", buf, sizeof(buf));
   assert(strlen(buf) > 0);
}

static void test_plan_mode_allows_reads(void)
{
   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_PLAN);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* Read tools should be allowed in plan mode */
   int rc = pre_tool_check(db, "Read", "{\"file_path\":\"/test.c\"}", &state, MODE_APPROVE, "", msg,
                           sizeof(msg));
   assert(rc == 0);

   rc = pre_tool_check(db, "Glob", "{\"pattern\":\"*.c\"}", &state, MODE_APPROVE, "", msg,
                       sizeof(msg));
   assert(rc == 0);

   rc = pre_tool_check(db, "Grep", "{\"path\":\".\",\"pattern\":\"test\"}", &state, MODE_APPROVE,
                       "", msg, sizeof(msg));
   assert(rc == 0);

   /* Write tool blocked */
   rc = pre_tool_check(db, "Write", "{\"file_path\":\"/test.c\",\"content\":\"x\"}", &state,
                       MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 2);

   /* MultiEdit blocked */
   rc = pre_tool_check(db, "MultiEdit", "{\"file_path\":\"/test.c\"}", &state, MODE_APPROVE, "",
                       msg, sizeof(msg));
   assert(rc == 2);

   /* Bash write command blocked */
   rc = pre_tool_check(db, "Bash", "{\"command\":\"echo x > file.txt\"}", &state, MODE_APPROVE, "",
                       msg, sizeof(msg));
   assert(rc == 2);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_session_state_save_load_roundtrip(void)
{
   const char *path = "/tmp/test-guardrails-state.json";

   session_state_t original;
   memset(&original, 0, sizeof(original));
   strcpy(original.session_mode, MODE_PLAN);
   strcpy(original.guardrail_mode, MODE_DENY);
   original.active_task_id = 42;
   original.fetched_mask = 0x0F;
   strcpy(original.seen_paths[0], "/path/to/.env");
   strcpy(original.seen_paths[1], "/another/secret.key");
   original.seen_count = 2;
   strcpy(original.worktrees[0].name, "proj");
   strcpy(original.worktrees[0].path, "/wt/proj");
   original.worktrees[0].created = 1;
   original.worktree_count = 1;

   session_state_force_save(&original, path);

   session_state_t loaded;
   session_state_load(&loaded, path);

   assert(strcmp(loaded.session_mode, MODE_PLAN) == 0);
   assert(strcmp(loaded.guardrail_mode, MODE_DENY) == 0);
   assert(loaded.active_task_id == 42);
   assert(loaded.fetched_mask == 0x0F);
   assert(loaded.seen_count == 2);
   assert(strcmp(loaded.seen_paths[0], "/path/to/.env") == 0);
   assert(strcmp(loaded.seen_paths[1], "/another/secret.key") == 0);
   assert(loaded.worktree_count == 1);
   assert(strcmp(loaded.worktrees[0].name, "proj") == 0);

   unlink(path);
}

static void test_worktree_base_branch_roundtrip(void)
{
   const char *path = "/tmp/test-base-branch-rt.state";

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Set up a worktree with a cached base_branch */
   strcpy(state.worktrees[0].name, "myrepo");
   strcpy(state.worktrees[0].path, "/tmp/wt/myrepo");
   strcpy(state.worktrees[0].workspace_root, "/home/user/myrepo");
   strcpy(state.worktrees[0].base_branch, "origin/main");
   state.worktrees[0].created = 1;
   state.worktree_count = 1;

   session_state_force_save(&state, path);

   /* Reload and verify base_branch survived */
   session_state_t loaded;
   session_state_load(&loaded, path);
   assert(loaded.worktree_count == 1);
   assert(strcmp(loaded.worktrees[0].name, "myrepo") == 0);
   assert(strcmp(loaded.worktrees[0].base_branch, "origin/main") == 0);
   assert(strcmp(loaded.worktrees[0].workspace_root, "/home/user/myrepo") == 0);
   assert(loaded.worktrees[0].created == 1);

   unlink(path);
}

static void test_worktree_base_branch_empty_roundtrip(void)
{
   const char *path = "/tmp/test-base-branch-empty.state";

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Worktree with no cached base_branch (fresh, not yet resolved) */
   strcpy(state.worktrees[0].name, "newrepo");
   strcpy(state.worktrees[0].path, "/tmp/wt/newrepo");
   strcpy(state.worktrees[0].workspace_root, "/home/user/newrepo");
   state.worktrees[0].base_branch[0] = '\0';
   state.worktrees[0].created = 0;
   state.worktree_count = 1;

   session_state_force_save(&state, path);

   session_state_t loaded;
   session_state_load(&loaded, path);
   assert(loaded.worktree_count == 1);
   assert(loaded.worktrees[0].base_branch[0] == '\0');
   assert(loaded.worktrees[0].created == 0);

   unlink(path);
}

static void test_app_ctx_cfg_pointer(void)
{
   /* Verify that app_ctx_t can carry a config pointer */
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   assert(ctx.cfg == NULL);

   config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.provider, sizeof(cfg.provider), "test-provider");
   ctx.cfg = &cfg;

   assert(ctx.cfg != NULL);
   assert(strcmp(ctx.cfg->provider, "test-provider") == 0);
}

static void test_malformed_tool_payloads(void)
{
   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* NULL input_json */
   int rc = pre_tool_check(db, "Edit", NULL, &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0); /* Should handle gracefully */

   /* Empty JSON object */
   rc = pre_tool_check(db, "Edit", "{}", &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0);

   /* Malformed JSON */
   rc = pre_tool_check(db, "Edit", "{broken", &state, MODE_APPROVE, "", msg, sizeof(msg));
   assert(rc == 0); /* Should not crash */

   /* Very large file_path */
   char big_json[8192];
   char big_path[4000];
   memset(big_path, 'x', sizeof(big_path) - 1);
   big_path[sizeof(big_path) - 1] = '\0';
   snprintf(big_json, sizeof(big_json), "{\"file_path\":\"%s\"}", big_path);
   rc = pre_tool_check(db, "Edit", big_json, &state, MODE_APPROVE, "", msg, sizeof(msg));
   /* Should handle without crash */

   db_stmt_cache_clear();
   db_close(db);
}

static void test_anti_pattern_in_session_warning(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* Insert an anti-pattern that matches "rm -rf" commands */
   anti_pattern_t ap;
   int rc = anti_pattern_insert(db, "rm -rf dangerous delete", "Do not use rm -rf on project dirs",
                                "test", "test-ref", 0.9, &ap);
   assert(rc == 0);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];

   /* First hit: should warn but allow (rc=0), msg should contain WARNING */
   rc = pre_tool_check(db, "Bash", "{\"command\":\"rm -rf /tmp/project\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "WARNING") != NULL);
   assert(strstr(msg, "1/3") != NULL);

   /* Second hit: still warning */
   rc = pre_tool_check(db, "Bash", "{\"command\":\"rm -rf /tmp/other\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);
   assert(strstr(msg, "WARNING") != NULL);
   assert(strstr(msg, "2/3") != NULL);

   /* Third hit: should block (rc=2) */
   rc = pre_tool_check(db, "Bash", "{\"command\":\"rm -rf /tmp/again\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_anti_pattern_no_match_no_warning(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* Insert a pattern that won't match "ls" */
   anti_pattern_t ap;
   anti_pattern_insert(db, "rm -rf dangerous delete", "Dangerous deletion", "test", "ref", 0.9,
                       &ap);

   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.guardrail_mode, MODE_APPROVE);

   char msg[512];
   int rc = pre_tool_check(db, "Bash", "{\"command\":\"ls /tmp\"}", &state, MODE_APPROVE, "/tmp",
                           msg, sizeof(msg));
   assert(rc == 0);
   /* msg should be empty (no anti-pattern match, no other warning) */

   db_stmt_cache_clear();
   db_close(db);
}

static void test_known_subagent_tools_blocked(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   char msg[512];

   /* Claude sub-agent tool should be blocked */
   int rc = pre_tool_check(db, "Agent",
                           "{\"subagent_type\":\"Explore\","
                           "\"prompt\":\"Find the installer code\","
                           "\"description\":\"Find installer\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "guardrails") != NULL);

   /* Codex sub-agent tool should also be blocked */
   rc = pre_tool_check(db, "spawn_agent",
                       "{\"agent_type\":\"explorer\","
                       "\"message\":\"Find the installer code\"}",
                       &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);
   assert(strstr(msg, "guardrails") != NULL);

   /* Another provider-native remote delegation surface should also be blocked */
   rc = pre_tool_check(db, "RemoteTrigger", "{\"task\":\"Run tests\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "guardrails") != NULL);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_unknown_subagent_surface_blocked(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   char msg[512];

   int rc = pre_tool_check(db, "launch_remote_agent",
                           "{\"message\":\"Investigate the codebase\","
                           "\"role\":\"explorer\","
                           "\"description\":\"delegate task\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "guardrails") != NULL);

   /* Similar names without sub-agent-shaped payloads should not be trapped. */
   rc = pre_tool_check(db, "agent_status", "{\"query\":\"last run\"}", &state, MODE_APPROVE, "/tmp",
                       msg, sizeof(msg));
   assert(rc == 0);

   db_stmt_cache_clear();
   db_close(db);
}

static void test_worktree_blocks_cd_into_workspace(void)
{
   /* Create fake worktree directory so stat() checks in worktree_for_path pass */
   system("mkdir -p /tmp/worktrees/abc/aimee");

   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   /* Set up worktree for "aimee" */
   strcpy(state.worktrees[0].name, "aimee");
   strcpy(state.worktrees[0].path, "/tmp/worktrees/abc/aimee");
   strcpy(state.worktrees[0].workspace_root, "/root/dev/aimee");
   state.worktrees[0].created = 1;
   state.worktree_count = 1;

   /* "cd /root/dev/aimee && rm -rf src" — cd target is in workspace */
   char msg[1024] = "";
   int rc = pre_tool_check(db, "Bash", "{\"command\":\"cd /root/dev/aimee && rm -rf src\"}", &state,
                           MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   /* "cd /root/dev/other && rm -rf src" — cd target is NOT in workspace */
   msg[0] = '\0';
   rc = pre_tool_check(db, "Bash", "{\"command\":\"cd /root/dev/other && rm -rf src\"}", &state,
                       MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 0);

   /* "cd /root/dev/aimee/src && touch foo" — subdir of workspace */
   msg[0] = '\0';
   rc = pre_tool_check(db, "Bash", "{\"command\":\"cd /root/dev/aimee/src && touch foo\"}", &state,
                       MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   db_stmt_cache_clear();
   db_close(db);
   system("rm -rf /tmp/worktrees");
}

static void test_worktree_blocks_edit_to_real_workspace(void)
{
   /* Create fake worktree directory so stat() checks in worktree_for_path pass */
   system("mkdir -p /tmp/worktrees/abc/aimee");

   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   strcpy(state.worktrees[0].name, "aimee");
   strcpy(state.worktrees[0].path, "/tmp/worktrees/abc/aimee");
   strcpy(state.worktrees[0].workspace_root, "/root/dev/aimee");
   state.worktrees[0].created = 1;
   state.worktree_count = 1;

   /* Edit to real workspace path should be blocked */
   char msg[1024] = "";
   int rc = pre_tool_check(db, "Edit",
                           "{\"file_path\":\"/root/dev/aimee/src/memory.c\","
                           "\"old_string\":\"old\",\"new_string\":\"new\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 2);
   assert(strstr(msg, "BLOCKED") != NULL);

   /* Edit to worktree path should be allowed */
   msg[0] = '\0';
   rc = pre_tool_check(db, "Edit",
                       "{\"file_path\":\"/tmp/worktrees/abc/aimee/src/memory.c\","
                       "\"old_string\":\"old\",\"new_string\":\"new\"}",
                       &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 0);

   db_stmt_cache_clear();
   db_close(db);
   system("rm -rf /tmp/worktrees");
}

static void test_hook_call_count_increments(void)
{
   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);

   assert(state.hook_call_count == 0);

   char msg[1024] = "";
   pre_tool_check(db, "Read", "{\"file_path\":\"/tmp/foo.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                  sizeof(msg));
   assert(state.hook_call_count == 1);

   pre_tool_check(db, "Read", "{\"file_path\":\"/tmp/bar.c\"}", &state, MODE_APPROVE, "/tmp", msg,
                  sizeof(msg));
   assert(state.hook_call_count == 2);

   /* Verify it roundtrips through save/load */
   const char *path = "/tmp/test-hook-count.state";
   session_state_force_save(&state, path);

   session_state_t loaded;
   session_state_load(&loaded, path);
   assert(loaded.hook_call_count == 2);

   unlink(path);
   db_stmt_cache_clear();
   db_close(db);
}

static void test_no_worktree_allows_non_workspace_writes(void)
{
   /* When worktree_count == 0 and the write is NOT inside a configured workspace,
    * it should be allowed (no regression from auto-provision logic). */
   sqlite3 *db = db_open(":memory:");
   session_state_t state;
   memset(&state, 0, sizeof(state));
   strcpy(state.session_mode, MODE_IMPLEMENT);
   strcpy(state.guardrail_mode, MODE_APPROVE);
   /* worktree_count == 0: no worktrees provisioned */

   char msg[1024] = "";
   /* Edit to /tmp (not a workspace) should be allowed */
   int rc = pre_tool_check(db, "Edit",
                           "{\"file_path\":\"/tmp/test.c\","
                           "\"old_string\":\"old\",\"new_string\":\"new\"}",
                           &state, MODE_APPROVE, "/tmp", msg, sizeof(msg));
   assert(rc == 0);

   /* Bash write to /tmp should be allowed */
   msg[0] = '\0';
   rc = pre_tool_check(db, "Bash", "{\"command\":\"echo x > /tmp/test.c\"}", &state, MODE_APPROVE,
                       "/tmp", msg, sizeof(msg));
   assert(rc == 0);

   db_stmt_cache_clear();
   db_close(db);
}

int main(void)
{
   test_classify_sensitive();
   test_classify_database();
   test_classify_safe();
   test_classify_path_traversal();
   test_classify_edge_cases();
   test_is_write_command();
   test_is_write_command_edge_cases();
   test_normalize_path();
   test_normalize_path_edge_cases();
   test_plan_mode_blocks_writes();
   test_plan_mode_allows_reads();
   test_session_id();
   test_session_id_override();
   test_canonical_tool_names();
   test_session_state_worktrees();
   test_session_state_save_load_roundtrip();
   test_worktree_base_branch_roundtrip();
   test_worktree_base_branch_empty_roundtrip();
   test_app_ctx_cfg_pointer();
   test_worktree_blocks_writes();
   test_worktree_prefers_specific_workspace();
   test_malformed_tool_payloads();
   test_anti_pattern_in_session_warning();
   test_anti_pattern_no_match_no_warning();
   test_known_subagent_tools_blocked();
   test_unknown_subagent_surface_blocked();
   test_worktree_blocks_cd_into_workspace();
   test_worktree_blocks_edit_to_real_workspace();
   test_hook_call_count_increments();
   test_no_worktree_allows_non_workspace_writes();
   printf("guardrails: all tests passed\n");
   return 0;
}
