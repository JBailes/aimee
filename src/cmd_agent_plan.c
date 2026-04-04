/* cmd_agent_plan.c: plans and eval CLI commands */
#include "aimee.h"
#include "agent.h"
#include "commands.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- cmd_plans --- */

void cmd_plans(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Plan IR CLI (Feature 2) */
   if (argc < 1)
      fatal("usage: aimee plans list|show <id>");
   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");

   if (strcmp(argv[0], "list") == 0)
   {
      plan_t plans[20];
      int count = agent_plan_list(db, plans, 20);
      printf("%-6s %-12s %-10s %s\n", "ID", "Agent", "Status", "Task");
      for (int i = 0; i < count; i++)
      {
         printf("%-6d %-12s %-10s %.*s\n", plans[i].id, plans[i].agent_name, plans[i].status, 60,
                plans[i].task);
      }
   }
   else if (strcmp(argv[0], "show") == 0 && argc >= 2)
   {
      int pid = atoi(argv[1]);
      plan_t plan;
      if (agent_plan_load(db, pid, &plan) != 0)
         fatal("plan %d not found", pid);

      printf("Plan #%d [%s]\n", plan.id, plan.status);
      printf("Agent: %s\n", plan.agent_name);
      printf("Task:  %s\n\n", plan.task);
      printf("Steps:\n");
      static const char *status_names[] = {"pending", "running", "done", "failed", "rolled_back"};
      for (int i = 0; i < plan.step_count; i++)
      {
         plan_step_t *s = &plan.steps[i];
         const char *sn = (s->status >= 0 && s->status <= 4) ? status_names[s->status] : "unknown";
         printf("  %d. [%s] %s\n", i + 1, sn, s->action);
         if (s->precondition[0])
            printf("     Precondition: %s\n", s->precondition);
         if (s->success_predicate[0])
            printf("     Success: %s\n", s->success_predicate);
         if (s->rollback[0])
            printf("     Rollback: %s\n", s->rollback);
         if (s->output[0])
            printf("     Output: %.*s\n", 200, s->output);
      }
   }
   else if (strcmp(argv[0], "replay") == 0 && argc >= 2)
   {
      int pid = atoi(argv[1]);
      plan_t plan;
      if (agent_plan_load(db, pid, &plan) != 0)
         fatal("plan %d not found", pid);

      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0)
         fatal("no agents configured");

      agent_t *ag = agent_find(&acfg, plan.agent_name);
      int timeout = ag ? ag->timeout_ms : AGENT_DEFAULT_TIMEOUT_MS;
      int rc = agent_plan_execute(db, &plan, ag, timeout);
      printf("Replay %s.\n", rc == 0 ? "succeeded" : "failed");
   }

   ctx_db_close(ctx);
}

/* --- cmd_eval --- */

void cmd_eval(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Eval Harness CLI (Feature 6) */
   if (argc < 1)
      fatal("usage: aimee eval run <suite_dir>|results [suite]|memory-retrieval");

   if (strcmp(argv[0], "memory-retrieval") == 0)
   {
      sqlite3 *db = ctx_db_open(ctx);
      if (!db)
         fatal("cannot open database");

      /* Build eval cases from the database itself: for each L2 fact,
       * use its key as a query and expect it to be found */
      static const char *sql =
          "SELECT id, key, content FROM memories WHERE tier = 'L2' AND kind = 'fact' LIMIT 100";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (!stmt)
      {
         fprintf(stderr, "error: cannot prepare eval query\n");
         ctx_db_close(ctx);
         return;
      }

      mem_eval_case_t cases[100];
      memset(cases, 0, sizeof(cases));
      int n_cases = 0;
      sqlite3_reset(stmt);
      while (sqlite3_step(stmt) == SQLITE_ROW && n_cases < 100)
      {
         int64_t id = sqlite3_column_int64(stmt, 0);
         const char *key = (const char *)sqlite3_column_text(stmt, 1);
         snprintf(cases[n_cases].query, sizeof(cases[n_cases].query), "%s", key);
         cases[n_cases].expected_ids[0] = id;
         cases[n_cases].n_expected = 1;
         n_cases++;
      }

      if (n_cases == 0)
      {
         printf("No L2 facts in database. Add memories first.\n");
         ctx_db_close(ctx);
         return;
      }

      mem_eval_scores_t scores;
      mem_eval_run(db, cases, n_cases, &scores);

      printf("Memory Retrieval Evaluation (%d cases)\n", scores.n_cases);
      printf("  MRR:       %.4f\n", scores.mrr);
      printf("  NDCG@5:    %.4f\n", scores.ndcg_5);
      printf("  NDCG@10:   %.4f\n", scores.ndcg_10);
      printf("  Recall@5:  %.4f\n", scores.recall_5);
      printf("  Recall@10: %.4f\n", scores.recall_10);

      ctx_db_close(ctx);
      return;
   }
   else if (strcmp(argv[0], "run") == 0)
   {
      if (argc < 2)
         fatal("usage: aimee eval run <suite_dir>");

      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
         fatal("no agents configured");

      agent_http_init();
      sqlite3 *db = ctx_db_open(ctx);
      if (!db)
         fatal("cannot open database");
      eval_result_t results[AGENT_MAX_EVAL_TASKS];
      int passes = agent_eval_run(db, &acfg, argv[1], results, AGENT_MAX_EVAL_TASKS);

      /* Load task count for reporting */
      eval_task_t tasks[AGENT_MAX_EVAL_TASKS];
      int total = agent_eval_load_tasks(argv[1], tasks, AGENT_MAX_EVAL_TASKS);

      printf("%-30s %-12s %-6s %-6s %-10s\n", "Task", "Agent", "Pass", "Turns", "Latency");
      for (int i = 0; i < total; i++)
      {
         printf("%-30s %-12s %-6s %-6d %-10dms\n", results[i].task_name, results[i].agent_name,
                results[i].success ? "PASS" : "FAIL", results[i].turns, results[i].latency_ms);
      }
      printf("\n%d/%d passed.\n", passes, total);

      agent_http_cleanup();
      ctx_db_close(ctx);
   }
   else if (strcmp(argv[0], "results") == 0)
   {
      sqlite3 *db = ctx_db_open(ctx);
      if (!db)
         fatal("cannot open database");
      const char *suite_filter = (argc >= 2) ? argv[1] : NULL;

      const char *sql;
      if (suite_filter)
         sql = "SELECT suite, task_name, agent_name, success, turns, latency_ms, created_at"
               " FROM eval_results WHERE suite = ? ORDER BY id DESC LIMIT 50";
      else
         sql = "SELECT suite, task_name, agent_name, success, turns, latency_ms, created_at"
               " FROM eval_results ORDER BY id DESC LIMIT 50";

      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_reset(stmt);
         if (suite_filter)
            sqlite3_bind_text(stmt, 1, suite_filter, -1, SQLITE_TRANSIENT);

         printf("%-15s %-25s %-12s %-6s %-6s %-10s %s\n", "Suite", "Task", "Agent", "Pass", "Turns",
                "Latency", "Time");
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            printf("%-15s %-25s %-12s %-6s %-6d %-10dms %s\n",
                   (const char *)sqlite3_column_text(stmt, 0),
                   (const char *)sqlite3_column_text(stmt, 1),
                   (const char *)sqlite3_column_text(stmt, 2),
                   sqlite3_column_int(stmt, 3) ? "PASS" : "FAIL", sqlite3_column_int(stmt, 4),
                   sqlite3_column_int(stmt, 5), (const char *)sqlite3_column_text(stmt, 6));
         }
      }

      ctx_db_close(ctx);
   }
}
