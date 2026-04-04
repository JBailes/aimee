#define _GNU_SOURCE
/* cmd_work.c: inter-session work queue for distributing tasks across sessions */
#include "aimee.h"
#include "commands.h"
#include "workspace.h"
#include "platform_random.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Log a work queue state transition to the audit trail. */
static void wq_log(sqlite3 *db, const char *item_id, const char *old_status, const char *new_status,
                   const char *detail)
{
   char ts[32];
   now_utc(ts, sizeof(ts));
   static const char *sql = "INSERT INTO work_queue_log"
                            " (item_id, old_status, new_status, session_id, detail, created_at)"
                            " VALUES (?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_bind_text(stmt, 1, item_id ? item_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, old_status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, new_status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, session_id(), -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, detail ? detail : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "wq_log");
   sqlite3_reset(stmt);
}

/* Generate a short random ID */
static void generate_work_id(char *buf, size_t len)
{
   unsigned char raw[8];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      memset(raw, 0, sizeof(raw));
   snprintf(buf, len, "%02x%02x%02x%02x%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3], raw[4],
            raw[5], raw[6], raw[7]);
}

/* --- work add --- */

static void work_add(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *title = opt_pos(&opts, 0);
   if (!title)
      fatal("usage: aimee work add \"title\" [--desc \"...\"] [--source \"...\"] [--priority N]");

   const char *desc = opt_get(&opts, "desc");
   const char *source = opt_get(&opts, "source");
   int priority = opt_get_int(&opts, "priority", 0);

   char id[32];
   generate_work_id(id, sizeof(id));

   char ts[32];
   now_utc(ts, sizeof(ts));

   const char *sid = session_id();

   const char *sql = "INSERT INTO work_queue (id, title, description, source, priority, "
                     "status, created_by, created_at) VALUES (?, ?, ?, ?, ?, 'pending', ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      fatal("failed to prepare insert");
   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, desc ? desc : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, source ? source : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, priority);
   sqlite3_bind_text(stmt, 6, sid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 7, ts, -1, SQLITE_TRANSIENT);

   if (sqlite3_step(stmt) != SQLITE_DONE)
      fatal("failed to insert work item: %s", sqlite3_errmsg(db));

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "id", id);
      cJSON_AddStringToObject(obj, "title", title);
      cJSON_AddStringToObject(obj, "status", "pending");
      char *json = cJSON_PrintUnformatted(obj);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(obj);
   }
   else
   {
      printf("Added work item %s: %s\n", id, title);
   }
}

/* --- work add-batch --- */

static void work_add_batch(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {"from-proposals", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   if (!opt_has(&opts, "from-proposals"))
      fatal("usage: aimee work add-batch --from-proposals [--dir path]");

   const char *dir = opt_get(&opts, "dir");

   /* Default: docs/proposals/pending/ relative to cwd */
   char default_dir[MAX_PATH_LEN];
   if (!dir)
   {
      char cwd[MAX_PATH_LEN];
      if (!getcwd(cwd, sizeof(cwd)))
         fatal("cannot get cwd");
      snprintf(default_dir, sizeof(default_dir), "%s/docs/proposals/pending", cwd);
      dir = default_dir;
   }

   DIR *d = opendir(dir);
   if (!d)
      fatal("cannot open proposals directory: %s", dir);

   const char *sid = session_id();
   char ts[32];
   now_utc(ts, sizeof(ts));

   int added = 0;
   int skipped = 0;
   struct dirent *ent;

   while ((ent = readdir(d)) != NULL)
   {
      size_t namelen = strlen(ent->d_name);
      if (namelen < 4 || strcmp(ent->d_name + namelen - 3, ".md") != 0)
         continue;

      /* Build source identifier with relative path */
      char source[512];
      {
         char cwd_buf[MAX_PATH_LEN];
         char abs_dir_buf[MAX_PATH_LEN];
         const char *rel_path = dir;
         if (getcwd(cwd_buf, sizeof(cwd_buf)) && realpath(dir, abs_dir_buf))
         {
            size_t cwdlen = strlen(cwd_buf);
            if (strncmp(abs_dir_buf, cwd_buf, cwdlen) == 0 && abs_dir_buf[cwdlen] == '/')
            {
               rel_path = abs_dir_buf + cwdlen + 1;
            }
         }
         snprintf(source, sizeof(source), "proposal:%s/%s", rel_path, ent->d_name);
      }

      /* Check if already queued (pending, claimed, or done) */
      const char *check_sql = "SELECT COUNT(*) FROM work_queue "
                              "WHERE source = ? AND status IN ('pending', 'claimed', 'done')";
      sqlite3_stmt *check = db_prepare(db, check_sql);
      if (check)
      {
         sqlite3_bind_text(check, 1, source, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(check) == SQLITE_ROW && sqlite3_column_int(check, 0) > 0)
         {
            skipped++;
            sqlite3_reset(check);
            continue;
         }
         sqlite3_reset(check);
      }

      /* Check if proposal exists in done/ or rejected/ directories */
      {
         char proposals_root[MAX_PATH_LEN];
         snprintf(proposals_root, sizeof(proposals_root), "%s", dir);
         char *last_slash = strrchr(proposals_root, '/');
         if (last_slash)
         {
            *last_slash = '\0';
            char check_path[MAX_PATH_LEN];
            struct stat st;

            snprintf(check_path, sizeof(check_path), "%s/done/%s", proposals_root, ent->d_name);
            if (stat(check_path, &st) == 0)
            {
               skipped++;
               continue;
            }

            snprintf(check_path, sizeof(check_path), "%s/rejected/%s", proposals_root, ent->d_name);
            if (stat(check_path, &st) == 0)
            {
               skipped++;
               continue;
            }
         }
      }

      /* Extract title, effort, and priority from proposal */
      char filepath[MAX_PATH_LEN];
      snprintf(filepath, sizeof(filepath), "%s/%s", dir, ent->d_name);

      char title[256];
      char effort[8] = "";
      int priority = 0;
      title[0] = '\0';
      FILE *fp = fopen(filepath, "r");
      if (fp)
      {
         char line[512];
         while (fgets(line, sizeof(line), fp))
         {
            if (!title[0] && strncmp(line, "# ", 2) == 0)
            {
               /* Trim "# Proposal: " prefix if present */
               const char *t = line + 2;
               if (strncmp(t, "Proposal: ", 10) == 0)
                  t += 10;
               size_t tlen = strlen(t);
               while (tlen > 0 && (t[tlen - 1] == '\n' || t[tlen - 1] == '\r'))
                  tlen--;
               if (tlen >= sizeof(title))
                  tlen = sizeof(title) - 1;
               memcpy(title, t, tlen);
               title[tlen] = '\0';
            }

            /* Extract effort: **Effort:** S or **Effort:** M-L */
            if (!effort[0])
            {
               const char *ep = strstr(line, "**Effort:**");
               if (!ep)
                  ep = strstr(line, "**Effort:**");
               if (ep)
               {
                  ep += 11;
                  while (*ep == ' ')
                     ep++;
                  if (*ep == 'S' || *ep == 's')
                     snprintf(effort, sizeof(effort), "S");
                  else if (*ep == 'M' || *ep == 'm')
                     snprintf(effort, sizeof(effort), "M");
                  else if (*ep == 'L' || *ep == 'l')
                     snprintf(effort, sizeof(effort), "L");
                  else if (*ep == 'X' || *ep == 'x')
                     snprintf(effort, sizeof(effort), "XL");
               }
            }

            /* Extract priority: P0=30, P1=20, P2=10, P3=0 */
            if (priority == 0)
            {
               const char *pp = strstr(line, "| P0");
               if (pp)
                  priority = 30;
               else if ((pp = strstr(line, "| P1")) != NULL)
                  priority = 20;
               else if ((pp = strstr(line, "| P2")) != NULL)
                  priority = 10;
            }
         }
         fclose(fp);
      }
      if (!title[0])
      {
         /* Fallback: use filename without .md */
         snprintf(title, sizeof(title), "%.*s", (int)(namelen - 3), ent->d_name);
      }

      /* Insert */
      char id[32];
      generate_work_id(id, sizeof(id));

      const char *sql = "INSERT INTO work_queue (id, title, description, source, priority, "
                        "status, created_by, created_at, effort) "
                        "VALUES (?, ?, ?, ?, ?, 'pending', ?, ?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         continue;
      sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, source, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 5, priority);
      sqlite3_bind_text(stmt, 6, sid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 7, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 8, effort, -1, SQLITE_TRANSIENT);

      if (sqlite3_step(stmt) == SQLITE_DONE)
         added++;
      sqlite3_reset(stmt);
   }
   closedir(d);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "added", added);
      cJSON_AddNumberToObject(obj, "skipped", skipped);
      char *json = cJSON_PrintUnformatted(obj);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(obj);
   }
   else
   {
      printf("Added %d work items from proposals (%d already queued).\n", added, skipped);
   }
}

/* --- work claim --- */

static void work_claim(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *specific_id = opt_get(&opts, "id");
   const char *effort_filter = opt_get(&opts, "effort");
   const char *tag_filter = opt_get(&opts, "tag");
   const char *exclude_tag = opt_get(&opts, "exclude-tag");
   int skip = opt_get_int(&opts, "skip", 0);
   const char *sid = session_id();
   char ts[32];
   now_utc(ts, sizeof(ts));

   const char *sql;
   sqlite3_stmt *stmt;

   if (specific_id)
   {
      sql = "UPDATE work_queue SET status = 'claimed', claimed_by = ?, claimed_at = ? "
            "WHERE id = ? AND status = 'pending' "
            "RETURNING id, title, description, source, priority";
      stmt = db_prepare(db, sql);
      if (!stmt)
         fatal("failed to prepare claim");
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, specific_id, -1, SQLITE_TRANSIENT);
   }
   else
   {
      /* Build dynamic query with optional filters */
      char filter_sql[1024];
      int foff = 0;
      foff += snprintf(filter_sql + foff, sizeof(filter_sql) - (size_t)foff,
                       "SELECT id FROM work_queue WHERE status = 'pending'");
      if (effort_filter)
         foff += snprintf(filter_sql + foff, sizeof(filter_sql) - (size_t)foff, " AND effort = ?3");
      if (tag_filter)
         foff += snprintf(filter_sql + foff, sizeof(filter_sql) - (size_t)foff,
                          " AND (',' || tags || ',') LIKE '%%,' || ?4 || ',%%'");
      if (exclude_tag)
         foff += snprintf(filter_sql + foff, sizeof(filter_sql) - (size_t)foff,
                          " AND (',' || tags || ',') NOT LIKE '%%,' || ?5 || ',%%'");
      snprintf(filter_sql + foff, sizeof(filter_sql) - (size_t)foff,
               " ORDER BY priority DESC, created_at ASC LIMIT 1 OFFSET ?6");

      char full_sql[2048];
      snprintf(full_sql, sizeof(full_sql),
               "UPDATE work_queue SET status = 'claimed', claimed_by = ?1, claimed_at = ?2 "
               "WHERE id = (%s) AND status = 'pending' "
               "RETURNING id, title, description, source, priority",
               filter_sql);

      stmt = NULL;
      if (sqlite3_prepare_v2(db, full_sql, -1, &stmt, NULL) != SQLITE_OK)
         fatal("failed to prepare claim");
      sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      if (effort_filter)
         sqlite3_bind_text(stmt, 3, effort_filter, -1, SQLITE_TRANSIENT);
      if (tag_filter)
         sqlite3_bind_text(stmt, 4, tag_filter, -1, SQLITE_TRANSIENT);
      if (exclude_tag)
         sqlite3_bind_text(stmt, 5, exclude_tag, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 6, skip);
   }

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
   {
      const char *id = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *desc = (const char *)sqlite3_column_text(stmt, 2);
      const char *source = (const char *)sqlite3_column_text(stmt, 3);
      int priority = sqlite3_column_int(stmt, 4);

      wq_log(db, id, "pending", "claimed", NULL);

      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "id", id ? id : "");
         cJSON_AddStringToObject(obj, "title", title ? title : "");
         cJSON_AddStringToObject(obj, "description", desc ? desc : "");
         cJSON_AddStringToObject(obj, "source", source ? source : "");
         cJSON_AddNumberToObject(obj, "priority", priority);
         cJSON_AddStringToObject(obj, "status", "claimed");
         cJSON_AddStringToObject(obj, "claimed_by", sid);
         char *json = cJSON_PrintUnformatted(obj);
         printf("%s\n", json);
         free(json);
         cJSON_Delete(obj);
      }
      else
      {
         printf("Claimed: [%s] %s\n", id ? id : "", title ? title : "");
         if (source && source[0])
            printf("  Source: %s\n", source);
         if (desc && desc[0])
            printf("  Description: %s\n", desc);
      }
   }
   else
   {
      if (ctx->json_output)
         printf("{\"claimed\":false,\"message\":\"No pending work items\"}\n");
      else
         printf("No pending work items to claim.\n");
   }
   sqlite3_reset(stmt);
}

/* --- work complete / fail --- */

static void work_finish(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv, const char *new_status)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *specific_id = opt_get(&opts, "id");
   const char *result = opt_get(&opts, "result");
   const char *sid = session_id();
   char ts[32];
   now_utc(ts, sizeof(ts));

   const char *sql;
   sqlite3_stmt *stmt;

   if (specific_id)
   {
      sql = "UPDATE work_queue SET status = ?, completed_at = ?, result = ? "
            "WHERE id = ? AND claimed_by = ? AND status = 'claimed' "
            "RETURNING id, title";
      stmt = db_prepare(db, sql);
      if (!stmt)
         fatal("failed to prepare update");
      sqlite3_bind_text(stmt, 1, new_status, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, result ? result : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, specific_id, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 5, sid, -1, SQLITE_TRANSIENT);
   }
   else
   {
      /* Complete the item claimed by this session (most recently claimed) */
      sql = "UPDATE work_queue SET status = ?1, completed_at = ?2, result = ?3 "
            "WHERE id = ("
            "  SELECT id FROM work_queue WHERE claimed_by = ?4 AND status = 'claimed' "
            "  ORDER BY claimed_at DESC LIMIT 1"
            ") AND status = 'claimed' "
            "RETURNING id, title";
      stmt = db_prepare(db, sql);
      if (!stmt)
         fatal("failed to prepare update");
      sqlite3_bind_text(stmt, 1, new_status, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, result ? result : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, sid, -1, SQLITE_TRANSIENT);
   }

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
   {
      const char *id = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);

      wq_log(db, id, "claimed", new_status, result);

      if (ctx->json_output)
      {
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "id", id ? id : "");
         cJSON_AddStringToObject(obj, "title", title ? title : "");
         cJSON_AddStringToObject(obj, "status", new_status);
         char *json = cJSON_PrintUnformatted(obj);
         printf("%s\n", json);
         free(json);
         cJSON_Delete(obj);
      }
      else
      {
         printf("Marked %s: [%s] %s\n", new_status, id ? id : "", title ? title : "");
      }
   }
   else
   {
      if (ctx->json_output)
         printf("{\"updated\":false,\"message\":\"No matching claimed item found\"}\n");
      else
         printf("No matching claimed work item found for this session.\n");
   }
   sqlite3_reset(stmt);
}

static void work_complete(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   work_finish(ctx, db, argc, argv, "done");
}

static void work_fail(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   work_finish(ctx, db, argc, argv, "failed");
}

/* --- work list --- */

static void work_list(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *status_filter = opt_get(&opts, "status");

   const char *sql;
   sqlite3_stmt *stmt;

   if (status_filter && strcmp(status_filter, "all") == 0)
   {
      sql = "SELECT id, title, source, status, claimed_by, result, created_at, claimed_at "
            "FROM work_queue ORDER BY "
            "CASE status WHEN 'claimed' THEN 0 WHEN 'pending' THEN 1 "
            "WHEN 'done' THEN 2 ELSE 3 END, priority DESC, created_at ASC";
      stmt = db_prepare(db, sql);
   }
   else if (status_filter)
   {
      sql = "SELECT id, title, source, status, claimed_by, result, created_at, claimed_at "
            "FROM work_queue WHERE status = ? "
            "ORDER BY priority DESC, created_at ASC";
      stmt = db_prepare(db, sql);
      if (stmt)
         sqlite3_bind_text(stmt, 1, status_filter, -1, SQLITE_TRANSIENT);
   }
   else
   {
      /* Default: pending + claimed */
      sql = "SELECT id, title, source, status, claimed_by, result, created_at, claimed_at "
            "FROM work_queue WHERE status IN ('pending', 'claimed') "
            "ORDER BY CASE status WHEN 'claimed' THEN 0 ELSE 1 END, "
            "priority DESC, created_at ASC";
      stmt = db_prepare(db, sql);
   }

   if (!stmt)
      fatal("failed to prepare list query");

   if (ctx->json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         cJSON *obj = cJSON_CreateObject();
         const char *id = (const char *)sqlite3_column_text(stmt, 0);
         const char *title = (const char *)sqlite3_column_text(stmt, 1);
         const char *source = (const char *)sqlite3_column_text(stmt, 2);
         const char *status = (const char *)sqlite3_column_text(stmt, 3);
         const char *claimed = (const char *)sqlite3_column_text(stmt, 4);
         const char *result = (const char *)sqlite3_column_text(stmt, 5);
         cJSON_AddStringToObject(obj, "id", id ? id : "");
         cJSON_AddStringToObject(obj, "title", title ? title : "");
         cJSON_AddStringToObject(obj, "source", source ? source : "");
         cJSON_AddStringToObject(obj, "status", status ? status : "");
         if (claimed && claimed[0])
            cJSON_AddStringToObject(obj, "claimed_by", claimed);
         if (result && result[0])
            cJSON_AddStringToObject(obj, "result", result);
         cJSON_AddItemToArray(arr, obj);
      }
      char *json = cJSON_Print(arr);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(arr);
   }
   else
   {
      int count = 0;
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *id = (const char *)sqlite3_column_text(stmt, 0);
         const char *title = (const char *)sqlite3_column_text(stmt, 1);
         const char *source = (const char *)sqlite3_column_text(stmt, 2);
         const char *status = (const char *)sqlite3_column_text(stmt, 3);
         const char *claimed = (const char *)sqlite3_column_text(stmt, 4);
         const char *result = (const char *)sqlite3_column_text(stmt, 5);

         const char *claimed_at = (const char *)sqlite3_column_text(stmt, 7);
         printf("[%s] %-10s %s", id ? id : "", status ? status : "", title ? title : "");
         if (claimed && claimed[0])
            printf("  (session: %.8s)", claimed);
         if (claimed_at && claimed_at[0] && status && strcmp(status, "claimed") == 0)
         {
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            if (strptime(claimed_at, "%Y-%m-%dT%H:%M:%S", &tm))
            {
               time_t ct = timegm(&tm);
               time_t now = time(NULL);
               int age_min = (int)((now - ct) / 60);
               if (age_min < 60)
                  printf("  (%dm ago)", age_min);
               else
                  printf("  (%dh%dm ago)", age_min / 60, age_min % 60);
            }
         }
         if (source && source[0])
            printf("  [%s]", source);
         if (result && result[0])
            printf("  -> %s", result);
         printf("\n");
         count++;
      }
      if (count == 0)
         printf("No work items found.\n");
   }
   sqlite3_reset(stmt);
}

/* --- work cancel --- */

static void work_cancel(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *id = opt_get(&opts, "id");
   if (!id)
      fatal("usage: aimee work cancel --id ITEM_ID");

   const char *sql = "UPDATE work_queue SET status = 'cancelled' "
                     "WHERE id = ? AND status = 'pending' RETURNING id, title";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      fatal("failed to prepare cancel");
   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
   {
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      wq_log(db, id, "pending", "cancelled", NULL);
      printf("Cancelled: [%s] %s\n", id, title ? title : "");
   }
   else
   {
      printf("No pending item with id %s found.\n", id);
   }
   sqlite3_reset(stmt);
}

/* --- work release --- */

static void work_release(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *id = opt_get(&opts, "id");
   if (!id)
      fatal("usage: aimee work release --id ITEM_ID");

   const char *sql =
       "UPDATE work_queue SET status = 'pending', claimed_by = NULL, claimed_at = NULL "
       "WHERE id = ? AND status = 'claimed' RETURNING id, title";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      fatal("failed to prepare release");
   sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_ROW)
   {
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      wq_log(db, id, "claimed", "pending", "released");
      printf("Released: [%s] %s\n", id, title ? title : "");
   }
   else
   {
      printf("No claimed item with id %s found.\n", id);
   }
   sqlite3_reset(stmt);
}

/* --- work clear --- */

static void work_clear(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   const char *status = opt_get(&opts, "status");
   if (!status)
      fatal("usage: aimee work clear --status done|failed|cancelled");

   /* Log each item before deletion */
   {
      const char *sel = "SELECT id FROM work_queue WHERE status = ?";
      sqlite3_stmt *sel_stmt = db_prepare(db, sel);
      if (sel_stmt)
      {
         sqlite3_bind_text(sel_stmt, 1, status, -1, SQLITE_TRANSIENT);
         while (sqlite3_step(sel_stmt) == SQLITE_ROW)
         {
            const char *item_id = (const char *)sqlite3_column_text(sel_stmt, 0);
            wq_log(db, item_id, status, "cleared", NULL);
         }
         sqlite3_reset(sel_stmt);
      }
   }

   const char *sql = "DELETE FROM work_queue WHERE status = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      fatal("failed to prepare delete");
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);

   DB_STEP_LOG(stmt, "work_clear");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);

   printf("Cleared %d %s items.\n", changes, status);
}

/* --- work gc --- */

static void work_gc(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)ctx;
   static const char *bool_flags[] = {NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);

   int max_age_hours = opt_get_int(&opts, "max-age", 2);

   char cutoff[32];
   {
      time_t now = time(NULL);
      time_t threshold = now - (max_age_hours * 3600);
      struct tm tm;
      gmtime_r(&threshold, &tm);
      strftime(cutoff, sizeof(cutoff), "%Y-%m-%dT%H:%M:%SZ", &tm);
   }

   const char *sql =
       "UPDATE work_queue SET status = 'pending', claimed_by = NULL, claimed_at = NULL "
       "WHERE status = 'claimed' AND claimed_at < ? "
       "RETURNING id, title";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      fatal("failed to prepare gc");
   sqlite3_bind_text(stmt, 1, cutoff, -1, SQLITE_TRANSIENT);

   int released = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *id = (const char *)sqlite3_column_text(stmt, 0);
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      wq_log(db, id, "claimed", "pending", "gc: stale claim released");
      printf("Released stale claim: [%s] %s\n", id ? id : "", title ? title : "");
      released++;
   }
   sqlite3_reset(stmt);

   if (released == 0)
      printf("No stale claims found (threshold: %d hours).\n", max_age_hours);
   else
      printf("Released %d stale claims.\n", released);
}

/* --- work stats --- */

static void work_stats(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   /* Count items by status from current queue */
   int pending = 0, claimed = 0, done = 0, failed = 0, cancelled = 0;
   {
      const char *sql = "SELECT status, COUNT(*) FROM work_queue GROUP BY status";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            const char *s = (const char *)sqlite3_column_text(stmt, 0);
            int c = sqlite3_column_int(stmt, 1);
            if (!s)
               continue;
            if (strcmp(s, "pending") == 0)
               pending = c;
            else if (strcmp(s, "claimed") == 0)
               claimed = c;
            else if (strcmp(s, "done") == 0)
               done = c;
            else if (strcmp(s, "failed") == 0)
               failed = c;
            else if (strcmp(s, "cancelled") == 0)
               cancelled = c;
         }
         sqlite3_reset(stmt);
      }
   }

   /* Count completed items and avg completion time from audit log */
   int log_completed = 0;
   double avg_minutes = 0;
   {
      const char *sql =
          "SELECT COUNT(*), AVG("
          "  (julianday(done_log.created_at) - julianday(claim_log.created_at)) * 24 * 60"
          ") FROM work_queue_log done_log"
          " JOIN work_queue_log claim_log"
          "   ON claim_log.item_id = done_log.item_id"
          "   AND claim_log.new_status = 'claimed'"
          " WHERE done_log.new_status = 'done'";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            log_completed = sqlite3_column_int(stmt, 0);
            if (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
               avg_minutes = sqlite3_column_double(stmt, 1);
         }
         sqlite3_reset(stmt);
      }
   }

   int total = pending + claimed + done + failed + cancelled;

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddNumberToObject(obj, "total", total);
      cJSON_AddNumberToObject(obj, "pending", pending);
      cJSON_AddNumberToObject(obj, "claimed", claimed);
      cJSON_AddNumberToObject(obj, "done", done);
      cJSON_AddNumberToObject(obj, "failed", failed);
      cJSON_AddNumberToObject(obj, "cancelled", cancelled);
      cJSON_AddNumberToObject(obj, "completed_with_timing", log_completed);
      if (log_completed > 0)
         cJSON_AddNumberToObject(obj, "avg_completion_minutes", avg_minutes);
      char *json = cJSON_PrintUnformatted(obj);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(obj);
   }
   else
   {
      printf("Queue stats:\n");
      printf("  Total items: %d\n", total);
      printf("  Pending:     %d\n", pending);
      printf("  Claimed:     %d\n", claimed);
      printf("  Completed:   %d", done);
      if (log_completed > 0)
         printf(" (avg %.0fm)", avg_minutes);
      printf("\n");
      printf("  Failed:      %d\n", failed);
      printf("  Cancelled:   %d\n", cancelled);
   }
}

/* --- Subcmd table --- */

static const subcmd_t work_subcmds[] = {
    {"add", "Add a work item to the queue", work_add},
    {"add-batch", "Batch-add items (--from-proposals)", work_add_batch},
    {"claim", "Claim the next pending item", work_claim},
    {"complete", "Mark claimed item as done", work_complete},
    {"fail", "Mark claimed item as failed", work_fail},
    {"list", "List work items", work_list},
    {"cancel", "Cancel a pending item", work_cancel},
    {"release", "Release a claimed item back to pending", work_release},
    {"clear", "Remove items by status", work_clear},
    {"gc", "Release stale claims (--max-age N hours, default 2)", work_gc},
    {"stats", "Show queue statistics and throughput", work_stats},
    {NULL, NULL, NULL},
};

const subcmd_t *get_work_subcmds(void)
{
   return work_subcmds;
}

/* --- work queue summary (for session-start context) --- */

int work_queue_summary(sqlite3 *db, char *buf, size_t cap)
{
   int pending = 0, claimed = 0;

   const char *sql = "SELECT status, COUNT(*) FROM work_queue "
                     "WHERE status IN ('pending', 'claimed') GROUP BY status";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *s = (const char *)sqlite3_column_text(stmt, 0);
      int c = sqlite3_column_int(stmt, 1);
      if (s && strcmp(s, "pending") == 0)
         pending = c;
      else if (s && strcmp(s, "claimed") == 0)
         claimed = c;
   }
   sqlite3_reset(stmt);

   if (pending == 0 && claimed == 0)
      return 0;

   int len = snprintf(buf, cap,
                      "# Work Queue\n"
                      "There are %d pending items in the work queue. "
                      "Run `aimee work claim` to pick one up.\n",
                      pending);
   if (claimed > 0 && (size_t)len < cap)
      len += snprintf(buf + len, cap - (size_t)len,
                      "Currently claimed by other sessions: %d items.\n", claimed);

   return len;
}

/* --- Main entry point --- */

void cmd_work(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("work", work_subcmds);
      return;
   }

   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");

   subcmd_dispatch(work_subcmds, argv[0], ctx, db, argc - 1, argv + 1);
   ctx_db_close(ctx);
}
