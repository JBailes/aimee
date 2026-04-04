/* memory_advanced.c: anti-patterns, style learning, contradiction detection, compaction, provenance
 */
#include "aimee.h"
#include "memory.h"
#include <ctype.h>

/* --- Anti-Patterns --- */

int anti_pattern_insert(sqlite3 *db, const char *pattern, const char *desc, const char *source,
                        const char *ref, double confidence, anti_pattern_t *out)
{
   if (!db || !pattern)
      return -1;

   static const char *sql = "INSERT INTO anti_patterns (pattern, description, source,"
                            " source_ref, confidence)"
                            " VALUES (?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, desc ? desc : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, source ? source : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, ref ? ref : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 5, confidence);

   if (sqlite3_step(stmt) != SQLITE_DONE)
   {
      sqlite3_reset(stmt);
      return -1;
   }

   int64_t new_id = sqlite3_last_insert_rowid(db);
   sqlite3_reset(stmt);

   if (out)
   {
      out->id = new_id;
      snprintf(out->pattern, sizeof(out->pattern), "%s", pattern);
      snprintf(out->description, sizeof(out->description), "%s", desc ? desc : "");
      snprintf(out->source, sizeof(out->source), "%s", source ? source : "");
      snprintf(out->source_ref, sizeof(out->source_ref), "%s", ref ? ref : "");
      out->hit_count = 0;
      out->confidence = confidence;
   }
   return 0;
}

int anti_pattern_list(sqlite3 *db, anti_pattern_t *out, int max)
{
   static const char *sql = "SELECT id, pattern, description, source, source_ref,"
                            " hit_count, confidence"
                            " FROM anti_patterns ORDER BY hit_count DESC, confidence DESC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);

      const char *p = (const char *)sqlite3_column_text(stmt, 1);
      const char *d = (const char *)sqlite3_column_text(stmt, 2);
      const char *s = (const char *)sqlite3_column_text(stmt, 3);
      const char *r = (const char *)sqlite3_column_text(stmt, 4);

      snprintf(out[count].pattern, sizeof(out[count].pattern), "%s", p ? p : "");
      snprintf(out[count].description, sizeof(out[count].description), "%s", d ? d : "");
      snprintf(out[count].source, sizeof(out[count].source), "%s", s ? s : "");
      snprintf(out[count].source_ref, sizeof(out[count].source_ref), "%s", r ? r : "");
      out[count].hit_count = sqlite3_column_int(stmt, 5);
      out[count].confidence = sqlite3_column_double(stmt, 6);
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int anti_pattern_check(sqlite3 *db, const char *file_path, const char *command, anti_pattern_t *out,
                       int max)
{
   if (!db)
      return 0;

   /* Build target string from file_path + command */
   char target[MAX_PATH_LEN + 1024];
   int tpos = 0;
   if (file_path)
      tpos += snprintf(target + tpos, sizeof(target) - tpos, "%s ", file_path);
   if (command)
      tpos += snprintf(target + tpos, sizeof(target) - tpos, "%s", command);
   target[tpos] = '\0';

   /* Lowercase the target for matching */
   for (int i = 0; target[i]; i++)
      target[i] = tolower((unsigned char)target[i]);

   /* Load all anti-patterns and match */
   static const char *sql = "SELECT id, pattern, description, source, source_ref,"
                            " hit_count, confidence"
                            " FROM anti_patterns ORDER BY confidence DESC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      const char *pattern = (const char *)sqlite3_column_text(stmt, 1);
      if (!pattern)
         continue;

      /* Tokenize pattern and check if words appear in target */
      char pat_copy[512];
      snprintf(pat_copy, sizeof(pat_copy), "%s", pattern);

      /* Lowercase pattern */
      for (int i = 0; pat_copy[i]; i++)
         pat_copy[i] = tolower((unsigned char)pat_copy[i]);

      /* Split on whitespace and check each word */
      int match_count = 0;
      int word_count = 0;
      char *saveptr = NULL;
      char *tok = strtok_r(pat_copy, " \t\n", &saveptr);

      while (tok)
      {
         word_count++;
         if (strstr(target, tok))
            match_count++;
         tok = strtok_r(NULL, " \t\n", &saveptr);
      }

      /* Require at least half the pattern words to match */
      if (word_count > 0 && match_count * 2 >= word_count)
      {
         out[count].id = sqlite3_column_int64(stmt, 0);

         const char *d = (const char *)sqlite3_column_text(stmt, 2);
         const char *s = (const char *)sqlite3_column_text(stmt, 3);
         const char *r = (const char *)sqlite3_column_text(stmt, 4);

         snprintf(out[count].pattern, sizeof(out[count].pattern), "%s", pattern);
         snprintf(out[count].description, sizeof(out[count].description), "%s", d ? d : "");
         snprintf(out[count].source, sizeof(out[count].source), "%s", s ? s : "");
         snprintf(out[count].source_ref, sizeof(out[count].source_ref), "%s", r ? r : "");
         out[count].hit_count = sqlite3_column_int(stmt, 5);
         out[count].confidence = sqlite3_column_double(stmt, 6);

         /* Bump hit_count */
         static const char *bump_sql = "UPDATE anti_patterns SET hit_count = hit_count + 1"
                                       " WHERE id = ?";
         sqlite3_stmt *bs = db_prepare(db, bump_sql);
         if (bs)
         {
            sqlite3_bind_int64(bs, 1, out[count].id);
            DB_STEP_LOG(bs, "anti_pattern_check");
            sqlite3_reset(bs);
         }

         count++;
      }
   }
   sqlite3_reset(stmt);
   return count;
}

int anti_pattern_delete(sqlite3 *db, int64_t id)
{
   static const char *sql = "DELETE FROM anti_patterns WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);
   DB_STEP_LOG(stmt, "anti_pattern_delete");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

int anti_pattern_extract_from_feedback(sqlite3 *db)
{
   /* Scan negative rules for anti-pattern indicators */
   static const char *sql = "SELECT id, title, description FROM rules"
                            " WHERE polarity = 'negative' AND weight >= 50";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int extracted = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *title = (const char *)sqlite3_column_text(stmt, 1);
      const char *desc = (const char *)sqlite3_column_text(stmt, 2);
      int rule_id = sqlite3_column_int(stmt, 0);

      if (!title)
         continue;

      /* Check if already extracted */
      char ref[64];
      snprintf(ref, sizeof(ref), "rule:%d", rule_id);

      static const char *check = "SELECT COUNT(*) FROM anti_patterns WHERE source_ref = ?";
      sqlite3_stmt *cs = db_prepare(db, check);
      if (cs)
      {
         sqlite3_bind_text(cs, 1, ref, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(cs) == SQLITE_ROW && sqlite3_column_int(cs, 0) > 0)
         {
            sqlite3_reset(cs);
            continue;
         }
         sqlite3_reset(cs);
      }

      anti_pattern_insert(db, title, desc ? desc : "", "feedback", ref, 0.8, NULL);
      extracted++;
   }
   sqlite3_reset(stmt);
   return extracted;
}

int anti_pattern_extract_from_failures(sqlite3 *db)
{
   /* Scan decision_log for failed outcomes */
   static const char *sql = "SELECT id, chosen, rationale FROM decision_log"
                            " WHERE outcome = 'failure'";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   int extracted = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int64_t dec_id = sqlite3_column_int64(stmt, 0);
      const char *chosen = (const char *)sqlite3_column_text(stmt, 1);
      const char *rationale = (const char *)sqlite3_column_text(stmt, 2);

      if (!chosen)
         continue;

      char ref[64];
      snprintf(ref, sizeof(ref), "decision:%lld", (long long)dec_id);

      /* Check if already extracted */
      static const char *check = "SELECT COUNT(*) FROM anti_patterns WHERE source_ref = ?";
      sqlite3_stmt *cs = db_prepare(db, check);
      if (cs)
      {
         sqlite3_bind_text(cs, 1, ref, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(cs) == SQLITE_ROW && sqlite3_column_int(cs, 0) > 0)
         {
            sqlite3_reset(cs);
            continue;
         }
         sqlite3_reset(cs);
      }

      anti_pattern_insert(db, chosen, rationale ? rationale : "", "failure", ref, 0.7, NULL);
      extracted++;
   }
   sqlite3_reset(stmt);
   return extracted;
}

int anti_pattern_escalate(sqlite3 *db, int hit_threshold)
{
   if (!db || hit_threshold <= 0)
      return 0;

   /* Promote high-hit anti-patterns to blocking rules */
   static const char *sql = "SELECT id, pattern, description, hit_count FROM anti_patterns"
                            " WHERE hit_count >= ? ORDER BY hit_count DESC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int(stmt, 1, hit_threshold);

   int escalated = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *pattern = (const char *)sqlite3_column_text(stmt, 1);
      const char *desc = (const char *)sqlite3_column_text(stmt, 2);
      if (!pattern)
         continue;

      /* Check if a blocking rule already exists for this pattern */
      rule_t existing;
      if (rules_find_by_title(db, pattern, &existing) == 0)
      {
         /* Already a rule; bump to hard directive if not already */
         if (strcmp(existing.directive_type, "hard") != 0)
         {
            static const char *upd = "UPDATE rules SET directive_type = 'hard',"
                                     " weight = 90, updated_at = datetime('now'),"
                                     " last_reinforced_at = datetime('now')"
                                     " WHERE id = ?";
            sqlite3_stmt *us = db_prepare(db, upd);
            if (us)
            {
               sqlite3_bind_int(us, 1, existing.id);
               DB_STEP_LOG(us, "anti_pattern_escalate");
               sqlite3_reset(us);
               escalated++;
            }
         }
         continue;
      }

      /* Create a new hard directive rule from the anti-pattern */
      int dummy = 0;
      feedback_record(db, "negative", pattern, desc ? desc : "", 90, &dummy);
      /* Mark as hard directive */
      rule_t new_rule;
      if (rules_find_by_title(db, pattern, &new_rule) == 0)
      {
         static const char *upd = "UPDATE rules SET directive_type = 'hard',"
                                  " updated_at = datetime('now'),"
                                  " last_reinforced_at = datetime('now') WHERE id = ?";
         sqlite3_stmt *us = db_prepare(db, upd);
         if (us)
         {
            sqlite3_bind_int(us, 1, new_rule.id);
            DB_STEP_LOG(us, "anti_pattern_escalate");
            sqlite3_reset(us);
         }
      }
      escalated++;
   }
   sqlite3_reset(stmt);
   return escalated;
}

/* --- Temporal Facts --- */

/* Local provenance helper (same logic as memory.c) */
static void add_provenance_local(sqlite3 *db, int64_t memory_id, const char *session_id,
                                 const char *action, const char *details)
{
   static const char *sql = "INSERT INTO memory_provenance (memory_id, session_id, action,"
                            " details, created_at) VALUES (?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   char ts[32];
   now_utc(ts, sizeof(ts));

   sqlite3_bind_int64(stmt, 1, memory_id);
   sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, action, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, details ? details : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "add_provenance_local");
   sqlite3_reset(stmt);
}

int memory_supersede(sqlite3 *db, int64_t old_id, const char *new_content, double confidence,
                     const char *session_id, memory_t *out)
{
   if (!db || old_id <= 0)
      return -1;

   /* Get the old memory */
   memory_t old_mem;
   if (memory_get(db, old_id, &old_mem) != 0)
      return -1;

   /* Find next version number */
   int version = 1;
   {
      char like_pattern[560];
      snprintf(like_pattern, sizeof(like_pattern), "%s#v%%", old_mem.key);

      static const char *sql = "SELECT COUNT(*) FROM memories WHERE key LIKE ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) == SQLITE_ROW)
            version = sqlite3_column_int(stmt, 0) + 1;
         sqlite3_reset(stmt);
      }
   }

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Rename old key to key#vN, set valid_until */
   {
      char versioned_key[560];
      snprintf(versioned_key, sizeof(versioned_key), "%s#v%d", old_mem.key, version);

      static const char *sql = "UPDATE memories SET key = ?, valid_until = ?,"
                               " updated_at = ? WHERE id = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return -1;

      sqlite3_bind_text(stmt, 1, versioned_key, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt, 4, old_id);
      DB_STEP_LOG(stmt, "memory_supersede");
      sqlite3_reset(stmt);
   }

   /* INSERT new memory with same key and supersedes link */
   int rc = memory_insert(db, old_mem.tier, old_mem.kind, old_mem.key, new_content, confidence,
                          session_id, out);
   if (rc != 0)
      return -1;

   /* Set valid_from on the new memory */
   if (out)
   {
      static const char *vf_sql = "UPDATE memories SET valid_from = ? WHERE id = ?";
      sqlite3_stmt *stmt = db_prepare(db, vf_sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(stmt, 2, out->id);
         DB_STEP_LOG(stmt, "memory_supersede");
         sqlite3_reset(stmt);
      }

      /* Record provenance with supersedes reference */
      char details[128];
      snprintf(details, sizeof(details), "supersedes memory %lld", (long long)old_id);
      add_provenance_local(db, out->id, session_id, "supersede", details);

      /* Auto-link: new memory supersedes old */
      memory_link_create(db, out->id, old_id, "supersedes");
   }

   return 0;
}

int memory_fact_history(sqlite3 *db, const char *key, memory_t *out, int max)
{
   if (!db || !key)
      return 0;

   char norm[512];
   normalize_key(key, norm, sizeof(norm));

   /* Match exact key or versioned key#vN */
   char like_pattern[560];
   snprintf(like_pattern, sizeof(like_pattern), "%s#v%%", norm);

   static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                            " last_used_at, created_at, updated_at, source_session"
                            " FROM memories"
                            " WHERE key = ? OR key LIKE ?"
                            " ORDER BY created_at DESC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_text(stmt, 1, norm, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, like_pattern, -1, SQLITE_TRANSIENT);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);

      const char *tier = (const char *)sqlite3_column_text(stmt, 1);
      const char *kind = (const char *)sqlite3_column_text(stmt, 2);
      const char *k = (const char *)sqlite3_column_text(stmt, 3);
      const char *content = (const char *)sqlite3_column_text(stmt, 4);

      snprintf(out[count].tier, sizeof(out[count].tier), "%s", tier ? tier : "");
      snprintf(out[count].kind, sizeof(out[count].kind), "%s", kind ? kind : "");
      snprintf(out[count].key, sizeof(out[count].key), "%s", k ? k : "");
      snprintf(out[count].content, sizeof(out[count].content), "%s", content ? content : "");

      out[count].confidence = sqlite3_column_double(stmt, 5);
      out[count].use_count = sqlite3_column_int(stmt, 6);

      const char *lua = (const char *)sqlite3_column_text(stmt, 7);
      const char *cat = (const char *)sqlite3_column_text(stmt, 8);
      const char *uat = (const char *)sqlite3_column_text(stmt, 9);
      const char *src = (const char *)sqlite3_column_text(stmt, 10);

      snprintf(out[count].last_used_at, sizeof(out[count].last_used_at), "%s", lua ? lua : "");
      snprintf(out[count].created_at, sizeof(out[count].created_at), "%s", cat ? cat : "");
      snprintf(out[count].updated_at, sizeof(out[count].updated_at), "%s", uat ? uat : "");
      snprintf(out[count].source_session, sizeof(out[count].source_session), "%s", src ? src : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

/* --- Drift Detection --- */

int memory_check_drift(sqlite3 *db, int64_t task_id, const char *file_path, const char *command,
                       drift_result_t *out)
{
   if (!db || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   /* Get active task */
   static const char *task_sql = "SELECT id, title, session_id FROM tasks WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, task_sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, task_id);

   char title[256] = {0};
   char task_session[128] = {0};

   if (sqlite3_step(stmt) != SQLITE_ROW)
   {
      sqlite3_reset(stmt);
      return -1;
   }

   const char *t = (const char *)sqlite3_column_text(stmt, 1);
   const char *s = (const char *)sqlite3_column_text(stmt, 2);
   snprintf(title, sizeof(title), "%s", t ? t : "");
   snprintf(task_session, sizeof(task_session), "%s", s ? s : "");
   sqlite3_reset(stmt);

   out->task_id = task_id;
   snprintf(out->task_title, sizeof(out->task_title), "%s", title);

   /* Build scope from title words */
   char *scope_terms[64];
   int scope_count = tokenize_for_search(title, scope_terms, 64);

   /* Add session project terms */
   if (task_session[0])
   {
      static const char *proj_sql = "SELECT DISTINCT project_name FROM window_projects"
                                    " WHERE window_id IN"
                                    " (SELECT id FROM windows WHERE session_id = ?)"
                                    " LIMIT 10";
      sqlite3_stmt *ps = db_prepare(db, proj_sql);
      if (ps)
      {
         sqlite3_bind_text(ps, 1, task_session, -1, SQLITE_TRANSIENT);
         while (sqlite3_step(ps) == SQLITE_ROW && scope_count < 64)
         {
            const char *pn = (const char *)sqlite3_column_text(ps, 0);
            if (pn)
            {
               scope_terms[scope_count] = strdup(pn);
               if (scope_terms[scope_count])
                  scope_count++;
            }
         }
         sqlite3_reset(ps);
      }
   }

   /* Add subtask title terms */
   {
      static const char *sub_sql = "SELECT title FROM tasks WHERE parent_id = ?";
      sqlite3_stmt *ss = db_prepare(db, sub_sql);
      if (ss)
      {
         sqlite3_bind_int64(ss, 1, task_id);
         while (sqlite3_step(ss) == SQLITE_ROW)
         {
            const char *st = (const char *)sqlite3_column_text(ss, 0);
            if (!st)
               continue;

            char *sub_terms[16];
            int sc = tokenize_for_search(st, sub_terms, 16);
            for (int i = 0; i < sc && scope_count < 64; i++)
               scope_terms[scope_count++] = sub_terms[i];
         }
         sqlite3_reset(ss);
      }
   }

   /* Check if file_path overlaps scope */
   int file_in_scope = 0;
   if (file_path && file_path[0])
   {
      /* Extract basename and directory components */
      char fp_lower[MAX_PATH_LEN];
      snprintf(fp_lower, sizeof(fp_lower), "%s", file_path);
      for (int i = 0; fp_lower[i]; i++)
         fp_lower[i] = tolower((unsigned char)fp_lower[i]);

      for (int s = 0; s < scope_count; s++)
      {
         if (scope_terms[s] && strstr(fp_lower, scope_terms[s]))
         {
            file_in_scope = 1;
            break;
         }
      }
   }

   /* Check if command overlaps scope */
   int cmd_in_scope = 0;
   if (command && command[0])
   {
      char cmd_lower[1024];
      snprintf(cmd_lower, sizeof(cmd_lower), "%s", command);
      for (int i = 0; cmd_lower[i]; i++)
         cmd_lower[i] = tolower((unsigned char)cmd_lower[i]);

      for (int s = 0; s < scope_count; s++)
      {
         if (scope_terms[s] && strstr(cmd_lower, scope_terms[s]))
         {
            cmd_in_scope = 1;
            break;
         }
      }
   }

   /* Determine drift */
   int has_input = (file_path && file_path[0]) || (command && command[0]);
   if (has_input && !file_in_scope && !cmd_in_scope)
   {
      out->drifted = 1;
      snprintf(out->message, sizeof(out->message), "Action does not appear related to task: %s",
               title);
   }
   else
   {
      out->drifted = 0;
   }

   /* Clean up */
   for (int i = 0; i < scope_count; i++)
      free(scope_terms[i]);

   return 0;
}

/* --- Style Learning --- */

#define STYLE_MAX_KEYWORDS    16
#define STYLE_MATCH_THRESHOLD 2

typedef struct
{
   const char *dimension;                        /* memory key: style_{dimension} */
   const char *neg_keywords[STYLE_MAX_KEYWORDS]; /* triggers from negative rules */
   const char *pos_keywords[STYLE_MAX_KEYWORDS]; /* triggers from positive rules */
   const char *neg_preference;                   /* generated when neg matches >= threshold */
   const char *pos_preference;                   /* generated when pos matches >= threshold */
} style_dimension_t;

static const style_dimension_t style_dimensions[] = {
    {"verbosity",
     {"verbose", "wordy", "lengthy", "chatty", "too long", "too much", "wall of text",
      "overwhelming", NULL},
     {"concise", "brief", "terse", "short", "minimal", "succinct", "compact", NULL},
     "User prefers concise, non-verbose output",
     "User explicitly prefers brief responses"},
    {"explanations",
     {"obvious", "unnecessary", "over-explain", "don't explain", "stop explaining", "already know",
      NULL},
     {"explain", "why", "reasoning", "context", "more detail", "elaborate", NULL},
     "User prefers minimal explanations; avoid stating the obvious",
     "User values detailed explanations with reasoning"},
    {"commit_style",
     {"vague commit", "bad message", "commit message", "unclear commit", NULL},
     {"conventional", "semantic commit", "good commit", "commit convention", NULL},
     "User wants clear, descriptive commit messages",
     "User follows conventional commit style"},
    {"naming",
     {"inconsistent", "wrong case", "naming", "rename", NULL},
     {"snake_case", "camelcase", "consistent naming", "naming convention", NULL},
     "User wants consistent naming conventions",
     "User prefers specific naming conventions"},
    {"comments",
     {"too many comments", "obvious comments", "unnecessary comment", "remove comment", NULL},
     {"document", "comment", "annotate", "docstring", "jsdoc", NULL},
     "User prefers minimal code comments",
     "User values thorough code documentation"},
    {"structure",
     {"wall of text", "no headers", "unstructured", "hard to read", "formatting", NULL},
     {"headers", "sections", "bullet", "structured", "organized", NULL},
     "User prefers structured output with sections and headers",
     "User values well-organized, structured responses"},
};

#define STYLE_DIMENSION_COUNT (sizeof(style_dimensions) / sizeof(style_dimensions[0]))

static int match_keywords(const char *text, const char *const keywords[])
{
   for (int k = 0; keywords[k]; k++)
      if (strstr(text, keywords[k]))
         return 1;
   return 0;
}

int memory_learn_style(sqlite3 *db)
{
   if (!db)
      return 0;

   int neg_counts[STYLE_DIMENSION_COUNT];
   int pos_counts[STYLE_DIMENSION_COUNT];
   memset(neg_counts, 0, sizeof(neg_counts));
   memset(pos_counts, 0, sizeof(pos_counts));

   /* Scan both positive and negative feedback rules */
   {
      static const char *sql = "SELECT polarity, description FROM rules"
                               " WHERE polarity IN ('positive', 'negative')";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return 0;

      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *polarity = (const char *)sqlite3_column_text(stmt, 0);
         const char *desc = (const char *)sqlite3_column_text(stmt, 1);
         if (!polarity || !desc)
            continue;

         char lower[1024];
         snprintf(lower, sizeof(lower), "%s", desc);
         for (int i = 0; lower[i]; i++)
            lower[i] = tolower((unsigned char)lower[i]);

         int is_neg = (strcmp(polarity, "negative") == 0);

         for (size_t d = 0; d < STYLE_DIMENSION_COUNT; d++)
         {
            if (is_neg && match_keywords(lower, style_dimensions[d].neg_keywords))
               neg_counts[d]++;
            else if (!is_neg && match_keywords(lower, style_dimensions[d].pos_keywords))
               pos_counts[d]++;
         }
      }
      sqlite3_reset(stmt);
   }

   /* Scan decision_log for style-relevant successful decisions */
   {
      static const char *sql = "SELECT chosen, rationale FROM decision_log"
                               " WHERE outcome = 'success'"
                               " AND (LOWER(chosen) LIKE '%style%'"
                               " OR LOWER(chosen) LIKE '%format%'"
                               " OR LOWER(chosen) LIKE '%naming%'"
                               " OR LOWER(chosen) LIKE '%convention%'"
                               " OR LOWER(chosen) LIKE '%commit%'"
                               " OR LOWER(chosen) LIKE '%comment%')";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            const char *chosen = (const char *)sqlite3_column_text(stmt, 0);
            const char *rationale = (const char *)sqlite3_column_text(stmt, 1);

            char lower[2048];
            snprintf(lower, sizeof(lower), "%s %s", chosen ? chosen : "",
                     rationale ? rationale : "");
            for (int i = 0; lower[i]; i++)
               lower[i] = tolower((unsigned char)lower[i]);

            for (size_t d = 0; d < STYLE_DIMENSION_COUNT; d++)
            {
               if (match_keywords(lower, style_dimensions[d].pos_keywords))
                  pos_counts[d]++;
               else if (match_keywords(lower, style_dimensions[d].neg_keywords))
                  neg_counts[d]++;
            }
         }
         sqlite3_reset(stmt);
      }
   }

   /* Generate preference memories for dimensions with sufficient signal */
   int generated = 0;
   for (size_t d = 0; d < STYLE_DIMENSION_COUNT; d++)
   {
      char key[128];
      snprintf(key, sizeof(key), "style_%s", style_dimensions[d].dimension);

      if (neg_counts[d] >= STYLE_MATCH_THRESHOLD && style_dimensions[d].neg_preference)
      {
         char content[512];
         snprintf(content, sizeof(content), "%s. Evidence: %d feedback signal(s).",
                  style_dimensions[d].neg_preference, neg_counts[d]);
         memory_insert(db, TIER_L1, KIND_PREFERENCE, key, content, 0.8, "style_learning", NULL);
         generated++;
      }
      else if (pos_counts[d] >= STYLE_MATCH_THRESHOLD && style_dimensions[d].pos_preference)
      {
         char content[512];
         snprintf(content, sizeof(content), "%s. Evidence: %d feedback signal(s).",
                  style_dimensions[d].pos_preference, pos_counts[d]);
         memory_insert(db, TIER_L1, KIND_PREFERENCE, key, content, 0.9, "style_learning", NULL);
         generated++;
      }
   }

   return generated;
}

/* --- Pre-fetch --- */

int memory_prefetch_projects(sqlite3 *db, char projects[][128], int max_projects)
{
   if (!db)
      return 0;

   /* Top projects by window_files frequency in recent sessions */
   static const char *sql = "SELECT DISTINCT"
                            " SUBSTR(wf.file_path, 1, INSTR(wf.file_path, '/') - 1)"
                            " AS project"
                            " FROM window_files wf"
                            " JOIN windows w ON w.id = wf.window_id"
                            " WHERE w.created_at > datetime('now', '-7 days')"
                            " AND INSTR(wf.file_path, '/') > 0"
                            " GROUP BY project"
                            " ORDER BY COUNT(*) DESC"
                            " LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int(stmt, 1, max_projects);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max_projects)
   {
      const char *p = (const char *)sqlite3_column_text(stmt, 0);
      if (p && p[0])
      {
         snprintf(projects[count], 128, "%s", p);
         count++;
      }
   }
   sqlite3_reset(stmt);
   return count;
}

/* --- Provenance Surfacing --- */

int memory_get_provenance(sqlite3 *db, int64_t memory_id, provenance_entry_t *out, int max)
{
   static const char *sql = "SELECT id, memory_id, session_id, action, details, created_at"
                            " FROM memory_provenance WHERE memory_id = ?"
                            " ORDER BY created_at ASC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int64(stmt, 1, memory_id);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      provenance_entry_t *e = &out[count];
      e->id = sqlite3_column_int64(stmt, 0);
      e->memory_id = sqlite3_column_int64(stmt, 1);
      const char *s = (const char *)sqlite3_column_text(stmt, 2);
      snprintf(e->session_id, sizeof(e->session_id), "%s", s ? s : "");
      s = (const char *)sqlite3_column_text(stmt, 3);
      snprintf(e->action, sizeof(e->action), "%s", s ? s : "");
      s = (const char *)sqlite3_column_text(stmt, 4);
      snprintf(e->details, sizeof(e->details), "%s", s ? s : "");
      s = (const char *)sqlite3_column_text(stmt, 5);
      snprintf(e->created_at, sizeof(e->created_at), "%s", s ? s : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

char *memory_get_latest_supersede_date(sqlite3 *db, int64_t memory_id)
{
   static const char *sql = "SELECT created_at FROM memory_provenance"
                            " WHERE memory_id = ? AND action = 'supersede'"
                            " ORDER BY created_at DESC LIMIT 1";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return NULL;

   sqlite3_bind_int64(stmt, 1, memory_id);
   char *result = NULL;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *ts = (const char *)sqlite3_column_text(stmt, 0);
      if (ts)
         result = safe_strdup(ts);
   }
   sqlite3_reset(stmt);
   return result;
}

/* --- Memory-to-Memory Linking --- */

int memory_link_create(sqlite3 *db, int64_t source_id, int64_t target_id, const char *relation)
{
   if (!db || !relation || source_id <= 0 || target_id <= 0)
      return -1;

   static const char *sql = "INSERT OR IGNORE INTO memory_links (source_id, target_id, relation)"
                            " VALUES (?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, source_id);
   sqlite3_bind_int64(stmt, 2, target_id);
   sqlite3_bind_text(stmt, 3, relation, -1, SQLITE_TRANSIENT);

   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_reset(stmt);
   return rc;
}

int memory_link_query(sqlite3 *db, int64_t memory_id, memory_link_t *out, int max)
{
   if (!db || !out || max <= 0)
      return 0;

   static const char *sql = "SELECT id, source_id, target_id, relation, created_at"
                            " FROM memory_links"
                            " WHERE source_id = ? OR target_id = ?"
                            " ORDER BY created_at DESC LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_bind_int64(stmt, 1, memory_id);
   sqlite3_bind_int64(stmt, 2, memory_id);
   sqlite3_bind_int(stmt, 3, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      out[count].id = sqlite3_column_int64(stmt, 0);
      out[count].source_id = sqlite3_column_int64(stmt, 1);
      out[count].target_id = sqlite3_column_int64(stmt, 2);
      const char *rel = (const char *)sqlite3_column_text(stmt, 3);
      snprintf(out[count].relation, sizeof(out[count].relation), "%s", rel ? rel : "");
      const char *ts = (const char *)sqlite3_column_text(stmt, 4);
      snprintf(out[count].created_at, sizeof(out[count].created_at), "%s", ts ? ts : "");
      count++;
   }
   sqlite3_reset(stmt);
   return count;
}

int memory_link_delete(sqlite3 *db, int64_t link_id)
{
   if (!db || link_id <= 0)
      return -1;

   static const char *sql = "DELETE FROM memory_links WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, link_id);
   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_reset(stmt);
   return rc;
}
