/* memory_scan.c: conversation scanning, JSONL parsing, window extraction */
#include "aimee.h"
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

/* --- File ref extraction via simple path regex --- */

static int extract_file_refs(const char *text, char **out, int max)
{
   int count = 0;
   const char *p = text;

   while (*p && count < max)
   {
      /* Look for / followed by path chars */
      if (*p == '/' &&
          (p == text || isspace((unsigned char)p[-1]) || p[-1] == '"' || p[-1] == '\''))
      {
         const char *start = p;
         p++;
         while (*p && !isspace((unsigned char)*p) && *p != '"' && *p != '\'' && *p != ')' &&
                *p != ']' && *p != '>')
            p++;

         int len = (int)(p - start);
         if (len > 3 && len < MAX_PATH_LEN)
         {
            /* Must contain a dot (file extension) */
            int has_dot = 0;
            for (const char *d = start; d < p; d++)
            {
               if (*d == '.')
               {
                  has_dot = 1;
                  break;
               }
            }
            if (has_dot)
            {
               out[count] = malloc(len + 1);
               if (out[count])
               {
                  memcpy(out[count], start, len);
                  out[count][len] = '\0';
                  count++;
               }
            }
         }
      }
      else
      {
         p++;
      }
   }
   return count;
}

/* --- Detect compaction markers --- */

static int is_compaction_marker(const char *line)
{
   if (strstr(line, "\"type\":\"summary\""))
      return 1;
   if (strstr(line, "[compacted]"))
      return 1;
   return 0;
}

/* --- Detect Write/Edit tool calls for decisions --- */

static int is_tool_decision(const char *line, char *desc, size_t desc_len)
{
   /* Look for tool_use with Write or Edit */
   const char *tool_use = strstr(line, "\"tool_use\"");
   if (!tool_use)
      return 0;

   int is_write = 0;
   const char *name_pos = strstr(line, "\"name\":");
   if (name_pos)
   {
      if (strstr(name_pos, "\"Write\"") || strstr(name_pos, "\"Edit\""))
         is_write = 1;
   }

   if (!is_write)
      return 0;

   /* Extract file_path if present */
   const char *fp_pos = strstr(line, "\"file_path\":");
   if (fp_pos)
   {
      fp_pos += 13; /* skip past "file_path":" */
      while (*fp_pos == ' ' || *fp_pos == '"')
         fp_pos++;
      const char *end = fp_pos;
      while (*end && *end != '"' && *end != ',')
         end++;
      int len = (int)(end - fp_pos);
      if (len > 0 && len < (int)desc_len - 16)
         snprintf(desc, desc_len, "Write/Edit: %.*s", len, fp_pos);
      else
         snprintf(desc, desc_len, "Write/Edit tool call");
   }
   else
   {
      snprintf(desc, desc_len, "Write/Edit tool call");
   }
   return 1;
}

/* --- Scan a single JSONL file --- */

static int scan_file(sqlite3 *db, const char *session_id, const char *file_path)
{
   /* Get count + max end_line of existing windows for this session */
   int existing_count = 0;
   int max_end_line = 0;

   {
      static const char *sql = "SELECT COUNT(*), COALESCE(MAX(seq), 0)"
                               " FROM windows WHERE session_id = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            existing_count = sqlite3_column_int(stmt, 0);
            max_end_line = sqlite3_column_int(stmt, 1);
         }
         sqlite3_reset(stmt);
      }
   }

   FILE *fp = fopen(file_path, "r");
   if (!fp)
      return -1;

   char *line_buf = malloc(MAX_LINE_LEN);
   if (!line_buf)
   {
      fclose(fp);
      return -1;
   }

   /* Begin transaction */
   sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL);

   int line_num = 0;
   int seq = existing_count;
   int windows_added = 0;

   /* Accumulate a window of ~20 lines */
   char *user_text_parts[64];
   int user_text_count = 0;
   char *file_refs[128];
   int file_ref_count = 0;
   char decision_descs[16][256];
   int decision_count = 0;
   int window_start = max_end_line + 1;

   while (fgets(line_buf, MAX_LINE_LEN, fp))
   {
      line_num++;

      /* Skip already-scanned lines */
      if (line_num <= max_end_line)
         continue;

      /* Skip compaction markers */
      if (is_compaction_marker(line_buf))
         continue;

      /* Extract user text: look for "role":"user" or "role":"human" */
      int is_user = (strstr(line_buf, "\"role\":\"user\"") != NULL ||
                     strstr(line_buf, "\"role\":\"human\"") != NULL);

      if (is_user)
      {
         /* Extract content text */
         const char *cp = strstr(line_buf, "\"content\":");
         if (cp)
         {
            cp += 10;
            while (*cp == ' ' || *cp == '"')
               cp++;
            const char *end = cp;
            while (*end && *end != '"' && (end - cp) < 512)
               end++;
            int len = (int)(end - cp);
            if (len > 0)
            {
               char *part = malloc(len + 1);
               if (part)
               {
                  memcpy(part, cp, len);
                  part[len] = '\0';
                  if (user_text_count < 64)
                     user_text_parts[user_text_count++] = part;
                  else
                     free(part);
               }
            }
         }
      }

      /* Extract file refs from any line */
      {
         char *refs[16];
         int rc = extract_file_refs(line_buf, refs, 16);
         for (int i = 0; i < rc && file_ref_count < 128; i++)
            file_refs[file_ref_count++] = refs[i];
      }

      /* Detect decisions */
      if (decision_count < 16)
      {
         char desc[256];
         if (is_tool_decision(line_buf, desc, sizeof(desc)))
         {
            snprintf(decision_descs[decision_count], 256, "%s", desc);
            decision_count++;
         }
      }

      /* Every ~20 lines or at EOF-ish, flush window */
      if ((line_num - window_start + 1) >= 20 || user_text_count >= 10)
      {
         /* Build user terms from accumulated text */
         char combined[4096];
         int cpos = 0;
         for (int i = 0; i < user_text_count; i++)
         {
            int space = (int)sizeof(combined) - cpos - 2;
            if (space <= 0)
               break;
            int len = (int)strlen(user_text_parts[i]);
            if (len > space)
               len = space;
            if (cpos > 0)
               combined[cpos++] = ' ';
            memcpy(combined + cpos, user_text_parts[i], len);
            cpos += len;
         }
         combined[cpos] = '\0';

         char *terms[128];
         int term_count = tokenize_for_search(combined, terms, 128);

         /* Build summary (first 200 chars of combined) */
         char summary[1024];
         snprintf(summary, sizeof(summary), "%.200s", combined);

         /* INSERT window */
         char ts[32];
         now_utc(ts, sizeof(ts));
         seq++;

         static const char *win_sql = "INSERT INTO windows (session_id, seq, summary,"
                                      " created_at, tier)"
                                      " VALUES (?, ?, ?, ?, 'raw')";
         sqlite3_stmt *ws = db_prepare(db, win_sql);
         int64_t window_id = 0;

         if (ws)
         {
            sqlite3_bind_text(ws, 1, session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ws, 2, seq);
            sqlite3_bind_text(ws, 3, summary, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ws, 4, ts, -1, SQLITE_TRANSIENT);

            if (sqlite3_step(ws) == SQLITE_DONE)
               window_id = sqlite3_last_insert_rowid(db);
            sqlite3_reset(ws);
         }

         if (window_id > 0)
         {
            /* INSERT decisions */
            for (int d = 0; d < decision_count; d++)
            {
               static const char *dsql = "INSERT INTO decisions (window_id, description,"
                                         " created_at) VALUES (?, ?, ?)";
               sqlite3_stmt *ds = db_prepare(db, dsql);
               if (ds)
               {
                  sqlite3_bind_int64(ds, 1, window_id);
                  sqlite3_bind_text(ds, 2, decision_descs[d], -1, SQLITE_TRANSIENT);
                  sqlite3_bind_text(ds, 3, ts, -1, SQLITE_TRANSIENT);
                  DB_STEP_LOG(ds, "memory_scan");
                  sqlite3_reset(ds);
               }
            }

            /* INSERT window_terms */
            static const char *tsql = "INSERT INTO window_terms (window_id, term)"
                                      " VALUES (?, ?)";
            sqlite3_stmt *tst = db_prepare(db, tsql);
            if (tst)
            {
               for (int t = 0; t < term_count; t++)
               {
                  sqlite3_bind_int64(tst, 1, window_id);
                  sqlite3_bind_text(tst, 2, terms[t], -1, SQLITE_TRANSIENT);
                  DB_STEP_LOG(tst, "memory_scan");
                  sqlite3_reset(tst);
               }
            }

            /* INSERT window_files */
            static const char *fsql = "INSERT INTO window_files (window_id, file_path)"
                                      " VALUES (?, ?)";
            sqlite3_stmt *fst = db_prepare(db, fsql);
            if (fst)
            {
               for (int f = 0; f < file_ref_count; f++)
               {
                  sqlite3_bind_int64(fst, 1, window_id);
                  sqlite3_bind_text(fst, 2, file_refs[f], -1, SQLITE_TRANSIENT);
                  DB_STEP_LOG(fst, "memory_scan");
                  sqlite3_reset(fst);
               }
            }

            /* INSERT into window_fts */
            static const char *fts_sql = "INSERT INTO window_fts (rowid, summary)"
                                         " VALUES (?, ?)";
            sqlite3_stmt *fts = db_prepare(db, fts_sql);
            if (fts)
            {
               sqlite3_bind_int64(fts, 1, window_id);
               sqlite3_bind_text(fts, 2, summary, -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(fts, "memory_scan");
               sqlite3_reset(fts);
            }

            /* Extract entity edges */
            memory_extract_edges(db, window_id, file_refs, file_ref_count, terms, term_count);

            windows_added++;
         }

         /* Clean up window state */
         for (int i = 0; i < user_text_count; i++)
            free(user_text_parts[i]);
         user_text_count = 0;

         for (int i = 0; i < term_count; i++)
            free(terms[i]);

         for (int i = 0; i < file_ref_count; i++)
            free(file_refs[i]);
         file_ref_count = 0;

         decision_count = 0;
         window_start = line_num + 1;
      }
   }

   /* Flush remaining partial window */
   if (user_text_count > 0 || file_ref_count > 0)
   {
      char combined[4096];
      int cpos = 0;
      for (int i = 0; i < user_text_count; i++)
      {
         int space = (int)sizeof(combined) - cpos - 2;
         if (space <= 0)
            break;
         int len = (int)strlen(user_text_parts[i]);
         if (len > space)
            len = space;
         if (cpos > 0)
            combined[cpos++] = ' ';
         memcpy(combined + cpos, user_text_parts[i], len);
         cpos += len;
      }
      combined[cpos] = '\0';

      char *terms[128];
      int term_count = tokenize_for_search(combined, terms, 128);

      char summary[1024];
      snprintf(summary, sizeof(summary), "%.200s", combined);

      char ts[32];
      now_utc(ts, sizeof(ts));
      seq++;

      static const char *win_sql = "INSERT INTO windows (session_id, seq, summary,"
                                   " created_at, tier)"
                                   " VALUES (?, ?, ?, ?, 'raw')";
      sqlite3_stmt *ws = db_prepare(db, win_sql);
      int64_t window_id = 0;

      if (ws)
      {
         sqlite3_bind_text(ws, 1, session_id, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(ws, 2, seq);
         sqlite3_bind_text(ws, 3, summary, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(ws, 4, ts, -1, SQLITE_TRANSIENT);

         if (sqlite3_step(ws) == SQLITE_DONE)
            window_id = sqlite3_last_insert_rowid(db);
         sqlite3_reset(ws);
      }

      if (window_id > 0)
      {
         for (int d = 0; d < decision_count; d++)
         {
            static const char *dsql = "INSERT INTO decisions (window_id, description,"
                                      " created_at) VALUES (?, ?, ?)";
            sqlite3_stmt *ds = db_prepare(db, dsql);
            if (ds)
            {
               sqlite3_bind_int64(ds, 1, window_id);
               sqlite3_bind_text(ds, 2, decision_descs[d], -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(ds, 3, ts, -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(ds, "memory_scan");
               sqlite3_reset(ds);
            }
         }

         static const char *tsql = "INSERT INTO window_terms (window_id, term)"
                                   " VALUES (?, ?)";
         sqlite3_stmt *tst = db_prepare(db, tsql);
         if (tst)
         {
            for (int t = 0; t < term_count; t++)
            {
               sqlite3_bind_int64(tst, 1, window_id);
               sqlite3_bind_text(tst, 2, terms[t], -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(tst, "memory_scan");
               sqlite3_reset(tst);
            }
         }

         static const char *fsql = "INSERT INTO window_files (window_id, file_path)"
                                   " VALUES (?, ?)";
         sqlite3_stmt *fst = db_prepare(db, fsql);
         if (fst)
         {
            for (int f = 0; f < file_ref_count; f++)
            {
               sqlite3_bind_int64(fst, 1, window_id);
               sqlite3_bind_text(fst, 2, file_refs[f], -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(fst, "memory_scan");
               sqlite3_reset(fst);
            }
         }

         static const char *fts_sql = "INSERT INTO window_fts (rowid, summary)"
                                      " VALUES (?, ?)";
         sqlite3_stmt *fts = db_prepare(db, fts_sql);
         if (fts)
         {
            sqlite3_bind_int64(fts, 1, window_id);
            sqlite3_bind_text(fts, 2, summary, -1, SQLITE_TRANSIENT);
            DB_STEP_LOG(fts, "memory_scan");
            sqlite3_reset(fts);
         }

         memory_extract_edges(db, window_id, file_refs, file_ref_count, terms, term_count);
         windows_added++;
      }

      for (int i = 0; i < user_text_count; i++)
         free(user_text_parts[i]);
      for (int i = 0; i < term_count; i++)
         free(terms[i]);
      for (int i = 0; i < file_ref_count; i++)
         free(file_refs[i]);
   }

   sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

   free(line_buf);
   fclose(fp);
   return windows_added;
}

/* --- Recursive directory walk for .jsonl files --- */

static void walk_dir(sqlite3 *db, const char *dir_path, int *total_windows)
{
   DIR *dir = opendir(dir_path);
   if (!dir)
      return;

   struct dirent *entry;
   while ((entry = readdir(dir)) != NULL)
   {
      if (entry->d_name[0] == '.')
         continue;

      char full_path[MAX_PATH_LEN];
      snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

      struct stat st;
      if (stat(full_path, &st) != 0)
         continue;

      if (S_ISDIR(st.st_mode))
      {
         walk_dir(db, full_path, total_windows);
         continue;
      }

      if (!S_ISREG(st.st_mode))
         continue;

      /* Check .jsonl extension */
      const char *ext = strrchr(entry->d_name, '.');
      if (!ext || strcmp(ext, ".jsonl") != 0)
         continue;

      /* Use directory name as session_id if possible */
      char session_id[128];
      const char *parent = strrchr(dir_path, '/');
      if (parent)
         snprintf(session_id, sizeof(session_id), "%s", parent + 1);
      else
         snprintf(session_id, sizeof(session_id), "%s", dir_path);

      int added = scan_file(db, session_id, full_path);
      if (added > 0 && total_windows)
         *total_windows += added;
   }

   closedir(dir);
}

int memory_scan_conversations(sqlite3 *db, char dirs[][MAX_PATH_LEN], int dir_count)
{
   int total = 0;

   for (int i = 0; i < dir_count; i++)
   {
      struct stat st;
      if (stat(dirs[i], &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      walk_dir(db, dirs[i], &total);
   }

   return total;
}
