/* cmd_index.c: code indexing CLI (scan, overview, find, blast-radius, structure) */
#include "aimee.h"
#include "commands.h"
#include "workspace.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- helpers --- */

/* Register a project path in config.json workspaces (dedup). */
static void register_workspace_path(const char *path)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return;

   for (int i = 0; i < cfg.workspace_count; i++)
   {
      if (strcmp(cfg.workspaces[i], path) == 0)
         return; /* already registered */
   }

   if (cfg.workspace_count >= 64)
      return; /* full */

   snprintf(cfg.workspaces[cfg.workspace_count], MAX_PATH_LEN, "%s", path);
   cfg.workspace_count++;
   config_save(&cfg);
}

/* Resolve the real repo root for a git directory (follows worktree links).
 * For a worktree, git-common-dir points to the main repo's .git dir.
 * Returns 0 on success, -1 on failure. */
static int get_real_repo_root(const char *repo_path, char *out, size_t out_len)
{
   char cmd[MAX_PATH_LEN + 64];
   snprintf(cmd, sizeof(cmd), "git -C \"%s\" rev-parse --git-common-dir 2>/dev/null", repo_path);
   FILE *fp = popen(cmd, "r");
   if (!fp)
      return -1;
   char git_common[MAX_PATH_LEN];
   if (!fgets(git_common, (int)sizeof(git_common), fp))
   {
      pclose(fp);
      return -1;
   }
   pclose(fp);

   /* Strip trailing newline */
   size_t len = strlen(git_common);
   while (len > 0 && (git_common[len - 1] == '\n' || git_common[len - 1] == '\r'))
      git_common[--len] = '\0';

   /* Resolve to absolute path (git-common-dir may be relative) */
   char resolve_base[MAX_PATH_LEN];
   if (git_common[0] == '/')
   {
      snprintf(resolve_base, sizeof(resolve_base), "%s", git_common);
   }
   else
   {
      snprintf(resolve_base, sizeof(resolve_base), "%s/%s", repo_path, git_common);
   }

   char abs[MAX_PATH_LEN];
   if (!realpath(resolve_base, abs))
      return -1;

   /* Parent of .git dir is the repo root */
   char *slash = strrchr(abs, '/');
   if (!slash || slash == abs)
      return -1;
   *slash = '\0';
   snprintf(out, out_len, "%s", abs);
   return 0;
}

/* Discover git repos in a directory (recursively up to depth 10), scan each,
 * and register in config. */
static int discover_and_scan(sqlite3 *db, const char *base_dir, int force, int verbose)
{
   char projects[MAX_DISCOVERED_PROJECTS][MAX_PATH_LEN];
   int count = workspace_discover_projects(base_dir, MAX_WORKSPACE_DEPTH, projects,
                                           MAX_DISCOVERED_PROJECTS);
   if (count < 0)
      return -1;

   for (int i = 0; i < count; i++)
   {
      const char *name = strrchr(projects[i], '/');
      name = name ? name + 1 : projects[i];
      int scanned = index_scan_project(db, name, projects[i], force);
      if (verbose)
         printf("  %-30s %3d file(s) scanned\n", name, scanned);
      register_workspace_path(projects[i]);
   }

   if (count > 0)
      register_workspace_path(base_dir);

   return count;
}

/* --- index subcommand handlers --- */

static void idx_scan(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   /* Parse --force flag */
   int force = 0;
   int pos_argc = 0;
   char *pos_argv[64];
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--force") == 0)
         force = 1;
      else if (pos_argc < 64)
         pos_argv[pos_argc++] = argv[i];
   }

   int verbose = !ctx->json_output;

   if (pos_argc == 0)
   {
      config_t cfg;
      config_load(&cfg);

      int total_found = 0;

      if (cfg.workspace_count > 0)
      {
         /* Scan all configured workspaces */
         for (int w = 0; w < cfg.workspace_count; w++)
         {
            if (verbose)
               printf("==> Scanning workspace: %s\n", cfg.workspaces[w]);
            int found = discover_and_scan(db, cfg.workspaces[w], force, verbose);
            if (found > 0)
               total_found += found;

            /* If in a worktree, also scan the real repo root */
            char real_root[MAX_PATH_LEN];
            if (get_real_repo_root(cfg.workspaces[w], real_root, sizeof(real_root)) == 0 &&
                strcmp(real_root, cfg.workspaces[w]) != 0)
            {
               if (verbose)
                  printf("==> Scanning worktree root: %s\n", real_root);
               found = discover_and_scan(db, real_root, force, verbose);
               if (found > 0)
                  total_found += found;
            }
         }
      }
      else
      {
         /* No workspaces configured: auto-discover in CWD */
         char cwd[MAX_PATH_LEN];
         if (!getcwd(cwd, sizeof(cwd)))
            fatal("cannot get current directory");

         if (verbose)
            printf("==> Scanning: %s\n", cwd);
         total_found = discover_and_scan(db, cwd, force, verbose);

         /* If in a worktree, also scan the real repo root */
         char real_root[MAX_PATH_LEN];
         if (get_real_repo_root(cwd, real_root, sizeof(real_root)) == 0 &&
             strcmp(real_root, cwd) != 0)
         {
            if (verbose)
               printf("==> Scanning worktree root: %s\n", real_root);
            int found = discover_and_scan(db, real_root, force, verbose);
            if (found > 0)
               total_found += found;
         }

         if (total_found == 0)
            fatal("no workspaces configured and no git repos found in %s", cwd);
      }

      if (verbose)
         printf("==> Scan complete: %d project(s)\n", total_found);
   }
   else
   {
      for (int i = 0; i < pos_argc; i += 2)
      {
         if (i + 1 < pos_argc)
         {
            int scanned = index_scan_project(db, pos_argv[i], pos_argv[i + 1], force);
            if (verbose)
               printf("  %-30s %3d file(s) scanned\n", pos_argv[i], scanned);
         }
      }
   }
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
}

static void idx_overview(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   project_info_t projects[64];
   int count = index_list_projects(db, projects, 64);

   static const char *sql = "SELECT (SELECT COUNT(*) FROM files WHERE project_id = p.id),"
                            "       (SELECT COUNT(*) FROM terms t JOIN files f ON f.id = t.file_id "
                            "        WHERE f.project_id = p.id AND t.kind = 'definition')"
                            " FROM projects p WHERE p.name = ?";

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *p = cJSON_CreateObject();
         cJSON_AddStringToObject(p, "name", projects[i].name);
         cJSON_AddStringToObject(p, "root", projects[i].root);
         cJSON_AddStringToObject(p, "scanned_at", projects[i].scanned_at);

         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            sqlite3_bind_text(stmt, 1, projects[i].name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
               cJSON_AddNumberToObject(p, "files", sqlite3_column_int(stmt, 0));
               cJSON_AddNumberToObject(p, "definitions", sqlite3_column_int(stmt, 1));
            }
            sqlite3_reset(stmt);
         }

         cJSON_AddItemToArray(arr, p);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (count == 0)
      {
         printf("No indexed projects.\n");
         return;
      }
      printf("%-30s %6s %6s  %s\n", "PROJECT", "FILES", "DEFS", "ROOT");
      printf("%-30s %6s %6s  %s\n", "-------", "-----", "----", "----");
      for (int i = 0; i < count; i++)
      {
         int files = 0, defs = 0;
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            sqlite3_bind_text(stmt, 1, projects[i].name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW)
            {
               files = sqlite3_column_int(stmt, 0);
               defs = sqlite3_column_int(stmt, 1);
            }
            sqlite3_reset(stmt);
         }
         printf("%-30s %6d %6d  %s\n", projects[i].name, files, defs, projects[i].root);
      }
   }
}

static void idx_find(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("index find requires an identifier");
   term_hit_t hits[128];
   int count = index_find(db, argv[0], hits, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *h = cJSON_CreateObject();
         cJSON_AddStringToObject(h, "project", hits[i].project);
         cJSON_AddStringToObject(h, "file_path", hits[i].file_path);
         cJSON_AddNumberToObject(h, "line", hits[i].line);
         cJSON_AddStringToObject(h, "kind", hits[i].kind);
         cJSON_AddItemToArray(arr, h);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (count == 0)
      {
         printf("No matches for '%s'\n", argv[0]);
         return;
      }
      for (int i = 0; i < count; i++)
      {
         printf("  %s:%d  %-12s [%s]\n", hits[i].file_path, hits[i].line, hits[i].kind,
                hits[i].project);
      }
   }
}

static void idx_blast_radius(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 2)
      fatal("index blast-radius requires project and file");
   blast_radius_t br;
   index_blast_radius(db, argv[0], argv[1], &br);
   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "file", br.file);
      cJSON *deps = cJSON_AddArrayToObject(j, "dependents");
      for (int i = 0; i < br.dependent_count; i++)
         cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependents[i]));
      cJSON *ddeps = cJSON_AddArrayToObject(j, "dependencies");
      for (int i = 0; i < br.dependency_count; i++)
         cJSON_AddItemToArray(ddeps, cJSON_CreateString(br.dependencies[i]));
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      printf("Blast radius for %s:\n", br.file);
      if (br.dependent_count > 0)
      {
         printf("  Dependents (%d):\n", br.dependent_count);
         for (int i = 0; i < br.dependent_count; i++)
            printf("    %s\n", br.dependents[i]);
      }
      if (br.dependency_count > 0)
      {
         printf("  Dependencies (%d):\n", br.dependency_count);
         for (int i = 0; i < br.dependency_count; i++)
            printf("    %s\n", br.dependencies[i]);
      }
      if (br.dependent_count == 0 && br.dependency_count == 0)
         printf("  No dependents or dependencies found.\n");
   }
}

static void idx_structure(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 2)
      fatal("index structure requires project and file");
   definition_t defs[256];
   int count = index_structure(db, argv[0], argv[1], defs, 256);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *d = cJSON_CreateObject();
         cJSON_AddStringToObject(d, "name", defs[i].name);
         cJSON_AddStringToObject(d, "kind", defs[i].kind);
         cJSON_AddNumberToObject(d, "line", defs[i].line);
         cJSON_AddItemToArray(arr, d);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (count == 0)
      {
         printf("No definitions found in %s/%s\n", argv[0], argv[1]);
         return;
      }
      for (int i = 0; i < count; i++)
      {
         printf("  %4d  %-12s %s\n", defs[i].line, defs[i].kind, defs[i].name);
      }
   }
}

static void idx_callers(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   if (argc < 1)
      fatal("index callers requires a symbol name");
   const char *project = argc >= 2 ? argv[1] : NULL;
   caller_hit_t hits[128];
   int count = index_find_callers(db, project, argv[0], hits, 128);
   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < count; i++)
      {
         cJSON *h = cJSON_CreateObject();
         cJSON_AddStringToObject(h, "project", hits[i].project);
         cJSON_AddStringToObject(h, "file_path", hits[i].file_path);
         cJSON_AddStringToObject(h, "caller", hits[i].caller);
         cJSON_AddNumberToObject(h, "line", hits[i].line);
         cJSON_AddItemToArray(arr, h);
      }
      emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      if (count == 0)
      {
         printf("No callers found for '%s'\n", argv[0]);
         return;
      }
      printf("Callers of '%s':\n", argv[0]);
      for (int i = 0; i < count; i++)
      {
         if (hits[i].caller[0])
            printf("  %s:%d in %s() [%s]\n", hits[i].file_path, hits[i].line, hits[i].caller,
                   hits[i].project);
         else
            printf("  %s:%d (file scope) [%s]\n", hits[i].file_path, hits[i].line, hits[i].project);
      }
   }
}

static void idx_placeholder(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;
   (void)argc;
   /* argv[-1] would be the subcommand name, but we receive it via the table.
    * Instead, just emit ok. The caller already matched the name. */
   if (ctx->json_output)
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   (void)argv;
}

/* --- subcommand table --- */

static const subcmd_t index_subcmds[] = {
    {"scan", "Scan a project directory for indexing", idx_scan},
    {"overview", "List all indexed projects", idx_overview},
    {"find", "Find definitions/references by identifier", idx_find},
    {"blast-radius", "Show files affected by changes to a file", idx_blast_radius},
    {"structure", "Show file structure (functions, classes)", idx_structure},
    {"callers", "Find all callers of a symbol", idx_callers},
    {"map", "Show project dependency map", idx_placeholder},
    {"lookup", "Look up a symbol by name", idx_placeholder},
    {"schema", "Show database schema for a project", idx_placeholder},
    {"resolve", "Resolve an import or reference", idx_placeholder},
    {"aliases", "Show identifier aliases", idx_placeholder},
    {NULL, NULL, NULL},
};

const subcmd_t *get_index_subcmds(void)
{
   return index_subcmds;
}

/* --- cmd_index --- */

void cmd_index(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("index", index_subcmds);
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   sqlite3 *db = ctx_db_open_fast(ctx);
   if (!db)
      fatal("cannot open database");

   if (subcmd_dispatch(index_subcmds, sub, ctx, db, argc, argv) != 0)
      fatal("unknown index subcommand: %s", sub);

   ctx_db_close(ctx);
}
