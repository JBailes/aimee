/* fuzz_config_load.c: fuzz harness for config JSON parsing and validation
 *
 * Exercises the cJSON parser and config field extraction logic with
 * arbitrary JSON input to find crashes in validation paths.
 *
 * Build:
 *   libFuzzer: clang -fsanitize=fuzzer,address -o fuzz_config_load ...
 *   Standalone: gcc -DFUZZ_STANDALONE -o fuzz_config_load ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"

static void fuzz_one(const char *data, size_t size)
{
   /* Null-terminate and parse as JSON (same path config_load takes) */
   char *buf = malloc(size + 1);
   if (!buf)
      return;
   memcpy(buf, data, size);
   buf[size] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return;

   /* Exercise config field extraction patterns used by config_load:
    * GetObjectItem, IsString, IsNumber, IsArray, etc. */
   cJSON *item;
   item = cJSON_GetObjectItemCaseSensitive(root, "provider");
   if (cJSON_IsString(item))
      (void)item->valuestring;

   item = cJSON_GetObjectItemCaseSensitive(root, "guardrail_mode");
   if (cJSON_IsString(item))
      (void)item->valuestring;

   item = cJSON_GetObjectItemCaseSensitive(root, "db_path");
   if (cJSON_IsString(item))
      (void)item->valuestring;

   item = cJSON_GetObjectItemCaseSensitive(root, "workspaces");
   if (cJSON_IsArray(item))
   {
      cJSON *ws;
      cJSON_ArrayForEach(ws, item)
      {
         cJSON *p = cJSON_GetObjectItemCaseSensitive(ws, "path");
         if (cJSON_IsString(p))
            (void)p->valuestring;
         cJSON *n = cJSON_GetObjectItemCaseSensitive(ws, "name");
         if (cJSON_IsString(n))
            (void)n->valuestring;
      }
   }

   cJSON_Delete(root);
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
   printf("fuzz_config_load: %d inputs ok\n", argc > 1 ? argc - 1 : 1);
   return 0;
}
#endif
