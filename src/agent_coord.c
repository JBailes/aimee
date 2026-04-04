/* agent_coord.c: multi-agent coordination (planner/critic/worker), quorum voting, hard directives
 */
#include "aimee.h"
#include "agent_coord.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "agent_plan.h"
#include "cJSON.h"
#include <ctype.h>

/* --- Multi-Agent Coordination (Feature 8) --- */

int agent_coordinate(sqlite3 *db, agent_config_t *cfg, const char *task, agent_result_t *out)
{
   if (!db || !cfg || !task || !out)
      return -1;

   memset(out, 0, sizeof(*out));

   /* Phase 1: Planner creates a plan */
   char planner_prompt[4096];
   snprintf(planner_prompt, sizeof(planner_prompt),
            "Create a step-by-step plan as a JSON array for this task. "
            "Each step should have: action (shell command), precondition (text), "
            "success_predicate (text to find in output), rollback (shell command or empty). "
            "Task: %s\n\nRespond ONLY with a JSON array, no other text.",
            task);

   agent_result_t planner_result;
   int rc = agent_run(db, cfg, "execute", NULL, planner_prompt, 0, &planner_result);
   if (rc != 0 || !planner_result.response)
   {
      snprintf(out->error, sizeof(out->error), "planner failed: %s", planner_result.error);
      free(planner_result.response);
      return -1;
   }

   /* Phase 2: Critic reviews the plan */
   char critic_prompt[8192];
   snprintf(critic_prompt, sizeof(critic_prompt),
            "Review this plan for a task. Flag any risks, missing steps, or unsafe commands. "
            "If the plan is good, respond with APPROVED. If not, respond with REJECTED followed "
            "by your concerns.\n\nTask: %s\n\nPlan:\n%s",
            task, planner_result.response);

   agent_result_t critic_result;
   rc = agent_run(db, cfg, "execute", NULL, critic_prompt, 0, &critic_result);

   int approved = 1;
   if (rc == 0 && critic_result.response)
   {
      if (strstr(critic_result.response, "REJECTED") != NULL)
         approved = 0;
   }
   free(critic_result.response);

   if (!approved)
   {
      snprintf(out->error, sizeof(out->error), "plan rejected by critic");
      out->response = planner_result.response;
      return -1;
   }

   /* Phase 3: Worker executes the plan */
   cJSON *plan_json = cJSON_Parse(planner_result.response);
   free(planner_result.response);

   if (!plan_json || !cJSON_IsArray(plan_json))
   {
      /* If the planner didn't return valid JSON, try direct execution */
      cJSON_Delete(plan_json);
      return agent_run(db, cfg, "execute", NULL, task, 0, out);
   }

   int plan_id = agent_plan_create(db, cfg->default_agent, task, plan_json);
   cJSON_Delete(plan_json);

   if (plan_id < 0)
      return agent_run(db, cfg, "execute", NULL, task, 0, out);

   plan_t plan;
   if (agent_plan_load(db, plan_id, &plan) != 0)
      return -1;

   agent_t *ag = agent_route(cfg, "execute");
   int timeout = ag ? ag->timeout_ms : AGENT_DEFAULT_TIMEOUT_MS;
   rc = agent_plan_execute(db, &plan, ag, timeout);

   /* Build response from plan outputs */
   size_t resp_len = 1024;
   for (int i = 0; i < plan.step_count; i++)
      resp_len += strlen(plan.steps[i].output) + 128;
   out->response = malloc(resp_len);
   if (out->response)
   {
      size_t pos = 0;
      for (int i = 0; i < plan.step_count; i++)
      {
         static const char *status_names[] = {"pending", "running", "done", "failed",
                                              "rolled_back"};
         const char *sn = (plan.steps[i].status >= 0 && plan.steps[i].status <= 4)
                              ? status_names[plan.steps[i].status]
                              : "unknown";
         pos += (size_t)snprintf(out->response + pos, resp_len - pos, "Step %d [%s]: %s\n%s\n\n",
                                 i + 1, sn, plan.steps[i].action, plan.steps[i].output);
      }
   }
   out->success = (rc == 0);
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", plan.agent_name);
   return rc;
}

int agent_vote(sqlite3 *db, agent_config_t *cfg, const char *role, const char *prompt, int n_voters,
               agent_result_t *out)
{
   if (!db || !cfg || !prompt || !out || n_voters <= 0)
      return -1;

   memset(out, 0, sizeof(*out));

   if (n_voters > AGENT_MAX_COORD_AGENTS)
      n_voters = AGENT_MAX_COORD_AGENTS;

   agent_task_t tasks[AGENT_MAX_COORD_AGENTS];
   agent_result_t results[AGENT_MAX_COORD_AGENTS];

   for (int i = 0; i < n_voters; i++)
   {
      tasks[i].role = role;
      tasks[i].system_prompt = NULL;
      tasks[i].user_prompt = prompt;
      tasks[i].max_tokens = 0;
      tasks[i].temperature = 0.3 + (0.1 * i); /* slight variation */
   }

   int successes = agent_run_parallel(db, cfg, tasks, n_voters, results);
   if (successes == 0)
   {
      snprintf(out->error, sizeof(out->error), "all voters failed");
      return -1;
   }

   /* Simple majority: pick the most common response (by first 200 chars) */
   int best_idx = -1;
   int best_count = 0;
   for (int i = 0; i < n_voters; i++)
   {
      if (!results[i].success || !results[i].response)
         continue;
      int count = 0;
      for (int j = 0; j < n_voters; j++)
      {
         if (!results[j].success || !results[j].response)
            continue;
         if (strncmp(results[i].response, results[j].response, 200) == 0)
            count++;
      }
      if (count > best_count)
      {
         best_count = count;
         best_idx = i;
      }
   }

   if (best_idx >= 0)
   {
      out->response = results[best_idx].response;
      results[best_idx].response = NULL; /* don't free, transferred to out */
      out->success = 1;
      snprintf(out->agent_name, MAX_AGENT_NAME, "%s", results[best_idx].agent_name);
      out->confidence = (best_count * 100) / n_voters;
   }

   /* Free remaining results */
   for (int i = 0; i < n_voters; i++)
      free(results[i].response);

   return out->success ? 0 : -1;
}

/* --- Hard Directive Enforcement (Feature 15) --- */

int directive_check_tool(sqlite3 *db, const char *tool_name, const char *args_json,
                         char *reason_out, size_t reason_len)
{
   if (!db || !tool_name)
      return 0;

   /* Load hard directives */
   static const char *sql =
       "SELECT description FROM rules WHERE directive_type = 'hard' AND weight > 0"
       " ORDER BY weight DESC";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   /* Parse command from args */
   char command[4096] = {0};
   if (args_json)
   {
      cJSON *args = cJSON_Parse(args_json);
      if (args)
      {
         cJSON *cmd = cJSON_GetObjectItem(args, "command");
         if (cmd && cJSON_IsString(cmd))
            snprintf(command, sizeof(command), "%s", cmd->valuestring);
         cJSON *path = cJSON_GetObjectItem(args, "path");
         if (path && cJSON_IsString(path))
            snprintf(command, sizeof(command), "%s", path->valuestring);
         cJSON_Delete(args);
      }
   }

   sqlite3_reset(stmt);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *directive = (const char *)sqlite3_column_text(stmt, 0);
      if (!directive)
         continue;

      /* Keyword matching: check if the tool call would violate the directive.
       * Extract keywords from the directive and check against the command. */

      /* "Never push to main" -> check for "push" + "main" */
      /* "Never delete production" -> check for "delete" + "production" */
      char lower_directive[1024];
      size_t dlen = strlen(directive);
      if (dlen >= sizeof(lower_directive))
         dlen = sizeof(lower_directive) - 1;
      for (size_t i = 0; i < dlen; i++)
         lower_directive[i] = (char)tolower((unsigned char)directive[i]);
      lower_directive[dlen] = '\0';

      char lower_command[4096];
      size_t clen = strlen(command);
      if (clen >= sizeof(lower_command))
         clen = sizeof(lower_command) - 1;
      for (size_t i = 0; i < clen; i++)
         lower_command[i] = (char)tolower((unsigned char)command[i]);
      lower_command[clen] = '\0';

      /* Check for "never" directives with keyword overlap */
      if (strstr(lower_directive, "never") || strstr(lower_directive, "must not") ||
          strstr(lower_directive, "do not"))
      {
         /* Extract significant words (>3 chars) from directive and check command */
         char *dp = lower_directive;
         char word[64];
         int matches = 0;
         int words_checked = 0;

         while (*dp)
         {
            /* Skip non-alpha */
            while (*dp && !isalpha((unsigned char)*dp))
               dp++;
            if (!*dp)
               break;

            /* Extract word */
            int wi = 0;
            while (*dp && isalpha((unsigned char)*dp) && wi < 63)
               word[wi++] = *dp++;
            word[wi] = '\0';

            /* Skip common words */
            if (strcmp(word, "never") == 0 || strcmp(word, "must") == 0 ||
                strcmp(word, "not") == 0 || strcmp(word, "the") == 0 || strcmp(word, "and") == 0 ||
                strcmp(word, "for") == 0 || strcmp(word, "this") == 0 ||
                strcmp(word, "that") == 0 || wi <= 3)
               continue;

            words_checked++;
            if (strstr(lower_command, word))
               matches++;
         }

         /* If more than half the significant words match, likely violation */
         if (words_checked > 0 && matches > 0 && matches * 2 >= words_checked)
         {
            snprintf(reason_out, reason_len, "hard directive violation: %s", directive);
            return -1;
         }
      }
   }

   return 0;
}

void directive_expire_session(sqlite3 *db)
{
   if (!db)
      return;
   static const char *sql = "DELETE FROM rules WHERE directive_type = 'session'";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_reset(stmt);
   DB_STEP_LOG(stmt, "directive_expire_session");
}

/* --- Delegation Checkpoints --- */

int delegation_checkpoint_save(sqlite3 *db, const char *delegation_id, const char *job_id,
                               int attempt, const char *steps_json, const char *last_output,
                               const char *error)
{
   if (!db || !delegation_id)
      return -1;

   static const char *sql = "INSERT OR REPLACE INTO delegation_checkpoint "
                            "(delegation_id, job_id, steps_completed, last_output, error, attempt, "
                            "failed_at, created_at) VALUES (?, ?, ?, ?, ?, ?, "
                            "strftime('%s','now'), strftime('%s','now'))";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, job_id ? job_id : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, steps_json ? steps_json : "[]", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, last_output ? last_output : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 5, error ? error : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 6, attempt);

   int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
   sqlite3_reset(stmt);
   return rc;
}

int delegation_checkpoint_load(sqlite3 *db, const char *delegation_id, char *steps_out,
                               size_t steps_cap, char *error_out, size_t error_cap,
                               char *output_out, size_t output_cap)
{
   if (!db || !delegation_id)
      return -1;

   static const char *sql = "SELECT steps_completed, error, last_output FROM delegation_checkpoint "
                            "WHERE delegation_id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;

   sqlite3_bind_text(stmt, 1, delegation_id, -1, SQLITE_TRANSIENT);

   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *steps = (const char *)sqlite3_column_text(stmt, 0);
      const char *err = (const char *)sqlite3_column_text(stmt, 1);
      const char *out = (const char *)sqlite3_column_text(stmt, 2);
      if (steps_out && steps)
         snprintf(steps_out, steps_cap, "%s", steps);
      if (error_out && err)
         snprintf(error_out, error_cap, "%s", err);
      if (output_out && out)
         snprintf(output_out, output_cap, "%s", out);
      rc = 0;
   }
   sqlite3_reset(stmt);
   return rc;
}
