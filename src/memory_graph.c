/* memory_graph.c: entity-relationship graph extraction from memory content */
#include "aimee.h"
#include <ctype.h>

/* --- Entity Edge Extraction --- */

int memory_extract_edges(sqlite3 *db, int64_t window_id, char **file_refs, int file_count,
                         char **terms, int term_count)
{
   if (!db || window_id <= 0)
      return 0;

   int edges_added = 0;

   /* Note: entity_edges has no UNIQUE constraint, so ON CONFLICT
    * will not help. We use a simple approach: just insert and allow
    * duplicates to be handled by weight aggregation later.
    * Instead, check existence first. */

   static const char *check_sql = "SELECT id FROM entity_edges"
                                  " WHERE source = ? AND relation = ? AND target = ?";

   static const char *ins_or_bump_sql = "UPDATE entity_edges SET weight = weight + 1"
                                        " WHERE source = ? AND relation = ? AND target = ?";

   static const char *fresh_sql = "INSERT INTO entity_edges (source, relation, target, weight,"
                                  " window_id)"
                                  " VALUES (?, ?, ?, 1, ?)";

   /* Co-edited pairs: files in the same window */
   for (int i = 0; i < file_count; i++)
   {
      for (int j = i + 1; j < file_count; j++)
      {
         if (!file_refs[i] || !file_refs[j])
            continue;

         /* Check if edge exists */
         sqlite3_stmt *cs = db_prepare(db, check_sql);
         if (!cs)
            continue;

         sqlite3_bind_text(cs, 1, file_refs[i], -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(cs, 2, "co_edited", -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(cs, 3, file_refs[j], -1, SQLITE_TRANSIENT);

         if (sqlite3_step(cs) == SQLITE_ROW)
         {
            sqlite3_reset(cs);
            /* Bump weight */
            sqlite3_stmt *us = db_prepare(db, ins_or_bump_sql);
            if (us)
            {
               sqlite3_bind_text(us, 1, file_refs[i], -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(us, 2, "co_edited", -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(us, 3, file_refs[j], -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(us, "memory_extract_edges");
               sqlite3_reset(us);
            }
         }
         else
         {
            sqlite3_reset(cs);
            /* Fresh insert */
            sqlite3_stmt *fs = db_prepare(db, fresh_sql);
            if (fs)
            {
               sqlite3_bind_text(fs, 1, file_refs[i], -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(fs, 2, "co_edited", -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(fs, 3, file_refs[j], -1, SQLITE_TRANSIENT);
               sqlite3_bind_int64(fs, 4, window_id);
               DB_STEP_LOG(fs, "memory_extract_edges");
               sqlite3_reset(fs);
               edges_added++;
            }
         }
      }
   }

   /* Co-discussed pairs: terms together in the same window.
    * Limit terms to 10 to avoid combinatorial explosion. */
   int max_terms = term_count < 10 ? term_count : 10;

   for (int i = 0; i < max_terms; i++)
   {
      for (int j = i + 1; j < max_terms; j++)
      {
         if (!terms[i] || !terms[j])
            continue;

         /* Skip very short terms */
         if (strlen(terms[i]) < 3 || strlen(terms[j]) < 3)
            continue;

         sqlite3_stmt *cs = db_prepare(db, check_sql);
         if (!cs)
            continue;

         sqlite3_bind_text(cs, 1, terms[i], -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(cs, 2, "co_discussed", -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(cs, 3, terms[j], -1, SQLITE_TRANSIENT);

         if (sqlite3_step(cs) == SQLITE_ROW)
         {
            sqlite3_reset(cs);
            sqlite3_stmt *us = db_prepare(db, ins_or_bump_sql);
            if (us)
            {
               sqlite3_bind_text(us, 1, terms[i], -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(us, 2, "co_discussed", -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(us, 3, terms[j], -1, SQLITE_TRANSIENT);
               DB_STEP_LOG(us, "memory_graph");
               sqlite3_reset(us);
            }
         }
         else
         {
            sqlite3_reset(cs);
            sqlite3_stmt *fs = db_prepare(db, fresh_sql);
            if (fs)
            {
               sqlite3_bind_text(fs, 1, terms[i], -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(fs, 2, "co_discussed", -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(fs, 3, terms[j], -1, SQLITE_TRANSIENT);
               sqlite3_bind_int64(fs, 4, window_id);
               DB_STEP_LOG(fs, "memory_graph");
               sqlite3_reset(fs);
               edges_added++;
            }
         }
      }
   }

   return edges_added;
}

/* --- Query edges --- */

int memory_query_edges(sqlite3 *db, const char *entity, edge_t *out, int max)
{
   static const char *sql = "SELECT id, source, relation, target, weight FROM entity_edges"
                            " WHERE source = ?"
                            " UNION ALL"
                            " SELECT id, source, relation, target, weight FROM entity_edges"
                            " WHERE target = ?"
                            " LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, entity, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, entity, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);

      const char *src = (const char *)sqlite3_column_text(stmt, 1);
      const char *rel = (const char *)sqlite3_column_text(stmt, 2);
      const char *tgt = (const char *)sqlite3_column_text(stmt, 3);

      snprintf(out[count].source, sizeof(out[count].source), "%s", src ? src : "");
      snprintf(out[count].relation, sizeof(out[count].relation), "%s", rel ? rel : "");
      snprintf(out[count].target, sizeof(out[count].target), "%s", tgt ? tgt : "");
      out[count].weight = sqlite3_column_int(stmt, 4);
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

/* --- Graph boost for search ---
 *
 * Traverse edges up to `depth` hops from query_terms,
 * decay weight by 1/(hop+1). Returns a boost map as
 * entity names + scores stored in parallel arrays.
 */

/* boost_map_t and boost_entry_t are defined in memory.h */

static void boost_map_add(boost_map_t *map, const char *entity, double score)
{
   /* Check if entity already in map */
   for (int i = 0; i < map->count; i++)
   {
      if (strcmp(map->entries[i].entity, entity) == 0)
      {
         if (score > map->entries[i].score)
            map->entries[i].score = score;
         return;
      }
   }

   if (map->count >= MAX_BOOST_ENTRIES)
      return;

   snprintf(map->entries[map->count].entity, sizeof(map->entries[0].entity), "%s", entity);
   map->entries[map->count].score = score;
   map->count++;
}

static void graph_boost(sqlite3 *db, char **query_terms, int term_count, int depth,
                        boost_map_t *result)
{
   result->count = 0;

   static const char *edge_sql = "SELECT target, weight FROM entity_edges WHERE source = ?"
                                 " UNION ALL"
                                 " SELECT source, weight FROM entity_edges WHERE target = ?"
                                 " LIMIT 50";

   /* Hop 0: direct neighbors of query terms */
   for (int t = 0; t < term_count; t++)
   {
      sqlite3_stmt *stmt = db_prepare(db, edge_sql);
      if (!stmt)
         continue;

      sqlite3_bind_text(stmt, 1, query_terms[t], -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, query_terms[t], -1, SQLITE_TRANSIENT);

      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *neighbor = (const char *)sqlite3_column_text(stmt, 0);
         int w = sqlite3_column_int(stmt, 1);
         if (!neighbor)
            continue;

         double score = (double)w / 1.0; /* hop 0: decay = 1 */
         boost_map_add(result, neighbor, score);
      }
      sqlite3_reset(stmt);
   }

   /* Hop 1+: traverse from discovered neighbors */
   if (depth >= 2 && result->count > 0)
   {
      int hop0_count = result->count;
      for (int i = 0; i < hop0_count && i < 50; i++)
      {
         sqlite3_stmt *stmt = db_prepare(db, edge_sql);
         if (!stmt)
            continue;

         sqlite3_bind_text(stmt, 1, result->entries[i].entity, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, result->entries[i].entity, -1, SQLITE_TRANSIENT);

         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            const char *neighbor = (const char *)sqlite3_column_text(stmt, 0);
            int w = sqlite3_column_int(stmt, 1);
            if (!neighbor)
               continue;

            double score = (double)w / 2.0; /* hop 1: decay = 1/2 */
            boost_map_add(result, neighbor, score);
         }
         sqlite3_reset(stmt);
      }
   }
}

/* Apply graph boosts to search results by matching summary words
 * and file basenames against the boost map. This is called internally
 * by memory_search; exposed here for potential direct use. */

/* Not exposed in header, but available for linking */
void apply_graph_boost(sqlite3 *db, char **query_terms, int term_count, search_result_t *results,
                       int result_count)
{
   boost_map_t bmap;
   graph_boost(db, query_terms, term_count, 2, &bmap);

   if (bmap.count == 0)
      return;

   for (int r = 0; r < result_count; r++)
   {
      double boost = 0.0;

      /* Match against summary words */
      for (int b = 0; b < bmap.count; b++)
      {
         if (strstr(results[r].summary, bmap.entries[b].entity))
            boost += bmap.entries[b].score * 0.1;
      }

      /* Match against file basenames */
      for (int f = 0; f < results[r].file_count; f++)
      {
         const char *base = strrchr(results[r].files[f], '/');
         if (base)
            base++;
         else
            base = results[r].files[f];

         for (int b = 0; b < bmap.count; b++)
         {
            if (strstr(base, bmap.entries[b].entity))
               boost += bmap.entries[b].score * 0.05;
         }
      }

      results[r].score += boost;
   }
}

/* Public wrapper: compute graph boost map from query terms (2-hop traversal). */
void memory_graph_boost(sqlite3 *db, char **query_terms, int term_count, boost_map_t *out)
{
   graph_boost(db, query_terms, term_count, 2, out);
}

/* --- Graph-powered related memory retrieval ---
 *
 * Given seed memory keys (e.g. from initial retrieval), walk co_discussed
 * edges 1-hop to find related terms, then look up memories whose key or
 * content contains those terms. Returns up to `max` related memories
 * not already in the seed set, scored by edge weight.
 */

int memory_graph_related(sqlite3 *db, char **seed_keys, int seed_count, graph_related_t *out,
                         int max)
{
   if (!db || !seed_keys || seed_count <= 0 || !out || max <= 0)
      return 0;

   /* Collect related terms via 1-hop co_discussed edges from seed keys */
   boost_map_t related;
   related.count = 0;

   static const char *edge_sql = "SELECT target, weight FROM entity_edges"
                                 " WHERE source = ? AND relation = 'co_discussed'"
                                 " UNION ALL"
                                 " SELECT source, weight FROM entity_edges"
                                 " WHERE target = ? AND relation = 'co_discussed'"
                                 " ORDER BY weight DESC LIMIT 20";

   for (int s = 0; s < seed_count && s < 16; s++)
   {
      if (!seed_keys[s])
         continue;

      /* Tokenize the seed key to get individual terms */
      char copy[512];
      snprintf(copy, sizeof(copy), "%s", seed_keys[s]);

      /* Lowercase for matching */
      for (char *p = copy; *p; p++)
         *p = tolower((unsigned char)*p);

      char *saveptr = NULL;
      char *tok = strtok_r(copy, " \t\n_-/.", &saveptr);
      while (tok)
      {
         if (strlen(tok) >= 3)
         {
            sqlite3_stmt *stmt = db_prepare(db, edge_sql);
            if (stmt)
            {
               sqlite3_bind_text(stmt, 1, tok, -1, SQLITE_TRANSIENT);
               sqlite3_bind_text(stmt, 2, tok, -1, SQLITE_TRANSIENT);

               while (sqlite3_step(stmt) == SQLITE_ROW)
               {
                  const char *neighbor = (const char *)sqlite3_column_text(stmt, 0);
                  int w = sqlite3_column_int(stmt, 1);
                  if (neighbor)
                     boost_map_add(&related, neighbor, (double)w);
               }
               sqlite3_reset(stmt);
            }
         }
         tok = strtok_r(NULL, " \t\n_-/.", &saveptr);
      }
   }

   if (related.count == 0)
      return 0;

   /* Look up memories whose key contains any of the related terms */
   int count = 0;
   static const char *mem_sql = "SELECT id, key, content FROM memories"
                                " WHERE (tier = 'L1' OR tier = 'L2')"
                                " AND (key LIKE ? OR content LIKE ?)"
                                " LIMIT 10";

   for (int r = 0; r < related.count && count < max; r++)
   {
      char pattern[256];
      snprintf(pattern, sizeof(pattern), "%%%s%%", related.entries[r].entity);

      sqlite3_stmt *stmt = db_prepare(db, mem_sql);
      if (!stmt)
         continue;

      sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);

      while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
      {
         int64_t id = sqlite3_column_int64(stmt, 0);
         const char *key = (const char *)sqlite3_column_text(stmt, 1);
         const char *content = (const char *)sqlite3_column_text(stmt, 2);

         /* Skip if already in seed set */
         int is_seed = 0;
         for (int s = 0; s < seed_count; s++)
         {
            if (seed_keys[s] && key && strstr(key, seed_keys[s]))
            {
               is_seed = 1;
               break;
            }
         }
         if (is_seed)
            continue;

         /* Skip duplicates in output */
         int dup = 0;
         for (int d = 0; d < count; d++)
         {
            if (out[d].id == id)
            {
               dup = 1;
               break;
            }
         }
         if (dup)
            continue;

         out[count].id = id;
         snprintf(out[count].key, sizeof(out[count].key), "%s", key ? key : "");
         snprintf(out[count].content, sizeof(out[count].content), "%s", content ? content : "");
         out[count].score = related.entries[r].score;
         count++;
      }
      sqlite3_reset(stmt);
   }

   return count;
}

/* --- Graph pruning ---
 *
 * Remove edges where both source and target have no corresponding L1+ memory.
 * This keeps the graph from growing unboundedly as memories expire.
 */

int memory_graph_prune(sqlite3 *db)
{
   if (!db)
      return 0;

   /* Delete edges where neither source nor target appears as a key
    * in any L1 or L2 memory. */
   static const char *sql =
       "DELETE FROM entity_edges WHERE id IN ("
       " SELECT e.id FROM entity_edges e"
       " WHERE NOT EXISTS ("
       "  SELECT 1 FROM memories m WHERE m.tier IN ('L1','L2')"
       "  AND (m.key LIKE '%' || e.source || '%' OR m.content LIKE '%' || e.source || '%')"
       " )"
       " AND NOT EXISTS ("
       "  SELECT 1 FROM memories m WHERE m.tier IN ('L1','L2')"
       "  AND (m.key LIKE '%' || e.target || '%' OR m.content LIKE '%' || e.target || '%')"
       " )"
       ")";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   DB_STEP_LOG(stmt, "memory_graph_prune");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes;
}

/* --- Edge weight normalization ---
 *
 * Normalize weights per relation type so the maximum weight is 1.0.
 * Uses integer scaling: new_weight = (old_weight * 100) / max_weight.
 * Stores as integer (0-100) to avoid floating-point in SQLite.
 */

int memory_graph_normalize(sqlite3 *db)
{
   if (!db)
      return 0;

   static const char *norm_sql = "UPDATE entity_edges SET weight = "
                                 " CAST(weight * 100.0 / "
                                 "  (SELECT MAX(weight) FROM entity_edges e2"
                                 "   WHERE e2.relation = entity_edges.relation)"
                                 " AS INTEGER)"
                                 " WHERE weight > 0"
                                 " AND (SELECT MAX(weight) FROM entity_edges e2"
                                 "      WHERE e2.relation = entity_edges.relation) > 1";

   sqlite3_stmt *stmt = db_prepare(db, norm_sql);
   if (!stmt)
      return 0;

   DB_STEP_LOG(stmt, "memory_graph_normalize");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes;
}
