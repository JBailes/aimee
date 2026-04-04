#ifndef DEC_AGENT_EVAL_H
#define DEC_AGENT_EVAL_H 1

#include "agent_types.h"

typedef struct
{
   char name[128];
   char prompt[2048];
   char role[32];
   char success_check_type[32];
   char success_check_value[512];
   int max_turns;
   int max_latency_ms;
} eval_task_t;

typedef struct
{
   char task_name[128];
   char agent_name[MAX_AGENT_NAME];
   int success;
   int turns;
   int tool_calls;
   int prompt_tokens;
   int completion_tokens;
   int latency_ms;
   char error[512];
} eval_result_t;

int agent_eval_run(sqlite3 *db, agent_config_t *cfg, const char *suite_dir, eval_result_t *results,
                   int max_results);
int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks);

/* Eval-to-behavior feedback loop: adjust rule weights based on recent eval results. */
int eval_feedback_loop(sqlite3 *db);

/* --- IR Metrics --- */

/* Compute Mean Reciprocal Rank. Returns 0.0 if no relevant results found. */
double ir_mrr(const int64_t *retrieved, int n_retrieved, const int64_t *relevant, int n_relevant);

/* Compute NDCG@k (Normalized Discounted Cumulative Gain). */
double ir_ndcg_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                    int n_relevant, int k);

/* Compute Recall@k. */
double ir_recall_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                      int n_relevant, int k);

/* --- Memory retrieval eval --- */

typedef struct
{
   char query[256];
   int64_t expected_ids[20];
   int n_expected;
} mem_eval_case_t;

typedef struct
{
   double mrr;
   double ndcg_5;
   double ndcg_10;
   double recall_5;
   double recall_10;
   int n_cases;
} mem_eval_scores_t;

int mem_eval_run(sqlite3 *db, mem_eval_case_t *cases, int n_cases, mem_eval_scores_t *out);

#endif /* DEC_AGENT_EVAL_H */
