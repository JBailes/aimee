/* index.c: code indexing, project scanning, symbol lookup, blast radius analysis */
#include "aimee.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Directories to skip during scanning */
static const char *skip_dirs[] = {"node_modules",
                                  ".git",
                                  ".worktrees",
                                  "bin",
                                  "obj",
                                  "target",
                                  "build",
                                  "dist",
                                  "out",
                                  "vendor",
                                  ".vs",
                                  ".idea",
                                  ".gradle",
                                  ".venv",
                                  "__pycache__",
                                  ".mypy_cache",
                                  "marked-for-deletion",
                                  NULL};

static int should_skip_dir(const char *name)
{
   for (int i = 0; skip_dirs[i]; i++)
   {
      if (strcmp(name, skip_dirs[i]) == 0)
         return 1;
   }
   return 0;
}

static const char *get_extension(const char *path)
{
   const char *dot = strrchr(path, '.');
   if (!dot || dot == path)
      return "";
   return dot;
}

/* --- Database helpers (normalized schema) --- */

/* Resolve project name to project_id. Returns -1 if not found. */
static int64_t resolve_project_id(sqlite3 *db, const char *name)
{
   static const char *sql = "SELECT id FROM projects WHERE name = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
   int64_t id = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      id = sqlite3_column_int64(stmt, 0);
   sqlite3_reset(stmt);
   return id;
}

/* Resolve file path within a project to file_id. Returns -1 if not found. */
static int64_t resolve_file_id(sqlite3 *db, int64_t project_id, const char *rel_path)
{
   static const char *sql = "SELECT id FROM files WHERE project_id = ? AND path = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, project_id);
   sqlite3_bind_text(stmt, 2, rel_path, -1, SQLITE_TRANSIENT);
   int64_t id = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      id = sqlite3_column_int64(stmt, 0);
   sqlite3_reset(stmt);
   return id;
}

static int upsert_project(sqlite3 *db, const char *name, const char *root)
{
   char ts[32];
   now_utc(ts, sizeof(ts));

   const char *sql = "INSERT INTO projects (name, root, scanned_at)"
                     " VALUES (?, ?, ?)"
                     " ON CONFLICT(name) DO UPDATE SET root = ?, scanned_at = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, root, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, root, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

/* Upsert a file record. Returns the file_id. */
static int64_t upsert_file(sqlite3 *db, int64_t project_id, const char *rel_path,
                           const char *scanned_at)
{
   /* Try insert first */
   static const char *ins_sql = "INSERT INTO files (project_id, path, scanned_at)"
                                " VALUES (?, ?, ?)"
                                " ON CONFLICT(project_id, path) DO UPDATE SET scanned_at = ?";
   sqlite3_stmt *stmt = db_prepare(db, ins_sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, project_id);
   sqlite3_bind_text(stmt, 2, rel_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, scanned_at, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, scanned_at, -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   sqlite3_reset(stmt);
   if (rc != SQLITE_DONE)
      return -1;

   return resolve_file_id(db, project_id, rel_path);
}

static int file_modified_since(sqlite3 *db, int64_t project_id, const char *rel_path, time_t mtime)
{
   static const char *sql = "SELECT scanned_at FROM files"
                            " WHERE project_id = ? AND path = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 1; /* assume modified if we can't check */

   sqlite3_bind_int64(stmt, 1, project_id);
   sqlite3_bind_text(stmt, 2, rel_path, -1, SQLITE_TRANSIENT);

   int modified = 1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *ts = (const char *)sqlite3_column_text(stmt, 0);
      if (ts)
      {
         /* Parse ISO timestamp to time_t (approximate) */
         struct tm tm;
         memset(&tm, 0, sizeof(tm));
         if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                    &tm.tm_min, &tm.tm_sec) == 6)
         {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            time_t scanned = mktime(&tm);
            if (mtime <= scanned)
               modified = 0;
         }
      }
   }
   sqlite3_reset(stmt);
   return modified;
}

static void replace_file_data(sqlite3 *db, int64_t file_id, const char *ext, const char *content)
{
   /* Begin transaction */
   sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

   /* Delete existing data via file_id FK */
   {
      static const char *del_exports = "DELETE FROM file_exports WHERE file_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, del_exports);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }
   }
   {
      static const char *del_imports = "DELETE FROM file_imports WHERE file_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, del_imports);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }
   }
   {
      static const char *del_terms = "DELETE FROM terms WHERE file_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, del_terms);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }
   }
   {
      static const char *del_calls = "DELETE FROM code_calls WHERE file_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, del_calls);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }
   }

   /* Upsert file contents for FTS */
   {
      static const char *upsert = "INSERT OR REPLACE INTO file_contents (file_id, content)"
                                  " VALUES (?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, upsert);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }

      /* Update FTS index */
      static const char *fts_del = "INSERT INTO code_fts(code_fts, rowid, content)"
                                   " VALUES('delete', ?, ?)";
      stmt = db_prepare(db, fts_del);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }
      static const char *fts_ins = "INSERT INTO code_fts(rowid, content) VALUES (?, ?)";
      stmt = db_prepare(db, fts_ins);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
         DB_STEP_LOG(stmt, "replace_file_data");
         sqlite3_reset(stmt);
      }
   }

   /* Insert exports */
   char *exports[128];
   int exp_count = extract_exports(ext, content, exports, 128);
   {
      static const char *ins_exp = "INSERT INTO file_exports (file_id, name) VALUES (?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, ins_exp);
      for (int i = 0; i < exp_count; i++)
      {
         if (stmt)
         {
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, exports[i], -1, SQLITE_TRANSIENT);
            DB_STEP_LOG(stmt, "replace_file_data");
            sqlite3_reset(stmt);
         }
         free(exports[i]);
      }
   }

   /* Insert imports */
   char *imports[128];
   int imp_count = extract_imports(ext, content, imports, 128);
   {
      static const char *ins_imp = "INSERT INTO file_imports (file_id, name) VALUES (?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, ins_imp);
      for (int i = 0; i < imp_count; i++)
      {
         if (stmt)
         {
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, imports[i], -1, SQLITE_TRANSIENT);
            DB_STEP_LOG(stmt, "index");
            sqlite3_reset(stmt);
         }
         free(imports[i]);
      }
   }

   /* Insert routes as terms with kind='route' */
   char *routes[64];
   int route_count = extract_routes(ext, content, routes, 64);
   {
      static const char *ins_route = "INSERT INTO terms (file_id, name, kind, line)"
                                     " VALUES (?, ?, 'route', 0)";
      sqlite3_stmt *stmt = db_prepare(db, ins_route);
      for (int i = 0; i < route_count; i++)
      {
         if (stmt)
         {
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, routes[i], -1, SQLITE_TRANSIENT);
            DB_STEP_LOG(stmt, "index");
            sqlite3_reset(stmt);
         }
         free(routes[i]);
      }
   }

   /* Insert definitions as terms */
   definition_t defs[256];
   int def_count = extract_definitions(ext, content, defs, 256);
   {
      static const char *ins_def = "INSERT INTO terms (file_id, name, kind, line)"
                                   " VALUES (?, ?, ?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, ins_def);
      for (int i = 0; i < def_count; i++)
      {
         if (stmt)
         {
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, defs[i].name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, defs[i].kind, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 4, defs[i].line);
            DB_STEP_LOG(stmt, "index");
            sqlite3_reset(stmt);
         }
      }
   }

   /* Insert call references */
   call_ref_t calls[512];
   int call_count = extract_calls(ext, content, calls, 512);
   {
      static const char *ins_call = "INSERT INTO code_calls (file_id, caller, callee, line)"
                                    " VALUES (?, ?, ?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, ins_call);
      for (int i = 0; i < call_count; i++)
      {
         if (stmt)
         {
            sqlite3_bind_int64(stmt, 1, file_id);
            sqlite3_bind_text(stmt, 2, calls[i].caller, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, calls[i].callee, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 4, calls[i].line);
            DB_STEP_LOG(stmt, "index");
            sqlite3_reset(stmt);
         }
      }
   }

   sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
}

/* Recursive directory walk, collecting file paths */
typedef struct
{
   char **paths;
   int count;
   int max;
} file_list_t;

static void collect_code_files(const char *dir, const char *root, file_list_t *list)
{
   DIR *d = opendir(dir);
   if (!d)
      return;

   struct dirent *entry;
   while ((entry = readdir(d)) != NULL && list->count < list->max)
   {
      if (entry->d_name[0] == '.')
         continue;

      char full[MAX_PATH_LEN];
      snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);

      struct stat st;
      if (lstat(full, &st) != 0)
         continue;

      /* Skip symlinks */
      if (S_ISLNK(st.st_mode))
         continue;

      if (S_ISDIR(st.st_mode))
      {
         if (!should_skip_dir(entry->d_name))
            collect_code_files(full, root, list);
         continue;
      }

      if (!S_ISREG(st.st_mode))
         continue;

      /* Skip files >1MB */
      if (st.st_size > MAX_FILE_SIZE)
         continue;

      /* Check if we have an extractor for this extension */
      const char *ext = get_extension(entry->d_name);
      if (!index_has_extractor(ext))
         continue;

      list->paths[list->count] = strdup(full);
      if (list->paths[list->count])
         list->count++;
   }
   closedir(d);
}

static char *read_file_content(const char *path, size_t *out_len)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (len <= 0 || len > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }

   char *buf = malloc(len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t nread = fread(buf, 1, len, f);
   buf[nread] = '\0';
   fclose(f);

   if (out_len)
      *out_len = nread;
   return buf;
}

int index_scan_project(sqlite3 *db, const char *name, const char *root, int force)
{
   /* Resolve to absolute path */
   char abs_root[MAX_PATH_LEN];
   if (realpath(root, abs_root) == NULL)
      snprintf(abs_root, sizeof(abs_root), "%s", root);

   upsert_project(db, name, abs_root);
   int64_t project_id = resolve_project_id(db, name);
   if (project_id < 0)
      return 0;

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Collect all code files */
   char *paths[4096];
   file_list_t list = {paths, 0, 4096};
   collect_code_files(abs_root, abs_root, &list);

   int scanned = 0;
   for (int i = 0; i < list.count; i++)
   {
      const char *full = list.paths[i];

      /* Compute relative path */
      const char *rel = full + strlen(abs_root);
      if (*rel == '/')
         rel++;

      /* Check mtime vs stored scanned_at */
      struct stat st;
      if (stat(full, &st) != 0)
      {
         free(list.paths[i]);
         continue;
      }

      if (!force && !file_modified_since(db, project_id, rel, st.st_mtime))
      {
         free(list.paths[i]);
         continue;
      }

      /* Read content */
      size_t content_len;
      char *content = read_file_content(full, &content_len);
      if (!content)
      {
         free(list.paths[i]);
         continue;
      }

      /* Upsert file record and get file_id */
      int64_t file_id = upsert_file(db, project_id, rel, ts);
      if (file_id < 0)
      {
         free(content);
         free(list.paths[i]);
         continue;
      }

      /* Extract and replace data */
      const char *ext = get_extension(full);
      replace_file_data(db, file_id, ext, content);

      free(content);
      free(list.paths[i]);
      scanned++;
   }

   return scanned;
}

int index_scan_single_file(sqlite3 *db, const char *project, const char *root,
                           const char *file_path)
{
   int64_t project_id = resolve_project_id(db, project);
   if (project_id < 0)
      return 0;

   /* Compute relative path */
   const char *rel = file_path;
   size_t rlen = strlen(root);
   if (strncmp(file_path, root, rlen) == 0)
   {
      rel = file_path + rlen;
      if (*rel == '/')
         rel++;
   }

   const char *ext = get_extension(file_path);
   if (!index_has_extractor(ext))
      return 0;

   size_t content_len;
   char *content = read_file_content(file_path, &content_len);
   if (!content)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));
   int64_t file_id = upsert_file(db, project_id, rel, ts);
   if (file_id < 0)
   {
      free(content);
      return -1;
   }

   replace_file_data(db, file_id, ext, content);

   free(content);
   return 1;
}

int index_list_projects(sqlite3 *db, project_info_t *out, int max)
{
   const char *sql = "SELECT name, root, scanned_at FROM projects ORDER BY name";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      const char *n = (const char *)sqlite3_column_text(stmt, 0);
      const char *r = (const char *)sqlite3_column_text(stmt, 1);
      const char *s = (const char *)sqlite3_column_text(stmt, 2);
      snprintf(out[count].name, sizeof(out[count].name), "%s", n ? n : "");
      snprintf(out[count].root, sizeof(out[count].root), "%s", r ? r : "");
      snprintf(out[count].scanned_at, sizeof(out[count].scanned_at), "%s", s ? s : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int index_find(sqlite3 *db, const char *identifier, term_hit_t *out, int max)
{
   /* Join terms -> files -> projects to get project name and file path */
   static const char *sql = "SELECT p.name, f.path, t.line, t.kind"
                            " FROM terms t"
                            " JOIN files f ON f.id = t.file_id"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE t.name = ?"
                            " GROUP BY p.name, f.path"
                            " ORDER BY CASE WHEN t.kind = 'definition' THEN 0 ELSE 1 END,"
                            " p.name, f.path"
                            " LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, identifier, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      const char *p = (const char *)sqlite3_column_text(stmt, 0);
      const char *f = (const char *)sqlite3_column_text(stmt, 1);
      int line = sqlite3_column_int(stmt, 2);
      const char *k = (const char *)sqlite3_column_text(stmt, 3);

      snprintf(out[count].project, sizeof(out[count].project), "%s", p ? p : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f ? f : "");
      out[count].line = line;
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int index_blast_radius(sqlite3 *db, const char *project, const char *file_path, blast_radius_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);

   int64_t project_id = resolve_project_id(db, project);
   if (project_id < 0)
      return -1;

   int64_t file_id = resolve_file_id(db, project_id, file_path);

   /* Get exports from this file to match against other files' imports */
   char exports[64][128];
   int export_count = 0;
   if (file_id >= 0)
   {
      static const char *exp_sql = "SELECT name FROM file_exports WHERE file_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, exp_sql);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         while (sqlite3_step(stmt) == SQLITE_ROW && export_count < 64)
         {
            const char *n = (const char *)sqlite3_column_text(stmt, 0);
            if (n)
               snprintf(exports[export_count++], 128, "%s", n);
         }
         sqlite3_reset(stmt);
      }
   }

   /* Find dependents: other files in this project that import any of our exports */
   if (export_count > 0)
   {
      /* Also find files that import by filename pattern */
      static const char *dep_sql = "SELECT DISTINCT f.path FROM file_imports fi"
                                   " JOIN files f ON f.id = fi.file_id"
                                   " WHERE f.project_id = ? AND fi.name LIKE ?";
      sqlite3_stmt *stmt = db_prepare(db, dep_sql);
      if (stmt)
      {
         char pattern[MAX_PATH_LEN];
         snprintf(pattern, sizeof(pattern), "%%%s%%", file_path);
         sqlite3_bind_int64(stmt, 1, project_id);
         sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);

         while (sqlite3_step(stmt) == SQLITE_ROW && out->dependent_count < 64)
         {
            const char *p = (const char *)sqlite3_column_text(stmt, 0);
            if (p && strcmp(p, file_path) != 0)
            {
               snprintf(out->dependents[out->dependent_count], MAX_PATH_LEN, "%s", p);
               out->dependent_count++;
            }
         }
         sqlite3_reset(stmt);
      }
   }

   /* Expand blast radius with co_edited graph edges (weight > 3) */
   {
      static const char *co_sql = "SELECT target FROM entity_edges"
                                  " WHERE source = ? AND relation = 'co_edited' AND weight > 3"
                                  " UNION"
                                  " SELECT source FROM entity_edges"
                                  " WHERE target = ? AND relation = 'co_edited' AND weight > 3"
                                  " LIMIT 16";
      sqlite3_stmt *co_stmt = db_prepare(db, co_sql);
      if (co_stmt)
      {
         /* Extract basename for graph matching */
         const char *base = strrchr(file_path, '/');
         const char *match_name = base ? base + 1 : file_path;

         sqlite3_bind_text(co_stmt, 1, match_name, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(co_stmt, 2, match_name, -1, SQLITE_TRANSIENT);

         while (sqlite3_step(co_stmt) == SQLITE_ROW && out->dependent_count < 64)
         {
            const char *related = (const char *)sqlite3_column_text(co_stmt, 0);
            if (!related || strcmp(related, match_name) == 0)
               continue;

            /* Avoid duplicates */
            int dup = 0;
            for (int d = 0; d < out->dependent_count; d++)
            {
               if (strstr(out->dependents[d], related))
               {
                  dup = 1;
                  break;
               }
            }
            if (!dup)
            {
               snprintf(out->dependents[out->dependent_count], MAX_PATH_LEN, "%s (co-edited)",
                        related);
               out->dependent_count++;
            }
         }
         sqlite3_reset(co_stmt);
      }
   }

   /* Find dependencies: what this file imports */
   if (file_id >= 0)
   {
      static const char *imp_sql = "SELECT DISTINCT name FROM file_imports WHERE file_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, imp_sql);
      if (stmt)
      {
         sqlite3_bind_int64(stmt, 1, file_id);
         while (sqlite3_step(stmt) == SQLITE_ROW && out->dependency_count < 64)
         {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t)
            {
               snprintf(out->dependencies[out->dependency_count], MAX_PATH_LEN, "%s", t);
               out->dependency_count++;
            }
         }
         sqlite3_reset(stmt);
      }
   }

   return 0;
}

int index_structure(sqlite3 *db, const char *project, const char *file_path, definition_t *out,
                    int max)
{
   int64_t project_id = resolve_project_id(db, project);
   if (project_id < 0)
      return 0;

   int64_t file_id = resolve_file_id(db, project_id, file_path);
   if (file_id < 0)
      return 0;

   static const char *sql = "SELECT name, kind, line FROM terms"
                            " WHERE file_id = ? AND kind = 'definition'"
                            " ORDER BY line";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int64(stmt, 1, file_id);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      const char *n = (const char *)sqlite3_column_text(stmt, 0);
      const char *k = (const char *)sqlite3_column_text(stmt, 1);
      int line = sqlite3_column_int(stmt, 2);

      snprintf(out[count].name, sizeof(out[count].name), "%s", n ? n : "");
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", k ? k : "");
      out[count].line = line;
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

/* --- Blast radius preview for multiple files --- */

static const char *severity_label(int dependent_count)
{
   if (dependent_count >= 10)
      return "red";
   if (dependent_count >= 3)
      return "yellow";
   return "green";
}

static int severity_rank(const char *sev)
{
   if (strcmp(sev, "red") == 0)
      return 2;
   if (strcmp(sev, "yellow") == 0)
      return 1;
   return 0;
}

char *index_blast_radius_preview(sqlite3 *db, const char *project, char **paths, int path_count)
{
   cJSON *root = cJSON_CreateObject();
   cJSON *files = cJSON_CreateArray();

   int total_dependents = 0;
   int max_sev = 0;
   int red_count = 0;

   /* Track unique directory prefixes to detect cross-subsystem changes */
   char dirs[20][64];
   int dir_count = 0;

   for (int i = 0; i < path_count; i++)
   {
      blast_radius_t br;
      int rc = index_blast_radius(db, project, paths[i], &br);

      cJSON *fobj = cJSON_CreateObject();
      cJSON_AddStringToObject(fobj, "path", paths[i]);

      if (rc == 0)
      {
         cJSON *deps = cJSON_CreateArray();
         for (int j = 0; j < br.dependent_count; j++)
            cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependents[j]));
         cJSON_AddItemToObject(fobj, "dependents", deps);
         cJSON_AddNumberToObject(fobj, "dependent_count", br.dependent_count);

         const char *sev = severity_label(br.dependent_count);
         cJSON_AddStringToObject(fobj, "severity", sev);

         total_dependents += br.dependent_count;
         int sr = severity_rank(sev);
         if (sr > max_sev)
            max_sev = sr;
         if (sr == 2)
            red_count++;
      }
      else
      {
         cJSON_AddItemToObject(fobj, "dependents", cJSON_CreateArray());
         cJSON_AddNumberToObject(fobj, "dependent_count", 0);
         cJSON_AddStringToObject(fobj, "severity", "green");
      }
      cJSON_AddItemToArray(files, fobj);

      /* Track directory prefix */
      const char *slash = strrchr(paths[i], '/');
      if (slash && dir_count < 20)
      {
         char prefix[64];
         int plen = (int)(slash - paths[i]);
         if (plen > 63)
            plen = 63;
         snprintf(prefix, sizeof(prefix), "%.*s", plen, paths[i]);

         int found = 0;
         for (int d = 0; d < dir_count; d++)
         {
            if (strcmp(dirs[d], prefix) == 0)
            {
               found = 1;
               break;
            }
         }
         if (!found)
            snprintf(dirs[dir_count++], sizeof(dirs[0]), "%s", prefix);
      }
   }

   const char *overall = (max_sev == 2) ? "red" : (max_sev == 1) ? "yellow" : "green";
   cJSON_AddNumberToObject(root, "total_dependents", total_dependents);
   cJSON_AddStringToObject(root, "severity", overall);
   cJSON_AddItemToObject(root, "files", files);

   /* Generate warnings */
   cJSON *warnings = cJSON_CreateArray();
   for (int i = 0; i < path_count; i++)
   {
      blast_radius_t br;
      if (index_blast_radius(db, project, paths[i], &br) == 0 && br.dependent_count > 10)
      {
         char warn[256];
         snprintf(warn, sizeof(warn), "%s has %d dependents, consider splitting the change",
                  paths[i], br.dependent_count);
         cJSON_AddItemToArray(warnings, cJSON_CreateString(warn));
      }
   }
   if (red_count > 1)
      cJSON_AddItemToArray(warnings,
                           cJSON_CreateString("Multiple high-impact files in this change set"));
   if (dir_count > 2)
      cJSON_AddItemToArray(
          warnings, cJSON_CreateString("Changes span multiple subsystems, consider separate PRs"));

   cJSON_AddItemToObject(root, "warnings", warnings);

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json ? json : strdup("{}");
}

int index_find_callers(sqlite3 *db, const char *project, const char *symbol, caller_hit_t *out,
                       int max)
{
   int count = 0;
   static const char *sql = "SELECT p.name, f.path, cc.caller, cc.line"
                            " FROM code_calls cc"
                            " JOIN files f ON f.id = cc.file_id"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE cc.callee = ?"
                            " AND (? IS NULL OR p.name = ?)"
                            " ORDER BY p.name, f.path, cc.line"
                            " LIMIT ?";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;
   sqlite3_bind_text(stmt, 1, symbol, -1, SQLITE_TRANSIENT);
   if (project && project[0])
   {
      sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, project, -1, SQLITE_TRANSIENT);
   }
   else
   {
      sqlite3_bind_null(stmt, 2);
      sqlite3_bind_null(stmt, 3);
   }
   sqlite3_bind_int(stmt, 4, max);

   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      const char *pname = (const char *)sqlite3_column_text(stmt, 0);
      const char *fpath = (const char *)sqlite3_column_text(stmt, 1);
      const char *caller = (const char *)sqlite3_column_text(stmt, 2);
      int line = sqlite3_column_int(stmt, 3);

      snprintf(out[count].project, sizeof(out[count].project), "%s", pname ? pname : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", fpath ? fpath : "");
      snprintf(out[count].caller, sizeof(out[count].caller), "%s", caller ? caller : "");
      out[count].line = line;
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int index_code_search(sqlite3 *db, const char *query, const char *project, code_search_hit_t *out,
                      int max)
{
   if (!query || !query[0])
      return 0;

   int count = 0;

   static const char *sql = "SELECT p.name, f.path,"
                            " snippet(code_fts, 0, '>>>', '<<<', '...', 20),"
                            " rank"
                            " FROM code_fts"
                            " JOIN files f ON f.id = code_fts.rowid"
                            " JOIN projects p ON p.id = f.project_id"
                            " WHERE code_fts MATCH ?"
                            " AND (? IS NULL OR p.name = ?)"
                            " ORDER BY rank"
                            " LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
   if (project && project[0])
   {
      sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, project, -1, SQLITE_TRANSIENT);
   }
   else
   {
      sqlite3_bind_null(stmt, 2);
      sqlite3_bind_null(stmt, 3);
   }
   sqlite3_bind_int(stmt, 4, max);

   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      const char *pname = (const char *)sqlite3_column_text(stmt, 0);
      const char *fpath = (const char *)sqlite3_column_text(stmt, 1);
      const char *snip = (const char *)sqlite3_column_text(stmt, 2);
      double rnk = sqlite3_column_double(stmt, 3);

      snprintf(out[count].project, sizeof(out[count].project), "%s", pname ? pname : "");
      snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", fpath ? fpath : "");
      snprintf(out[count].snippet, sizeof(out[count].snippet), "%s", snip ? snip : "");
      out[count].rank = rnk;
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}
