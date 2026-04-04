#ifndef DEC_RULES_H
#define DEC_RULES_H 1

typedef struct
{
   int id;
   char polarity[16];
   char title[256];
   char description[1024];
   int weight;
   char domain[128];
   char created_at[32];
   char updated_at[32];
   char directive_type[16]; /* "hard", "soft", "session" */
   char expires_at[32];
} rule_t;

/* List all rules ordered by weight desc. Returns count, fills array. */
int rules_list(sqlite3 *db, rule_t *out, int max_rules);

/* List rules with minimum weight. Returns count. */
int rules_list_by_tier(sqlite3 *db, int min_weight, rule_t *out, int max_rules);

/* Get a single rule by ID. Returns 0 on success, -1 if not found. */
int rules_get(sqlite3 *db, int id, rule_t *out);

/* Find rule by title (case-insensitive). Returns 0 on success, -1 if not found. */
int rules_find_by_title(sqlite3 *db, const char *title, rule_t *out);

/* Delete a rule by ID. Returns 0 on success. */
int rules_delete(sqlite3 *db, int id);

/* Update a rule's weight. Returns 0 on success. */
int rules_update_weight(sqlite3 *db, int id, int weight);

/* Generate rules.md content. Returns malloc'd string (caller frees). */
char *rules_generate(sqlite3 *db);

/* Invalidate the in-process rules_generate() cache. */
void rules_cache_invalidate(void);

/* Tier label for a weight. */
const char *rules_tier(int weight);

/* Polarity symbol (+, -, ~). */
char rules_polarity_symbol(const char *polarity);

/* Decay weights of stale rules. Returns number of rules affected.
 * Soft rules decay every 14 days, hard directives every 42 days.
 * Rules with weight < 10 for 30+ days are archived (deleted). */
int rules_decay(sqlite3 *db);

#endif /* DEC_RULES_H */
