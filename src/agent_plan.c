/* agent_plan.c: plan IR persistence and execution (plan_step_t, plan_t), two-phase mode */
#include "aimee.h"
#include "agent_plan.h"
#include "agent_exec.h"
#include "agent_tools.h"
#include "cJSON.h"

/* --- Plan IR (Feature 2) --- */

int agent_plan_create(sqlite3 *db, const char *agent_name, const char *task,
                      const cJSON *steps_json)
{
   if (!db || !agent_name || !task || !steps_json)
      return -1;

   static const char *plan_sql = "INSERT INTO execution_plans (agent_name, task, status)"
                                 " VALUES (?, ?, 'pending')";
   sqlite3_stmt *stmt = db_prepare(db, plan_sql);
   if (!stmt)
      return -1;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, task, -1, SQLITE_TRANSIENT);
   if (sqlite3_step(stmt) != SQLITE_DONE)
      return -1;
   int plan_id = (int)sqlite3_last_insert_rowid(db);

   static const char *step_sql = "INSERT INTO plan_steps (plan_id, seq, action, precondition,"
                                 " success_predicate, rollback, status)"
                                 " VALUES (?, ?, ?, ?, ?, ?, 'pending')";
   sqlite3_stmt *sstmt = db_prepare(db, step_sql);
   if (!sstmt)
      return plan_id;

   int n = cJSON_GetArraySize((cJSON *)steps_json);
   for (int i = 0; i < n && i < AGENT_MAX_PLAN_STEPS; i++)
   {
      cJSON *step = cJSON_GetArrayItem((cJSON *)steps_json, i);
      if (!step)
         continue;
      cJSON *action = cJSON_GetObjectItem(step, "action");
      cJSON *pre = cJSON_GetObjectItem(step, "precondition");
      cJSON *suc = cJSON_GetObjectItem(step, "success_predicate");
      cJSON *rb = cJSON_GetObjectItem(step, "rollback");

      sqlite3_reset(sstmt);
      sqlite3_bind_int(sstmt, 1, plan_id);
      sqlite3_bind_int(sstmt, 2, i);
      sqlite3_bind_text(sstmt, 3, (action && cJSON_IsString(action)) ? action->valuestring : "", -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(sstmt, 4, (pre && cJSON_IsString(pre)) ? pre->valuestring : NULL, -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(sstmt, 5, (suc && cJSON_IsString(suc)) ? suc->valuestring : NULL, -1,
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(sstmt, 6, (rb && cJSON_IsString(rb)) ? rb->valuestring : NULL, -1,
                        SQLITE_TRANSIENT);
      DB_STEP_LOG(sstmt, "agent_plan_create");
   }

   return plan_id;
}

int agent_plan_load(sqlite3 *db, int plan_id, plan_t *out)
{
   if (!db || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   static const char *sql = "SELECT id, agent_name, task, status FROM execution_plans WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return -1;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, plan_id);
   if (sqlite3_step(stmt) != SQLITE_ROW)
      return -1;

   out->id = sqlite3_column_int(stmt, 0);
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", (const char *)sqlite3_column_text(stmt, 1));
   snprintf(out->task, sizeof(out->task), "%s", (const char *)sqlite3_column_text(stmt, 2));
   snprintf(out->status, sizeof(out->status), "%s", (const char *)sqlite3_column_text(stmt, 3));

   static const char *step_sql =
       "SELECT id, seq, action, precondition, success_predicate, rollback,"
       " status, output FROM plan_steps WHERE plan_id = ? ORDER BY seq";
   sqlite3_stmt *sstmt = db_prepare(db, step_sql);
   if (!sstmt)
      return 0;
   sqlite3_reset(sstmt);
   sqlite3_bind_int(sstmt, 1, plan_id);

   while (sqlite3_step(sstmt) == SQLITE_ROW && out->step_count < AGENT_MAX_PLAN_STEPS)
   {
      plan_step_t *s = &out->steps[out->step_count];
      memset(s, 0, sizeof(*s));
      s->id = sqlite3_column_int(sstmt, 0);
      snprintf(s->action, sizeof(s->action), "%s", (const char *)sqlite3_column_text(sstmt, 2));
      const char *pre = (const char *)sqlite3_column_text(sstmt, 3);
      if (pre)
         snprintf(s->precondition, sizeof(s->precondition), "%s", pre);
      const char *suc = (const char *)sqlite3_column_text(sstmt, 4);
      if (suc)
         snprintf(s->success_predicate, sizeof(s->success_predicate), "%s", suc);
      const char *rb = (const char *)sqlite3_column_text(sstmt, 5);
      if (rb)
         snprintf(s->rollback, sizeof(s->rollback), "%s", rb);
      const char *st = (const char *)sqlite3_column_text(sstmt, 6);
      if (st)
      {
         if (strcmp(st, "running") == 0)
            s->status = 1;
         else if (strcmp(st, "done") == 0)
            s->status = 2;
         else if (strcmp(st, "failed") == 0)
            s->status = 3;
         else if (strcmp(st, "rolled_back") == 0)
            s->status = 4;
      }
      const char *output = (const char *)sqlite3_column_text(sstmt, 7);
      if (output)
         snprintf(s->output, sizeof(s->output), "%s", output);
      out->step_count++;
   }

   return 0;
}

int agent_plan_list(sqlite3 *db, plan_t *out, int max)
{
   if (!db || !out)
      return 0;

   static const char *sql = "SELECT id, agent_name, task, status"
                            " FROM execution_plans ORDER BY id DESC LIMIT ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;
   sqlite3_reset(stmt);
   sqlite3_bind_int(stmt, 1, max);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      memset(&out[count], 0, sizeof(out[count]));
      out[count].id = sqlite3_column_int(stmt, 0);
      snprintf(out[count].agent_name, MAX_AGENT_NAME, "%s",
               (const char *)sqlite3_column_text(stmt, 1));
      snprintf(out[count].task, sizeof(out[count].task), "%s",
               (const char *)sqlite3_column_text(stmt, 2));
      snprintf(out[count].status, sizeof(out[count].status), "%s",
               (const char *)sqlite3_column_text(stmt, 3));
      count++;
   }
   return count;
}

static void plan_step_update_status(sqlite3 *db, int step_id, const char *status,
                                    const char *output)
{
   static const char *sql = "UPDATE plan_steps SET status = ?, output = ?,"
                            " finished_at = datetime('now') WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, output, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, step_id);
   DB_STEP_LOG(stmt, "plan_step_update_status");
}

static void plan_update_status(sqlite3 *db, int plan_id, const char *status)
{
   static const char *sql = "UPDATE execution_plans SET status = ? WHERE id = ?";
   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;
   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 2, plan_id);
   DB_STEP_LOG(stmt, "plan_update_status");
}

static int is_safe_rollback(const char *cmd)
{
   if (!cmd || !cmd[0])
      return 0;
   static const char *safe_prefixes[] = {"git checkout ",
                                         "git restore ",
                                         "git revert ",
                                         "git reset --soft ",
                                         "git stash",
                                         "cp ",
                                         "mv ",
                                         "rm ",
                                         "systemctl restart ",
                                         "systemctl stop ",
                                         "docker restart ",
                                         "docker stop ",
                                         "kubectl rollout undo ",
                                         NULL};
   for (int i = 0; safe_prefixes[i]; i++)
   {
      if (strncmp(cmd, safe_prefixes[i], strlen(safe_prefixes[i])) == 0)
         return 1;
   }
   return !has_shell_metachar(cmd);
}

int agent_plan_rollback_step(sqlite3 *db, plan_t *plan, int step_idx, int timeout_ms)
{
   if (step_idx < 0 || step_idx >= plan->step_count)
      return -1;
   plan_step_t *s = &plan->steps[step_idx];
   if (!s->rollback[0])
      return -1; /* no rollback defined */

   /* Validate rollback command against safe operation whitelist */
   if (!is_safe_rollback(s->rollback))
   {
      fprintf(stderr, "aimee: rollback blocked (unsafe command): %.80s\n", s->rollback);
      plan_step_update_status(db, s->id, "rollback_blocked", "unsafe command");
      return -1;
   }

   /* Execute the rollback command via tool_bash (use cJSON to avoid injection) */
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "command", s->rollback);
   char *args_json = cJSON_PrintUnformatted(args);
   cJSON_Delete(args);
   if (!args_json)
      return -1;
   char *result = dispatch_tool_call("bash", args_json, timeout_ms);
   free(args_json);
   if (result)
   {
      plan_step_update_status(db, s->id, "rolled_back", result);
      s->status = 4;
      free(result);
      return 0;
   }
   return -1;
}

int agent_plan_execute(sqlite3 *db, plan_t *plan, const agent_t *agent, int timeout_ms)
{
   if (!db || !plan || plan->step_count == 0)
      return -1;

   plan_update_status(db, plan->id, "running");
   snprintf(plan->status, sizeof(plan->status), "running");

   int to = timeout_ms > 0 ? timeout_ms : AGENT_DEFAULT_TIMEOUT_MS;

   for (int i = 0; i < plan->step_count; i++)
   {
      plan_step_t *s = &plan->steps[i];
      if (s->status == 2) /* already done */
         continue;

      /* Check dependencies */
      int deps_met = 1;
      for (int d = 0; d < s->dep_count; d++)
      {
         for (int j = 0; j < plan->step_count; j++)
         {
            if (plan->steps[j].id == s->depends_on[d] && plan->steps[j].status != 2)
            {
               deps_met = 0;
               break;
            }
         }
         if (!deps_met)
            break;
      }
      if (!deps_met)
      {
         plan_step_update_status(db, s->id, "failed", "dependency not met");
         s->status = 3;
         plan_update_status(db, plan->id, "failed");
         snprintf(plan->status, sizeof(plan->status), "failed");
         return -1;
      }

      /* Mark running */
      plan_step_update_status(db, s->id, "running", NULL);
      s->status = 1;

      /* Policy check before execution */
      char policy_reason[256] = {0};
      if (policy_check_tool("bash", "exec", s->action, policy_reason, sizeof(policy_reason)) < 0)
      {
         char err[512];
         snprintf(err, sizeof(err), "blocked by policy: %s", policy_reason);
         plan_step_update_status(db, s->id, "failed", err);
         s->status = 3;
         plan_update_status(db, plan->id, "failed");
         snprintf(plan->status, sizeof(plan->status), "failed");
         return -1;
      }

      /* Execute the action as a bash command (use cJSON to avoid injection) */
      cJSON *args = cJSON_CreateObject();
      cJSON_AddStringToObject(args, "command", s->action);
      char *args_json = cJSON_PrintUnformatted(args);
      cJSON_Delete(args);
      if (!args_json)
      {
         plan_step_update_status(db, s->id, "failed", "out of memory");
         s->status = 3;
         plan_update_status(db, plan->id, "failed");
         snprintf(plan->status, sizeof(plan->status), "failed");
         return -1;
      }
      char *result = dispatch_tool_call("bash", args_json, to);
      free(args_json);

      /* Check success */
      int success = 0;
      if (result)
      {
         cJSON *rj = cJSON_Parse(result);
         if (rj)
         {
            cJSON *ec = cJSON_GetObjectItem(rj, "exit_code");
            if (ec && ec->valueint == 0)
               success = 1;

            /* Check success predicate if defined */
            if (success && s->success_predicate[0])
            {
               cJSON *out = cJSON_GetObjectItem(rj, "stdout");
               if (out && cJSON_IsString(out))
               {
                  if (strstr(out->valuestring, s->success_predicate) == NULL)
                     success = 0;
               }
            }
            cJSON_Delete(rj);
         }
         snprintf(s->output, sizeof(s->output), "%.*s", (int)(sizeof(s->output) - 1), result);
      }

      if (success)
      {
         plan_step_update_status(db, s->id, "done", result);
         s->status = 2;
      }
      else
      {
         plan_step_update_status(db, s->id, "failed", result);
         s->status = 3;

         /* Try rollback */
         if (s->rollback[0])
            agent_plan_rollback_step(db, plan, i, to);

         plan_update_status(db, plan->id, "failed");
         snprintf(plan->status, sizeof(plan->status), "failed");
         free(result);
         return -1;
      }
      free(result);
   }

   plan_update_status(db, plan->id, "done");
   snprintf(plan->status, sizeof(plan->status), "done");
   return 0;
}

/* --- Two-phase plan execution --- */

int agent_execute_with_plan(sqlite3 *db, const agent_t *agent, const agent_network_t *network,
                            const char *system_prompt, const char *user_prompt, int max_tokens,
                            double temperature, agent_result_t *out)
{
   if (!db || !agent || !user_prompt || !out)
      return -1;

   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);

   /* Phase 1: Ask the LLM to produce a plan */
   char plan_prompt[8192];
   snprintf(plan_prompt, sizeof(plan_prompt),
            "You are a planning agent. For the following task, produce a step-by-step "
            "execution plan as a JSON array. Each step must have:\n"
            "  - \"action\": a shell command to run\n"
            "  - \"precondition\": what must be true before this step (or empty)\n"
            "  - \"success_predicate\": text that must appear in stdout on success (or empty)\n"
            "  - \"rollback\": a shell command to undo this step on failure (or empty)\n\n"
            "Respond ONLY with a JSON array, no other text.\n\nTask: %s",
            user_prompt);

   agent_result_t plan_result;
   int rc =
       agent_execute(db, agent, system_prompt, plan_prompt, max_tokens, temperature, &plan_result);

   if (rc != 0 || !plan_result.response)
   {
      /* Planning failed, fall back to direct tool-use execution */
      free(plan_result.response);
      return agent_execute_with_tools(db, agent, network, system_prompt, user_prompt, max_tokens,
                                      temperature, out);
   }

   /* Try to parse as JSON array */
   /* Find the start of the JSON array (LLM may include preamble) */
   const char *json_start = strchr(plan_result.response, '[');
   cJSON *plan_json = json_start ? cJSON_Parse(json_start) : NULL;

   if (!plan_json || !cJSON_IsArray(plan_json) || cJSON_GetArraySize(plan_json) == 0)
   {
      /* Not valid JSON plan, fall back to direct execution */
      cJSON_Delete(plan_json);
      free(plan_result.response);
      return agent_execute_with_tools(db, agent, network, system_prompt, user_prompt, max_tokens,
                                      temperature, out);
   }

   free(plan_result.response);

   /* Phase 2: Store and execute the plan */
   int plan_id = agent_plan_create(db, agent->name, user_prompt, plan_json);
   cJSON_Delete(plan_json);

   if (plan_id < 0)
      return agent_execute_with_tools(db, agent, network, system_prompt, user_prompt, max_tokens,
                                      temperature, out);

   plan_t plan;
   if (agent_plan_load(db, plan_id, &plan) != 0)
      return -1;

   int timeout = agent->timeout_ms > 0 ? agent->timeout_ms : AGENT_DEFAULT_TIMEOUT_MS;
   rc = agent_plan_execute(db, &plan, agent, timeout);

   /* Build response from plan outputs */
   size_t resp_len = 256;
   for (int i = 0; i < plan.step_count; i++)
      resp_len += strlen(plan.steps[i].output) + 128;
   out->response = malloc(resp_len);
   if (out->response)
   {
      size_t pos = 0;
      static const char *snames[] = {"pending", "running", "done", "failed", "rolled_back"};
      for (int i = 0; i < plan.step_count; i++)
      {
         const char *sn = (plan.steps[i].status >= 0 && plan.steps[i].status <= 4)
                              ? snames[plan.steps[i].status]
                              : "unknown";
         pos += (size_t)snprintf(out->response + pos, resp_len - pos, "Step %d [%s]: %s\n%s\n\n",
                                 i + 1, sn, plan.steps[i].action, plan.steps[i].output);
      }
   }
   out->success = (rc == 0);
   out->turns = plan.step_count;
   out->tool_calls = plan.step_count;
   return rc;
}
