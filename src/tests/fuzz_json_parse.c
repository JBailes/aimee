/* fuzz_json_parse.c: fuzz harness for cJSON_Parse
 *
 * Exercises the JSON parser with arbitrary input to find crashes,
 * buffer overflows, and undefined behavior under sanitizers.
 *
 * Build:
 *   libFuzzer: clang -fsanitize=fuzzer,address -o fuzz_json_parse ...
 *   Standalone: gcc -DFUZZ_STANDALONE -o fuzz_json_parse ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void fuzz_one(const char *data, size_t size)
{
   char *buf = malloc(size + 1);
   if (!buf)
      return;
   memcpy(buf, data, size);
   buf[size] = '\0';

   cJSON *root = cJSON_Parse(buf);
   if (root)
   {
      /* Exercise printing and traversal */
      char *printed = cJSON_PrintUnformatted(root);
      free(printed);

      /* Walk the tree */
      cJSON *child = root->child;
      while (child)
      {
         (void)child->string;
         (void)child->valuestring;
         (void)child->valuedouble;
         (void)child->valueint;
         child = child->next;
      }

      cJSON_Delete(root);
   }

   free(buf);
}

#ifndef FUZZ_STANDALONE
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   if (size == 0 || size > 1024 * 1024)
      return 0;
   fuzz_one((const char *)data, size);
   return 0;
}
#else
static int fuzz_file(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
   {
      fprintf(stderr, "Cannot open: %s\n", path);
      return 1;
   }

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 10 * 1024 * 1024)
   {
      fclose(f);
      return 0;
   }

   char *data = malloc((size_t)sz);
   if (!data)
   {
      fclose(f);
      return 1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   fclose(f);

   fuzz_one(data, nread);
   free(data);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      char buf[4096];
      size_t n = fread(buf, 1, sizeof(buf), stdin);
      fuzz_one(buf, n);
   }
   else
   {
      for (int i = 1; i < argc; i++)
      {
         if (fuzz_file(argv[i]) != 0)
            return 1;
      }
   }
   printf("fuzz_json_parse: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
