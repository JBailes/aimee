/* memory_context.c: window compaction, context assembly, context cache */
#include "aimee.h"
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <strings.h>

/* --- Window Compaction --- */

/* Check if a window's session has any L2 memories (high-value indicator) */
static int window_has_l2_link(sqlite3 *db, int64_t window_id)
{
   static const char *sql = "SELECT COUNT(*) FROM memories WHERE tier = 'L2'"
                            " AND source_session = (SELECT session_id FROM windows WHERE id = ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;
   sqlite3_bind_int64(stmt, 1, window_id);
   int count = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      count = sqlite3_column_int(stmt, 0);
   sqlite3_reset(stmt);
   return count > 0;
}

/* Compute quality-scaled term retention count for a window */
static int compute_keep_terms(sqlite3 *db, int64_t window_id, int base_terms)
{
   if (window_has_l2_link(db, window_id))
      return base_terms * 2; /* 2x for L2-linked windows */
   return base_terms;
}

int memory_compact_windows(sqlite3 *db, int *summary_count, int *fact_count)
{
   int sc = 0, fc = 0;

   /* raw -> summary (>30 days): prune terms by quality-scaled limit */
   {
      static const char *sql = "SELECT id FROM windows"
                               " WHERE tier = 'raw'"
                               " AND created_at < datetime('now', '-' || ? || ' days')";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         char days[8];
         snprintf(days, sizeof(days), "%d", SUMMARY_AGE);
         sqlite3_bind_text(stmt, 1, days, -1, SQLITE_TRANSIENT);

         int64_t ids[500];
         int id_count = 0;

         while (sqlite3_step(stmt) == SQLITE_ROW && id_count < 500)
            ids[id_count++] = sqlite3_column_int64(stmt, 0);
         sqlite3_reset(stmt);

         /* Term deletion: keep top N terms ordered by length (longer = more specific) */
         static const char *del_terms_sql = "DELETE FROM window_terms WHERE window_id = ?"
                                            " AND rowid NOT IN"
                                            " (SELECT rowid FROM window_terms"
                                            "  WHERE window_id = ?"
                                            "  ORDER BY LENGTH(term) DESC, term LIMIT ?)";
         sqlite3_stmt *del_st = db_prepare(db, del_terms_sql);
         static const char *upd = "UPDATE windows SET tier = 'summary' WHERE id = ?";
         sqlite3_stmt *us = db_prepare(db, upd);

         for (int i = 0; i < id_count; i++)
         {
            int keep = compute_keep_terms(db, ids[i], COMPACT_BASE_TERMS_SUMMARY);

            if (del_st)
            {
               sqlite3_bind_int64(del_st, 1, ids[i]);
               sqlite3_bind_int64(del_st, 2, ids[i]);
               sqlite3_bind_int(del_st, 3, keep);
               DB_STEP_LOG(del_st, "memory_compact_windows");
               sqlite3_reset(del_st);
            }

            if (us)
            {
               sqlite3_bind_int64(us, 1, ids[i]);
               DB_STEP_LOG(us, "memory_compact_windows");
               sqlite3_reset(us);
            }
            sc++;
         }
      }
   }

   /* summary -> fact (>90 days): prune terms, conditionally preserve file refs */
   {
      static const char *sql = "SELECT id FROM windows"
                               " WHERE tier = 'summary'"
                               " AND created_at < datetime('now', '-' || ? || ' days')";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         char days[8];
         snprintf(days, sizeof(days), "%d", FACT_AGE);
         sqlite3_bind_text(stmt, 1, days, -1, SQLITE_TRANSIENT);

         int64_t ids[500];
         int id_count = 0;

         while (sqlite3_step(stmt) == SQLITE_ROW && id_count < 500)
            ids[id_count++] = sqlite3_column_int64(stmt, 0);
         sqlite3_reset(stmt);

         /* Term deletion: keep top N by length */
         static const char *del_terms_sql = "DELETE FROM window_terms WHERE window_id = ?"
                                            " AND rowid NOT IN"
                                            " (SELECT rowid FROM window_terms"
                                            "  WHERE window_id = ?"
                                            "  ORDER BY LENGTH(term) DESC, term LIMIT ?)";
         sqlite3_stmt *dt = db_prepare(db, del_terms_sql);

         /* Delete all file refs (non-L2 windows) */
         static const char *del_files_all_sql = "DELETE FROM window_files WHERE window_id = ?";
         sqlite3_stmt *dfa = db_prepare(db, del_files_all_sql);

         /* Delete file refs except top N (L2-linked windows) */
         static const char *del_files_keep_sql =
             "DELETE FROM window_files WHERE window_id = ?"
             " AND rowid NOT IN"
             " (SELECT rowid FROM window_files WHERE window_id = ?"
             "  ORDER BY rowid LIMIT ?)";
         sqlite3_stmt *dfk = db_prepare(db, del_files_keep_sql);

         static const char *upd = "UPDATE windows SET tier = 'fact' WHERE id = ?";
         sqlite3_stmt *us = db_prepare(db, upd);

         for (int i = 0; i < id_count; i++)
         {
            int keep = compute_keep_terms(db, ids[i], COMPACT_BASE_TERMS_FACT);
            int is_l2 = window_has_l2_link(db, ids[i]);

            /* Keep top N terms */
            if (dt)
            {
               sqlite3_bind_int64(dt, 1, ids[i]);
               sqlite3_bind_int64(dt, 2, ids[i]);
               sqlite3_bind_int(dt, 3, keep);
               DB_STEP_LOG(dt, "memory_context");
               sqlite3_reset(dt);
            }

            /* File refs: preserve top 3 for L2-linked, delete all for others */
            if (is_l2 && dfk)
            {
               sqlite3_bind_int64(dfk, 1, ids[i]);
               sqlite3_bind_int64(dfk, 2, ids[i]);
               sqlite3_bind_int(dfk, 3, COMPACT_FILE_REFS_KEEP);
               DB_STEP_LOG(dfk, "memory_context");
               sqlite3_reset(dfk);
            }
            else if (dfa)
            {
               sqlite3_bind_int64(dfa, 1, ids[i]);
               DB_STEP_LOG(dfa, "memory_context");
               sqlite3_reset(dfa);
            }

            if (us)
            {
               sqlite3_bind_int64(us, 1, ids[i]);
               DB_STEP_LOG(us, "memory_context");
               sqlite3_reset(us);
            }
            fc++;
         }
      }
   }

   if (summary_count)
      *summary_count = sc;
   if (fact_count)
      *fact_count = fc;
   return 0;
}

/* --- Context Assembly --- */

/* --- Retrieval Planner --- */

/* Keyword lists per intent */
static const char *debug_keywords[] = {"fix",   "bug",      "error",     "fail",  "broken",
                                       "crash", "debug",    "issue",     "wrong", "stacktrace",
                                       "panic", "segfault", "exception", NULL};
static const char *plan_keywords[] = {"plan",   "design", "architect", "propose", "new", "feature",
                                      "create", "build",  "implement", "add",     NULL};
static const char *review_keywords[] = {"review", "check", "audit",   "convention", "style",
                                        "verify", "lint",  "inspect", NULL};
static const char *deploy_keywords[] = {"deploy",  "release", "ship",    "rollout", "migrate",
                                        "upgrade", "install", "publish", NULL};

static int count_keyword_matches(const char *text, const char **keywords)
{
   int count = 0;
   for (int i = 0; keywords[i]; i++)
   {
      /* Case-insensitive substring search */
      const char *p = text;
      int klen = (int)strlen(keywords[i]);
      while (*p)
      {
         if (strncasecmp(p, keywords[i], klen) == 0)
         {
            /* Check word boundary: previous char must not be alpha */
            if (p == text || !isalpha((unsigned char)p[-1]))
            {
               count++;
               break;
            }
         }
         p++;
      }
   }
   return count;
}

task_intent_t classify_intent(const char *task_hint)
{
   if (!task_hint || !task_hint[0])
      return INTENT_GENERAL;

   int scores[4];
   scores[0] = count_keyword_matches(task_hint, debug_keywords);
   scores[1] = count_keyword_matches(task_hint, plan_keywords);
   scores[2] = count_keyword_matches(task_hint, review_keywords);
   scores[3] = count_keyword_matches(task_hint, deploy_keywords);

   int best = 0;
   int best_score = scores[0];
   for (int i = 1; i < 4; i++)
   {
      if (scores[i] > best_score)
      {
         best = i;
         best_score = scores[i];
      }
   }

   if (best_score == 0)
      return INTENT_GENERAL;

   return (task_intent_t)best;
}

/*                          fact  pref  dec   epi   task  scratch proc  policy = 1.0 */
static const double plan_debug[] = {0.15, 0.00, 0.10, 0.25, 0.05, 0.00, 0.30, 0.15};
static const double plan_plan[] = {0.25, 0.10, 0.25, 0.05, 0.10, 0.00, 0.05, 0.20};
static const double plan_review[] = {0.15, 0.25, 0.20, 0.10, 0.00, 0.00, 0.10, 0.20};
static const double plan_deploy[] = {0.15, 0.00, 0.10, 0.15, 0.05, 0.00, 0.30, 0.25};
static const double plan_general[] = {0.20, 0.10, 0.15, 0.15, 0.10, 0.00, 0.15, 0.15};

void retrieval_plan_for_intent(task_intent_t intent, retrieval_plan_t *plan)
{
   plan->intent = intent;

   const double *budgets;
   switch (intent)
   {
   case INTENT_DEBUG:
      budgets = plan_debug;
      plan->include_l3 = 1;
      plan->recency_weight = 0.7;
      break;
   case INTENT_PLAN:
      budgets = plan_plan;
      plan->include_l3 = 0;
      plan->recency_weight = 0.2;
      break;
   case INTENT_REVIEW:
      budgets = plan_review;
      plan->include_l3 = 0;
      plan->recency_weight = 0.3;
      break;
   case INTENT_DEPLOY:
      budgets = plan_deploy;
      plan->include_l3 = 1;
      plan->recency_weight = 0.5;
      break;
   default:
      budgets = plan_general;
      plan->include_l3 = 0;
      plan->recency_weight = 0.3;
      break;
   }

   for (int i = 0; i < NUM_KINDS; i++)
      plan->kind_budget[i] = budgets[i];
}

#define MAX_CANDIDATES 100

typedef struct
{
   int64_t id;
   char key[512];
   char content[2048];
   char kind[16];
   double confidence;
   int use_count;
   double score; /* computed relevance score */
} context_candidate_t;

static int append_section(char *buf, int pos, int cap, const char *header, sqlite3_stmt *stmt,
                          int max_chars, int max_items)
{
   int section_start = pos;
   pos += snprintf(buf + pos, cap - pos, "\n## %s\n", header);

   int items = 0;
   int section_len = 0;

   while (sqlite3_step(stmt) == SQLITE_ROW && items < max_items)
   {
      const char *key = (const char *)sqlite3_column_text(stmt, 0);
      const char *content = (const char *)sqlite3_column_text(stmt, 1);
      if (!key)
         continue;

      const char *text = (content && content[0]) ? content : key;

      char truncated[MAX_MEM_CONTENT_LEN + 8];
      if ((int)strlen(text) > MAX_MEM_CONTENT_LEN)
      {
         snprintf(truncated, sizeof(truncated), "%.*s...", MAX_MEM_CONTENT_LEN, text);
         text = truncated;
      }

      int line_len = (int)strlen(text) + 4;
      if (section_len + line_len > max_chars)
         break;

      pos += snprintf(buf + pos, cap - pos, "- %s\n", text);
      section_len += line_len;
      items++;
   }

   /* If no items, remove section header */
   if (items == 0)
      pos = section_start;

   return pos;
}

/* Emit a candidate into the buffer, respecting section budget.
 * Returns number of chars written, or 0 if budget exceeded. */
static int emit_candidate(char *buf, int pos, int cap, const context_candidate_t *c,
                          int *section_len, int max_chars)
{
   const char *text = (c->content[0]) ? c->content : c->key;

   char truncated[MAX_MEM_CONTENT_LEN + 8];
   if ((int)strlen(text) > MAX_MEM_CONTENT_LEN)
   {
      snprintf(truncated, sizeof(truncated), "%.*s...", MAX_MEM_CONTENT_LEN, text);
      text = truncated;
   }

   int line_len = (int)strlen(text) + 4;
   if (*section_len + line_len > max_chars)
      return 0;

   int written = snprintf(buf + pos, cap - pos, "- %s\n", text);
   *section_len += line_len;
   return written;
}

/* Compare candidates by score descending */
static int cmp_candidates(const void *a, const void *b)
{
   const context_candidate_t *ca = (const context_candidate_t *)a;
   const context_candidate_t *cb = (const context_candidate_t *)b;
   if (cb->score > ca->score)
      return 1;
   if (cb->score < ca->score)
      return -1;
   return 0;
}

/* Compute term overlap between query terms and a text string.
 * Returns fraction of query terms found in the lowercased text. */
static double compute_term_overlap(char **query_terms, int term_count, const char *text)
{
   if (term_count <= 0 || !text)
      return 0.0;

   /* Lowercase copy for matching */
   char lower[2560];
   int li = 0;
   for (int i = 0; text[i] && li < (int)sizeof(lower) - 1; i++)
      lower[li++] = (char)tolower((unsigned char)text[i]);
   lower[li] = '\0';

   int matched = 0;
   for (int t = 0; t < term_count; t++)
   {
      if (strstr(lower, query_terms[t]))
         matched++;
   }
   return (double)matched / (double)term_count;
}

/* Compute graph boost score for a candidate against the boost map */
static double compute_graph_score(const boost_map_t *bmap, const char *key, const char *content)
{
   if (!bmap || bmap->count == 0)
      return 0.0;

   double boost = 0.0;
   for (int b = 0; b < bmap->count; b++)
   {
      if (strstr(key, bmap->entries[b].entity))
         boost += bmap->entries[b].score * 0.1;
      if (content && strstr(content, bmap->entries[b].entity))
         boost += bmap->entries[b].score * 0.1;
   }
   return boost;
}

/* Task-aware context assembly.
 *
 * When task_hint is NULL, falls back to the original per-section query behavior.
 * When task_hint is provided, uses a unified candidate pool scored by:
 *   relevance = base_score + term_overlap + graph_boost
 * then fills sections from the sorted pool. */
char *memory_assemble_context(sqlite3 *db, const char *task_hint)
{
   int cap = MAX_CONTEXT_TOTAL + 256;

   /* Check context cache */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      char hash[32];
      cache_input_hash(db, hash, sizeof(hash));
      char *cached = malloc(cap);
      if (cached && cache_get(db, hash, cached, cap) == 0)
         return cached;
      free(cached);
   }

   char *buf = malloc(cap);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, cap, "# Memory Context");

   if (!task_hint || !task_hint[0])
   {
      /* --- Original behavior: per-section queries --- */

      /* Key Facts (L2 facts/preferences) — with freshness markers for superseded */
      {
         static const char *sql =
             "SELECT m.id, m.key, m.content,"
             " (SELECT MAX(p.created_at) FROM memory_provenance p"
             "  WHERE p.memory_id = m.id AND p.action = 'supersede') AS supersede_date"
             " FROM memories m"
             " WHERE m.tier = 'L2'"
             " AND (m.kind = 'fact' OR m.kind = 'preference')"
             " ORDER BY m.confidence DESC, m.use_count DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            int section_start = pos;
            pos += snprintf(buf + pos, cap - pos, "\n## Key Facts\n");
            int items = 0;
            int section_len = 0;
            /* Prepare depends_on lookup for dependency hints */
            static const char *dep_sql = "SELECT m.key FROM memory_links ml"
                                         " JOIN memories m ON m.id = ml.target_id"
                                         " WHERE ml.source_id = ? AND ml.relation = 'depends_on'"
                                         " LIMIT 3";

            while (sqlite3_step(stmt) == SQLITE_ROW && items < MAX_CONTEXT_MEMS)
            {
               int64_t mem_id = sqlite3_column_int64(stmt, 0);
               const char *content = (const char *)sqlite3_column_text(stmt, 2);
               const char *key = (const char *)sqlite3_column_text(stmt, 1);
               const char *supersede_ts = (const char *)sqlite3_column_text(stmt, 3);
               const char *text = (content && content[0]) ? content : (key ? key : "");

               /* Check for depends_on links */
               char dep_hint[128] = "";
               sqlite3_stmt *ds = db_prepare(db, dep_sql);
               if (ds)
               {
                  sqlite3_bind_int64(ds, 1, mem_id);
                  int dpos = 0;
                  while (sqlite3_step(ds) == SQLITE_ROW)
                  {
                     const char *dk = (const char *)sqlite3_column_text(ds, 0);
                     if (dk)
                     {
                        if (dpos == 0)
                           dpos += snprintf(dep_hint + dpos, sizeof(dep_hint) - dpos,
                                            " (depends on: %s", dk);
                        else
                           dpos += snprintf(dep_hint + dpos, sizeof(dep_hint) - dpos, ", %s", dk);
                     }
                  }
                  if (dpos > 0)
                     snprintf(dep_hint + dpos, sizeof(dep_hint) - dpos, ")");
                  sqlite3_reset(ds);
               }

               char line[MAX_MEM_CONTENT_LEN + 192];
               if (supersede_ts && supersede_ts[0])
               {
                  char date[11] = {0};
                  snprintf(date, sizeof(date), "%.10s", supersede_ts);
                  snprintf(line, sizeof(line), "- %.*s (updated %s)%s", MAX_MEM_CONTENT_LEN, text,
                           date, dep_hint);
               }
               else
               {
                  snprintf(line, sizeof(line), "- %.*s%s", MAX_MEM_CONTENT_LEN, text, dep_hint);
               }

               int line_len = (int)strlen(line) + 1;
               if (section_len + line_len > 2000)
                  break;
               pos += snprintf(buf + pos, cap - pos, "%s\n", line);
               section_len += line_len;
               items++;
            }
            if (items == 0)
               pos = section_start;
            sqlite3_reset(stmt);
         }
      }

      /* Active Tasks (L1/L2 tasks) */
      {
         static const char *sql = "SELECT key, content FROM memories"
                                  " WHERE (tier = 'L1' OR tier = 'L2')"
                                  " AND kind = 'task'"
                                  " ORDER BY updated_at DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            pos = append_section(buf, pos, cap, "Active Tasks", stmt, 1500, MAX_CONTEXT_MEMS);
            sqlite3_reset(stmt);
         }
      }

      /* Recent Context (L1 episodes) */
      {
         static const char *sql = "SELECT key, content FROM memories"
                                  " WHERE tier = 'L1' AND kind = 'episode'"
                                  " ORDER BY created_at DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            pos = append_section(buf, pos, cap, "Recent Context", stmt, 1200, MAX_CONTEXT_MEMS);
            sqlite3_reset(stmt);
         }
      }

      /* Constraints (L2/L3 decisions + policies) */
      {
         static const char *sql = "SELECT key, content FROM memories"
                                  " WHERE (tier = 'L2' OR tier = 'L3')"
                                  " AND (kind = 'decision' OR kind = 'policy')"
                                  " ORDER BY confidence DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            pos = append_section(buf, pos, cap, "Constraints", stmt, 800, MAX_CONTEXT_MEMS);
            sqlite3_reset(stmt);
         }
      }

      /* Procedures (L1/L2 procedures) */
      {
         static const char *sql = "SELECT key, content FROM memories"
                                  " WHERE (tier = 'L1' OR tier = 'L2')"
                                  " AND kind = 'procedure'"
                                  " ORDER BY confidence DESC, use_count DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            pos = append_section(buf, pos, cap, "Procedures", stmt, 600, MAX_CONTEXT_MEMS);
            sqlite3_reset(stmt);
         }
      }

      /* Failure Warnings (L3 episodes) */
      {
         static const char *sql = "SELECT key, content FROM memories"
                                  " WHERE tier = 'L3' AND kind = 'episode'"
                                  " AND confidence > 0.3"
                                  " ORDER BY created_at DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            pos = append_section(buf, pos, cap, "Failure Warnings", stmt, 600, 3);
            sqlite3_reset(stmt);
         }
      }
   }
   else
   {
      /* --- Task-aware: unified candidate pool with relevance scoring --- */

      /* 1. Tokenize task_hint */
      char *query_terms[64];
      int term_count = tokenize_for_search(task_hint, query_terms, 64);

      /* 2. Compute graph boost map */
      boost_map_t bmap;
      bmap.count = 0;
      if (term_count > 0)
         memory_graph_boost(db, query_terms, term_count, &bmap);

      /* 3. Optionally embed the task_hint for semantic scoring */
      float *query_embed = NULL;
      int query_embed_dim = 0;
      config_t embed_cfg;
      config_load(&embed_cfg);
      if (embed_cfg.embedding_command[0])
      {
         query_embed = malloc(EMBED_MAX_DIM * sizeof(float));
         if (query_embed)
         {
            query_embed_dim = memory_embed_text(task_hint, embed_cfg.embedding_command, query_embed,
                                                EMBED_MAX_DIM);
            if (query_embed_dim <= 0)
            {
               free(query_embed);
               query_embed = NULL;
            }
         }
      }

      /* 4. Load all L1/L2 candidates */
      static const char *cand_sql =
          "SELECT id, key, content, kind, confidence, use_count FROM memories"
          " WHERE tier IN ('L1', 'L2')"
          " ORDER BY confidence DESC, use_count DESC"
          " LIMIT ?";

      context_candidate_t candidates[MAX_CANDIDATES];
      int cand_count = 0;

      sqlite3_stmt *stmt = db_prepare(db, cand_sql);
      if (stmt)
      {
         sqlite3_bind_int(stmt, 1, MAX_CANDIDATES);

         while (sqlite3_step(stmt) == SQLITE_ROW && cand_count < MAX_CANDIDATES)
         {
            context_candidate_t *c = &candidates[cand_count];
            c->id = sqlite3_column_int64(stmt, 0);

            const char *k = (const char *)sqlite3_column_text(stmt, 1);
            const char *v = (const char *)sqlite3_column_text(stmt, 2);
            const char *ki = (const char *)sqlite3_column_text(stmt, 3);

            snprintf(c->key, sizeof(c->key), "%s", k ? k : "");
            snprintf(c->content, sizeof(c->content), "%s", v ? v : "");
            snprintf(c->kind, sizeof(c->kind), "%s", ki ? ki : "");
            c->confidence = sqlite3_column_double(stmt, 4);
            c->use_count = sqlite3_column_int(stmt, 5);

            /* Score: base + term_overlap + graph_boost (+ optional embedding) */
            double base = c->confidence * 0.3 + (c->use_count / 10.0) * 0.2;
            if (base > 0.5)
               base = 0.5; /* cap base contribution */

            double key_overlap = compute_term_overlap(query_terms, term_count, c->key);
            double content_overlap = compute_term_overlap(query_terms, term_count, c->content);

            double embed_sim = 0.0;
            if (query_embed)
            {
               /* Look up stored embedding for this candidate */
               static const char *esql = "SELECT embedding FROM memory_embeddings"
                                         " WHERE memory_id = ?";
               sqlite3_stmt *es = db_prepare(db, esql);
               if (es)
               {
                  sqlite3_bind_int64(es, 1, c->id);
                  if (sqlite3_step(es) == SQLITE_ROW)
                  {
                     const void *blob = sqlite3_column_blob(es, 0);
                     int bytes = sqlite3_column_bytes(es, 0);
                     if (blob && bytes == query_embed_dim * (int)sizeof(float))
                        embed_sim =
                            cosine_similarity(query_embed, (const float *)blob, query_embed_dim);
                  }
                  sqlite3_reset(es);
               }
            }

            /* Blend: with embeddings, reduce term_overlap weight and add embed component */
            double overlap, graph;
            if (query_embed && embed_sim > 0.0)
            {
               overlap = (key_overlap > content_overlap ? key_overlap : content_overlap) * 0.15;
               double embed_score = embed_sim * 0.15;
               graph = compute_graph_score(&bmap, c->key, c->content) * 0.2;
               c->score = base + overlap + embed_score + graph;
            }
            else
            {
               overlap = (key_overlap > content_overlap ? key_overlap : content_overlap) * 0.3;
               graph = compute_graph_score(&bmap, c->key, c->content) * 0.2;
               c->score = base + overlap + graph;
            }

            cand_count++;
         }
         sqlite3_reset(stmt);
      }

      free(query_embed);

      /* Free tokenized terms */
      for (int t = 0; t < term_count; t++)
         free(query_terms[t]);

      /* 4. Sort by score descending */
      if (cand_count > 1)
         qsort(candidates, cand_count, sizeof(context_candidate_t), cmp_candidates);

      /* 5. Classify intent and build retrieval plan for dynamic budgets */
      retrieval_plan_t rplan;
      {
         task_intent_t intent = classify_intent(task_hint);
         retrieval_plan_for_intent(intent, &rplan);
      }

      /* Compute per-section budgets from plan (total budget = MAX_CONTEXT_TOTAL - overhead) */
      int section_budget = MAX_CONTEXT_TOTAL - 500; /* reserve for headers/structure */

      struct
      {
         const char *header;
         const char *kind;
         const char *kind2;
         int max_chars;
      } sections[] = {
          {"Key Facts", "fact", "preference",
           (int)((rplan.kind_budget[0] + rplan.kind_budget[1]) * section_budget)},
          {"Constraints", "decision", "policy",
           (int)((rplan.kind_budget[2] + rplan.kind_budget[7]) * section_budget)},
          {"Procedures", "procedure", NULL, (int)(rplan.kind_budget[6] * section_budget)},
          {"Active Tasks", "task", NULL, (int)(rplan.kind_budget[4] * section_budget)},
          {"Recent Context", "episode", NULL, (int)(rplan.kind_budget[3] * section_budget)},
      };
      int nsections = 5;

      for (int s = 0; s < nsections; s++)
      {
         int section_start = pos;
         pos += snprintf(buf + pos, cap - pos, "\n## %s\n", sections[s].header);
         int section_len = 0;
         int items = 0;

         for (int i = 0; i < cand_count && items < MAX_CONTEXT_MEMS; i++)
         {
            context_candidate_t *c = &candidates[i];
            if (c->id == 0)
               continue; /* already consumed */

            int match = (strcmp(c->kind, sections[s].kind) == 0);
            if (!match && sections[s].kind2)
               match = (strcmp(c->kind, sections[s].kind2) == 0);
            if (!match)
               continue;

            int written = emit_candidate(buf, pos, cap, c, &section_len, sections[s].max_chars);
            if (written == 0)
               break;

            /* Record context snapshot for effectiveness tracking */
            {
               const char *sid = session_id();
               if (sid && sid[0])
                  memory_record_context_snapshot(db, sid, c->id, c->score);
            }

            pos += written;
            items++;
            c->id = 0; /* mark consumed */
         }

         if (items == 0)
            pos = section_start;
      }

      /* Failure Warnings (L3 episodes) - only include when plan says so */
      if (rplan.include_l3)
      {
         static const char *sql = "SELECT key, content FROM memories"
                                  " WHERE tier = 'L3' AND kind = 'episode'"
                                  " AND confidence > 0.3"
                                  " ORDER BY created_at DESC";
         sqlite3_stmt *stmt = db_prepare(db, sql);
         if (stmt)
         {
            pos = append_section(buf, pos, cap, "Failure Warnings", stmt, 600, 3);
            sqlite3_reset(stmt);
         }
      }
   }

   /* Entity relationships from graph */
   {
      static const char *sql = "SELECT DISTINCT source, relation, target FROM entity_edges"
                               " ORDER BY weight DESC LIMIT 10";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         int has_edges = 0;
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128)
         {
            if (!has_edges)
            {
               pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Entity Relationships\n");
               has_edges = 1;
            }
            const char *src = (const char *)sqlite3_column_text(stmt, 0);
            const char *rel = (const char *)sqlite3_column_text(stmt, 1);
            const char *tgt = (const char *)sqlite3_column_text(stmt, 2);
            pos += snprintf(buf + pos, (size_t)(cap - pos), "- %s -[%s]-> %s\n", src ? src : "?",
                            rel ? rel : "?", tgt ? tgt : "?");
         }
      }
   }

   /* Graph-adjacent related memories: expand context with memories
    * connected via co_discussed edges to what we already retrieved.
    * Only runs when task_hint is provided (task-aware path). */
   if (task_hint && task_hint[0])
   {
      /* Collect seed keys from already-assembled context (top-level memories).
       * Use simple keyword extraction from the task_hint as seeds. */
      char *seeds[16];
      int seed_count = 0;
      char hint_copy[512];
      snprintf(hint_copy, sizeof(hint_copy), "%s", task_hint);
      char *svp = NULL;
      char *t = strtok_r(hint_copy, " \t\n", &svp);
      while (t && seed_count < 16)
      {
         if (strlen(t) >= 3)
            seeds[seed_count++] = t;
         t = strtok_r(NULL, " \t\n", &svp);
      }

      if (seed_count > 0)
      {
         graph_related_t related[4];
         int rcount = memory_graph_related(db, seeds, seed_count, related, 4);
         if (rcount > 0)
         {
            pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Related Memories (graph)\n");
            for (int i = 0; i < rcount && pos < cap - 128; i++)
            {
               const char *text = (related[i].content[0]) ? related[i].content : related[i].key;
               pos +=
                   snprintf(buf + pos, (size_t)(cap - pos), "- %.*s\n", MAX_MEM_CONTENT_LEN, text);
            }
         }
      }
   }

   /* Artifact staleness check (Feature 9) */
   {
      static const char *sql = "SELECT id, artifact_type, artifact_ref, artifact_hash"
                               " FROM memories WHERE artifact_type IS NOT NULL"
                               " AND artifact_hash IS NOT NULL LIMIT 50";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            int mem_id = sqlite3_column_int(stmt, 0);
            const char *atype = (const char *)sqlite3_column_text(stmt, 1);
            const char *aref = (const char *)sqlite3_column_text(stmt, 2);
            const char *ahash = (const char *)sqlite3_column_text(stmt, 3);
            if (!atype || !aref || !ahash)
               continue;

            if (strcmp(atype, "file") == 0)
            {
               /* Re-hash the file and compare */
               FILE *f = fopen(aref, "r");
               if (f)
               {
                  unsigned long h = 0;
                  int c;
                  while ((c = fgetc(f)) != EOF)
                     h = h * 31 + (unsigned long)c;
                  fclose(f);
                  char current_hash[65];
                  snprintf(current_hash, sizeof(current_hash), "%016lx", h);
                  if (strcmp(current_hash, ahash) != 0)
                  {
                     /* Stale: reduce confidence */
                     static const char *upd = "UPDATE memories SET confidence = confidence * 0.7"
                                              " WHERE id = ?";
                     sqlite3_stmt *us = db_prepare(db, upd);
                     if (us)
                     {
                        sqlite3_reset(us);
                        sqlite3_bind_int(us, 1, mem_id);
                        DB_STEP_LOG(us, "memory_context");
                     }
                  }
               }
            }
         }
      }
   }

   buf[pos] = '\0';

   /* Store in context cache */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      char hash[32];
      cache_input_hash(db, hash, sizeof(hash));
      cache_put(db, hash, buf);
   }

   return buf;
}

/* Workspace-aware context assembly.
 *
 * Two-pass approach:
 * Pass 1 (70% budget): memories tagged with current workspace or _shared.
 * Pass 2 (30% budget): high-confidence memories from other workspaces.
 * Untagged memories (legacy) are treated as _shared. */
char *memory_assemble_context_ws(sqlite3 *db, const char *task_hint, const char *workspace)
{
   if (!workspace || !workspace[0])
      return memory_assemble_context(db, task_hint);

   int cap = MAX_CONTEXT_TOTAL + 256;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;

   int pos = snprintf(buf, cap, "# Memory Context");

   /* Budget split: 70% for workspace-scoped, 30% for cross-workspace */
   int ws_budget = (MAX_CONTEXT_TOTAL * 70) / 100;
   int cross_budget = MAX_CONTEXT_TOTAL - ws_budget;

   /* --- Pass 1: workspace-scoped memories (current workspace + _shared + untagged) --- */

   /* Key Facts (workspace-scoped) */
   {
      const char *sql = "SELECT m.key, m.content FROM memories m"
                        " WHERE m.tier = 'L2'"
                        " AND m.kind IN ('fact', 'preference')"
                        " AND (m.id IN (SELECT memory_id FROM memory_workspaces"
                        "               WHERE workspace = ? OR workspace = ?)"
                        "      OR m.id NOT IN (SELECT memory_id FROM memory_workspaces))"
                        " ORDER BY m.confidence DESC, m.use_count DESC";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
         pos = append_section(buf, pos, cap, "Key Facts", stmt, (ws_budget * 36) / 100,
                              MAX_CONTEXT_MEMS);
         sqlite3_reset(stmt);
      }
   }

   /* Active Tasks (workspace-scoped) */
   {
      const char *sql = "SELECT m.key, m.content FROM memories m"
                        " WHERE (m.tier = 'L1' OR m.tier = 'L2')"
                        " AND m.kind = 'task'"
                        " AND (m.id IN (SELECT memory_id FROM memory_workspaces"
                        "               WHERE workspace = ? OR workspace = ?)"
                        "      OR m.id NOT IN (SELECT memory_id FROM memory_workspaces))"
                        " ORDER BY m.updated_at DESC";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
         pos = append_section(buf, pos, cap, "Active Tasks", stmt, (ws_budget * 27) / 100,
                              MAX_CONTEXT_MEMS);
         sqlite3_reset(stmt);
      }
   }

   /* Recent Context (workspace-scoped) */
   {
      const char *sql = "SELECT m.key, m.content FROM memories m"
                        " WHERE m.tier = 'L1' AND m.kind = 'episode'"
                        " AND (m.id IN (SELECT memory_id FROM memory_workspaces"
                        "               WHERE workspace = ? OR workspace = ?)"
                        "      OR m.id NOT IN (SELECT memory_id FROM memory_workspaces))"
                        " ORDER BY m.created_at DESC";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
         pos = append_section(buf, pos, cap, "Recent Context", stmt, (ws_budget * 22) / 100,
                              MAX_CONTEXT_MEMS);
         sqlite3_reset(stmt);
      }
   }

   /* Constraints (workspace-scoped: decisions + policies) */
   {
      const char *sql = "SELECT m.key, m.content FROM memories m"
                        " WHERE (m.tier = 'L2' OR m.tier = 'L3')"
                        " AND m.kind IN ('decision', 'policy')"
                        " AND (m.id IN (SELECT memory_id FROM memory_workspaces"
                        "               WHERE workspace = ? OR workspace = ?)"
                        "      OR m.id NOT IN (SELECT memory_id FROM memory_workspaces))"
                        " ORDER BY m.confidence DESC";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
         pos = append_section(buf, pos, cap, "Constraints", stmt, (ws_budget * 12) / 100,
                              MAX_CONTEXT_MEMS);
         sqlite3_reset(stmt);
      }
   }

   /* Procedures (workspace-scoped) */
   {
      const char *sql = "SELECT m.key, m.content FROM memories m"
                        " WHERE (m.tier = 'L1' OR m.tier = 'L2')"
                        " AND m.kind = 'procedure'"
                        " AND (m.id IN (SELECT memory_id FROM memory_workspaces"
                        "               WHERE workspace = ? OR workspace = ?)"
                        "      OR m.id NOT IN (SELECT memory_id FROM memory_workspaces))"
                        " ORDER BY m.confidence DESC, m.use_count DESC";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
         pos = append_section(buf, pos, cap, "Procedures", stmt, (ws_budget * 10) / 100,
                              MAX_CONTEXT_MEMS);
         sqlite3_reset(stmt);
      }
   }

   /* --- Pass 2: high-confidence cross-workspace memories --- */
   {
      const char *sql = "SELECT m.key, m.content FROM memories m"
                        " WHERE m.tier = 'L2' AND m.confidence >= 0.9 AND m.use_count >= 5"
                        " AND m.sensitivity IN ('public', 'normal')"
                        " AND m.id NOT IN (SELECT memory_id FROM memory_workspaces"
                        "                   WHERE workspace = ? OR workspace = ?)"
                        " AND m.id IN (SELECT memory_id FROM memory_workspaces)"
                        " ORDER BY m.confidence DESC, m.use_count DESC";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, workspace, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, SHARED_WORKSPACE, -1, SQLITE_TRANSIENT);
         pos = append_section(buf, pos, cap, "Cross-Workspace", stmt, cross_budget,
                              MAX_CONTEXT_MEMS / 2);
         sqlite3_reset(stmt);
      }
   }

   /* Entity relationships from graph */
   {
      static const char *sql = "SELECT DISTINCT source, relation, target FROM entity_edges"
                               " ORDER BY weight DESC LIMIT 10";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         int has_edges = 0;
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128)
         {
            if (!has_edges)
            {
               pos += snprintf(buf + pos, (size_t)(cap - pos), "\n## Entity Relationships\n");
               has_edges = 1;
            }
            const char *src = (const char *)sqlite3_column_text(stmt, 0);
            const char *rel = (const char *)sqlite3_column_text(stmt, 1);
            const char *tgt = (const char *)sqlite3_column_text(stmt, 2);
            pos += snprintf(buf + pos, (size_t)(cap - pos), "- %s -[%s]-> %s\n", src ? src : "?",
                            rel ? rel : "?", tgt ? tgt : "?");
         }
      }
   }

   buf[pos] = '\0';
   return buf;
}

/* --- Context Cache --- */

int cache_get(sqlite3 *db, const char *hash, char *out, size_t out_len)
{
   static const char *sql = "SELECT output FROM context_cache WHERE hash = ?"
                            " AND created_at > datetime('now', '-' || ? || ' seconds')";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   char ttl[16];
   snprintf(ttl, sizeof(ttl), "%d", CACHE_TTL_SECONDS);

   sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, ttl, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *val = (const char *)sqlite3_column_text(stmt, 0);
      if (val)
      {
         snprintf(out, out_len, "%s", val);
         rc = 0;
      }
   }
   sqlite3_reset(stmt);
   return rc;
}

void cache_put(sqlite3 *db, const char *hash, const char *output)
{
   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT OR REPLACE INTO context_cache (hash, output, created_at)"
                            " VALUES (?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_bind_text(stmt, 1, hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, output, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "cache_put");
   sqlite3_reset(stmt);
}

void cache_invalidate(sqlite3 *db)
{
   static const char *sql = "DELETE FROM context_cache";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (stmt)
   {
      DB_STEP_LOG(stmt, "cache_invalidate");
      sqlite3_reset(stmt);
   }
}

/* Simple hash: djb2 on the concatenation of counts and timestamps */
char *cache_input_hash(sqlite3 *db, char *buf, size_t buf_len)
{
   char raw[512];
   int pos = 0;

   /* Memory count + max updated_at */
   {
      static const char *sql = "SELECT COUNT(*), MAX(updated_at) FROM memories";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            int cnt = sqlite3_column_int(stmt, 0);
            const char *ts = (const char *)sqlite3_column_text(stmt, 1);
            pos += snprintf(raw + pos, sizeof(raw) - pos, "m:%d:%s;", cnt, ts ? ts : "");
         }
         sqlite3_reset(stmt);
      }
   }

   /* Rule count + max updated_at */
   {
      static const char *sql = "SELECT COUNT(*), MAX(updated_at) FROM rules";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            int cnt = sqlite3_column_int(stmt, 0);
            const char *ts = (const char *)sqlite3_column_text(stmt, 1);
            pos += snprintf(raw + pos, sizeof(raw) - pos, "r:%d:%s;", cnt, ts ? ts : "");
         }
         sqlite3_reset(stmt);
      }
   }

   raw[pos] = '\0';

   /* djb2 hash */
   unsigned long hash = 5381;
   for (int i = 0; raw[i]; i++)
      hash = ((hash << 5) + hash) + (unsigned char)raw[i];

   snprintf(buf, buf_len, "%016lx", hash);
   return buf;
}
