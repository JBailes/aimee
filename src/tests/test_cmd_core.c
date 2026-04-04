/* test_cmd_core.c: top-level command argument handling tests.
 * Tests MCP handler argument validation and subcmd dispatch. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "aimee.h"
#include "commands.h"
#include "mcp_git.h"
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

/* --- Test handle_git_status in a real temp git repo --- */

static void test_handle_git_status_in_repo(void)
{
   char tmpdir[] = "/tmp/aimee-test-git-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo hello > file.txt && "
            "git add file.txt && git commit -q -m 'init'",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_status(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "branch:") != NULL);
   assert(strstr(text, "clean") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   /* Create an untracked file */
   snprintf(cmd, sizeof(cmd), "touch '%s/untracked.txt'", tmpdir);
   system(cmd);

   args = cJSON_CreateObject();
   resp = handle_git_status(args);
   text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "untracked") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test handle_git_commit parameter validation --- */

static void test_handle_git_commit_missing_message(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "message") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_branch parameter validation --- */

static void test_handle_git_branch_missing_action(void)
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

static void test_handle_git_branch_missing_name(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON *resp = handle_git_branch(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "name") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_clone parameter validation --- */

static void test_handle_git_clone_missing_url(void)
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

/* --- Test handle_git_pr parameter validation --- */

static void test_handle_git_pr_missing_action(void)
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

static void test_handle_git_pr_create_missing_title(void)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "action", "create");
   cJSON_AddBoolToObject(args, "skip_verify", 1);
   cJSON *resp = handle_git_pr(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "error") != NULL);
   assert(strstr(text, "title") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);
}

/* --- Test handle_git_commit in repo --- */

static void test_handle_git_commit_in_repo(void)
{
   char tmpdir[] = "/tmp/aimee-test-commit-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd),
            "cd '%s' && git init -q && git config user.email test@test && "
            "git config user.name test && echo hello > file.txt && "
            "git add file.txt && git commit -q -m 'init'",
            tmpdir);
   assert(system(cmd) == 0);

   char saved_cwd[4096];
   assert(getcwd(saved_cwd, sizeof(saved_cwd)) != NULL);
   assert(chdir(tmpdir) == 0);

   /* Modify and stage */
   FILE *fp = fopen("file.txt", "w");
   fputs("modified\n", fp);
   fclose(fp);
   system("git add file.txt");

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "message", "test commit");
   cJSON *resp = handle_git_commit(args);
   char *text = get_mcp_text(resp);
   assert(text != NULL);
   assert(strstr(text, "committed") != NULL);
   cJSON_Delete(resp);
   cJSON_Delete(args);

   assert(chdir(saved_cwd) == 0);
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test subcmd_dispatch returns -1 for unknown subcommand --- */

static void test_subcmd_dispatch_unknown(void)
{
   const subcmd_t *table = get_work_subcmds();
   assert(table != NULL);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));

   int rc = subcmd_dispatch(table, "zzz_does_not_exist", &ctx, NULL, 0, NULL);
   assert(rc == -1);
}

/* --- Test subcmd_usage does not crash --- */

static void test_subcmd_usage_no_crash(void)
{
   const subcmd_t *table = get_work_subcmds();

   /* Redirect stdout to /dev/null */
   fflush(stdout);
   int saved = dup(STDOUT_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   if (dev_null >= 0)
   {
      dup2(dev_null, STDOUT_FILENO);
      close(dev_null);
   }

   subcmd_usage("work", table);

   dup2(saved, STDOUT_FILENO);
   close(saved);
}

int main(void)
{
   printf("cmd_core: ");

   test_handle_git_status_in_repo();
   test_handle_git_commit_missing_message();
   test_handle_git_branch_missing_action();
   test_handle_git_branch_missing_name();
   test_handle_git_clone_missing_url();
   test_handle_git_pr_missing_action();
   test_handle_git_pr_create_missing_title();
   test_handle_git_commit_in_repo();
   test_subcmd_dispatch_unknown();
   test_subcmd_usage_no_crash();

   printf("all tests passed\n");
   return 0;
}
