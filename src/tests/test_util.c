#include <assert.h>
#include <regex.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"

static void test_normalize_key(void)
{
   char buf[256];
   normalize_key("OpenAI API Key Issue", buf, sizeof(buf));
   assert(strcmp(buf, "openai api key issue") == 0);

   normalize_key("  The  big   problem  ", buf, sizeof(buf));
   assert(strcmp(buf, "big problem") == 0);

   normalize_key("a simple test", buf, sizeof(buf));
   assert(strcmp(buf, "simple test") == 0);

   normalize_key("An Error Was Encountered", buf, sizeof(buf));
   assert(strcmp(buf, "error encountered") == 0);
}

static void test_trigram_similarity(void)
{
   double sim = trigram_similarity("hello world", "hello world");
   assert(sim > 0.99);

   sim = trigram_similarity("hello world", "goodbye moon");
   assert(sim < 0.3);

   sim = trigram_similarity("", "");
   assert(sim < 0.01); /* empty strings have no trigrams */
}

static void test_stem_word(void)
{
   char buf[64];
   stem_word("running", buf, sizeof(buf));
   assert(strcmp(buf, "runn") == 0);

   stem_word("tested", buf, sizeof(buf));
   assert(strcmp(buf, "test") == 0);

   stem_word("go", buf, sizeof(buf));
   assert(strcmp(buf, "go") == 0);
}

static void test_is_likely_path(void)
{
   assert(is_likely_path("/usr/bin/test") == 1);
   assert(is_likely_path("./relative/path") == 1);
   assert(is_likely_path("../parent") == 1);
   assert(is_likely_path("~/home/file") == 1);
   assert(is_likely_path("no") == 0);
   assert(is_likely_path("ls") == 0);
}

static void test_shlex_split(void)
{
   char *tokens[32];
   int n = shlex_split("echo 'hello world' | grep hello", tokens, 32);
   assert(n >= 3);
   /* echo, hello world, grep, hello */
   assert(strcmp(tokens[0], "echo") == 0);
   assert(strcmp(tokens[1], "hello world") == 0);
   for (int i = 0; i < n; i++)
      free(tokens[i]);
}

static void test_split_camel_case(void)
{
   char *parts[16];
   int n = split_camel_case("getUserName", parts, 16);
   assert(n == 3);
   assert(strcmp(parts[0], "get") == 0);
   assert(strcmp(parts[1], "User") == 0);
   assert(strcmp(parts[2], "Name") == 0);
   for (int i = 0; i < n; i++)
      free(parts[i]);
}

static void test_is_contradiction(void)
{
   assert(is_contradiction("always deploy on Friday", "never deploy on Friday") == 1);
   assert(is_contradiction("use tabs", "use spaces") == 0);
}

static void test_run_cmd(void)
{
   int ec = -1;
   char *out;

   printf("test_run_cmd\n");

   out = run_cmd("echo hello", &ec);
   assert(out != NULL);
   assert(strcmp(out, "hello\n") == 0);
   assert(ec == 0);
   free(out);

   out = run_cmd("false", &ec);
   assert(out != NULL);
   assert(strcmp(out, "") == 0);
   assert(ec != 0);
   free(out);
}

static void test_shell_escape(void)
{
   char *out;

   printf("test_shell_escape\n");

   out = shell_escape("hello");
   assert(out != NULL);
   assert(strcmp(out, "hello") == 0);
   free(out);

   out = shell_escape("it's");
   assert(out != NULL);
   assert(strcmp(out, "it'\\''s") == 0);
   free(out);

   out = shell_escape(NULL);
   assert(out != NULL);
   assert(strcmp(out, "") == 0);
   free(out);
}

static void test_regex_match(void)
{
   printf("test_regex_match\n");

   assert(regex_match("^hello", "hello world", REG_EXTENDED) == 1);
   assert(regex_match("^world", "hello world", REG_EXTENDED) == 0);
   assert(regex_match("HELLO", "hello", REG_EXTENDED | REG_ICASE) == 1);
   assert(regex_match("[", "test", REG_EXTENDED) == 0);
   assert(regex_match(NULL, "test", 0) == 0);
}

int main(void)
{
   test_normalize_key();
   test_trigram_similarity();
   test_stem_word();
   test_is_likely_path();
   test_shlex_split();
   test_split_camel_case();
   test_is_contradiction();
   test_run_cmd();
   test_shell_escape();
   test_regex_match();
   printf("util: all tests passed\n");
   return 0;
}
