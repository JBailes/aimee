/* test_platform_process.c: tests for platform_exec_capture timeout behavior */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_process.h"

static void test_exec_capture_basic(void)
{
   char *out = NULL;
   size_t len = 0;
   int rc = platform_exec_capture("echo hello", &out, &len, 0);
   assert(rc == 0);
   assert(out != NULL);
   assert(strncmp(out, "hello", 5) == 0);
   free(out);
}

static void test_exec_capture_exit_code(void)
{
   char *out = NULL;
   size_t len = 0;
   int rc = platform_exec_capture("exit 42", &out, &len, 0);
   assert(rc == 42);
   free(out);
}

static void test_exec_capture_timeout(void)
{
   char *out = NULL;
   size_t len = 0;
   /* sleep 10 should be killed well before completion */
   int rc = platform_exec_capture("sleep 10", &out, &len, 200);
   assert(rc == -1); /* timeout returns -1 */
   free(out);
}

static void test_exec_capture_no_timeout_fast_cmd(void)
{
   char *out = NULL;
   size_t len = 0;
   /* Fast command with generous timeout should succeed normally */
   int rc = platform_exec_capture("echo quick", &out, &len, 5000);
   assert(rc == 0);
   assert(out != NULL);
   assert(strncmp(out, "quick", 5) == 0);
   free(out);
}

int main(void)
{
   test_exec_capture_basic();
   test_exec_capture_exit_code();
   test_exec_capture_timeout();
   test_exec_capture_no_timeout_fast_cmd();
   printf("platform_process: all tests passed\n");
   return 0;
}
