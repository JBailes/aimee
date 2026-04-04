/* server_session.c: server-side session management handlers */
#include "aimee.h"
#include "server.h"
#include "platform_random.h"
#include "cJSON.h"
#include <unistd.h>

/* Generate a UUID using platform random */
static void generate_uuid(char *buf, size_t len)
{
   unsigned char raw[16];
   if (platform_random_bytes(raw, sizeof(raw)) != 0)
      memset(raw, 0, sizeof(raw));
   snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10],
            raw[11], raw[12], raw[13], raw[14], raw[15]);
}

int handle_session_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   cJSON *jct = cJSON_GetObjectItemCaseSensitive(req, "client_type");
   const char *client_type = cJSON_IsString(jct) ? jct->valuestring : "cli";

   (void)ctx;

   /* Generate session ID (persisted in DB, not RAM) */
   char sid[64];
   generate_uuid(sid, sizeof(sid));

   /* Build principal from peer UID */
   char principal[32];
   snprintf(principal, sizeof(principal), "uid:%d", (int)conn->peer_uid);

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Insert into DB */
   const char *sql = "INSERT INTO server_sessions (id, client_type, principal, title, "
                     "created_at, last_activity_at) VALUES (?, ?, ?, '', ?, ?)";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
   sqlite3_bind_text(stmt, 1, sid, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, client_type, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, principal, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);

   if (rc != SQLITE_DONE)
      return server_send_error(conn, "failed to create session", NULL);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "session_id", sid);
   cJSON_AddStringToObject(resp, "created_at", ts);
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

int handle_session_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   const char *sql = "SELECT id, client_type, principal, title, created_at, "
                     "last_activity_at, claude_session_id FROM server_sessions "
                     "ORDER BY last_activity_at DESC LIMIT 100";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);

   cJSON *sessions = cJSON_CreateArray();
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "id", (const char *)sqlite3_column_text(stmt, 0));
      cJSON_AddStringToObject(s, "client_type", (const char *)sqlite3_column_text(stmt, 1));
      cJSON_AddStringToObject(s, "principal", (const char *)sqlite3_column_text(stmt, 2));
      cJSON_AddStringToObject(s, "title", (const char *)sqlite3_column_text(stmt, 3));
      cJSON_AddStringToObject(s, "created_at", (const char *)sqlite3_column_text(stmt, 4));
      cJSON_AddStringToObject(s, "last_activity_at", (const char *)sqlite3_column_text(stmt, 5));
      const char *csid = (const char *)sqlite3_column_text(stmt, 6);
      if (csid && csid[0])
         cJSON_AddStringToObject(s, "claude_session_id", csid);
      cJSON_AddItemToArray(sessions, s);
   }
   sqlite3_finalize(stmt);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "sessions", sessions);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_session_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (!cJSON_IsString(jsid))
      return server_send_error(conn, "missing session_id", NULL);

   const char *sql = "SELECT id, client_type, principal, title, created_at, "
                     "last_activity_at, claude_session_id, metadata FROM server_sessions "
                     "WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
   sqlite3_bind_text(stmt, 1, jsid->valuestring, -1, SQLITE_TRANSIENT);

   cJSON *resp = cJSON_CreateObject();
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "id", (const char *)sqlite3_column_text(stmt, 0));
      cJSON_AddStringToObject(resp, "client_type", (const char *)sqlite3_column_text(stmt, 1));
      cJSON_AddStringToObject(resp, "principal", (const char *)sqlite3_column_text(stmt, 2));
      cJSON_AddStringToObject(resp, "title", (const char *)sqlite3_column_text(stmt, 3));
      cJSON_AddStringToObject(resp, "created_at", (const char *)sqlite3_column_text(stmt, 4));
      cJSON_AddStringToObject(resp, "last_activity_at", (const char *)sqlite3_column_text(stmt, 5));
      const char *csid = (const char *)sqlite3_column_text(stmt, 6);
      if (csid && csid[0])
         cJSON_AddStringToObject(resp, "claude_session_id", csid);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "session not found");
   }
   sqlite3_finalize(stmt);

   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_session_close(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (!cJSON_IsString(jsid))
      return server_send_error(conn, "missing session_id", NULL);

   const char *sql = "DELETE FROM server_sessions WHERE id = ?";
   sqlite3_stmt *stmt = NULL;
   sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL);
   sqlite3_bind_text(stmt, 1, jsid->valuestring, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "handle_session_close");
   sqlite3_finalize(stmt);

   int changes = sqlite3_changes(conn->db);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", changes > 0 ? "ok" : "error");
   if (changes == 0)
      cJSON_AddStringToObject(resp, "message", "session not found");
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
