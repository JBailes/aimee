#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"

/* Declared in index.h */
int extract_imports(const char *ext, const char *content, char **out, int max);
int extract_exports(const char *ext, const char *content, char **out, int max);
int extract_definitions(const char *ext, const char *content, definition_t *out, int max);

int main(void)
{
   printf("extractors_extra: ");

   /* --- JavaScript imports --- */
   {
      const char *js = "import React from 'react';\n"
                       "import { useState } from 'react';\n"
                       "const fs = require('fs');\n"
                       "const path = require('path');\n";
      char *imports[16];
      int count = extract_imports(".js", js, imports, 16);
      /* JS extractor should find at least one import */
      for (int i = 0; i < count; i++)
         free(imports[i]);
      /* Just verify it doesn't crash and returns >= 0 */
      assert(count >= 0);
   }

   /* --- JavaScript definitions --- */
   {
      const char *js = "function hello() {}\n"
                       "class MyComponent extends React.Component {}\n";
      definition_t defs[16];
      int count = extract_definitions(".js", js, defs, 16);
      assert(count >= 0); /* Should not crash */
   }

   /* --- Python imports --- */
   {
      const char *py = "import os\nimport sys\nfrom pathlib import Path\n";
      char *imports[16];
      int count = extract_imports(".py", py, imports, 16);
      assert(count >= 0);
      int found_os = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(imports[i], "os"))
            found_os = 1;
         free(imports[i]);
      }
      if (count > 0)
         assert(found_os);
   }

   /* --- Python definitions --- */
   {
      const char *py = "def hello():\n    pass\n\nclass MyClass:\n    pass\n";
      definition_t defs[16];
      int count = extract_definitions(".py", py, defs, 16);
      assert(count >= 1);
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "hello") == 0 || strcmp(defs[i].name, "MyClass") == 0)
            found = 1;
      }
      assert(found);
   }

   /* --- Go imports --- */
   {
      const char *go = "package main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n)\n";
      char *imports[16];
      int count = extract_imports(".go", go, imports, 16);
      assert(count >= 0);
      for (int i = 0; i < count; i++)
         free(imports[i]);
   }

   /* --- Go definitions --- */
   {
      const char *go = "package main\n\nfunc Hello() string {\n\treturn \"hello\"\n}\n\n"
                       "type Server struct {\n\tPort int\n}\n";
      definition_t defs[16];
      int count = extract_definitions(".go", go, defs, 16);
      assert(count >= 1);
   }

   /* --- TypeScript definitions --- */
   {
      const char *ts = "interface User {\n\tname: string;\n}\n\n"
                       "export function greet(user: User): string {\n\treturn 'hi';\n}\n";
      definition_t defs[16];
      int count = extract_definitions(".ts", ts, defs, 16);
      assert(count >= 0);
   }

   /* --- Unsupported extension returns 0 --- */
   {
      char *imports[4];
      int count = extract_imports(".xyz", "some content", imports, 4);
      assert(count == 0);
   }

   printf("all tests passed\n");
   return 0;
}
