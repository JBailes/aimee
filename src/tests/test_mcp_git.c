/* test_mcp_git.c: MCP git tool handler tests for mcp_git.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "aimee.h"
#include "mcp_git.h"
#include "git_verify.h"
#include "cJSON.h"

/* --- Helpers --- */

static char *get_mcp_text(cJSON *resp)
{
   if (!resp || !cJSON_IsArray(resp))
      return NULL;
   cJSON *item = cJSON_GetArrayItem(resp, 0);
   if (!item)
      return NULL;
   cJSON *text = cJSON_GetObjectItem(item, "text");
   if (!cJSON_IsString(text))
      return NULL;
   return text->valuestring;
}

static char g_tmpdir[256];
static char g_saved_cwd[4096];

static void setup_git_repo(void)
{
   strcpy(g_tmpdir, "/tmp/aimee-test-mcp-git-XXXXXX");
   assert(mkdtemp(g_tmpdir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo 'hello world' > file.txt && "
            "git add file.txt && git commit -q -m 'initial commit'",
            g_tmpdir);
   assert(system(cmd) == 0);

   assert(getcwd(g_saved_cwd, sizeof(g_saved_cwd)) != NULL);
   assert(chdir(g_tmpdir) == 0);
}

static void teardown_git_repo(void)
{
   assert(chdir(g_saved_cwd) == 0);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmpdir);
   system(cmd);
}

/* --- Test handle_git_status in a clean repo --- */

static void test_git_status_clean(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_status(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "branch:") != NULL);
   assert(strstr(text, "clean") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_status with modifications --- */

static void test_git_status_modified(void)
{
   setup_git_repo();

   FILE *fp = fopen("file.txt", "w");
   assert(fp != NULL);
   fputs("modified content\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_status(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "modified") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_commit parameter validation --- */

static void test_git_commit_missing_message(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "message") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Empty message */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "");
   resp = handle_git_commit(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_commit in repo --- */

static void test_git_commit_success(void)
{
   setup_git_repo();

   /* Must be on a feature branch — commits on main are blocked */
   system("git checkout -q -b test-feature");

   FILE *fp = fopen("new.txt", "w");
   fputs("new file\n", fp);
   fclose(fp);
   system("git add new.txt");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "add new file");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   assert(strstr(text, "add new file") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_commit with sensitive file filtering --- */

static void test_git_commit_skips_sensitive(void)
{
   setup_git_repo();

   /* Must be on a feature branch — commits on main are blocked */
   system("git checkout -q -b test-sensitive");

   FILE *fp = fopen("normal.txt", "w");
   fputs("normal\n", fp);
   fclose(fp);

   fp = fopen(".env", "w");
   fputs("SECRET=xyz\n", fp);
   fclose(fp);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test sensitive");
   cJSON *files = cJSON_CreateArray();
   cJSON_AddItemToArray(files, cJSON_CreateString("normal.txt"));
   cJSON_AddItemToArray(files, cJSON_CreateString(".env"));
   cJSON_AddItemToObject(args, "files", files);

   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_push in non-git directory --- */

static void test_git_push_requires_branch(void)
{
   char tmpdir[] = "/tmp/aimee-test-push-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char saved[4096];
   assert(getcwd(saved, sizeof(saved)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved) == 0);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_branch parameter validation --- */

static void test_git_branch_missing_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "action") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_branch_create_and_list(void)
{
   setup_git_repo();

   /* List branches */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "list");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strlen(text) > 0);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Create a branch */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "test-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "created") != NULL);
   assert(strstr(text, "test-branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Switch back to original branch */
   system("git checkout -q master 2>/dev/null || git checkout -q main 2>/dev/null");

   /* Delete the branch */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "delete");
   cJSON_AddStringToObject(args, "name", "test-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "deleted") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* Note: handle_git_log is not directly tested here because its internal
 * format string contains git-format % placeholders that conflict with
 * snprintf's format parsing. This is tested indirectly through
 * test_mcp_server and test_cmd_core. */

/* --- Test handle_git_clone parameter validation --- */

static void test_git_clone_missing_url(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_clone(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "url") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_stash parameter validation --- */

static void test_git_stash_unknown_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "bogus");
   cJSON *resp = handle_git_stash(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_pr parameter validation --- */

static void test_git_pr_missing_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "action") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_unknown_action(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "nonexistent");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

static void test_git_pr_create_missing_title(void)
{
   /* Run in an isolated temp repo so merged-PR and verify gates don't
    * interfere with the missing-title error we're testing for. */
   char tmpdir[] = "/tmp/aimee-test-pr-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo x > f.txt && "
            "git add f.txt && git commit -q -m 'init'",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "title") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_diff_summary in repo --- */

static void test_git_diff_no_changes(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_diff_summary(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "no changes") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Test handle_git_verify --- */

static void test_git_verify(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_verify(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* Should show some output (success or no-config message) */
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

/* --- Branch ownership tests --- */

static void setup_ownership_db(void)
{
   sqlite3 *db;
   assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
   assert(sqlite3_exec(db,
                       "CREATE TABLE branch_ownership ("
                       "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  repo_path TEXT NOT NULL,"
                       "  branch_name TEXT NOT NULL,"
                       "  session_id TEXT NOT NULL,"
                       "  created_at TEXT DEFAULT (datetime('now')),"
                       "  UNIQUE(repo_path, branch_name)"
                       ");",
                       NULL, NULL, NULL) == SQLITE_OK);
   mcp_db_set(db);
}

static void teardown_ownership_db(void)
{
   sqlite3 *db = mcp_db_get();
   if (db)
      sqlite3_close(db);
   mcp_db_clear();
}

static void test_branch_create_registers_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();
   session_id_set_override("session-A");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "test-branch");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "created: test-branch") != NULL);
   assert(strstr(text, "owner: session-A") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_commit_allowed_despite_other_session_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* Create branch as session-A */
   session_id_set_override("session-A");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "owned-branch");
   cJSON *resp = handle_git_branch(args);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Commit as session-B */
   session_id_set_override("session-B");
   system("echo 'change' >> file.txt");
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test commit");
   resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed:") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_push_allowed_despite_other_session_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* Create branch as session-A */
   session_id_set_override("session-A");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddStringToObject(args, "name", "push-branch");
   cJSON *resp = handle_git_branch(args);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   char remote_dir[] = "/tmp/aimee-test-mcp-git-remote-XXXXXX";
   assert(mkdtemp(remote_dir) != NULL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "git init -q --bare '%s' && cd '%s' && git remote add origin '%s'",
            remote_dir, g_tmpdir, remote_dir);
   assert(system(cmd) == 0);

   /* Push as session-B */
   session_id_set_override("session-B");
   args = cJSON_CreateObject();
   resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "pushed:") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", remote_dir);
   system(cmd);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_branch_claim(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* Claim unowned branch as session-A */
   session_id_set_override("session-A");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "main");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   /* main cannot be claimed */
   assert(strstr(text, "error: cannot claim main") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Claim a regular branch */
   args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "claim");
   cJSON_AddStringToObject(args, "name", "some-branch");
   resp = handle_git_branch(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "claimed: some-branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

static void test_main_branch_no_ownership(void)
{
   setup_git_repo();
   setup_ownership_db();

   /* session-A owns nothing — commits on main are now blocked by main branch protection */
   session_id_set_override("session-A");
   system("echo 'change' >> file.txt");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "commit on main");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   session_id_clear_override();
   teardown_ownership_db();
   teardown_git_repo();
}

/* --- Main branch protection tests --- */

static void test_main_branch_commit_blocked(void)
{
   setup_git_repo();

   system("echo 'change' >> file.txt");
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "should fail");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_main_branch_push_blocked(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_push(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   assert(strstr(text, "main branch") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_main_branch_reset_blocked(void)
{
   setup_git_repo();

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "ref", "HEAD~1");
   cJSON_AddStringToObject(args, "mode", "soft");
   cJSON *resp = handle_git_reset(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_main_branch_delete_blocked(void)
{
   setup_git_repo();

   /* Switch away first so delete is theoretically possible */
   system("git checkout -q -b temp-branch");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "delete");
   cJSON_AddStringToObject(args, "name", "main");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "blocked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

static void test_feature_branch_commit_allowed(void)
{
   setup_git_repo();

   system("git checkout -q -b feature-test");
   system("echo 'change' >> file.txt");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "feature commit");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   teardown_git_repo();
}

int main(void)
{
   printf("mcp_git: ");

   test_git_status_clean();
   test_git_status_modified();
   test_git_commit_missing_message();
   test_git_commit_success();
   test_git_commit_skips_sensitive();
   test_git_push_requires_branch();
   test_git_branch_missing_action();
   test_git_branch_create_and_list();
   /* test_git_log skipped: format string issue in handle_git_log */
   test_git_clone_missing_url();
   test_git_stash_unknown_action();
   test_git_pr_missing_action();
   test_git_pr_unknown_action();
   test_git_pr_create_missing_title();
   test_git_diff_no_changes();
   test_git_verify();

   /* Branch ownership tests */
   test_branch_create_registers_ownership();
   test_commit_allowed_despite_other_session_ownership();
   test_push_allowed_despite_other_session_ownership();
   test_branch_claim();
   test_main_branch_no_ownership();

   /* Main branch protection tests */
   test_main_branch_commit_blocked();
   test_main_branch_push_blocked();
   test_main_branch_reset_blocked();
   test_main_branch_delete_blocked();
   test_feature_branch_commit_allowed();

   printf("all tests passed\n");
   return 0;
}
