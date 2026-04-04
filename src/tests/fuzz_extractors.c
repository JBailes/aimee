/* fuzz_extractors.c: fuzz harness for extract_imports/exports/definitions
 *
 * Supports two modes:
 *   1. libFuzzer (clang -fsanitize=fuzzer): defines LLVMFuzzerTestOneInput
 *   2. Standalone (gcc): reads files from argv or stdin, useful for
 *      running the seed corpus without a fuzzer installed.
 *
 * Build:
 *   libFuzzer: clang -fsanitize=fuzzer,address -o fuzz_extractors ...
 *   Standalone: gcc -DFUZZ_STANDALONE -o fuzz_extractors ...
 */
#include "aimee.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All supported extensions to cycle through */
static const char *all_exts[] = {".js",  ".ts",   ".py", ".go",  ".cs", ".sh",
                                 ".css", ".dart", ".c",  ".lua", NULL};

static void fuzz_one(const char *data, size_t size, const char *ext)
{
   /* Null-terminate the input */
   char *buf = malloc(size + 1);
   if (!buf)
      return;
   memcpy(buf, data, size);
   buf[size] = '\0';

   /* Exercise extract_imports */
   char *imports[64];
   int nimports = extract_imports(ext, buf, imports, 64);
   for (int i = 0; i < nimports; i++)
      free(imports[i]);

   /* Exercise extract_exports */
   char *exports[64];
   int nexports = extract_exports(ext, buf, exports, 64);
   for (int i = 0; i < nexports; i++)
      free(exports[i]);

   /* Exercise extract_definitions */
   definition_t defs[64];
   int ndefs = extract_definitions(ext, buf, defs, 64);
   (void)ndefs;

   /* Exercise extract_routes */
   char *routes[64];
   int nroutes = extract_routes(ext, buf, routes, 64);
   for (int i = 0; i < nroutes; i++)
      free(routes[i]);

   free(buf);
}

#ifndef FUZZ_STANDALONE
/* libFuzzer entry point */
int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
   if (size == 0)
      return 0;

   /* Use first byte to select extension, rest is content */
   int ext_idx = data[0] % 10;
   fuzz_one((const char *)data + 1, size - 1, all_exts[ext_idx]);
   return 0;
}
#else
/* Standalone mode: run each file through all extractors */
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

   /* Detect extension from filename */
   const char *dot = strrchr(path, '.');
   if (dot && index_has_extractor(dot))
   {
      fuzz_one(data, nread, dot);
   }
   else
   {
      /* No extension match: run through all extractors */
      for (int i = 0; all_exts[i]; i++)
         fuzz_one(data, nread, all_exts[i]);
   }

   free(data);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "Usage: %s <file> [file...]\n", argv[0]);
      return 1;
   }

   int failures = 0;
   for (int i = 1; i < argc; i++)
   {
      fprintf(stderr, "  fuzz: %s\n", argv[i]);
      failures += fuzz_file(argv[i]);
   }

   fprintf(stderr, "fuzz_extractors: %d files, %d failures\n", argc - 1, failures);
   return failures > 0 ? 1 : 0;
}
#endif
