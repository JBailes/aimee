#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cli_client.h"

static void test_parse_basic_provider(void)
{
   launch_meta_t meta;
   const char *output = "session info\n__LAUNCH__{\"provider\":\"claude\",\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "claude") == 0);
   assert(meta.builtin == 0);
   assert(meta.worktree_cwd[0] == '\0');
   assert(meta.context_len == 13); /* "session info\n" */
}

static void test_parse_custom_provider(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"cursor\",\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "cursor") == 0);
   assert(meta.context_len == 0);
}

static void test_parse_builtin(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"claude\",\"builtin\":true}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(meta.builtin == 1);
}

static void test_parse_worktree_cwd(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"claude\",\"builtin\":false,"
                        "\"worktree_cwd\":\"/tmp/wt/branch\"}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.worktree_cwd, "/tmp/wt/branch") == 0);
}

static void test_parse_missing_provider_defaults_claude(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "claude") == 0);
}

static void test_parse_no_launch_marker(void)
{
   launch_meta_t meta;
   const char *output = "just some regular output, no marker here\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 0);
}

static void test_parse_empty_output(void)
{
   launch_meta_t meta;
   int rc = parse_launch_meta("", &meta);
   assert(rc == 0);
}

static void test_parse_invalid_json(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__not-json-at-all\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 0);
}

static void test_parse_no_trailing_newline(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"aider\",\"builtin\":false}";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "aider") == 0);
}

static void test_parse_long_provider_truncated(void)
{
   launch_meta_t meta;
   /* provider field is 64 bytes; a 100-char name should be truncated, not overflow */
   char output[256];
   snprintf(output, sizeof(output),
            "__LAUNCH__{\"provider\":\""
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "\",\"builtin\":false}\n");
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strlen(meta.provider) < 64);
}

static void test_parse_context_preserved(void)
{
   launch_meta_t meta;
   const char *output = "line1\nline2\nline3\n__LAUNCH__{\"provider\":\"claude\"}\ntrailing\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(meta.context_len == 18); /* "line1\nline2\nline3\n" */
   /* Verify context_len points exactly to the marker */
   assert(strncmp(output + meta.context_len, "__LAUNCH__", 10) == 0);
}

int main(void)
{
   test_parse_basic_provider();
   test_parse_custom_provider();
   test_parse_builtin();
   test_parse_worktree_cwd();
   test_parse_missing_provider_defaults_claude();
   test_parse_no_launch_marker();
   test_parse_empty_output();
   test_parse_invalid_json();
   test_parse_no_trailing_newline();
   test_parse_long_provider_truncated();
   test_parse_context_preserved();
   printf("cli_launch: all tests passed\n");
   return 0;
}
