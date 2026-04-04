/* test_cmd_doctor.c: tests for the aimee doctor diagnostic command */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "aimee.h"
#include "commands.h"

/* --- Test: doctor command runs without crashing on a fresh database --- */

static void test_doctor_runs_on_fresh_db(void)
{
   /* Create a temp directory for config/db */
   char tmpdir[] = "/tmp/aimee-test-doctor-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   /* Set HOME to tmpdir so config_default_dir() uses it */
   char config_dir[4096];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", tmpdir);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", config_dir);
   assert(system(cmd) == 0);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   setenv("HOME", tmpdir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   /* Initialize a database */
   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open(cfg.db_path);
   assert(db != NULL);
   db_close(db);

   /* Run doctor with JSON output (to avoid exit() calls interfering) */
   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   /* Redirect stdout to capture JSON */
   fflush(stdout);
   int saved_stdout = dup(STDOUT_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   assert(dev_null >= 0);
   dup2(dev_null, STDOUT_FILENO);
   close(dev_null);

   /* Redirect stderr too */
   fflush(stderr);
   int saved_stderr = dup(STDERR_FILENO);
   dev_null = open("/dev/null", O_WRONLY);
   assert(dev_null >= 0);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   /* Doctor will call exit() on warnings/errors, so we fork */
   pid_t pid = fork();
   if (pid == 0)
   {
      /* Child: run doctor */
      cmd_doctor(&ctx, 0, NULL);
      _exit(0);
   }

   /* Parent: wait for child */
   int status;
   waitpid(pid, &status, 0);
   /* Doctor exits 0/1/2 depending on check results — all are valid */
   assert(WIFEXITED(status));
   int exit_code = WEXITSTATUS(status);
   assert(exit_code == 0 || exit_code == 1 || exit_code == 2);

   /* Restore stdout/stderr */
   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   /* Restore HOME */
   if (old_home)
   {
      setenv("HOME", old_home, 1);
      free(old_home);
   }

   /* Cleanup */
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test: doctor exits 2 on database error --- */

static void test_doctor_exits_2_on_db_error(void)
{
   char tmpdir[] = "/tmp/aimee-test-doctor-err-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char config_dir[4096];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", tmpdir);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", config_dir);
   assert(system(cmd) == 0);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   setenv("HOME", tmpdir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   /* Create config but with a DB path pointing to a non-existent location */
   config_t cfg;
   config_load(&cfg);
   /* Write config but don't create the DB */
   config_save(&cfg);

   /* Remove the DB file if it was auto-created */
   unlink(cfg.db_path);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   /* Suppress output */
   fflush(stdout);
   fflush(stderr);
   int saved_stdout = dup(STDOUT_FILENO);
   int saved_stderr = dup(STDERR_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   dup2(dev_null, STDOUT_FILENO);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   pid_t pid = fork();
   if (pid == 0)
   {
      cmd_doctor(&ctx, 0, NULL);
      _exit(0);
   }

   int status;
   waitpid(pid, &status, 0);
   assert(WIFEXITED(status));
   int exit_code = WEXITSTATUS(status);
   /* Should exit 2 (errors) because DB doesn't exist */
   assert(exit_code == 2);

   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   if (old_home)
   {
      setenv("HOME", old_home, 1);
      free(old_home);
   }

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test: doctor --fix flag is parsed --- */

static void test_doctor_fix_flag_parsed(void)
{
   char tmpdir[] = "/tmp/aimee-test-doctor-fix-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char config_dir[4096];
   snprintf(config_dir, sizeof(config_dir), "%s/.config/aimee", tmpdir);
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", config_dir);
   assert(system(cmd) == 0);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   setenv("HOME", tmpdir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);

   /* Initialize database */
   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open(cfg.db_path);
   assert(db != NULL);
   db_close(db);

   app_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.json_output = 1;

   /* Suppress output */
   fflush(stdout);
   fflush(stderr);
   int saved_stdout = dup(STDOUT_FILENO);
   int saved_stderr = dup(STDERR_FILENO);
   int dev_null = open("/dev/null", O_WRONLY);
   dup2(dev_null, STDOUT_FILENO);
   dup2(dev_null, STDERR_FILENO);
   close(dev_null);

   pid_t pid = fork();
   if (pid == 0)
   {
      char *args[] = {"--fix"};
      cmd_doctor(&ctx, 1, args);
      _exit(0);
   }

   int status;
   waitpid(pid, &status, 0);
   assert(WIFEXITED(status));
   /* Should not crash with --fix flag */
   int exit_code = WEXITSTATUS(status);
   assert(exit_code == 0 || exit_code == 1);

   dup2(saved_stdout, STDOUT_FILENO);
   close(saved_stdout);
   dup2(saved_stderr, STDERR_FILENO);
   close(saved_stderr);

   if (old_home)
   {
      setenv("HOME", old_home, 1);
      free(old_home);
   }

   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

int main(void)
{
   printf("cmd_doctor: ");

   test_doctor_runs_on_fresh_db();
   test_doctor_exits_2_on_db_error();
   test_doctor_fix_flag_parsed();

   printf("all tests passed\n");
   return 0;
}
