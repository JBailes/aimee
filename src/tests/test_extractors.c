#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "extractors_extra.h"

/* --- C extractor tests --- */

static void test_c_imports(void)
{
   char *imports[16];
   memset(imports, 0, sizeof(imports));
   import_ctx_t ic = {imports, 0, 16, 0};

   c_import_line("#include \"foo.h\"", 1, &ic);
   assert(ic.count == 1);
   assert(strcmp(imports[0], "foo.h") == 0);

   c_import_line("#include \"bar/baz.h\"", 2, &ic);
   assert(ic.count == 2);
   assert(strcmp(imports[1], "bar/baz.h") == 0);

   /* System includes should be skipped */
   c_import_line("#include <stdio.h>", 3, &ic);
   assert(ic.count == 2);

   c_import_line("#include <stdlib.h>", 4, &ic);
   assert(ic.count == 2);

   /* Non-include lines ignored */
   c_import_line("int x = 5;", 5, &ic);
   assert(ic.count == 2);

   c_import_line("// #include \"commented.h\"", 6, &ic);
   assert(ic.count == 2);

   for (int i = 0; i < ic.count; i++)
      free(imports[i]);
}

static void test_c_exports(void)
{
   char *exports[16];
   memset(exports, 0, sizeof(exports));
   export_ctx_t ec = {exports, 0, 16};

   /* Non-static function declaration */
   c_export_line("int agent_execute(sqlite3 *db, const agent_t *agent);", 1, &ec);
   assert(ec.count == 1);
   assert(strcmp(exports[0], "agent_execute") == 0);

   /* Static should be skipped */
   c_export_line("static void helper(void);", 2, &ec);
   assert(ec.count == 1);

   /* Struct declaration */
   c_export_line("struct agent_t {", 3, &ec);
   assert(ec.count == 2);
   assert(strcmp(exports[1], "agent_t") == 0);

   /* Enum declaration */
   c_export_line("enum color { RED, GREEN };", 4, &ec);
   assert(ec.count == 3);
   assert(strcmp(exports[2], "color") == 0);

   /* Typedef */
   c_export_line("typedef struct { int x; } point_t;", 5, &ec);
   assert(ec.count == 4);
   assert(strcmp(exports[3], "point_t") == 0);

   /* Preprocessor lines skipped */
   c_export_line("#define MAX 100", 6, &ec);
   assert(ec.count == 4);

   for (int i = 0; i < ec.count; i++)
      free(exports[i]);
}

static void test_c_definitions(void)
{
   definition_t defs[16];
   memset(defs, 0, sizeof(defs));
   c_def_ctx_t dc = {defs, 0, 16, 0};

   /* Function at indent 0 */
   c_def_line("int main(int argc, char **argv)", 1, &dc);
   assert(dc.count == 1);
   assert(strcmp(defs[0].name, "main") == 0);
   assert(defs[0].line == 1);

   /* Static function still indexed */
   c_def_line("static void helper(void)", 5, &dc);
   assert(dc.count == 2);
   assert(strcmp(defs[1].name, "helper") == 0);
   assert(defs[1].line == 5);

   /* #define macro */
   c_def_line("#define MAX_SIZE 1024", 10, &dc);
   assert(dc.count == 3);
   assert(strcmp(defs[2].name, "MAX_SIZE") == 0);
   assert(defs[2].line == 10);

   /* Struct */
   c_def_line("struct agent_t {", 15, &dc);
   assert(dc.count == 4);
   assert(strcmp(defs[3].name, "agent_t") == 0);

   /* Enum */
   c_def_line("enum color { RED, GREEN };", 20, &dc);
   assert(dc.count == 5);
   assert(strcmp(defs[4].name, "color") == 0);

   /* Typedef */
   c_def_line("typedef unsigned int uint32_t;", 25, &dc);
   assert(dc.count == 6);
   assert(strcmp(defs[5].name, "uint32_t") == 0);

   /* Indented lines skipped (inside function body) */
   c_def_line("   int x = 5;", 30, &dc);
   assert(dc.count == 6);

   /* Block comment tracking */
   c_def_line("/* start of comment", 35, &dc);
   assert(dc.in_block_comment == 1);
   c_def_line("int should_be_skipped(void)", 36, &dc);
   assert(dc.count == 6);
   c_def_line("end of comment */", 37, &dc);
   assert(dc.in_block_comment == 0);

   /* After comment, normal parsing resumes */
   c_def_line("void *after_comment(int x)", 38, &dc);
   assert(dc.count == 7);
   assert(strcmp(defs[6].name, "after_comment") == 0);
}

/* --- Lua extractor tests --- */

static void test_lua_imports(void)
{
   char *imports[16];
   memset(imports, 0, sizeof(imports));
   import_ctx_t ic = {imports, 0, 16, 0};

   /* require with double quotes and parens */
   lua_import_line("require(\"socket\")", 1, &ic);
   assert(ic.count == 1);
   assert(strcmp(imports[0], "socket") == 0);

   /* require with single quotes */
   lua_import_line("require('json')", 2, &ic);
   assert(ic.count == 2);
   assert(strcmp(imports[1], "json") == 0);

   /* require without parens */
   lua_import_line("require \"lpeg\"", 3, &ic);
   assert(ic.count == 3);
   assert(strcmp(imports[2], "lpeg") == 0);

   /* local assignment with require */
   lua_import_line("local http = require(\"http\")", 4, &ic);
   assert(ic.count == 4);
   assert(strcmp(imports[3], "http") == 0);

   /* Non-require lines ignored */
   lua_import_line("local x = 5", 5, &ic);
   assert(ic.count == 4);

   lua_import_line("print(\"hello\")", 6, &ic);
   assert(ic.count == 4);

   for (int i = 0; i < ic.count; i++)
      free(imports[i]);
}

static void test_lua_exports(void)
{
   char *exports[16];
   memset(exports, 0, sizeof(exports));
   export_ctx_t ec = {exports, 0, 16};

   /* Module method with dot */
   lua_export_line("function M.connect(host, port)", 1, &ec);
   assert(ec.count == 1);
   assert(strcmp(exports[0], "connect") == 0);

   /* Module method with colon */
   lua_export_line("function M:close()", 2, &ec);
   assert(ec.count == 2);
   assert(strcmp(exports[1], "close") == 0);

   /* Plain function is NOT an export */
   lua_export_line("function helper(x)", 3, &ec);
   assert(ec.count == 2);

   /* Local function is NOT an export */
   lua_export_line("local function internal()", 4, &ec);
   assert(ec.count == 2);

   for (int i = 0; i < ec.count; i++)
      free(exports[i]);
}

static void test_lua_definitions(void)
{
   definition_t defs[16];
   memset(defs, 0, sizeof(defs));
   def_ctx_t dc = {defs, 0, 16, 0};

   /* Global function */
   lua_def_line("function global_func(x, y)", 1, &dc);
   assert(dc.count == 1);
   assert(strcmp(defs[0].name, "global_func") == 0);
   assert(defs[0].line == 1);

   /* Local function */
   lua_def_line("local function helper(a)", 5, &dc);
   assert(dc.count == 2);
   assert(strcmp(defs[1].name, "helper") == 0);
   assert(defs[1].line == 5);

   /* Module method with dot */
   lua_def_line("function M.connect(host)", 10, &dc);
   assert(dc.count == 3);
   assert(strcmp(defs[2].name, "M.connect") == 0);
   assert(defs[2].line == 10);

   /* Module method with colon */
   lua_def_line("function M:close()", 15, &dc);
   assert(dc.count == 4);
   assert(strcmp(defs[3].name, "M:close") == 0);
   assert(defs[3].line == 15);

   /* Non-function lines ignored */
   lua_def_line("local x = 5", 20, &dc);
   assert(dc.count == 4);

   lua_def_line("return M", 25, &dc);
   assert(dc.count == 4);
}

int main(void)
{
   test_c_imports();
   test_c_exports();
   test_c_definitions();
   test_lua_imports();
   test_lua_exports();
   test_lua_definitions();
   printf("extractors: all tests passed\n");
   return 0;
}
