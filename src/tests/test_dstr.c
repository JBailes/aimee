/* test_dstr.c: unit tests for the dynamic string library */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dstr.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_init_free(void)
{
   dstr_t s;
   dstr_init(&s);
   assert(dstr_len(&s) == 0);
   assert(strcmp(dstr_cstr(&s), "") == 0);
   dstr_free(&s);
   assert(s.data == NULL);
   assert(s.len == 0);
   PASS("init_free");
}

static void test_append_str(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "hello");
   assert(dstr_len(&s) == 5);
   assert(strcmp(dstr_cstr(&s), "hello") == 0);
   dstr_append_str(&s, " world");
   assert(dstr_len(&s) == 11);
   assert(strcmp(dstr_cstr(&s), "hello world") == 0);
   dstr_free(&s);
   PASS("append_str");
}

static void test_append_char(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_char(&s, 'a');
   dstr_append_char(&s, 'b');
   dstr_append_char(&s, 'c');
   assert(dstr_len(&s) == 3);
   assert(strcmp(dstr_cstr(&s), "abc") == 0);
   dstr_free(&s);
   PASS("append_char");
}

static void test_append_raw(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append(&s, "hello\0world", 11);
   assert(dstr_len(&s) == 11);
   assert(memcmp(s.data, "hello\0world", 11) == 0);
   dstr_free(&s);
   PASS("append_raw");
}

static void test_appendf(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_appendf(&s, "num=%d str=%s", 42, "test");
   assert(strcmp(dstr_cstr(&s), "num=42 str=test") == 0);
   dstr_appendf(&s, " more=%d", 99);
   assert(strcmp(dstr_cstr(&s), "num=42 str=test more=99") == 0);
   dstr_free(&s);
   PASS("appendf");
}

static void test_reset(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "hello world");
   size_t old_cap = s.cap;
   dstr_reset(&s);
   assert(dstr_len(&s) == 0);
   assert(strcmp(dstr_cstr(&s), "") == 0);
   assert(s.cap == old_cap); /* buffer retained */
   dstr_free(&s);
   PASS("reset");
}

static void test_steal(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_append_str(&s, "stolen");
   char *p = dstr_steal(&s);
   assert(strcmp(p, "stolen") == 0);
   assert(s.data == NULL);
   assert(s.len == 0);
   free(p);

   /* Steal from empty */
   dstr_init(&s);
   assert(dstr_steal(&s) == NULL);
   PASS("steal");
}

static void test_reserve(void)
{
   dstr_t s;
   dstr_init(&s);
   dstr_reserve(&s, 1000);
   assert(s.cap >= 1001);
   dstr_free(&s);
   PASS("reserve");
}

static void test_large_append(void)
{
   dstr_t s;
   dstr_init(&s);
   /* Append 10000 characters to trigger multiple reallocs */
   for (int i = 0; i < 10000; i++)
      dstr_append_char(&s, 'x');
   assert(dstr_len(&s) == 10000);
   assert(s.data[0] == 'x');
   assert(s.data[9999] == 'x');
   assert(s.data[10000] == '\0');
   dstr_free(&s);
   PASS("large_append");
}

static void test_appendf_large(void)
{
   dstr_t s;
   dstr_init(&s);
   /* Format a string longer than the initial capacity */
   char buf[200];
   memset(buf, 'A', 199);
   buf[199] = '\0';
   dstr_appendf(&s, "prefix:%s:suffix", buf);
   assert(dstr_len(&s) == 7 + 199 + 7);
   assert(strncmp(dstr_cstr(&s), "prefix:", 7) == 0);
   dstr_free(&s);
   PASS("appendf_large");
}

int main(void)
{
   printf("dstr:\n");
   test_init_free();
   test_append_str();
   test_append_char();
   test_append_raw();
   test_appendf();
   test_reset();
   test_steal();
   test_reserve();
   test_large_append();
   test_appendf_large();
   printf("all dstr tests passed\n");
   return 0;
}
