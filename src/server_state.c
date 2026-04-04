/* server_state.c: server handlers for memory, index, rules, working memory, dashboard, workspace */
#include "aimee.h"
#include "server.h"
#include "dashboard.h"
#include "workspace.h"
#include "cJSON.h"

/* --- Memory handlers --- */

int handle_memory_search(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jkw = cJSON_GetObjectItemCaseSensitive(req, "keywords");
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");
   int limit = cJSON_IsNumber(jlimit) ? (int)jlimit->valuedouble : 10;

   if (!cJSON_IsArray(jkw) || cJSON_GetArraySize(jkw) == 0)
      return server_send_error(conn, "missing or empty keywords array", NULL);

   int count = cJSON_GetArraySize(jkw);
   if (count > 16)
      count = 16;

   char *clusters[16];
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(jkw, i);
      clusters[i] = cJSON_IsString(item) ? item->valuestring : "";
   }

   /* Build query string for fact search */
   char query_buf[2048];
   int qpos = 0;
   for (int i = 0; i < count; i++)
   {
      if (i > 0)
         qpos += snprintf(query_buf + qpos, sizeof(query_buf) - qpos, " ");
      qpos += snprintf(query_buf + qpos, sizeof(query_buf) - qpos, "%s", clusters[i]);
   }

   /* Search stored facts */
   memory_t facts[32];
   int fact_count = memory_find_facts(conn->db, query_buf, limit, facts, 32);

   /* Search conversation windows */
   search_result_t results[32];
   int found = memory_search(conn->db, clusters, count, limit, results, 32);

   cJSON *farr = cJSON_CreateArray();
   for (int i = 0; i < fact_count; i++)
      cJSON_AddItemToArray(farr, memory_to_json(&facts[i]));

   cJSON *warr = cJSON_CreateArray();
   for (int i = 0; i < found; i++)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "session_id", results[i].session_id);
      cJSON_AddNumberToObject(r, "seq", results[i].seq);
      cJSON_AddStringToObject(r, "summary", results[i].summary);
      cJSON_AddNumberToObject(r, "score", results[i].score);
      cJSON_AddItemToArray(warr, r);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "facts", farr);
   cJSON_AddItemToObject(resp, "windows", warr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_memory_store(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jtier = cJSON_GetObjectItemCaseSensitive(req, "tier");
   cJSON *jkind = cJSON_GetObjectItemCaseSensitive(req, "kind");
   cJSON *jkey = cJSON_GetObjectItemCaseSensitive(req, "key");
   cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(req, "content");
   cJSON *jconf = cJSON_GetObjectItemCaseSensitive(req, "confidence");
   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");

   if (!cJSON_IsString(jkey) || !cJSON_IsString(jcontent))
      return server_send_error(conn, "missing key or content", NULL);

   const char *tier = cJSON_IsString(jtier) ? jtier->valuestring : TIER_L0;
   const char *kind = cJSON_IsString(jkind) ? jkind->valuestring : KIND_FACT;
   double confidence = cJSON_IsNumber(jconf) ? jconf->valuedouble : 1.0;
   const char *sid = cJSON_IsString(jsid) ? jsid->valuestring : "";

   memory_t out;
   int rc = memory_insert(conn->db, tier, kind, jkey->valuestring, jcontent->valuestring,
                          confidence, sid, &out);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddNumberToObject(resp, "id", (double)out.id);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "failed to store memory");
   }
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

int handle_memory_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jtier = cJSON_GetObjectItemCaseSensitive(req, "tier");
   cJSON *jkind = cJSON_GetObjectItemCaseSensitive(req, "kind");
   cJSON *jlimit = cJSON_GetObjectItemCaseSensitive(req, "limit");

   const char *tier = cJSON_IsString(jtier) ? jtier->valuestring : NULL;
   const char *kind = cJSON_IsString(jkind) ? jkind->valuestring : NULL;
   int limit = cJSON_IsNumber(jlimit) ? (int)jlimit->valuedouble : 20;

   memory_t results[64];
   int count = memory_list(conn->db, tier, kind, limit, results, 64);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, memory_to_json(&results[i]));

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "memories", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_memory_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(jid))
      return server_send_error(conn, "missing id", NULL);

   memory_t m;
   int rc = memory_get(conn->db, (int64_t)jid->valuedouble, &m);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON *mj = memory_to_json(&m);
      /* Merge memory fields into resp */
      cJSON *child = mj->child;
      while (child)
      {
         cJSON *next = child->next;
         cJSON_DetachItemViaPointer(mj, child);
         cJSON_AddItemToObject(resp, child->string, child);
         child = next;
      }
      cJSON_Delete(mj);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "memory not found");
   }
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

/* --- Index handlers --- */

int handle_index_find(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "identifier");
   if (!cJSON_IsString(jid))
      return server_send_error(conn, "missing identifier", NULL);

   term_hit_t hits[64];
   int count = index_find(conn->db, jid->valuestring, hits, 64);

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

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "hits", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_index_blast_radius(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jproj = cJSON_GetObjectItemCaseSensitive(req, "project");
   cJSON *jfile = cJSON_GetObjectItemCaseSensitive(req, "file_path");

   if (!cJSON_IsString(jproj) || !cJSON_IsString(jfile))
      return server_send_error(conn, "missing project or file_path", NULL);

   blast_radius_t br;
   int rc = index_blast_radius(conn->db, jproj->valuestring, jfile->valuestring, &br);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "file", br.file);

      cJSON *deps = cJSON_CreateArray();
      for (int i = 0; i < br.dependency_count; i++)
         cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependencies[i]));
      cJSON_AddItemToObject(resp, "dependencies", deps);

      cJSON *depts = cJSON_CreateArray();
      for (int i = 0; i < br.dependent_count; i++)
         cJSON_AddItemToArray(depts, cJSON_CreateString(br.dependents[i]));
      cJSON_AddItemToObject(resp, "dependents", depts);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "blast radius lookup failed");
   }
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

int handle_blast_radius_preview(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jproj = cJSON_GetObjectItemCaseSensitive(req, "project");
   cJSON *jpaths = cJSON_GetObjectItemCaseSensitive(req, "paths");

   if (!cJSON_IsString(jproj) || !cJSON_IsArray(jpaths))
      return server_send_error(conn, "missing project or paths array", NULL);

   int count = cJSON_GetArraySize(jpaths);
   if (count <= 0 || count > 100)
      return server_send_error(conn, "paths must contain 1-100 file paths", NULL);

   char *paths[100];
   for (int i = 0; i < count; i++)
   {
      cJSON *item = cJSON_GetArrayItem(jpaths, i);
      paths[i] = cJSON_IsString(item) ? item->valuestring : "";
   }

   char *json = index_blast_radius_preview(conn->db, jproj->valuestring, paths, count);
   cJSON *resp = cJSON_Parse(json);
   free(json);

   if (!resp)
      resp = cJSON_CreateObject();

   cJSON_AddStringToObject(resp, "status", "ok");
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_index_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   project_info_t projects[32];
   int count = index_list_projects(conn->db, projects, 32);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", projects[i].name);
      cJSON_AddStringToObject(p, "root", projects[i].root);
      cJSON_AddStringToObject(p, "scanned_at", projects[i].scanned_at);
      cJSON_AddItemToArray(arr, p);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "projects", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Rules handlers --- */

int handle_rules_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   rule_t rules[64];
   int count = rules_list(conn->db, rules, 64);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddNumberToObject(r, "id", rules[i].id);
      cJSON_AddStringToObject(r, "title", rules[i].title);
      cJSON_AddStringToObject(r, "polarity", rules[i].polarity);
      cJSON_AddNumberToObject(r, "weight", rules[i].weight);
      cJSON_AddStringToObject(r, "tier", rules_tier(rules[i].weight));
      cJSON_AddItemToArray(arr, r);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "rules", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_rules_generate(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *markdown = rules_generate(conn->db);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "content", markdown ? markdown : "");
   free(markdown);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Working memory handlers --- */

int handle_wm_set(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jkey = cJSON_GetObjectItemCaseSensitive(req, "key");
   cJSON *jval = cJSON_GetObjectItemCaseSensitive(req, "value");
   cJSON *jcat = cJSON_GetObjectItemCaseSensitive(req, "category");
   cJSON *jttl = cJSON_GetObjectItemCaseSensitive(req, "ttl");

   if (!cJSON_IsString(jsid) || !cJSON_IsString(jkey) || !cJSON_IsString(jval))
      return server_send_error(conn, "missing session_id, key, or value", NULL);

   const char *category = cJSON_IsString(jcat) ? jcat->valuestring : "general";
   int ttl = cJSON_IsNumber(jttl) ? (int)jttl->valuedouble : 0;

   int rc =
       wm_set(conn->db, jsid->valuestring, jkey->valuestring, jval->valuestring, category, ttl);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

int handle_wm_get(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jkey = cJSON_GetObjectItemCaseSensitive(req, "key");

   if (!cJSON_IsString(jsid) || !cJSON_IsString(jkey))
      return server_send_error(conn, "missing session_id or key", NULL);

   wm_entry_t entry;
   int rc = wm_get(conn->db, jsid->valuestring, jkey->valuestring, &entry);

   cJSON *resp = cJSON_CreateObject();
   if (rc == 0)
   {
      cJSON_AddStringToObject(resp, "status", "ok");
      cJSON_AddStringToObject(resp, "key", entry.key);
      cJSON_AddStringToObject(resp, "value", entry.value);
      cJSON_AddStringToObject(resp, "category", entry.category);
      cJSON_AddStringToObject(resp, "updated_at", entry.updated_at);
   }
   else
   {
      cJSON_AddStringToObject(resp, "status", "error");
      cJSON_AddStringToObject(resp, "message", "key not found or expired");
   }
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

int handle_wm_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jcat = cJSON_GetObjectItemCaseSensitive(req, "category");

   if (!cJSON_IsString(jsid))
      return server_send_error(conn, "missing session_id", NULL);

   const char *category = cJSON_IsString(jcat) ? jcat->valuestring : NULL;

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = wm_list(conn->db, jsid->valuestring, category, entries, WM_MAX_RESULTS);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      cJSON *e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "key", entries[i].key);
      cJSON_AddStringToObject(e, "value", entries[i].value);
      cJSON_AddStringToObject(e, "category", entries[i].category);
      cJSON_AddStringToObject(e, "updated_at", entries[i].updated_at);
      cJSON_AddItemToArray(arr, e);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "entries", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_wm_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   if (!cJSON_IsString(jsid))
      return server_send_error(conn, "missing session_id", NULL);

   char *context = wm_assemble_context(conn->db, jsid->valuestring);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "context", context ? context : "");
   free(context);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Attempt log handlers --- */

int handle_attempt_record(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jctx = cJSON_GetObjectItemCaseSensitive(req, "task_context");
   cJSON *japp = cJSON_GetObjectItemCaseSensitive(req, "approach");
   cJSON *jout = cJSON_GetObjectItemCaseSensitive(req, "outcome");
   cJSON *jles = cJSON_GetObjectItemCaseSensitive(req, "lesson");

   if (!cJSON_IsString(jsid) || !cJSON_IsString(japp) || !cJSON_IsString(jout))
      return server_send_error(conn, "missing session_id, approach, or outcome", NULL);

   /* Build structured JSON value */
   cJSON *val = cJSON_CreateObject();
   cJSON_AddStringToObject(val, "task_context", cJSON_IsString(jctx) ? jctx->valuestring : "");
   cJSON_AddStringToObject(val, "approach", japp->valuestring);
   cJSON_AddStringToObject(val, "outcome", jout->valuestring);
   cJSON_AddStringToObject(val, "lesson", cJSON_IsString(jles) ? jles->valuestring : "");

   char *json_val = cJSON_PrintUnformatted(val);
   cJSON_Delete(val);
   if (!json_val)
      return server_send_error(conn, "failed to serialize attempt", NULL);

   /* Generate a unique key: attempt:<counter> */
   char key[64];
   static int attempt_counter = 0;
   snprintf(key, sizeof(key), "attempt:%d", ++attempt_counter);

   /* Store with 'attempt' category, 4-hour TTL (session-scoped) */
   int rc = wm_set(conn->db, jsid->valuestring, key, json_val, "attempt", 14400);
   free(json_val);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (rc == 0)
      cJSON_AddStringToObject(resp, "key", key);
   int src = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return src;
}

int handle_attempt_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;

   cJSON *jsid = cJSON_GetObjectItemCaseSensitive(req, "session_id");
   cJSON *jfilter = cJSON_GetObjectItemCaseSensitive(req, "filter");

   if (!cJSON_IsString(jsid))
      return server_send_error(conn, "missing session_id", NULL);

   const char *filter = cJSON_IsString(jfilter) ? jfilter->valuestring : NULL;

   wm_entry_t entries[WM_MAX_RESULTS];
   int count = wm_list(conn->db, jsid->valuestring, "attempt", entries, WM_MAX_RESULTS);

   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < count; i++)
   {
      /* Parse the structured value */
      cJSON *val = cJSON_Parse(entries[i].value);
      if (!val)
         continue;

      /* Apply keyword filter if provided */
      if (filter && filter[0])
      {
         cJSON *jtc = cJSON_GetObjectItemCaseSensitive(val, "task_context");
         cJSON *jap = cJSON_GetObjectItemCaseSensitive(val, "approach");
         const char *tc = cJSON_IsString(jtc) ? jtc->valuestring : "";
         const char *ap = cJSON_IsString(jap) ? jap->valuestring : "";
         if (!strstr(tc, filter) && !strstr(ap, filter))
         {
            cJSON_Delete(val);
            continue;
         }
      }

      cJSON_AddStringToObject(val, "key", entries[i].key);
      cJSON_AddStringToObject(val, "recorded_at", entries[i].created_at);
      cJSON_AddItemToArray(arr, val);
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddItemToObject(resp, "attempts", arr);
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Dashboard handlers --- */

int handle_dashboard_metrics(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json_str = api_metrics(conn->db);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   if (json_str)
   {
      cJSON *data = cJSON_Parse(json_str);
      if (data)
         cJSON_AddItemToObject(resp, "data", data);
      free(json_str);
   }
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

int handle_dashboard_delegations(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   char *json_str = api_delegations(conn->db);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "ok");
   if (json_str)
   {
      cJSON *data = cJSON_Parse(json_str);
      if (data)
         cJSON_AddItemToObject(resp, "data", data);
      free(json_str);
   }
   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}

/* --- Workspace handler --- */

int handle_workspace_context(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   (void)req;

   cJSON *resp = cJSON_CreateObject();

   config_t cfg;
   config_load(&cfg);
   sqlite3 *db = db_open(cfg.db_path);
   char *context = workspace_build_context_from_config(db, &cfg);
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "context", context ? context : "");
   free(context);
   if (db)
   {
      db_stmt_cache_clear();
      db_close(db);
   }

   int rc = server_send_response(conn, resp);
   cJSON_Delete(resp);
   return rc;
}
