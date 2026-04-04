/* memory.c: 4-tier memory system (L0-L3), promotion/demotion, context assembly, staleness,
 *           embedding-based retrieval */
#include "aimee.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <regex.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>
#include <unistd.h>

/* --- Precompiled regex patterns (compiled once via pthread_once) --- */

#include <pthread.h>

static pthread_once_t regex_once = PTHREAD_ONCE_INIT;

/* Gate: sensitive content patterns */
#define SENSITIVE_PATTERN_COUNT 4
static regex_t sensitive_re[SENSITIVE_PATTERN_COUNT];
static regex_t sensitive_re_cap[SENSITIVE_PATTERN_COUNT]; /* same patterns, with capture */
static int sensitive_compiled[SENSITIVE_PATTERN_COUNT];

/* Gate: ephemeral content patterns */
static regex_t ephemeral_re[2];
static int ephemeral_compiled[2];

/* Gate: evidence markers */
static regex_t evidence_re;
static int evidence_compiled;

/* Scan: content safety rules */
#define SCAN_RE_MAX 16
static regex_t scan_re[SCAN_RE_MAX];
static int scan_compiled[SCAN_RE_MAX];

static void compile_regex_patterns(void)
{
   static const char *sensitive_patterns[] = {
       "(api[_-]?key|token|secret|password|passwd|credential)[[:space:]]*[:=][[:space:]]*"
       "[^[:space:]]+",
       "AKIA[0-9A-Z]{16}",
       "-----BEGIN[[:space:]](RSA[[:space:]]|EC[[:space:]]|DSA[[:space:]])?PRIVATE[[:space:]]"
       "KEY-----",
       "(social[_. ]security|ssn|date[_. ]of[_. ]birth|dob)[[:space:]]*[:=][[:space:]]*"
       "[^[:space:]]+",
   };
   for (int i = 0; i < SENSITIVE_PATTERN_COUNT; i++)
   {
      sensitive_compiled[i] = (regcomp(&sensitive_re[i], sensitive_patterns[i],
                                       REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0);
      /* Also compile with capture for redaction */
      if (sensitive_compiled[i])
         regcomp(&sensitive_re_cap[i], sensitive_patterns[i], REG_EXTENDED | REG_ICASE);
   }

   ephemeral_compiled[0] =
       (regcomp(&ephemeral_re[0], "[0-9]+ (lines|bytes|files)", REG_EXTENDED | REG_NOSUB) == 0);
   ephemeral_compiled[1] =
       (regcomp(&ephemeral_re[1], "(just now|currently|right now|at the moment)",
                REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0);

   evidence_compiled = (regcomp(&evidence_re,
                                "(/[a-zA-Z0-9_./]+\\.[a-z]+|`[^`]+`|https?://|error:|failed:|"
                                "output:)",
                                REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0);
}

static void ensure_regex_init(void)
{
   pthread_once(&regex_once, compile_regex_patterns);
}

/* --- Row helpers --- */

static void row_to_memory(sqlite3_stmt *stmt, memory_t *m)
{
   m->id = sqlite3_column_int64(stmt, 0);
   const char *tier = (const char *)sqlite3_column_text(stmt, 1);
   const char *kind = (const char *)sqlite3_column_text(stmt, 2);
   const char *key = (const char *)sqlite3_column_text(stmt, 3);
   const char *content = (const char *)sqlite3_column_text(stmt, 4);
   m->confidence = sqlite3_column_double(stmt, 5);
   m->use_count = sqlite3_column_int(stmt, 6);
   const char *lua = (const char *)sqlite3_column_text(stmt, 7);
   const char *cat = (const char *)sqlite3_column_text(stmt, 8);
   const char *uat = (const char *)sqlite3_column_text(stmt, 9);
   const char *src = (const char *)sqlite3_column_text(stmt, 10);

   snprintf(m->tier, sizeof(m->tier), "%s", tier ? tier : "");
   snprintf(m->kind, sizeof(m->kind), "%s", kind ? kind : "");
   snprintf(m->key, sizeof(m->key), "%s", key ? key : "");
   snprintf(m->content, sizeof(m->content), "%s", content ? content : "");
   snprintf(m->last_used_at, sizeof(m->last_used_at), "%s", lua ? lua : "");
   snprintf(m->created_at, sizeof(m->created_at), "%s", cat ? cat : "");
   snprintf(m->updated_at, sizeof(m->updated_at), "%s", uat ? uat : "");
   snprintf(m->source_session, sizeof(m->source_session), "%s", src ? src : "");
}

static void add_provenance(sqlite3 *db, int64_t memory_id, const char *session_id,
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
   DB_STEP_LOG(stmt, "add_provenance");
   sqlite3_reset(stmt);
}

/* --- Write Quality Gates --- */

/* Sensitivity gate: detect secrets, API keys, PII */
static int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap)
{
   if (!content || !content[0])
      return 0; /* not sensitive */

   ensure_regex_init();

   for (int i = 0; i < SENSITIVE_PATTERN_COUNT; i++)
   {
      if (!sensitive_compiled[i])
         continue;
      if (regexec(&sensitive_re[i], content, 0, NULL, 0) != 0)
         continue;

      /* Match found — try to redact using the capture variant */
      regmatch_t m[1];
      if (regexec(&sensitive_re_cap[i], content, 1, m, 0) == 0)
      {
         size_t content_len = strlen(content);
         if (content_len < redacted_cap)
         {
            memcpy(redacted, content, (size_t)m[0].rm_so);
            int pos =
                snprintf(redacted + m[0].rm_so, redacted_cap - (size_t)m[0].rm_so, "[REDACTED]");
            size_t rest_start = (size_t)m[0].rm_eo;
            size_t rest_len = content_len - rest_start;
            if ((size_t)m[0].rm_so + (size_t)pos + rest_len < redacted_cap)
            {
               memcpy(redacted + m[0].rm_so + pos, content + rest_start, rest_len);
               redacted[m[0].rm_so + pos + (int)rest_len] = '\0';
               return 1; /* redactable */
            }
         }
      }
      return 2; /* sensitive but cannot redact cleanly, reject */
   }
   return 0;
}

/* Stability gate: detect ephemeral content */
static int gate_check_ephemeral(const char *content)
{
   if (!content)
      return 0;

   ensure_regex_init();

   for (int i = 0; i < 2; i++)
   {
      if (ephemeral_compiled[i] && regexec(&ephemeral_re[i], content, 0, NULL, 0) == 0)
         return 1;
   }
   return 0;
}

/* Source gate: check if high confidence is grounded in evidence */
static int gate_has_evidence_markers(const char *content)
{
   if (!content)
      return 0;

   ensure_regex_init();
   return evidence_compiled && regexec(&evidence_re, content, 0, NULL, 0) == 0;
}

int memory_gate_check(sqlite3 *db, const char *tier, const char *kind, const char *key,
                      const char *content, double confidence, gate_verdict_t *verdict)
{
   memset(verdict, 0, sizeof(*verdict));
   verdict->result = GATE_ACCEPT;

   if ((!content || !content[0]) && strcmp(tier, TIER_L0) != 0)
   {
      verdict->result = GATE_REJECT;
      snprintf(verdict->reason, sizeof(verdict->reason), "empty content at tier %s", tier);
      return 0;
   }

   if (!content || !content[0])
      return 0; /* L0 scratch allows empty content */

   /* Gate 1: Sensitivity check (secrets, PII) */
   int sens =
       gate_check_sensitive(content, verdict->redacted_content, sizeof(verdict->redacted_content));
   if (sens == 2)
   {
      verdict->result = GATE_REJECT;
      snprintf(verdict->reason, sizeof(verdict->reason),
               "sensitivity: content matches secret/PII pattern");
      return 0;
   }
   if (sens == 1)
   {
      verdict->result = GATE_REDACT;
      snprintf(verdict->reason, sizeof(verdict->reason), "sensitivity: secret/PII redacted");
      return 0;
   }

   /* Gate 2: Stability check (ephemeral content at L1+) */
   if (strcmp(tier, TIER_L0) != 0 && gate_check_ephemeral(content))
   {
      verdict->result = GATE_DOWNGRADE;
      snprintf(verdict->reason, sizeof(verdict->reason),
               "stability: ephemeral content downgraded to L0");
      return 0;
   }

   /* Gate 3: Conflict check (contradiction with high-confidence L2) */
   if (db && key && key[0])
   {
      const char *sql = "SELECT id, content, confidence FROM memories "
                        "WHERE key = ? AND tier = 'L2' AND confidence >= 0.8 "
                        "AND content != ? LIMIT 1";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            double existing_conf = sqlite3_column_double(stmt, 2);
            sqlite3_reset(stmt);

            if (confidence <= existing_conf && strcmp(tier, TIER_L2) == 0)
            {
               verdict->result = GATE_DOWNGRADE;
               snprintf(verdict->reason, sizeof(verdict->reason),
                        "conflict: contradicts L2 memory with confidence %.2f", existing_conf);
               return 0;
            }
         }
         else
         {
            sqlite3_reset(stmt);
         }
      }
   }

   /* Gate 4: Source check (high confidence without evidence, L1+ only) */
   if (strcmp(tier, TIER_L0) != 0 && confidence > 0.9 && !gate_has_evidence_markers(content))
   {
      /* Don't downgrade/reject, but signal that confidence should be capped.
       * We encode this in the verdict reason so the caller can adjust. */
      snprintf(verdict->reason, sizeof(verdict->reason),
               "source: confidence capped at %.1f (no evidence markers)", GATE_CONFIDENCE_FLOOR);
   }

   (void)kind; /* reserved for kind-specific gates in the future */
   return 0;
}

/* --- Content Safety Scanning --- */

#include <regex.h>

typedef struct
{
   const char *pattern;
   const char *class_name;
   int action;
} scan_rule_t;

static const scan_rule_t scan_rules[] = {
    /* Block: never persist */
    {"-----BEGIN.*PRIVATE KEY-----", "restricted", SCAN_BLOCK},
    {"AKIA[0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z]"
     "[0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z][0-9A-Z]",
     "restricted", SCAN_BLOCK},
    {"ghp_[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]"
     "[A-Za-z0-9][A-Za-z0-9][A-Za-z0-9][A-Za-z0-9]",
     "restricted", SCAN_BLOCK},

    /* Redact: persist with value masked */
    {"(password|passwd|secret|api_key|api-key|apikey|token)[[:space:]]*[:=][[:space:]]*[^[:space:]]"
     "+",
     "restricted", SCAN_REDACT},

    /* Classify: persist but mark sensitive */
    {"[0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9]", "restricted", SCAN_REDACT},
    {"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z][A-Za-z]+", "sensitive", SCAN_CLASSIFY},
    {"(10\\.[0-9]+\\.[0-9]+\\.[0-9]+|192\\.168\\.[0-9]+\\.[0-9]+)", "sensitive", SCAN_CLASSIFY},

    {NULL, NULL, 0},
};

static pthread_once_t scan_regex_once = PTHREAD_ONCE_INIT;

static int scan_rule_count;

static void compile_scan_regex(void)
{
   for (int i = 0; scan_rules[i].pattern; i++)
   {
      if (i >= SCAN_RE_MAX)
         break;
      scan_compiled[i] =
          (regcomp(&scan_re[i], scan_rules[i].pattern, REG_EXTENDED | REG_ICASE) == 0);
      scan_rule_count = i + 1;
   }
}

const char *memory_scan_content(char *content, size_t content_len)
{
   if (!content || !content[0])
      return "normal";

   pthread_once(&scan_regex_once, compile_scan_regex);

   const char *highest_class = "normal";
   int class_rank = 0; /* 0=normal, 1=sensitive, 2=restricted */

   for (int i = 0; i < scan_rule_count; i++)
   {
      if (!scan_compiled[i])
         continue;

      regmatch_t match;
      if (regexec(&scan_re[i], content, 1, &match, 0) == 0)
      {
         if (scan_rules[i].action == SCAN_BLOCK)
            return NULL; /* block */

         if (scan_rules[i].action == SCAN_REDACT && match.rm_so >= 0)
         {
            /* Find the value portion (after = or :) and redact */
            for (int j = match.rm_so; j < match.rm_eo && j < (int)content_len; j++)
            {
               if (content[j] == '=' || content[j] == ':')
               {
                  /* Skip whitespace after delimiter */
                  j++;
                  while (j < match.rm_eo && content[j] == ' ')
                     j++;
                  /* Redact the value */
                  int redact_start = j;
                  int redact_end = match.rm_eo;
                  if (redact_end - redact_start > 0)
                  {
                     const char *redacted = "[REDACTED]";
                     int rlen = (int)strlen(redacted);
                     int vlen = redact_end - redact_start;
                     if (rlen <= vlen)
                     {
                        memcpy(content + redact_start, redacted, rlen);
                        memmove(content + redact_start + rlen, content + redact_end,
                                content_len - redact_end + 1);
                     }
                  }
                  break;
               }
            }
         }

         int rank = (strcmp(scan_rules[i].class_name, "restricted") == 0)  ? 2
                    : (strcmp(scan_rules[i].class_name, "sensitive") == 0) ? 1
                                                                           : 0;
         if (rank > class_rank)
         {
            class_rank = rank;
            highest_class = scan_rules[i].class_name;
         }
      }
   }

   return highest_class;
}

int memory_insert_ephemeral(sqlite3 *db, const char *key, const char *content,
                            const char *session_id)
{
   if (!db || !key)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT INTO memories (tier, kind, key, content, confidence,"
                            " use_count, last_used_at, source_session, created_at, updated_at,"
                            " sensitivity, valid_from)"
                            " VALUES ('L0', 'scratch', ?, ?, 0.5, 0, ?, ?, ?, ?, 'restricted',"
                            " datetime('now', '+1 hour'))";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, content ? content : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);

   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_reset(stmt);
   return rc;
}

/* --- Core CRUD --- */

int memory_insert(sqlite3 *db, const char *tier, const char *kind, const char *key,
                  const char *content, double confidence, const char *session_id, memory_t *out)
{
   if (!db || !tier || !kind || !key)
      return -1;

   /* Run write quality gates */
   gate_verdict_t verdict;
   memory_gate_check(db, tier, kind, key, content, confidence, &verdict);

   switch (verdict.result)
   {
   case GATE_REJECT:
      /* Log the rejection in provenance (use id=0 since no memory was created) */
      return -1;
   case GATE_DOWNGRADE:
      tier = TIER_L0;
      kind = KIND_SCRATCH;
      break;
   case GATE_REDACT:
      content = verdict.redacted_content;
      break;
   case GATE_ACCEPT:
      break;
   }

   /* Source gate: cap confidence for user-facing fact insertions without evidence.
    * Internal callers (promotion, learning) pass empty session_id and set confidence
    * based on actual signal, so we only apply this to interactive sessions. */
   (void)0; /* source gate logs reason but does not modify confidence at insert time;
             * promotion/lifecycle will validate confidence changes separately */

   /* Content safety scan */
   char safe_content[2048];
   const char *sensitivity = "normal";
   if (content && content[0])
   {
      snprintf(safe_content, sizeof(safe_content), "%s", content);
      sensitivity = memory_scan_content(safe_content, strlen(safe_content));
      if (!sensitivity) /* blocked */
         return -1;
      content = safe_content;
   }

   /* Normalize key */
   char norm_key[512];
   normalize_key(key, norm_key, sizeof(norm_key));

   char ts[32];
   now_utc(ts, sizeof(ts));

   /* Check for exact key match */
   {
      static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                               " last_used_at, created_at, updated_at, source_session"
                               " FROM memories WHERE key = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return -1;

      sqlite3_bind_text(stmt, 1, norm_key, -1, SQLITE_TRANSIENT);

      if (sqlite3_step(stmt) == SQLITE_ROW)
      {
         /* Merge: keep higher confidence, increment use_count */
         int64_t existing_id = sqlite3_column_int64(stmt, 0);
         double old_conf = sqlite3_column_double(stmt, 5);
         int old_use = sqlite3_column_int(stmt, 6);
         sqlite3_reset(stmt);

         double new_conf = confidence > old_conf ? confidence : old_conf;
         int new_use = old_use + 1;

         static const char *upd = "UPDATE memories SET content = ?, confidence = ?,"
                                  " use_count = ?, last_used_at = ?, updated_at = ?"
                                  " WHERE id = ?";
         sqlite3_stmt *us = db_prepare(db, upd);
         if (!us)
            return -1;

         sqlite3_bind_text(us, 1, content ? content : "", -1, SQLITE_TRANSIENT);
         sqlite3_bind_double(us, 2, new_conf);
         sqlite3_bind_int(us, 3, new_use);
         sqlite3_bind_text(us, 4, ts, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(us, 5, ts, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(us, 6, existing_id);
         DB_STEP_LOG(us, "memory_insert");
         sqlite3_reset(us);

         add_provenance(db, existing_id, session_id, "merge", "exact key match");

         if (out)
            memory_get(db, existing_id, out);
         cache_invalidate(db);
         return 0;
      }
      sqlite3_reset(stmt);
   }

   /* Check trigram near-duplicate against same-kind memories */
   {
      static const char *sql = "SELECT id, key, confidence, use_count"
                               " FROM memories WHERE kind = ? LIMIT 100";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return -1;

      sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);

      int64_t dup_id = 0;
      double dup_conf = 0.0;
      int dup_use = 0;

      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *cand_key = (const char *)sqlite3_column_text(stmt, 1);
         if (!cand_key)
            continue;

         double sim = trigram_similarity(norm_key, cand_key);
         if (sim >= DEDUP_THRESHOLD)
         {
            dup_id = sqlite3_column_int64(stmt, 0);
            dup_conf = sqlite3_column_double(stmt, 2);
            dup_use = sqlite3_column_int(stmt, 3);
            break;
         }
      }
      sqlite3_reset(stmt);

      if (dup_id > 0)
      {
         /* Merge into near-duplicate */
         double new_conf = confidence > dup_conf ? confidence : dup_conf;
         int new_use = dup_use + 1;

         static const char *upd = "UPDATE memories SET content = ?, confidence = ?,"
                                  " use_count = ?, last_used_at = ?, updated_at = ?"
                                  " WHERE id = ?";
         sqlite3_stmt *us = db_prepare(db, upd);
         if (!us)
            return -1;

         sqlite3_bind_text(us, 1, content ? content : "", -1, SQLITE_TRANSIENT);
         sqlite3_bind_double(us, 2, new_conf);
         sqlite3_bind_int(us, 3, new_use);
         sqlite3_bind_text(us, 4, ts, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(us, 5, ts, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int64(us, 6, dup_id);
         DB_STEP_LOG(us, "memory");
         sqlite3_reset(us);

         add_provenance(db, dup_id, session_id, "merge", "trigram near-duplicate");

         if (out)
            memory_get(db, dup_id, out);
         cache_invalidate(db);
         return 0;
      }
   }

   /* Truly new: INSERT */
   {
      static const char *sql = "INSERT INTO memories (tier, kind, key, content, confidence,"
                               " use_count, last_used_at, source_session, created_at,"
                               " updated_at, sensitivity)"
                               " VALUES (?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?)";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
         return -1;

      sqlite3_bind_text(stmt, 1, tier, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 3, norm_key, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 4, content ? content : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_double(stmt, 5, confidence);
      sqlite3_bind_text(stmt, 6, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 7, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 8, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 9, ts, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 10, sensitivity, -1, SQLITE_TRANSIENT);

      int step_rc = sqlite3_step(stmt);
      if (step_rc != SQLITE_DONE)
      {
         fprintf(stderr, "aimee: memory insert failed: %s (rc=%d)\n", sqlite3_errmsg(db), step_rc);
         sqlite3_reset(stmt);
         return -1;
      }

      int64_t new_id = sqlite3_last_insert_rowid(db);
      sqlite3_reset(stmt);

      add_provenance(db, new_id, session_id, "insert", NULL);
      memory_auto_tag_workspace(db, new_id, norm_key, content ? content : "");

      /* Auto-embed if embedding command is configured (background, non-fatal) */
      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         if (embed_cfg.embedding_command[0])
         {
            pid_t epid = fork();
            if (epid == 0)
            {
               memory_embed(db, new_id, embed_cfg.embedding_command);
               _exit(0);
            }
            if (epid > 0)
               waitpid(epid, NULL, WNOHANG);
         }
      }

      if (out)
         memory_get(db, new_id, out);
      cache_invalidate(db);
      return 0;
   }
}

int memory_get(sqlite3 *db, int64_t id, memory_t *out)
{
   static const char *sql = "SELECT id, tier, kind, key, content, confidence, use_count,"
                            " last_used_at, created_at, updated_at, source_session"
                            " FROM memories WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      row_to_memory(stmt, out);
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}

int memory_touch(sqlite3 *db, int64_t id)
{
   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "UPDATE memories SET use_count = use_count + 1,"
                            " last_used_at = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int64(stmt, 2, id);
   DB_STEP_LOG(stmt, "memory_touch");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   return changes > 0 ? 0 : -1;
}

int memory_list(sqlite3 *db, const char *tier, const char *kind, int limit, memory_t *out, int max)
{
   char query[MAX_QUERY_LEN];
   int pos = 0;
   pos += snprintf(query + pos, sizeof(query) - pos,
                   "SELECT id, tier, kind, key, content, confidence, use_count,"
                   " last_used_at, created_at, updated_at, source_session"
                   " FROM memories WHERE 1=1");
   if (pos >= (int)sizeof(query))
      pos = (int)sizeof(query) - 1;

   int bind_idx = 1;
   int tier_bind = 0, kind_bind = 0;
   if (tier && tier[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND tier = ?");
      if (pos >= (int)sizeof(query))
         pos = (int)sizeof(query) - 1;
      tier_bind = bind_idx++;
   }
   if (kind && kind[0])
   {
      pos += snprintf(query + pos, sizeof(query) - pos, " AND kind = ?");
      if (pos >= (int)sizeof(query))
         pos = (int)sizeof(query) - 1;
      kind_bind = bind_idx++;
   }

   pos += snprintf(query + pos, sizeof(query) - pos, " ORDER BY updated_at DESC");
   if (pos >= (int)sizeof(query))
      pos = (int)sizeof(query) - 1;

   if (limit > 0)
      snprintf(query + pos, sizeof(query) - pos, " LIMIT %d", limit);

   /* Cannot use db_prepare with dynamic SQL, so prepare directly */
   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   if (tier_bind)
      sqlite3_bind_text(stmt, tier_bind, tier, -1, SQLITE_TRANSIENT);
   if (kind_bind)
      sqlite3_bind_text(stmt, kind_bind, kind, -1, SQLITE_TRANSIENT);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      row_to_memory(stmt, &out[count]);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int memory_delete(sqlite3 *db, int64_t id)
{
   /* Delete provenance first (CASCADE should handle, but be explicit) */
   static const char *del_prov = "DELETE FROM memory_provenance WHERE memory_id = ?";
   sqlite3_stmt *stmt = db_prepare(db, del_prov);
   if (stmt)
   {
      sqlite3_bind_int64(stmt, 1, id);
      DB_STEP_LOG(stmt, "memory_delete");
      sqlite3_reset(stmt);
   }

   static const char *sql = "DELETE FROM memories WHERE id = ?";
   stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, id);
   DB_STEP_LOG(stmt, "memory_delete");
   int changes = sqlite3_changes(db);
   sqlite3_reset(stmt);
   if (changes > 0)
      cache_invalidate(db);
   return changes > 0 ? 0 : -1;
}

int memory_stats(sqlite3 *db, memory_stats_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Counts per tier */
   static const char *tier_sql = "SELECT tier, COUNT(*) FROM memories GROUP BY tier";
   sqlite3_stmt *stmt = db_prepare(db, tier_sql);
   if (stmt)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *t = (const char *)sqlite3_column_text(stmt, 0);
         int c = sqlite3_column_int(stmt, 1);
         if (!t)
            continue;
         if (strcmp(t, TIER_L0) == 0)
            out->tier_counts[0] = c;
         else if (strcmp(t, TIER_L1) == 0)
            out->tier_counts[1] = c;
         else if (strcmp(t, TIER_L2) == 0)
            out->tier_counts[2] = c;
         else if (strcmp(t, TIER_L3) == 0)
            out->tier_counts[3] = c;
         out->total += c;
      }
      sqlite3_reset(stmt);
   }

   /* Counts per kind */
   static const char *kind_sql = "SELECT kind, COUNT(*) FROM memories GROUP BY kind";
   stmt = db_prepare(db, kind_sql);
   if (stmt)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *k = (const char *)sqlite3_column_text(stmt, 0);
         int c = sqlite3_column_int(stmt, 1);
         if (!k)
            continue;
         if (strcmp(k, KIND_FACT) == 0)
            out->kind_counts[0] = c;
         else if (strcmp(k, KIND_PREFERENCE) == 0)
            out->kind_counts[1] = c;
         else if (strcmp(k, KIND_DECISION) == 0)
            out->kind_counts[2] = c;
         else if (strcmp(k, KIND_EPISODE) == 0)
            out->kind_counts[3] = c;
         else if (strcmp(k, KIND_TASK) == 0)
            out->kind_counts[4] = c;
         else if (strcmp(k, KIND_SCRATCH) == 0)
            out->kind_counts[5] = c;
         else if (strcmp(k, KIND_PROCEDURE) == 0)
            out->kind_counts[6] = c;
         else if (strcmp(k, KIND_POLICY) == 0)
            out->kind_counts[7] = c;
      }
      sqlite3_reset(stmt);
   }

   /* Unresolved conflicts */
   static const char *conf_sql = "SELECT COUNT(*) FROM memory_conflicts WHERE resolved = 0";
   stmt = db_prepare(db, conf_sql);
   if (stmt)
   {
      if (sqlite3_step(stmt) == SQLITE_ROW)
         out->conflicts = sqlite3_column_int(stmt, 0);
      sqlite3_reset(stmt);
   }

   return 0;
}

/* --- Search --- */

/* Time decay: exponential decay based on days since last use */
static double time_decay(const char *last_used)
{
   if (!last_used || !last_used[0])
      return 0.5;

   /* Parse YYYY-MM-DD from timestamp */
   struct tm tm_used;
   memset(&tm_used, 0, sizeof(tm_used));
   if (sscanf(last_used, "%d-%d-%d", &tm_used.tm_year, &tm_used.tm_mon, &tm_used.tm_mday) < 3)
      return 0.5;

   tm_used.tm_year -= 1900;
   tm_used.tm_mon -= 1;
   time_t t_used = timegm(&tm_used);
   time_t t_now = time(NULL);

   double days = difftime(t_now, t_used) / 86400.0;
   if (days < 0)
      days = 0;

   return exp(-0.02 * days);
}

int memory_search(sqlite3 *db, char **clusters, int cluster_count, int limit, search_result_t *out,
                  int max)
{
   if (!db || !clusters || cluster_count <= 0)
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   /* Parse cluster terms and load stopwords */
   char *all_terms[256];
   int term_count = 0;

   for (int ci = 0; ci < cluster_count && term_count < 256; ci++)
   {
      char *tok_buf[64];
      int tc = tokenize_for_search(clusters[ci], tok_buf, 64);
      for (int t = 0; t < tc && term_count < 256; t++)
         all_terms[term_count++] = tok_buf[t];
   }

   if (term_count == 0)
      return 0;

   /* Load promoted stopwords */
   static const char *stop_sql = "SELECT word FROM stopwords";
   sqlite3_stmt *stop_stmt = db_prepare(db, stop_sql);
   char stopwords[256][32];
   int stop_count = 0;
   if (stop_stmt)
   {
      while (sqlite3_step(stop_stmt) == SQLITE_ROW && stop_count < 256)
      {
         const char *w = (const char *)sqlite3_column_text(stop_stmt, 0);
         if (w)
            snprintf(stopwords[stop_count++], 32, "%s", w);
      }
      sqlite3_reset(stop_stmt);
   }

   /* Filter out stopwords from terms */
   char *filtered[256];
   int fcount = 0;
   for (int i = 0; i < term_count; i++)
   {
      int is_stop = 0;
      for (int s = 0; s < stop_count; s++)
      {
         if (strcasecmp(all_terms[i], stopwords[s]) == 0)
         {
            is_stop = 1;
            break;
         }
      }
      if (!is_stop)
         filtered[fcount++] = all_terms[i];
      else
         free(all_terms[i]);
   }

   if (fcount == 0)
      return 0;

   /* Build parameterized IN clause for window_terms query */
   char placeholders[MAX_QUERY_LEN];
   int ppos = 0;
   for (int i = 0; i < fcount; i++)
   {
      if (i > 0)
         ppos += snprintf(placeholders + ppos, sizeof(placeholders) - ppos, ",");
      ppos += snprintf(placeholders + ppos, sizeof(placeholders) - ppos, "?");
   }

   /* Candidate windows: JOIN window_terms */
   char query[MAX_QUERY_LEN];
   snprintf(query, sizeof(query),
            "SELECT w.id, w.session_id, w.seq, w.summary, w.created_at,"
            " COUNT(DISTINCT wt.term) AS match_count"
            " FROM windows w"
            " JOIN window_terms wt ON wt.window_id = w.id"
            " WHERE LOWER(wt.term) IN (%s)"
            " GROUP BY w.id"
            " ORDER BY match_count DESC"
            " LIMIT 200",
            placeholders);

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
   {
      for (int i = 0; i < fcount; i++)
         free(filtered[i]);
      return 0;
   }
   for (int i = 0; i < fcount; i++)
      sqlite3_bind_text(stmt, i + 1, filtered[i], -1, SQLITE_TRANSIENT);

   /* Score candidates */
   typedef struct
   {
      int64_t window_id;
      char session_id[128];
      int seq;
      char summary[1024];
      char created_at[32];
      int match_count;
      double score;
   } candidate_t;

   candidate_t candidates[200];
   int cand_count = 0;

   while (sqlite3_step(stmt) == SQLITE_ROW && cand_count < 200)
   {
      candidate_t *c = &candidates[cand_count];
      c->window_id = sqlite3_column_int64(stmt, 0);
      const char *sid = (const char *)sqlite3_column_text(stmt, 1);
      c->seq = sqlite3_column_int(stmt, 2);
      const char *sum = (const char *)sqlite3_column_text(stmt, 3);
      const char *cat = (const char *)sqlite3_column_text(stmt, 4);
      c->match_count = sqlite3_column_int(stmt, 5);

      snprintf(c->session_id, sizeof(c->session_id), "%s", sid ? sid : "");
      snprintf(c->summary, sizeof(c->summary), "%s", sum ? sum : "");
      snprintf(c->created_at, sizeof(c->created_at), "%s", cat ? cat : "");

      /* Score: term match * cluster boost * time decay */
      double term_score = (double)c->match_count / (double)fcount;
      double t_decay = time_decay(c->created_at);
      c->score = term_score * 2.0 * t_decay;

      cand_count++;
   }
   sqlite3_finalize(stmt);

   /* Merge FTS5 scores if available */
   if (db_fts5_available(db))
   {
      /* Build FTS query string; quote terms to prevent FTS operator injection */
      char fts_query[MAX_QUERY_LEN];
      int fpos = 0;
      for (int i = 0; i < fcount; i++)
      {
         if (i > 0)
            fpos += snprintf(fts_query + fpos, sizeof(fts_query) - (size_t)fpos, " OR ");
         fpos +=
             snprintf(fts_query + fpos, sizeof(fts_query) - (size_t)fpos, "\"%s\"", filtered[i]);
      }

      static const char *fts_sql = "SELECT rowid, rank FROM window_fts WHERE window_fts MATCH ?";
      sqlite3_stmt *fts_stmt = db_prepare(db, fts_sql);
      if (fts_stmt)
      {
         sqlite3_bind_text(fts_stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
         while (sqlite3_step(fts_stmt) == SQLITE_ROW)
         {
            int64_t rid = sqlite3_column_int64(fts_stmt, 0);
            double rank = sqlite3_column_double(fts_stmt, 1);
            /* rank is negative (lower = better), normalize; clamp denominator to avoid div-by-zero
             */
            double denom = 1.0 - rank;
            if (denom < 0.001)
               denom = 0.001;
            double fts_score = 1.0 / denom;

            for (int i = 0; i < cand_count; i++)
            {
               if (candidates[i].window_id == rid)
               {
                  candidates[i].score += fts_score;
                  break;
               }
            }
         }
         sqlite3_reset(fts_stmt);
      }
   }

   /* Sort by score descending */
   for (int i = 0; i < cand_count - 1; i++)
   {
      for (int j = i + 1; j < cand_count; j++)
      {
         if (candidates[j].score > candidates[i].score)
         {
            candidate_t tmp = candidates[i];
            candidates[i] = candidates[j];
            candidates[j] = tmp;
         }
      }
   }

   /* Take top N */
   int result_count = cand_count < limit ? cand_count : limit;

   /* Zero-initialize output before populating */
   memset(out, 0, (size_t)result_count * sizeof(out[0]));

   /* Batch fetch files via temp table */
   sqlite3_exec(db,
                "CREATE TEMP TABLE IF NOT EXISTS _search_ids"
                " (wid INTEGER)",
                NULL, NULL, NULL);
   sqlite3_exec(db, "DELETE FROM _search_ids", NULL, NULL, NULL);

   for (int i = 0; i < result_count; i++)
   {
      static const char *ins_sql = "INSERT INTO _search_ids (wid) VALUES (?)";
      sqlite3_stmt *is = db_prepare(db, ins_sql);
      if (is)
      {
         sqlite3_bind_int64(is, 1, candidates[i].window_id);
         DB_STEP_LOG(is, "memory");
         sqlite3_reset(is);
      }
   }

   /* Fetch files for all results */
   static const char *files_sql = "SELECT wf.window_id, wf.file_path FROM window_files wf"
                                  " JOIN _search_ids s ON s.wid = wf.window_id";
   sqlite3_stmt *fstmt = db_prepare(db, files_sql);

   /* Build a quick file map */
   if (fstmt)
   {
      while (sqlite3_step(fstmt) == SQLITE_ROW)
      {
         int64_t wid = sqlite3_column_int64(fstmt, 0);
         const char *fp = (const char *)sqlite3_column_text(fstmt, 1);
         if (!fp)
            continue;

         for (int i = 0; i < result_count; i++)
         {
            if (candidates[i].window_id == wid && out[i].file_count < 32)
            {
               snprintf(out[i].files[out[i].file_count], MAX_PATH_LEN, "%s", fp);
               out[i].file_count++;
               break;
            }
         }
      }
      sqlite3_reset(fstmt);
   }

   sqlite3_exec(db, "DROP TABLE IF EXISTS _search_ids", NULL, NULL, NULL);

   /* Apply graph boost from entity_edges */
   static const char *edge_sql = "SELECT target, weight FROM entity_edges"
                                 " WHERE source = ? LIMIT 50";
   sqlite3_stmt *edge_stmt = db_prepare(db, edge_sql);

   if (edge_stmt)
   {
      for (int i = 0; i < result_count; i++)
      {
         double boost = 0.0;
         /* Check if any query term has edges to result content */
         for (int t = 0; t < fcount; t++)
         {
            sqlite3_bind_text(edge_stmt, 1, filtered[t], -1, SQLITE_TRANSIENT);
            while (sqlite3_step(edge_stmt) == SQLITE_ROW)
            {
               const char *target = (const char *)sqlite3_column_text(edge_stmt, 0);
               int w = sqlite3_column_int(edge_stmt, 1);
               if (!target)
                  continue;

               /* Check if target appears in summary */
               if (strstr(candidates[i].summary, target))
                  boost += 0.1 * w;
            }
            sqlite3_reset(edge_stmt);
         }
         candidates[i].score += boost;
      }
   }

   /* Re-sort after boost */
   for (int i = 0; i < result_count - 1; i++)
   {
      for (int j = i + 1; j < result_count; j++)
      {
         if (candidates[j].score > candidates[i].score)
         {
            candidate_t tmp = candidates[i];
            candidates[i] = candidates[j];
            candidates[j] = tmp;

            /* Swap file data too */
            search_result_t ftmp = out[i];
            out[i] = out[j];
            out[j] = ftmp;
         }
      }
   }

   /* Fill output */
   for (int i = 0; i < result_count; i++)
   {
      snprintf(out[i].session_id, sizeof(out[i].session_id), "%s", candidates[i].session_id);
      out[i].seq = candidates[i].seq;
      snprintf(out[i].summary, sizeof(out[i].summary), "%s", candidates[i].summary);
      out[i].score = candidates[i].score;
      out[i].start_line = 0;
      out[i].end_line = 0;
      out[i].file_path[0] = '\0';
   }

   /* Free terms */
   for (int i = 0; i < fcount; i++)
      free(filtered[i]);

   return result_count;
}

/* --- Fact Search --- */

static int memory_find_facts_like(sqlite3 *db, const char *query, int limit, memory_t *out, int max)
{
   static const char *sql =
       "SELECT id, tier, kind, key, content, confidence, use_count,"
       " last_used_at, created_at, updated_at, source_session"
       " FROM memories"
       " WHERE (LOWER(key) LIKE '%' || LOWER(?) || '%'"
       "    OR LOWER(content) LIKE '%' || LOWER(?) || '%')"
       " ORDER BY CASE tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 WHEN 'L1' THEN 2 ELSE 3 END,"
       "          use_count DESC, confidence DESC"
       " LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, query, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, limit);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      row_to_memory(stmt, &out[count]);
      count++;
   }
   sqlite3_finalize(stmt);
   return count;
}

int memory_find_facts(sqlite3 *db, const char *query, int limit, memory_t *out, int max)
{
   if (!db || !query || !query[0])
      return 0;
   if (limit <= 0)
      limit = 20;
   if (limit > max)
      limit = max;

   /* Try FTS5 first */
   static const char *fts_sql =
       "SELECT m.id, m.tier, m.kind, m.key, m.content, m.confidence,"
       " m.use_count, m.last_used_at, m.created_at, m.updated_at, m.source_session"
       " FROM memories_fts fts"
       " JOIN memories m ON m.id = fts.rowid"
       " WHERE memories_fts MATCH ?"
       " ORDER BY CASE m.tier WHEN 'L3' THEN 0 WHEN 'L2' THEN 1 WHEN 'L1' THEN 2 ELSE 3 END,"
       "          m.use_count DESC, m.confidence DESC"
       " LIMIT ?";

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, fts_sql, -1, &stmt, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text(stmt, 1, query, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 2, limit);

      int count = 0;
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW || rc == SQLITE_DONE)
      {
         /* FTS5 query succeeded */
         if (rc == SQLITE_ROW)
         {
            row_to_memory(stmt, &out[count++]);
            while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
            {
               row_to_memory(stmt, &out[count]);
               count++;
            }
         }
         sqlite3_finalize(stmt);
         return count;
      }
      /* FTS5 MATCH failed (bad syntax, etc.) — fall through to LIKE */
      sqlite3_finalize(stmt);
   }

   /* Fallback: LIKE search */
   return memory_find_facts_like(db, query, limit, out, max);
}

/* --- Workspace Scoping --- */

int memory_tag_workspace(sqlite3 *db, int64_t memory_id, const char *workspace)
{
   if (!db || memory_id <= 0 || !workspace || !workspace[0])
      return -1;

   static const char *sql = "INSERT OR IGNORE INTO memory_workspaces (memory_id, workspace)"
                            " VALUES (?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, memory_id);
   sqlite3_bind_text(stmt, 2, workspace, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "memory_tag_workspace");
   sqlite3_reset(stmt);
   return 0;
}

/* Keywords that indicate shared/cross-cutting infrastructure knowledge */
static const char *shared_keywords[] = {
    "network", "deploy",   "auth",    "postgres", "spire",    "proxmox", "cert",     "tls",
    "ssl",     "firewall", "gateway", "dns",      "database", "backup",  "security", NULL};

int memory_auto_tag_workspace(sqlite3 *db, int64_t memory_id, const char *key, const char *content)
{
   if (!db || memory_id <= 0)
      return -1;

   /* Tag with current workspace from cwd */
   char cwd[MAX_PATH_LEN];
   if (getcwd(cwd, sizeof(cwd)))
   {
      config_t cfg;
      config_load(&cfg);
      for (int i = 0; i < cfg.workspace_count; i++)
      {
         size_t wlen = strlen(cfg.workspaces[i]);
         if (wlen > 0 && strncmp(cwd, cfg.workspaces[i], wlen) == 0 &&
             (cwd[wlen] == '/' || cwd[wlen] == '\0'))
         {
            const char *slash = strrchr(cfg.workspaces[i], '/');
            const char *ws_name = slash ? slash + 1 : cfg.workspaces[i];
            memory_tag_workspace(db, memory_id, ws_name);
            break;
         }
      }
   }

   /* Check for shared keywords in key and content */
   char lower_buf[1024];
   int li = 0;
   const char *texts[] = {key, content, NULL};
   for (int t = 0; texts[t] && li < (int)sizeof(lower_buf) - 1; t++)
   {
      for (int i = 0; texts[t][i] && li < (int)sizeof(lower_buf) - 1; i++)
         lower_buf[li++] = (char)tolower((unsigned char)texts[t][i]);
      lower_buf[li++] = ' ';
   }
   lower_buf[li] = '\0';

   for (int i = 0; shared_keywords[i]; i++)
   {
      if (strstr(lower_buf, shared_keywords[i]))
      {
         memory_tag_workspace(db, memory_id, SHARED_WORKSPACE);
         return 0;
      }
   }

   return 0;
}
/* --- Embedding Retrieval --- */

double cosine_similarity(const float *a, const float *b, int dim)
{
   double dot = 0.0, na = 0.0, nb = 0.0;
   for (int i = 0; i < dim; i++)
   {
      dot += (double)a[i] * (double)b[i];
      na += (double)a[i] * (double)a[i];
      nb += (double)b[i] * (double)b[i];
   }
   double denom = sqrt(na) * sqrt(nb);
   return denom > 1e-9 ? dot / denom : 0.0;
}

/* Run embedding command: pipes text on stdin, reads JSON float array from stdout. */
int memory_embed_text(const char *text, const char *command, float *out, int max_dim)
{
   if (!text || !command || !command[0] || !out || max_dim <= 0)
      return 0;

   /* Build shell command with safe pipe */
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "%s", command);

   /* Use popen-style fork/exec with stdin pipe */
   int stdin_pipe[2], stdout_pipe[2];
   if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0)
      return 0;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      return 0;
   }

   if (pid == 0)
   {
      /* Child: redirect stdin/stdout */
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      dup2(stdin_pipe[0], STDIN_FILENO);
      dup2(stdout_pipe[1], STDOUT_FILENO);
      close(stdin_pipe[0]);
      close(stdout_pipe[1]);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   /* Parent: write text to stdin, read JSON from stdout */
   close(stdin_pipe[0]);
   close(stdout_pipe[1]);

   size_t text_len = strlen(text);
   write(stdin_pipe[1], text, text_len);
   close(stdin_pipe[1]);

   char buf[EMBED_MAX_OUTPUT];
   size_t total = 0;
   ssize_t n;
   while ((n = read(stdout_pipe[0], buf + total, sizeof(buf) - total - 1)) > 0)
      total += (size_t)n;
   close(stdout_pipe[0]);
   buf[total] = '\0';

   int status;
   waitpid(pid, &status, 0);
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
   {
      fprintf(stderr, "aimee: embedding command failed (exit %d)\n",
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      return 0;
   }

   /* Parse JSON array of floats: [0.1, 0.2, ...] */
   cJSON *arr = cJSON_Parse(buf);
   if (!arr || !cJSON_IsArray(arr))
   {
      cJSON_Delete(arr);
      fprintf(stderr, "aimee: embedding command returned invalid JSON\n");
      return 0;
   }

   int dim = 0;
   cJSON *el;
   cJSON_ArrayForEach(el, arr)
   {
      if (dim >= max_dim)
         break;
      if (cJSON_IsNumber(el))
         out[dim++] = (float)el->valuedouble;
   }
   cJSON_Delete(arr);
   return dim;
}

/* Embed a single memory and store in memory_embeddings. */
int memory_embed(sqlite3 *db, int64_t memory_id, const char *command)
{
   if (!db || memory_id <= 0 || !command || !command[0])
      return -1;

   memory_t mem;
   if (memory_get(db, memory_id, &mem) != 0)
      return -1;

   /* Embed the key + content */
   char text[3072];
   snprintf(text, sizeof(text), "%s: %s", mem.key, mem.content);

   float vec[EMBED_MAX_DIM];
   int dim = memory_embed_text(text, command, vec, EMBED_MAX_DIM);
   if (dim <= 0)
      return -1;

   /* Store as blob */
   char ts[32];
   now_utc(ts, sizeof(ts));

   static const char *sql = "INSERT OR REPLACE INTO memory_embeddings"
                            " (memory_id, embedding, model, created_at)"
                            " VALUES (?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_int64(stmt, 1, memory_id);
   sqlite3_bind_blob(stmt, 2, vec, dim * (int)sizeof(float), SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, command, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, ts, -1, SQLITE_TRANSIENT);

   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_reset(stmt);
   return rc;
}

/* Semantic memory search: embed query, compare against stored embeddings. */
int memory_search_semantic(sqlite3 *db, const char *query, const char *command,
                           search_result_t *out, int max)
{
   if (!db || !query || !command || !command[0] || !out || max <= 0)
      return 0;

   /* Embed the query */
   float qvec[EMBED_MAX_DIM];
   int qdim = memory_embed_text(query, command, qvec, EMBED_MAX_DIM);
   if (qdim <= 0)
      return 0;

   /* Brute-force scan all stored embeddings for L1/L2 memories */
   static const char *sql = "SELECT e.memory_id, e.embedding, m.key, m.content"
                            " FROM memory_embeddings e"
                            " JOIN memories m ON m.id = e.memory_id"
                            " WHERE m.tier IN ('L1', 'L2')";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   typedef struct
   {
      int64_t id;
      double sim;
      char key[512];
      char content[2048];
   } embed_hit_t;

   embed_hit_t hits[256];
   memset(hits, 0, sizeof(hits));
   int hit_count = 0;

   while (sqlite3_step(stmt) == SQLITE_ROW && hit_count < 256)
   {
      const void *blob = sqlite3_column_blob(stmt, 1);
      int blob_bytes = sqlite3_column_bytes(stmt, 1);
      if (!blob || blob_bytes <= 0)
         continue;

      int dim = blob_bytes / (int)sizeof(float);
      if (dim != qdim)
         continue; /* dimension mismatch */

      double sim = cosine_similarity(qvec, (const float *)blob, dim);
      if (sim < EMBED_SIMILARITY_THRESHOLD)
         continue;

      embed_hit_t *h = &hits[hit_count];
      h->id = sqlite3_column_int64(stmt, 0);
      h->sim = sim;
      const char *k = (const char *)sqlite3_column_text(stmt, 2);
      const char *c = (const char *)sqlite3_column_text(stmt, 3);
      snprintf(h->key, sizeof(h->key), "%s", k ? k : "");
      snprintf(h->content, sizeof(h->content), "%s", c ? c : "");
      hit_count++;
   }
   sqlite3_reset(stmt);

   /* Sort by similarity descending */
   for (int i = 0; i < hit_count - 1; i++)
   {
      for (int j = i + 1; j < hit_count; j++)
      {
         if (hits[j].sim > hits[i].sim)
         {
            embed_hit_t tmp = hits[i];
            hits[i] = hits[j];
            hits[j] = tmp;
         }
      }
   }

   int count = hit_count < max ? hit_count : max;
   memset(out, 0, (size_t)count * sizeof(out[0]));
   for (int i = 0; i < count; i++)
   {
      const char *text = hits[i].content[0] ? hits[i].content : hits[i].key;
      snprintf(out[i].summary, sizeof(out[i].summary), "%s", text);
      out[i].score = hits[i].sim;
   }
   return count;
}
