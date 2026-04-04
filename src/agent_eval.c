/* agent_eval.c: eval harness, task suite loading, success checking, result storage */
#include "aimee.h"
#include "agent_eval.h"
#include "agent_exec.h"
#include "memory.h"
#include "cJSON.h"

#ifndef _WIN32
#include <glob.h>
#endif

/* --- Eval Harness (Feature 6) --- */

int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks)
{
#ifdef _WIN32
   (void)suite_dir;
   (void)tasks;
   (void)max_tasks;
   return 0;
#else
   if (!suite_dir || !tasks)
      return 0;

   /* Scan directory for *.json files */
   char pattern[MAX_PATH_LEN];
   snprintf(pattern, sizeof(pattern), "%s/*.json", suite_dir);

   glob_t gl;
   if (glob(pattern, 0, NULL, &gl) != 0)
      return 0;

   int count = 0;
   for (size_t i = 0; i < gl.gl_pathc && count < max_tasks; i++)
   {
      FILE *f = fopen(gl.gl_pathv[i], "r");
      if (!f)
         continue;
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz <= 0 || sz > 65536)
      {
         fclose(f);
         continue;
      }
      char *data = malloc((size_t)sz + 1);
      size_t nread = fread(data, 1, (size_t)sz, f);
      data[nread] = '\0';
      fclose(f);

      cJSON *root = cJSON_Parse(data);
      free(data);
      if (!root)
         continue;

      eval_task_t *t = &tasks[count];
      memset(t, 0, sizeof(*t));

      cJSON *name = cJSON_GetObjectItem(root, "name");
      cJSON *prompt = cJSON_GetObjectItem(root, "prompt");
      cJSON *role = cJSON_GetObjectItem(root, "role");

      if (name && cJSON_IsString(name))
         snprintf(t->name, sizeof(t->name), "%s", name->valuestring);
      if (prompt && cJSON_IsString(prompt))
         snprintf(t->prompt, sizeof(t->prompt), "%s", prompt->valuestring);
      if (role && cJSON_IsString(role))
         snprintf(t->role, sizeof(t->role), "%s", role->valuestring);
      else
         snprintf(t->role, sizeof(t->role), "execute");

      cJSON *sc = cJSON_GetObjectItem(root, "success_check");
      if (sc)
      {
         cJSON *sctype = cJSON_GetObjectItem(sc, "type");
         cJSON *scval = cJSON_GetObjectItem(sc, "value");
         if (sctype && cJSON_IsString(sctype))
            snprintf(t->success_check_type, sizeof(t->success_check_type), "%s",
                     sctype->valuestring);
         if (scval && cJSON_IsString(scval))
            snprintf(t->success_check_value, sizeof(t->success_check_value), "%s",
                     scval->valuestring);
      }

      cJSON *mt = cJSON_GetObjectItem(root, "max_turns");
      t->max_turns = (mt && cJSON_IsNumber(mt)) ? mt->valueint : 10;

      cJSON *ml = cJSON_GetObjectItem(root, "max_latency_ms");
      t->max_latency_ms = (ml && cJSON_IsNumber(ml)) ? ml->valueint : 60000;

      cJSON_Delete(root);
      count++;
   }
   globfree(&gl);
   return count;
#endif /* _WIN32 */
}

static int eval_check_success(const eval_task_t *task, const agent_result_t *result)
{
   if (!result->success || !result->response)
      return 0;

   if (task->success_check_type[0] == '\0')
      return result->success; /* no check defined, use agent success */

   if (strcmp(task->success_check_type, "contains") == 0)
      return strstr(result->response, task->success_check_value) != NULL;

   if (strcmp(task->success_check_type, "exit_code") == 0)
      return result->success; /* agent already checks exit codes */

   return result->success;
}

static void eval_store_result(sqlite3 *db, const char *suite, const eval_task_t *task,
                              const agent_result_t *result, int passed)
{
   static const char *sql = "INSERT INTO eval_results (suite, task_name, agent_name, success,"
                            " turns, tool_calls, prompt_tokens, completion_tokens, latency_ms,"
                            " response, error)"
                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, suite, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, task->name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, result->agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, passed);
   sqlite3_bind_int(stmt, 5, result->turns);
   sqlite3_bind_int(stmt, 6, result->tool_calls);
   sqlite3_bind_int(stmt, 7, result->prompt_tokens);
   sqlite3_bind_int(stmt, 8, result->completion_tokens);
   sqlite3_bind_int(stmt, 9, result->latency_ms);
   sqlite3_bind_text(stmt, 10, result->response, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 11, result->error[0] ? result->error : NULL, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "eval_store_result");
}

int agent_eval_run(sqlite3 *db, agent_config_t *cfg, const char *suite_dir, eval_result_t *results,
                   int max_results)
{
   if (!db || !cfg || !suite_dir)
      return 0;

   eval_task_t tasks[AGENT_MAX_EVAL_TASKS];
   int task_count = agent_eval_load_tasks(suite_dir, tasks, AGENT_MAX_EVAL_TASKS);
   if (task_count == 0)
      return 0;

   /* Extract suite name from directory path */
   const char *suite_name = strrchr(suite_dir, '/');
   suite_name = suite_name ? suite_name + 1 : suite_dir;

   int passes = 0;
   for (int i = 0; i < task_count && i < max_results; i++)
   {
      agent_result_t ar;
      int rc = agent_run(db, cfg, tasks[i].role, NULL, tasks[i].prompt, 0, &ar);

      int passed = (rc == 0) && eval_check_success(&tasks[i], &ar);
      if (passed && tasks[i].max_latency_ms > 0 && ar.latency_ms > tasks[i].max_latency_ms)
         passed = 0;

      eval_store_result(db, suite_name, &tasks[i], &ar, passed);

      if (results)
      {
         eval_result_t *r = &results[i];
         memset(r, 0, sizeof(*r));
         snprintf(r->task_name, sizeof(r->task_name), "%s", tasks[i].name);
         snprintf(r->agent_name, sizeof(r->agent_name), "%s", ar.agent_name);
         r->success = passed;
         r->turns = ar.turns;
         r->tool_calls = ar.tool_calls;
         r->prompt_tokens = ar.prompt_tokens;
         r->completion_tokens = ar.completion_tokens;
         r->latency_ms = ar.latency_ms;
         if (ar.error[0])
            snprintf(r->error, sizeof(r->error), "%s", ar.error);
      }

      if (passed)
         passes++;
      free(ar.response);
   }

   return passes;
}

int eval_feedback_loop(sqlite3 *db)
{
   if (!db)
      return 0;

   int adjustments = 0;

   /* Find recently failed eval tasks and reinforce matching rules */
   static const char *fail_sql = "SELECT DISTINCT task_name, error FROM eval_results"
                                 " WHERE success = 0"
                                 " AND created_at > datetime('now', '-7 days')"
                                 " ORDER BY created_at DESC LIMIT 20";
   sqlite3_stmt *stmt = db_prepare(db, fail_sql);
   if (!stmt)
      return 0;

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *task_name = (const char *)sqlite3_column_text(stmt, 0);
      const char *error = (const char *)sqlite3_column_text(stmt, 1);
      if (!task_name)
         continue;

      /* Search for rules whose title words overlap with the failed task */
      rule_t rules[32];
      int rcount = rules_list(db, rules, 32);
      for (int i = 0; i < rcount; i++)
      {
         /* Check if rule title words appear in task name or error */
         char title_copy[256];
         snprintf(title_copy, sizeof(title_copy), "%s", rules[i].title);

         char *saveptr = NULL;
         char *word = strtok_r(title_copy, " \t", &saveptr);
         int matched = 0;
         int total = 0;

         while (word)
         {
            if (strlen(word) > 3)
            {
               total++;
               if ((task_name && strcasestr(task_name, word)) || (error && strcasestr(error, word)))
                  matched++;
            }
            word = strtok_r(NULL, " \t", &saveptr);
         }

         /* If >50% of rule words match the failure, bump weight */
         if (total > 0 && matched * 2 >= total && rules[i].weight < 100)
         {
            int new_weight = rules[i].weight + 10;
            if (new_weight > 100)
               new_weight = 100;
            rules_update_weight(db, rules[i].id, new_weight);
            adjustments++;
         }
      }
   }
   sqlite3_reset(stmt);

   /* Reinforce rules that correlate with recent successes */
   static const char *pass_sql = "SELECT DISTINCT task_name FROM eval_results"
                                 " WHERE success = 1"
                                 " AND created_at > datetime('now', '-7 days')"
                                 " ORDER BY created_at DESC LIMIT 20";
   stmt = db_prepare(db, pass_sql);
   if (!stmt)
      return adjustments;

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *task_name = (const char *)sqlite3_column_text(stmt, 0);
      if (!task_name)
         continue;

      rule_t rules[32];
      int rcount = rules_list(db, rules, 32);
      for (int i = 0; i < rcount; i++)
      {
         char title_copy[256];
         snprintf(title_copy, sizeof(title_copy), "%s", rules[i].title);

         char *saveptr = NULL;
         char *word = strtok_r(title_copy, " \t", &saveptr);
         int matched = 0;
         int total = 0;

         while (word)
         {
            if (strlen(word) > 3)
            {
               total++;
               if (strcasestr(task_name, word))
                  matched++;
            }
            word = strtok_r(NULL, " \t", &saveptr);
         }

         /* Successful task correlated with rule: small reinforcement */
         if (total > 0 && matched * 2 >= total && rules[i].weight < 100)
         {
            int new_weight = rules[i].weight + 5;
            if (new_weight > 100)
               new_weight = 100;
            rules_update_weight(db, rules[i].id, new_weight);
            adjustments++;
         }
      }
   }
   sqlite3_reset(stmt);

   /* Also process agent_outcomes: failures and errors reinforce rules */
   static const char *outcome_fail_sql = "SELECT DISTINCT role, reason FROM agent_outcomes"
                                         " WHERE outcome IN ('failure', 'error')"
                                         " AND created_at > datetime('now', '-7 days')"
                                         " ORDER BY created_at DESC LIMIT 20";
   stmt = db_prepare(db, outcome_fail_sql);
   if (stmt)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *orole = (const char *)sqlite3_column_text(stmt, 0);
         const char *reason = (const char *)sqlite3_column_text(stmt, 1);
         if (!reason)
            continue;

         rule_t rules[32];
         int rcount = rules_list(db, rules, 32);
         for (int i = 0; i < rcount; i++)
         {
            char title_copy[256];
            snprintf(title_copy, sizeof(title_copy), "%s", rules[i].title);

            char *saveptr = NULL;
            char *word = strtok_r(title_copy, " \t", &saveptr);
            int matched = 0;
            int total = 0;

            while (word)
            {
               if (strlen(word) > 3)
               {
                  total++;
                  if ((orole && strcasestr(orole, word)) || strcasestr(reason, word))
                     matched++;
               }
               word = strtok_r(NULL, " \t", &saveptr);
            }

            if (total > 0 && matched * 2 >= total && rules[i].weight < 100)
            {
               int new_weight = rules[i].weight + 10;
               if (new_weight > 100)
                  new_weight = 100;
               rules_update_weight(db, rules[i].id, new_weight);
               adjustments++;
            }
         }
      }
      sqlite3_reset(stmt);
   }

   /* Auto-extract anti-patterns from repeated tool error patterns.
    * If the same tool_error_pattern appears in 3+ outcomes, create an anti-pattern. */
   static const char *pattern_sql =
       "SELECT tool_error_pattern, COUNT(*) as cnt FROM agent_outcomes"
       " WHERE tool_error_pattern != '' AND tool_error_pattern IS NOT NULL"
       " GROUP BY tool_error_pattern HAVING cnt >= 3";
   stmt = db_prepare(db, pattern_sql);
   if (stmt)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *pattern = (const char *)sqlite3_column_text(stmt, 0);
         int cnt = sqlite3_column_int(stmt, 1);
         if (!pattern || !pattern[0])
            continue;

         /* Check if this anti-pattern already exists */
         anti_pattern_t existing[1];
         int existing_count = anti_pattern_check(db, NULL, pattern, existing, 1);
         if (existing_count == 0)
         {
            char desc[512];
            snprintf(desc, sizeof(desc), "Auto-detected from %d agent failures: %s", cnt, pattern);
            anti_pattern_insert(db, pattern, desc, "auto-outcome", "", 0.7, NULL);
         }
      }
      sqlite3_reset(stmt);
   }

   return adjustments;
}

/* --- IR Metrics --- */

#include <math.h>

static int is_relevant(int64_t id, const int64_t *relevant, int n_relevant)
{
   for (int i = 0; i < n_relevant; i++)
      if (relevant[i] == id)
         return 1;
   return 0;
}

double ir_mrr(const int64_t *retrieved, int n_retrieved, const int64_t *relevant, int n_relevant)
{
   for (int i = 0; i < n_retrieved; i++)
   {
      if (is_relevant(retrieved[i], relevant, n_relevant))
         return 1.0 / (i + 1);
   }
   return 0.0;
}

double ir_ndcg_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                    int n_relevant, int k)
{
   if (k <= 0 || n_relevant == 0)
      return 0.0;
   int limit = n_retrieved < k ? n_retrieved : k;

   /* DCG */
   double dcg = 0.0;
   for (int i = 0; i < limit; i++)
   {
      if (is_relevant(retrieved[i], relevant, n_relevant))
         dcg += 1.0 / log2(i + 2.0);
   }

   /* Ideal DCG */
   int ideal_limit = n_relevant < k ? n_relevant : k;
   double idcg = 0.0;
   for (int i = 0; i < ideal_limit; i++)
      idcg += 1.0 / log2(i + 2.0);

   return idcg > 0.0 ? dcg / idcg : 0.0;
}

double ir_recall_at_k(const int64_t *retrieved, int n_retrieved, const int64_t *relevant,
                      int n_relevant, int k)
{
   if (n_relevant == 0)
      return 0.0;
   int limit = n_retrieved < k ? n_retrieved : k;
   int found = 0;
   for (int i = 0; i < limit; i++)
   {
      if (is_relevant(retrieved[i], relevant, n_relevant))
         found++;
   }
   return (double)found / n_relevant;
}

/* --- Memory Retrieval Eval --- */

int mem_eval_run(sqlite3 *db, mem_eval_case_t *cases, int n_cases, mem_eval_scores_t *out)
{
   if (!db || !cases || !out || n_cases <= 0)
      return -1;

   memset(out, 0, sizeof(*out));
   out->n_cases = n_cases;

   double total_mrr = 0, total_ndcg5 = 0, total_ndcg10 = 0;
   double total_recall5 = 0, total_recall10 = 0;

   for (int c = 0; c < n_cases; c++)
   {
      /* Run memory search */
      memory_t results[20];
      int n_results = memory_find_facts(db, cases[c].query, 20, results, 20);

      /* Extract retrieved IDs */
      int64_t retrieved[20];
      memset(retrieved, 0, sizeof(retrieved));
      for (int i = 0; i < n_results; i++)
         retrieved[i] = results[i].id;

      total_mrr += ir_mrr(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected);
      total_ndcg5 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 5);
      total_ndcg10 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 10);
      total_recall5 +=
          ir_recall_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 5);
      total_recall10 +=
          ir_recall_at_k(retrieved, n_results, cases[c].expected_ids, cases[c].n_expected, 10);
   }

   out->mrr = total_mrr / n_cases;
   out->ndcg_5 = total_ndcg5 / n_cases;
   out->ndcg_10 = total_ndcg10 / n_cases;
   out->recall_5 = total_recall5 / n_cases;
   out->recall_10 = total_recall10 / n_cases;

   return 0;
}
