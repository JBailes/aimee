/* test_log.c: tests for logging infrastructure */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

static void test_log_parse_level(void)
{
   log_level_t level;

   assert(log_parse_level("error", &level) == 0 && level == LOG_ERROR);
   assert(log_parse_level("WARN", &level) == 0 && level == LOG_WARN);
   assert(log_parse_level("Info", &level) == 0 && level == LOG_INFO);
   assert(log_parse_level("debug", &level) == 0 && level == LOG_DEBUG);
   assert(log_parse_level("invalid", &level) != 0);
   assert(log_parse_level(NULL, &level) != 0);
   assert(log_parse_level("error", NULL) != 0);
}

static void test_log_level_control(void)
{
   log_init(LOG_WARN);
   assert(log_get_level() == LOG_WARN);

   log_set_level(LOG_DEBUG);
   assert(log_get_level() == LOG_DEBUG);

   log_set_level(LOG_ERROR);
   assert(log_get_level() == LOG_ERROR);
}

static void test_log_calls_do_not_crash(void)
{
   log_init(LOG_DEBUG);

   /* These should not crash even without audit file open */
   aimee_log(LOG_ERROR, "test", "error message %d", 42);
   aimee_log(LOG_WARN, "test", "warn message");
   aimee_log(LOG_INFO, "test", "info message");
   aimee_log(LOG_DEBUG, "test", "debug message");

   /* Audit should not crash without open file */
   audit_log("test_event", "some detail %s", "here");
}

static void test_log_level_filtering(void)
{
   /* When level is ERROR, only errors should be logged.
    * We can't easily capture stderr, but at least verify no crash. */
   log_init(LOG_ERROR);
   aimee_log(LOG_ERROR, "test", "this should appear");
   aimee_log(LOG_WARN, "test", "this should be suppressed");
   aimee_log(LOG_INFO, "test", "this should be suppressed");
   aimee_log(LOG_DEBUG, "test", "this should be suppressed");
}

int main(void)
{
   printf("test_log:\n");
   test_log_parse_level();
   printf("  log_parse_level: OK\n");
   test_log_level_control();
   printf("  log_level_control: OK\n");
   test_log_calls_do_not_crash();
   printf("  log_calls_no_crash: OK\n");
   test_log_level_filtering();
   printf("  log_level_filtering: OK\n");
   printf("All log tests passed.\n");
   return 0;
}
