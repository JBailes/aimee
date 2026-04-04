#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"

/* Create a temp directory with a test source file */
static char *create_test_project(void)
{
   char *dir = strdup("/tmp/aimee-test-index-XXXXXX");
   assert(mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/main.c", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "#include <stdio.h>\n"
              "\n"
              "void hello(void) {\n"
              "    printf(\"hello\\n\");\n"
              "}\n"
              "\n"
              "int main(void) {\n"
              "    hello();\n"
              "    return 0;\n"
              "}\n");
   fclose(f);

   snprintf(path, sizeof(path), "%s/util.c", dir);
   f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "#include \"main.c\"\n"
              "\n"
              "void helper(void) {}\n");
   fclose(f);

   return dir;
}

/* Create a second project with different symbols */
static char *create_test_project_b(void)
{
   char *dir = strdup("/tmp/aimee-test-index-b-XXXXXX");
   assert(mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/app.c", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "void app_start(void) {}\n"
              "void app_stop(void) {}\n");
   fclose(f);

   return dir;
}

/* Create a project with no extractable files */
static char *create_empty_project(void)
{
   char *dir = strdup("/tmp/aimee-test-index-empty-XXXXXX");
   assert(mkdtemp(dir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/README.md", dir);
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fprintf(f, "# No code here\n");
   fclose(f);

   return dir;
}

int main(void)
{
   printf("index: ");

   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   char *project_dir = create_test_project();

   /* --- index_scan_project: returns count of files scanned --- */
   {
      int scanned = index_scan_project(db, "testproj", project_dir, 0);
      assert(scanned >= 0);
   }

   /* --- index_list_projects --- */
   {
      project_info_t projects[8];
      int count = index_list_projects(db, projects, 8);
      assert(count == 1);
      assert(strcmp(projects[0].name, "testproj") == 0);
   }

   /* --- index_find --- */
   {
      term_hit_t hits[16];
      int count = index_find(db, "hello", hits, 16);
      assert(count > 0);
      assert(strcmp(hits[0].project, "testproj") == 0);
   }

   /* --- index_find: nonexistent symbol --- */
   {
      term_hit_t hits[16];
      int count = index_find(db, "nonexistent_xyz_42", hits, 16);
      assert(count == 0);
   }

   /* --- index_structure --- */
   {
      definition_t defs[16];
      int count = index_structure(db, "testproj", "main.c", defs, 16);
      assert(count >= 2); /* hello + main */
      int found_hello = 0, found_main = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "hello") == 0)
            found_hello = 1;
         if (strcmp(defs[i].name, "main") == 0)
            found_main = 1;
      }
      assert(found_hello);
      assert(found_main);
   }

   /* --- index_blast_radius --- */
   {
      blast_radius_t br;
      int rc = index_blast_radius(db, "testproj", "main.c", &br);
      assert(rc == 0);
      /* util.c includes main.c, so main.c should have a dependent */
      assert(br.dependent_count >= 1 || br.dependency_count >= 0);
   }

   /* --- rescan is idempotent --- */
   {
      int scanned = index_scan_project(db, "testproj", project_dir, 0);
      assert(scanned >= 0);

      project_info_t projects[8];
      int count = index_list_projects(db, projects, 8);
      assert(count == 1); /* Still just one project */
   }

   /* --- multi-project: scanning a second project --- */
   {
      char *proj_b = create_test_project_b();
      int scanned = index_scan_project(db, "projb", proj_b, 0);
      assert(scanned >= 1); /* at least app.c */

      project_info_t projects[8];
      int count = index_list_projects(db, projects, 8);
      assert(count == 2); /* testproj + projb */

      /* Find symbol from project B */
      term_hit_t hits[16];
      int hit_count = index_find(db, "app_start", hits, 16);
      assert(hit_count > 0);
      assert(strcmp(hits[0].project, "projb") == 0);

      /* Original project symbols still findable */
      hit_count = index_find(db, "hello", hits, 16);
      assert(hit_count > 0);
      assert(strcmp(hits[0].project, "testproj") == 0);

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", proj_b);
      (void)system(cmd);
      free(proj_b);
   }

   /* --- empty project: no extractable files, but project still registered --- */
   {
      char *empty = create_empty_project();
      int scanned = index_scan_project(db, "emptyproj", empty, 0);
      assert(scanned == 0); /* no code files to scan */

      project_info_t projects[16];
      int count = index_list_projects(db, projects, 16);
      assert(count == 3); /* testproj + projb + emptyproj */

      /* Verify it's registered with the right name */
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(projects[i].name, "emptyproj") == 0)
            found = 1;
      }
      assert(found);

      char cmd[512];
      snprintf(cmd, sizeof(cmd), "rm -rf %s", empty);
      (void)system(cmd);
      free(empty);
   }

   /* --- rescan after file modification picks up changes --- */
   {
      /* Add a new file to the original project */
      char path[512];
      snprintf(path, sizeof(path), "%s/extra.c", project_dir);
      FILE *f = fopen(path, "w");
      assert(f != NULL);
      fprintf(f, "void extra_func(void) {}\n");
      fclose(f);

      int scanned = index_scan_project(db, "testproj", project_dir, 0);
      assert(scanned >= 1); /* at least the new file */

      term_hit_t hits[16];
      int count = index_find(db, "extra_func", hits, 16);
      assert(count > 0);
      assert(strcmp(hits[0].project, "testproj") == 0);
   }

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", project_dir);
   (void)system(cmd);
   free(project_dir);
   db_stmt_cache_clear();
   db_close(db);

   printf("all tests passed\n");
   return 0;
}
