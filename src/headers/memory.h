#ifndef DEC_MEMORY_H
#define DEC_MEMORY_H 1

typedef struct
{
   int64_t id;
   char tier[4];
   char kind[16];
   char key[512];
   char content[2048];
   double confidence;
   int use_count;
   char last_used_at[32];
   char created_at[32];
   char updated_at[32];
   char source_session[128];
} memory_t;

typedef struct
{
   char session_id[128];
   int seq;
   char file_path[MAX_PATH_LEN];
   int start_line;
   int end_line;
   char summary[1024];
   double score;
   char files[32][MAX_PATH_LEN];
   int file_count;
} search_result_t;

typedef struct
{
   int64_t id;
   int64_t memory_a;
   int64_t memory_b;
   char detected_at[32];
   int resolved;
   char resolution[64];
} conflict_t;

typedef struct
{
   int64_t id;
   char pattern[512];
   char description[1024];
   char source[32];
   char source_ref[64];
   int hit_count;
   double confidence;
} anti_pattern_t;

typedef struct
{
   int tier_counts[4];          /* L0, L1, L2, L3 */
   int kind_counts[KIND_COUNT]; /* fact, pref, decision, episode, task, scratch, procedure, policy
                                 */
   int total;
   int conflicts;
} memory_stats_t;

/* Kind-specific lifecycle configuration (loaded from kind_lifecycle table) */
typedef struct
{
   int promote_use_count;
   double promote_confidence;
   int demote_days;
   double demote_confidence;
   int expire_days;
   double demotion_resistance;
} kind_lifecycle_t;

int kind_lifecycle_load(sqlite3 *db, const char *kind, kind_lifecycle_t *out);

/* --- Write Quality Gates --- */

typedef enum
{
   GATE_ACCEPT,   /* write as requested */
   GATE_DOWNGRADE, /* write to L0 scratch instead of requested tier */
   GATE_REDACT,   /* write with sensitive content masked */
   GATE_REJECT    /* do not write */
} gate_result_t;

typedef struct
{
   gate_result_t result;
   char reason[256];
   char redacted_content[2048]; /* set when result == GATE_REDACT */
} gate_verdict_t;

int memory_gate_check(sqlite3 *db, const char *tier, const char *kind, const char *key,
                      const char *content, double confidence, gate_verdict_t *verdict);

/* --- Tiered Memory --- */
int memory_insert(sqlite3 *db, const char *tier, const char *kind, const char *key,
                  const char *content, double confidence, const char *session_id, memory_t *out);
int memory_get(sqlite3 *db, int64_t id, memory_t *out);
int memory_touch(sqlite3 *db, int64_t id);
int memory_list(sqlite3 *db, const char *tier, const char *kind, int limit, memory_t *out, int max);
int memory_delete(sqlite3 *db, int64_t id);
int memory_stats(sqlite3 *db, memory_stats_t *out);

/* Promotion/demotion/expiry. Returns count of affected memories. */
int memory_promote(sqlite3 *db);
int memory_promote_delegation_patterns(sqlite3 *db);
int memory_demote(sqlite3 *db);
int memory_expire(sqlite3 *db);
int memory_run_maintenance(sqlite3 *db, int *promoted, int *demoted, int *expired);

/* Health metrics: record maintenance cycle stats and prune old data. */
void memory_record_health(sqlite3 *db, int promotions, int demotions, int expirations);
void memory_prune_health(sqlite3 *db);

/* Health query: rolling 7-day stats. */
typedef struct
{
   double contradiction_rate; /* contradictions / new memories over 7 days */
   double promotion_rate;     /* promotions / eligible L1 per cycle */
   double demotion_rate;      /* demotions / total L2 per cycle */
   double staleness;          /* % of L2 facts unused in 30+ days */
   int total_contradictions;
   int total_promotions;
   int total_demotions;
   int total_expirations;
   int cycles;
} memory_health_t;

int memory_query_health(sqlite3 *db, memory_health_t *out);

/* Contradiction audit log. */
void memory_log_contradiction(sqlite3 *db, int64_t mem_a, int64_t mem_b, const char *resolution,
                              const char *details);

/* Provenance surfacing. */
typedef struct
{
   int64_t id;
   int64_t memory_id;
   char session_id[64];
   char action[32];
   char details[512];
   char created_at[32];
} provenance_entry_t;

#define MAX_PROVENANCE_ENTRIES 64

int memory_get_provenance(sqlite3 *db, int64_t memory_id, provenance_entry_t *out, int max);
char *memory_get_latest_supersede_date(sqlite3 *db, int64_t memory_id);

/* Session folding: compress L0 into L1 checkpoint. */
int memory_fold_session(sqlite3 *db, const char *session_id);

/* --- Search --- */
int memory_search(sqlite3 *db, char **clusters, int cluster_count, int limit, search_result_t *out,
                  int max);
int memory_find_facts(sqlite3 *db, const char *query, int limit, memory_t *out, int max);

/* --- Conversation Scanning --- */
int memory_scan_conversations(sqlite3 *db, char dirs[][MAX_PATH_LEN], int dir_count);

/* --- Window Compaction --- */
int memory_compact_windows(sqlite3 *db, int *summary_count, int *fact_count);

/* --- Workspace Scoping --- */
int memory_tag_workspace(sqlite3 *db, int64_t memory_id, const char *workspace);
int memory_auto_tag_workspace(sqlite3 *db, int64_t memory_id, const char *key, const char *content);

/* --- Retrieval Planner --- */

typedef enum
{
   INTENT_DEBUG = 0,
   INTENT_PLAN,
   INTENT_REVIEW,
   INTENT_DEPLOY,
   INTENT_GENERAL
} task_intent_t;

#define NUM_KINDS 8 /* fact, pref, decision, episode, task, scratch, procedure, policy */

typedef struct
{
   task_intent_t intent;
   double kind_budget[NUM_KINDS]; /* fraction per kind (sums to ~1.0) */
   int include_l3;                /* include L3 failure episodes */
   double recency_weight;         /* 0.0=no bias, 1.0=strongly prefer recent */
} retrieval_plan_t;

task_intent_t classify_intent(const char *task_hint);
void retrieval_plan_for_intent(task_intent_t intent, retrieval_plan_t *plan);

/* --- Context Assembly --- */
char *memory_assemble_context(sqlite3 *db, const char *task_hint);
char *memory_assemble_context_ws(sqlite3 *db, const char *task_hint, const char *workspace);

/* --- Graph Boost (for context scoring) --- */
#define MAX_BOOST_ENTRIES 256

typedef struct
{
   char entity[128];
   double score;
} boost_entry_t;

typedef struct
{
   boost_entry_t entries[MAX_BOOST_ENTRIES];
   int count;
} boost_map_t;

void memory_graph_boost(sqlite3 *db, char **query_terms, int term_count, boost_map_t *out);

/* --- Context Cache --- */
int cache_get(sqlite3 *db, const char *hash, char *out, size_t out_len);
void cache_put(sqlite3 *db, const char *hash, const char *output);
void cache_invalidate(sqlite3 *db);
char *cache_input_hash(sqlite3 *db, char *buf, size_t buf_len);

/* --- Conflict Detection --- */
int64_t memory_detect_conflict(sqlite3 *db, const char *key, const char *content);
int memory_record_conflict(sqlite3 *db, int64_t mem_a, int64_t mem_b);
int memory_list_conflicts(sqlite3 *db, conflict_t *out, int max);
int memory_resolve_conflict(sqlite3 *db, int64_t conflict_id, const char *resolution);
int memory_scan_retroactive_conflicts(sqlite3 *db);

/* --- L3 Failure Episodes --- */
int memory_synthesize_failure_episodes(sqlite3 *db);

/* --- Anti-Patterns --- */
int anti_pattern_insert(sqlite3 *db, const char *pattern, const char *desc, const char *source,
                        const char *ref, double confidence, anti_pattern_t *out);
int anti_pattern_list(sqlite3 *db, anti_pattern_t *out, int max);
int anti_pattern_check(sqlite3 *db, const char *file_path, const char *command, anti_pattern_t *out,
                       int max);
int anti_pattern_delete(sqlite3 *db, int64_t id);
int anti_pattern_extract_from_feedback(sqlite3 *db);
int anti_pattern_extract_from_failures(sqlite3 *db);

/* Escalate high-hit anti-patterns to hard directive rules. */
int anti_pattern_escalate(sqlite3 *db, int hit_threshold);

/* --- Temporal Facts --- */
int memory_supersede(sqlite3 *db, int64_t old_id, const char *new_content, double confidence,
                     const char *session_id, memory_t *out);
int memory_fact_history(sqlite3 *db, const char *key, memory_t *out, int max);

/* --- Drift Detection --- */
typedef struct
{
   int drifted;
   int64_t task_id;
   char task_title[256];
   char message[512];
} drift_result_t;

int memory_check_drift(sqlite3 *db, int64_t task_id, const char *file_path, const char *command,
                       drift_result_t *out);

/* --- Style Learning --- */
int memory_learn_style(sqlite3 *db);

/* --- Graph --- */
typedef struct
{
   int64_t id;
   char source[128];
   char relation[32];
   char target[128];
   int weight;
} edge_t;

int memory_extract_edges(sqlite3 *db, int64_t window_id, char **file_refs, int file_count,
                         char **terms, int term_count);
int memory_query_edges(sqlite3 *db, const char *entity, edge_t *out, int max);

/* Graph-powered related memory retrieval: given seed memory keys, walk
 * co_discussed edges (1-hop) and return related memory IDs scored by weight.
 * Returns count of related memories found (up to max). */
typedef struct
{
   int64_t id;
   char key[512];
   char content[2048];
   double score;
} graph_related_t;

int memory_graph_related(sqlite3 *db, char **seed_keys, int seed_count, graph_related_t *out,
                         int max);

/* Prune edges where both source and target have no corresponding L1+ memory. */
int memory_graph_prune(sqlite3 *db);

/* Normalize edge weights per relation type so max weight is 1.0. */
int memory_graph_normalize(sqlite3 *db);

/* --- Embedding Retrieval --- */
int memory_embed(sqlite3 *db, int64_t memory_id, const char *command);
int memory_embed_text(const char *text, const char *command, float *out, int max_dim);
int memory_search_semantic(sqlite3 *db, const char *query, const char *command,
                           search_result_t *out, int max);
double cosine_similarity(const float *a, const float *b, int dim);

/* --- Effectiveness Tracking --- */

typedef struct
{
   int64_t memory_id;
   int times_surfaced;
   int success_present;
   int failure_present;
   double effectiveness;
} memory_effectiveness_t;

/* Record which memories were included in context for a session */
void memory_record_context_snapshot(sqlite3 *db, const char *session_id, int64_t memory_id,
                                    double relevance_score);

/* Record session outcome: "success", "failure", "partial", "unknown" */
void memory_record_outcome(sqlite3 *db, const char *session_id, const char *outcome);

/* Compute effectiveness scores for all memories with enough data. Returns count updated. */
int memory_compute_effectiveness(sqlite3 *db);

/* Demote memories with low effectiveness. Returns count demoted. */
int memory_demote_low_effectiveness(sqlite3 *db);

/* Get effectiveness stats for display */
typedef struct
{
   double avg_effectiveness;
   int low_effectiveness_count;
   int high_impact_count;
   int never_surfaced_l2;
} effectiveness_stats_t;

int memory_effectiveness_stats(sqlite3 *db, effectiveness_stats_t *out);
/* --- Memory-to-Memory Linking --- */
typedef struct
{
   int64_t id;
   int64_t source_id;
   int64_t target_id;
   char relation[32];
   char created_at[32];
} memory_link_t;

int memory_link_create(sqlite3 *db, int64_t source_id, int64_t target_id, const char *relation);
int memory_link_query(sqlite3 *db, int64_t memory_id, memory_link_t *out, int max);
int memory_link_delete(sqlite3 *db, int64_t link_id);
/* --- Content Safety --- */

#define SCAN_BLOCK    0 /* never persist */
#define SCAN_REDACT   1 /* persist with value masked */
#define SCAN_CLASSIFY 2 /* persist but mark sensitive */

/* Scan content for sensitive data. Returns sensitivity class ("normal", "sensitive", "restricted").
 * If action is SCAN_BLOCK, returns NULL (caller must reject).
 * If action is SCAN_REDACT, modifies content in-place. */
const char *memory_scan_content(char *content, size_t content_len);

/* Insert ephemeral (session-only) memory that cannot be promoted */
int memory_insert_ephemeral(sqlite3 *db, const char *key, const char *content,
                            const char *session_id);

/* Enforce retention policies: delete expired sensitive/restricted memories */
int memory_enforce_retention(sqlite3 *db);

/* --- Pre-fetch --- */
int memory_prefetch_projects(sqlite3 *db, char projects[][128], int max_projects);

#endif /* DEC_MEMORY_H */
