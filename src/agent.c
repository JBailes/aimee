/* agent.c: core agentic loop, execution, outcome tracking */
#include "aimee.h"
#include "agent.h"
#include "agent_protocol.h"
#include "agent_tunnel.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#endif

/* Forward declarations for static helpers */
static void agent_store_feedback(sqlite3 *db, const agent_result_t *result, const char *role,
                                 const char *prompt_summary);

static int is_chatgpt_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "chatgpt") == 0;
}

static int is_anthropic_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "anthropic") == 0;
}

/* --- Default execution system prompt --- */

static const char *default_exec_instructions =
    "# Instructions\n"
    "- Use the bash tool to run commands, including SSH to remote hosts.\n"
    "- Use read_file and write_file for file operations.\n"
    "- Use list_files to explore directories.\n"
    "- When you have completed the task, respond with a final summary.\n"
    "- If you encounter an error, try to diagnose and fix it.\n"
    "- Do not ask for confirmation. Execute the task directly.\n";

/* --- Agentic tool-use loop --- */

int agent_execute_with_tools(sqlite3 *db, const agent_t *agent, const agent_network_t *network,
                             const char *system_prompt, const char *user_prompt, int max_tokens,
                             double temperature, agent_result_t *out)
{
#ifdef _WIN32
   memset(out, 0, sizeof(*out));
   snprintf(out->error, sizeof(out->error), "tool execution not supported on Windows");
   return -1;
#else
   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);

   if (!user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }

   struct timespec loop_start;
   clock_gettime(CLOCK_MONOTONIC, &loop_start);
   int total_timeout_ms = agent->timeout_ms * 4;

   /* Per-invocation checkpoint context (no global mutable state) */
   checkpoint_ctx_t *ckpt_ctx = checkpoint_ctx_new();

   /* Ephemeral SSH setup */
   char ephemeral_key[MAX_PATH_LEN] = {0};
   char session_id[128] = {0};
   int has_ephemeral_ssh = 0;

   /* Start tunnels if configured */
   int has_tunnels = 0;
   if (network && network->tunnel_mgr && network->tunnel_mgr->tunnel_count > 0)
   {
      agent_tunnel_mgr_init(network->tunnel_mgr);
      if (agent_tunnel_start_all(network->tunnel_mgr) == 0)
         has_tunnels = 1;
   }

   /* Build effective network config with ephemeral key if available */
   agent_network_t eff_network;
   if (network && network->ssh_entry[0])
   {
      memcpy(&eff_network, network, sizeof(eff_network));
      if (agent_ssh_setup(network, ephemeral_key, sizeof(ephemeral_key), session_id,
                          sizeof(session_id)) == 0)
      {
         has_ephemeral_ssh = 1;
         /* Update the effective network's ssh_entry to use the ephemeral key */
         char new_entry[512];
         snprintf(new_entry, sizeof(new_entry), "%s -i %s", network->ssh_entry, ephemeral_key);
         snprintf(eff_network.ssh_entry, sizeof(eff_network.ssh_entry), "%s", new_entry);
         snprintf(eff_network.ssh_key, sizeof(eff_network.ssh_key), "%s", ephemeral_key);
      }
   }
   else
   {
      memset(&eff_network, 0, sizeof(eff_network));
   }

   /* Link tunnel manager to effective network */
   if (has_tunnels)
      eff_network.tunnel_mgr = network->tunnel_mgr;

   /* Build context-rich system prompt */
   char *assembled_sys = agent_build_exec_context(
       db, agent, has_ephemeral_ssh ? &eff_network : (network ? network : NULL), system_prompt);
   const char *sys = assembled_sys ? assembled_sys : system_prompt;
   if (!sys || !sys[0])
      sys = default_exec_instructions;

   /* Resolve auth */
   char auth_header[MAX_API_KEY_LEN + 32];
   if (agent_resolve_auth(agent, auth_header, sizeof(auth_header)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "auth resolution failed");
      free(assembled_sys);
      checkpoint_ctx_free(ckpt_ctx);
      if (has_ephemeral_ssh)
         agent_ssh_cleanup(network, ephemeral_key, session_id);
      if (has_tunnels && network && network->tunnel_mgr)
         agent_tunnel_stop_all(network->tunnel_mgr);
      return -1;
   }

   /* Mutable copy for model fallback */
   agent_t fb_agent;
   memcpy(&fb_agent, agent, sizeof(fb_agent));

   /* Build URL */
   char url[MAX_ENDPOINT_LEN + 64];
   int chatgpt = is_chatgpt_provider(agent);
   int anthropic = is_anthropic_provider(agent);
   if (chatgpt)
      snprintf(url, sizeof(url), "%s/responses", agent->endpoint);
   else if (anthropic)
      snprintf(url, sizeof(url), "%s/messages", agent->endpoint);
   else
      snprintf(url, sizeof(url), "%s/chat/completions", agent->endpoint);

   /* Build tools JSON (reused each turn, format depends on provider) */
   cJSON *tools = chatgpt     ? build_tools_array_responses()
                  : anthropic ? build_tools_array_anthropic()
                              : build_tools_array();

   /* Build conversation history */
   cJSON *messages = cJSON_CreateArray();

   /* For OpenAI, system prompt goes in messages array. For Anthropic and ChatGPT,
    * it goes in the request body (handled by request builder). */
   if (!chatgpt && !anthropic)
   {
      cJSON *sys_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(sys_msg, "role", "system");
      cJSON_AddStringToObject(sys_msg, "content", sys);
      cJSON_AddItemToArray(messages, sys_msg);
   }

   cJSON *user_msg = cJSON_CreateObject();
   cJSON_AddStringToObject(user_msg, "role", "user");
   cJSON_AddStringToObject(user_msg, "content", user_prompt);
   cJSON_AddItemToArray(messages, user_msg);

   int turn = 0;
   int total_calls = 0;
   int tok = (max_tokens > 0) ? max_tokens : agent->max_tokens;
   /* max_turns == 0 means unlimited (capped at 1000 as safety net);
    * negative or unset falls back to the default. */
   int max_t = (agent->max_turns > 0)    ? agent->max_turns
               : (agent->max_turns == 0) ? 1000
                                         : AGENT_DEFAULT_MAX_TURNS;

   /* Stuck detection state */
   char last_tool_sig[256] = {0};
   int repeat_count = 0;
   int transient_retries = 0;

   /* Adaptive context refresh: more frequent for complex tasks (many tool calls),
    * less frequent for simple ones. Start at 5, decrease as calls accumulate. */
   int refresh_interval = 5;

   while (turn < max_t)
   {
      /* Check total timeout */
      struct timespec now_ts;
      clock_gettime(CLOCK_MONOTONIC, &now_ts);
      int elapsed_ms = (int)((now_ts.tv_sec - loop_start.tv_sec) * 1000 +
                             (now_ts.tv_nsec - loop_start.tv_nsec) / 1000000);
      if (elapsed_ms > total_timeout_ms)
      {
         snprintf(out->error, sizeof(out->error), "total timeout exceeded (%dms)", elapsed_ms);
         break;
      }

      /* Adaptive context refresh: adjust interval based on task complexity */
      if (turn > 0 && (turn % refresh_interval) == 0)
      {
         char *refreshed = agent_build_exec_context(
             db, agent, has_ephemeral_ssh ? &eff_network : (network ? network : NULL),
             system_prompt);
         if (refreshed)
         {
            free(assembled_sys);
            assembled_sys = refreshed;
            sys = assembled_sys;

            /* Update system message in conversation for OpenAI providers */
            if (!chatgpt && !anthropic)
            {
               cJSON *first = cJSON_GetArrayItem(messages, 0);
               if (first)
                  cJSON_ReplaceItemInObject(first, "content", cJSON_CreateString(sys));
            }
         }
      }

      /* Compact consecutive same-role messages before sending */
      messages_compact_consecutive(messages);

      /* Build request (use fb_agent which may have fallback model after turn 0) */
      cJSON *req;
      if (chatgpt)
         req = agent_build_request_responses(&fb_agent, messages, tools, sys);
      else if (anthropic)
         req = agent_build_request_anthropic(&fb_agent, messages, tools, sys, tok, temperature);
      else
         req = agent_build_request_openai(&fb_agent, messages, tools, tok, temperature);

      char *body = cJSON_PrintUnformatted(req);
      cJSON_Delete(req);
      if (!body)
      {
         snprintf(out->error, sizeof(out->error), "failed to build request");
         break;
      }

      /* Log request trace */
      agent_trace_log(db, 0, turn, "request", body, NULL, NULL, NULL, NULL);

      /* POST */
      char *response_body = NULL;
      int remaining = total_timeout_ms - elapsed_ms;
      int per_call = (agent->timeout_ms < remaining) ? agent->timeout_ms : remaining;
      int http_status =
          agent_http_post(url, auth_header, body, &response_body, per_call, agent->extra_headers);
      free(body);

      /* Model fallback on first turn: if 400, retry with fallback_model */
      if (http_status == 400 && turn == 0 && fb_agent.fallback_model[0])
      {
         free(response_body);
         response_body = NULL;
         snprintf(fb_agent.model, MAX_MODEL_LEN, "%s", fb_agent.fallback_model);
         fb_agent.fallback_model[0] = '\0';

         cJSON *fb_req;
         if (chatgpt)
            fb_req = agent_build_request_responses(&fb_agent, messages, tools, sys);
         else if (anthropic)
            fb_req =
                agent_build_request_anthropic(&fb_agent, messages, tools, sys, tok, temperature);
         else
            fb_req = agent_build_request_openai(&fb_agent, messages, tools, tok, temperature);
         char *fb_body = cJSON_PrintUnformatted(fb_req);
         cJSON_Delete(fb_req);
         if (fb_body)
         {
            http_status = agent_http_post(url, auth_header, fb_body, &response_body, per_call,
                                          fb_agent.extra_headers);
            free(fb_body);
         }
      }

      /* Update provider health cache */
      provider_health_update(agent->provider, http_status);

      /* --- Transient error retry: connection failures and 503 --- */
      if (http_status < 0 ||
          (http_status == 503 && response_body && strstr(response_body, "Loading model")))
      {
         free(response_body);
         response_body = NULL;
         if (transient_retries < 3)
         {
            transient_retries++;
            int wait_sec = transient_retries * 5;
            fprintf(stderr, "agent: transient error, retrying in %ds (%d/3)...\n", wait_sec,
                    transient_retries);
            sleep((unsigned)wait_sec);
            continue; /* retry same turn */
         }
         provider_err_class_t cls = provider_classify_error(http_status);
         snprintf(out->error, sizeof(out->error),
                  "provider '%s' %s Local commands (memory, index, rules, db) still work. "
                  "Run 'aimee agent test %s' for diagnostics.",
                  agent->name, provider_error_message(cls), agent->name);
         break;
      }

      /* --- Context overflow: truncate old messages and retry --- */
      if (http_status == 400 && response_body &&
          strstr(response_body, "exceeds the available context"))
      {
         free(response_body);
         response_body = NULL;

         /* Remove oldest non-system, non-first-user messages to free space.
          * Keep: [0] system, [1] first user message, then trim from index 2. */
         int msg_count = cJSON_GetArraySize(messages);
         int removed = 0;
         while (msg_count > 4 && removed < (msg_count / 3))
         {
            cJSON_DeleteItemFromArray(messages, 2);
            removed++;
            msg_count = cJSON_GetArraySize(messages);
         }
         if (removed > 0)
         {
            fprintf(stderr, "agent: context overflow, truncated %d messages, retrying...\n",
                    removed);
            continue; /* retry same turn with shorter context */
         }
         /* Nothing left to truncate */
         snprintf(out->error, sizeof(out->error),
                  "provider '%s' (HTTP 400): context overflow, cannot truncate further",
                  agent->name);
         break;
      }

      if (http_status < 0 || !response_body)
      {
         provider_err_class_t cls = provider_classify_error(http_status);
         snprintf(out->error, sizeof(out->error),
                  "provider '%s' %s Local commands (memory, index, rules, db) still work. "
                  "Run 'aimee agent test %s' for diagnostics.",
                  agent->name, provider_error_message(cls), agent->name);
         free(response_body);
         break;
      }

      if (http_status != 200)
      {
         provider_err_class_t cls = provider_classify_error(http_status);
         char snippet[128] = {0};
         if (response_body)
            snprintf(snippet, sizeof(snippet), "%.120s", response_body);
         snprintf(out->error, sizeof(out->error), "provider '%s' (HTTP %d): %s %s", agent->name,
                  http_status, provider_error_message(cls), snippet);
         free(response_body);
         break;
      }

      /* Reset transient retry counter on success */
      transient_retries = 0;

      /* Parse response */
      parsed_response_t parsed;
      if (chatgpt)
      {
         agent_parse_response_responses(response_body, &parsed);
      }
      else
      {
         cJSON *root = cJSON_Parse(response_body);
         if (root)
         {
            if (anthropic)
               agent_parse_response_anthropic(root, &parsed);
            else
               agent_parse_response_openai(root, &parsed);
            cJSON_Delete(root);
         }
         else
         {
            memset(&parsed, 0, sizeof(parsed));
         }
      }
      free(response_body);

      /* Log response trace */
      agent_trace_log(db, 0, turn, "response", parsed.content ? parsed.content : "(tool_calls)",
                      NULL, NULL, NULL, NULL);

      /* Accumulate tokens */
      out->prompt_tokens += parsed.prompt_tokens;
      out->completion_tokens += parsed.completion_tokens;

      if (!parsed.is_tool_call)
      {
         /* Final text response */
         if (parsed.content)
         {
            out->response = parsed.content;
            parsed.content = NULL; /* prevent agent_free_parsed_response from freeing it */
            out->success = 1;
         }
         else
         {
            snprintf(out->error, sizeof(out->error), "no content in final response");
         }
         agent_free_parsed_response(&parsed);
         break;
      }

      /* Execute tool calls */
      if (anthropic && parsed.assistant_message)
      {
         /* Anthropic: assistant_message is the content array itself */
         cJSON *asst = cJSON_CreateObject();
         cJSON_AddStringToObject(asst, "role", "assistant");
         cJSON_AddItemToObject(asst, "content", cJSON_Duplicate(parsed.assistant_message, 1));
         cJSON_AddItemToArray(messages, asst);
      }
      else if (!chatgpt && parsed.assistant_message)
      {
         /* OpenAI: append the assistant message (with tool_calls) to conversation */
         cJSON *asst = cJSON_CreateObject();
         cJSON_AddStringToObject(asst, "role", "assistant");

         cJSON *tc_arr = cJSON_GetObjectItem(parsed.assistant_message, "tool_calls");
         if (tc_arr)
            cJSON_AddItemToObject(asst, "tool_calls", cJSON_Duplicate(tc_arr, 1));

         cJSON *asst_content = cJSON_GetObjectItem(parsed.assistant_message, "content");
         if (asst_content && !cJSON_IsNull(asst_content))
            cJSON_AddItemToObject(asst, "content", cJSON_Duplicate(asst_content, 1));
         else
            cJSON_AddNullToObject(asst, "content");

         cJSON_AddItemToArray(messages, asst);
      }
      else if (chatgpt && parsed.assistant_message)
      {
         /* Responses API: append only the function_call items we will actually
          * process (i.e. up to parsed.call_count). Appending calls beyond that
          * would cause the API to expect function_call_output for IDs we never
          * return, resulting in HTTP 400. */
         for (int i = 0; i < parsed.call_count; i++)
         {
            int n = cJSON_GetArraySize(parsed.assistant_message);
            for (int j = 0; j < n; j++)
            {
               cJSON *item = cJSON_GetArrayItem(parsed.assistant_message, j);
               cJSON *itype = cJSON_GetObjectItem(item, "type");
               cJSON *cid = cJSON_GetObjectItem(item, "call_id");
               if (itype && cJSON_IsString(itype) &&
                   strcmp(itype->valuestring, "function_call") == 0 && cid && cJSON_IsString(cid) &&
                   strcmp(cid->valuestring, parsed.calls[i].id) == 0)
               {
                  cJSON_AddItemToArray(messages, cJSON_Duplicate(item, 1));
                  break;
               }
            }
         }
      }

      /* For Anthropic, collect all tool_result blocks into one user message */
      cJSON *anth_results = anthropic ? cJSON_CreateArray() : NULL;

      for (int i = 0; i < parsed.call_count; i++)
      {
         /* Validate arguments against tool registry */
         char val_err[256] = {0};
         if (tool_validate(db, parsed.calls[i].name, parsed.calls[i].arguments, val_err,
                           sizeof(val_err)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               total_calls++;
               continue;
            }
            snprintf(err_result, 512, "error: validation failed: %s", val_err);
            /* Send error back to LLM so it can self-correct */
            if (anthropic)
            {
               cJSON *tr = cJSON_CreateObject();
               cJSON_AddStringToObject(tr, "type", "tool_result");
               cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tr, "content", err_result);
               cJSON_AddItemToArray(anth_results, tr);
            }
            else if (!chatgpt)
            {
               cJSON *tool_msg = cJSON_CreateObject();
               cJSON_AddStringToObject(tool_msg, "role", "tool");
               cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tool_msg, "content", err_result);
               cJSON_AddItemToArray(messages, tool_msg);
            }
            else
            {
               cJSON *out_item = cJSON_CreateObject();
               cJSON_AddStringToObject(out_item, "type", "function_call_output");
               cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(out_item, "output", err_result);
               cJSON_AddItemToArray(messages, out_item);
            }
            free(err_result);
            total_calls++;
            continue;
         }

         /* Check policy */
         char policy_reason[256] = {0};
         const char *se = tool_side_effect(db, parsed.calls[i].name);
         if (policy_check_tool(parsed.calls[i].name, se, parsed.calls[i].arguments, policy_reason,
                               sizeof(policy_reason)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               total_calls++;
               continue;
            }
            snprintf(err_result, 512, "error: blocked by policy: %s", policy_reason);
            if (anthropic)
            {
               cJSON *tr = cJSON_CreateObject();
               cJSON_AddStringToObject(tr, "type", "tool_result");
               cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tr, "content", err_result);
               cJSON_AddItemToArray(anth_results, tr);
            }
            else if (!chatgpt)
            {
               cJSON *tool_msg = cJSON_CreateObject();
               cJSON_AddStringToObject(tool_msg, "role", "tool");
               cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tool_msg, "content", err_result);
               cJSON_AddItemToArray(messages, tool_msg);
            }
            else
            {
               cJSON *out_item = cJSON_CreateObject();
               cJSON_AddStringToObject(out_item, "type", "function_call_output");
               cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(out_item, "output", err_result);
               cJSON_AddItemToArray(messages, out_item);
            }
            free(err_result);
            total_calls++;
            continue;
         }

         /* Check hard directives (Feature 15) */
         char directive_reason[256] = {0};
         if (directive_check_tool(db, parsed.calls[i].name, parsed.calls[i].arguments,
                                  directive_reason, sizeof(directive_reason)) != 0)
         {
            char *err_result = malloc(512);
            if (!err_result)
            {
               total_calls++;
               continue;
            }
            snprintf(err_result, 512, "error: blocked by directive: %s", directive_reason);
            if (anthropic)
            {
               cJSON *tr = cJSON_CreateObject();
               cJSON_AddStringToObject(tr, "type", "tool_result");
               cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tr, "content", err_result);
               cJSON_AddItemToArray(anth_results, tr);
            }
            else if (!chatgpt)
            {
               cJSON *tool_msg = cJSON_CreateObject();
               cJSON_AddStringToObject(tool_msg, "role", "tool");
               cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(tool_msg, "content", err_result);
               cJSON_AddItemToArray(messages, tool_msg);
            }
            else
            {
               cJSON *out_item = cJSON_CreateObject();
               cJSON_AddStringToObject(out_item, "type", "function_call_output");
               cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
               cJSON_AddStringToObject(out_item, "output", err_result);
               cJSON_AddItemToArray(messages, out_item);
            }
            free(err_result);
            total_calls++;
            continue;
         }

         char *result_str = dispatch_tool_call_ctx(parsed.calls[i].name, parsed.calls[i].arguments,
                                                   agent->timeout_ms, ckpt_ctx);
         total_calls++;

         /* Log trace */
         agent_trace_log(db, 0, turn, "tool_call", NULL, parsed.calls[i].name,
                         parsed.calls[i].arguments, result_str, NULL);

         if (anthropic)
         {
            /* Anthropic: tool_result content block */
            cJSON *tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "type", "tool_result");
            cJSON_AddStringToObject(tr, "tool_use_id", parsed.calls[i].id);
            cJSON_AddStringToObject(tr, "content", result_str ? result_str : "");
            cJSON_AddItemToArray(anth_results, tr);
         }
         else if (!chatgpt)
         {
            /* OpenAI format: role=tool, tool_call_id */
            cJSON *tool_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", parsed.calls[i].id);
            cJSON_AddStringToObject(tool_msg, "content", result_str ? result_str : "");
            cJSON_AddItemToArray(messages, tool_msg);
         }
         else
         {
            /* Responses API: function_call_output */
            cJSON *out_item = cJSON_CreateObject();
            cJSON_AddStringToObject(out_item, "type", "function_call_output");
            cJSON_AddStringToObject(out_item, "call_id", parsed.calls[i].id);
            cJSON_AddStringToObject(out_item, "output", result_str ? result_str : "");
            cJSON_AddItemToArray(messages, out_item);
         }

         free(result_str);
      }

      /* Anthropic: append all tool results as a single user message */
      if (anth_results)
      {
         if (cJSON_GetArraySize(anth_results) > 0)
         {
            cJSON *user_tr = cJSON_CreateObject();
            cJSON_AddStringToObject(user_tr, "role", "user");
            cJSON_AddItemToObject(user_tr, "content", anth_results);
            cJSON_AddItemToArray(messages, user_tr);
         }
         else
         {
            cJSON_Delete(anth_results);
         }
      }

      /* Stuck detection: if the agent calls the same tool with the same args
       * 3+ times in a row, inject a warning and adjust refresh interval */
      if (parsed.call_count > 0)
      {
         char tool_sig[256];
         snprintf(tool_sig, sizeof(tool_sig), "%s:%.*s", parsed.calls[0].name, 200,
                  parsed.calls[0].arguments ? parsed.calls[0].arguments : "");
         if (strcmp(tool_sig, last_tool_sig) == 0)
         {
            repeat_count++;
            if (repeat_count >= 3)
            {
               /* Inject a hint to the LLM that it appears stuck */
               cJSON *hint = cJSON_CreateObject();
               if (chatgpt)
               {
                  cJSON_AddStringToObject(hint, "role", "user");
                  cJSON_AddStringToObject(hint, "content",
                                          "Warning: You appear to be repeating the same operation. "
                                          "Try a different approach or summarize your findings.");
               }
               else
               {
                  cJSON_AddStringToObject(hint, "role", "user");
                  cJSON_AddStringToObject(hint, "content",
                                          "Warning: You appear to be repeating the same operation. "
                                          "Try a different approach or summarize your findings.");
               }
               cJSON_AddItemToArray(messages, hint);
               repeat_count = 0;
               /* Also increase refresh frequency to give more context */
               if (refresh_interval > 2)
                  refresh_interval = 2;
            }
         }
         else
         {
            snprintf(last_tool_sig, sizeof(last_tool_sig), "%s", tool_sig);
            repeat_count = 0;
         }

         /* Adaptive refresh: after many tool calls, refresh more often */
         if (total_calls > 10 && refresh_interval > 3)
            refresh_interval = 3;
      }

      agent_free_parsed_response(&parsed);
      turn++;

      /* Update durable job cursor and heartbeat */
      int durable_job_id = agent_get_durable_job_id();
      sqlite3 *durable_db = agent_get_durable_db();
      if (durable_job_id > 0 && durable_db)
      {
         agent_job_update(durable_db, durable_job_id, "running", turn, NULL);
         agent_job_heartbeat(durable_db, durable_job_id);
      }
   }

   /* If we exhausted turns without a final response, set a clear error */
   if (!out->success && !out->response && turn >= max_t)
   {
      snprintf(out->error, sizeof(out->error), "max turns exhausted (%d/%d) without final response",
               turn, max_t);
   }

   /* Early exit hint: if we got stuck (repeat_count > 0) or exhausted turns,
    * record the last tool results as a partial success */
   if (!out->success && !out->response && total_calls > 0 && repeat_count >= 2)
   {
      out->response = safe_strdup("(agent appears stuck, returning partial results)");
      out->success = 0;
      snprintf(out->error, sizeof(out->error), "agent stuck: repeated same tool call %d times",
               repeat_count);
   }

   /* Record timing */
   struct timespec end_ts;
   clock_gettime(CLOCK_MONOTONIC, &end_ts);
   out->latency_ms = (int)((end_ts.tv_sec - loop_start.tv_sec) * 1000 +
                           (end_ts.tv_nsec - loop_start.tv_nsec) / 1000000);
   out->turns = turn;
   out->tool_calls = total_calls;

   /* Confidence estimation and abstention (Feature 7) */
   if (out->response)
      out->confidence = agent_estimate_confidence(out->response);
   else
      out->confidence = 0;

   if (out->confidence >= 0 && out->confidence < 50 && out->tool_calls > 0)
   {
      out->abstained = 1;
      snprintf(out->abstain_reason, sizeof(out->abstain_reason),
               "low confidence (%d) after %d tool calls", out->confidence, out->tool_calls);
   }

   /* Cleanup per-invocation checkpoint context */
   checkpoint_ctx_free(ckpt_ctx);

   /* Cleanup */
   cJSON_Delete(tools);
   cJSON_Delete(messages);
   free(assembled_sys);

   /* Cleanup ephemeral SSH */
   if (has_ephemeral_ssh)
      agent_ssh_cleanup(network, ephemeral_key, session_id);

   /* Stop tunnels */
   if (has_tunnels && network && network->tunnel_mgr)
      agent_tunnel_stop_all(network->tunnel_mgr);

   /* Log */
   if (db)
      agent_log_call(db, out, "");

   /* Write Prometheus metrics */
   if (db)
      agent_write_metrics(db);

   /* Write change manifest */
   if (db)
   {
      char run_id[64];
      snprintf(run_id, sizeof(run_id), "aimee-run-%d-%ld", (int)getpid(), (long)loop_start.tv_sec);
      agent_write_manifest(db, run_id, out, "");
   }

   /* Store execution outcome as feedback for future context */
   if (db)
      agent_store_feedback(db, out, "", user_prompt);

   return out->success ? 0 : -1;
#endif /* _WIN32 */
}

/* --- Outcome classification and recording --- */

static void classify_outcome(const agent_result_t *result, int max_turns, agent_outcome_t *outcome)
{
   memset(outcome, 0, sizeof(*outcome));
   outcome->turns_used = result->turns;
   outcome->tools_called = result->tool_calls;
   outcome->tokens_used = result->prompt_tokens + result->completion_tokens;

   if (result->abstained)
   {
      outcome->outcome = OUTCOME_PARTIAL;
      snprintf(outcome->reason, sizeof(outcome->reason), "abstained: %s", result->abstain_reason);
      return;
   }

   if (!result->success)
   {
      if (result->error[0])
      {
         outcome->outcome = OUTCOME_ERROR;
         snprintf(outcome->reason, sizeof(outcome->reason), "%.250s", result->error);

         /* Extract tool+error pattern for anti-pattern detection */
         const char *tool_err = strstr(result->error, "tool ");
         if (tool_err)
            snprintf(outcome->tool_error_pattern, sizeof(outcome->tool_error_pattern), "%.120s",
                     tool_err);
      }
      else
      {
         outcome->outcome = OUTCOME_FAILURE;
         snprintf(outcome->reason, sizeof(outcome->reason), "execution failed");
      }
      return;
   }

   /* Success but check if max turns was hit (likely partial) */
   if (max_turns > 0 && result->turns >= max_turns)
   {
      outcome->outcome = OUTCOME_PARTIAL;
      snprintf(outcome->reason, sizeof(outcome->reason), "max turns reached (%d)", max_turns);
      return;
   }

   outcome->outcome = OUTCOME_SUCCESS;
   snprintf(outcome->reason, sizeof(outcome->reason), "completed in %d turns", result->turns);
}

static void record_outcome(sqlite3 *db, const char *agent_name, const char *role,
                           const agent_outcome_t *outcome)
{
   if (!db)
      return;

   static const char *sql = "INSERT INTO agent_outcomes"
                            " (agent_name, role, outcome, reason, turns_used,"
                            "  tools_called, tokens_used, tool_error_pattern)"
                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   static const char *outcome_str[] = {"success", "partial", "failure", "error"};
   const char *otype = outcome_str[outcome->outcome];

   sqlite3_bind_text(stmt, 1, agent_name ? agent_name : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, role ? role : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, otype, -1, SQLITE_STATIC);
   sqlite3_bind_text(stmt, 4, outcome->reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 5, outcome->turns_used);
   sqlite3_bind_int(stmt, 6, outcome->tools_called);
   sqlite3_bind_int64(stmt, 7, outcome->tokens_used);
   sqlite3_bind_text(stmt, 8, outcome->tool_error_pattern, -1, SQLITE_TRANSIENT);

   DB_STEP_LOG(stmt, "record_outcome");
   sqlite3_reset(stmt);
}

/* --- Run with fallback --- */

int agent_run(sqlite3 *db, agent_config_t *cfg, const char *role, const char *system_prompt,
              const char *user_prompt, int max_tokens, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Check result cache first (#2) */
   char *cached = agent_cache_get(db, role, user_prompt);
   if (cached)
   {
      out->response = cached;
      out->success = 1;
      snprintf(out->agent_name, MAX_AGENT_NAME, "cache");
      return 0;
   }

   /* Try fallback chain first */
   for (int i = 0; i < cfg->fallback_count; i++)
   {
      agent_t *ag = agent_find(cfg, cfg->fallback_chain[i]);
      if (!ag || !ag->enabled || !agent_has_role(ag, role))
         continue;

      int use_tools = ag->tools_enabled && agent_is_exec_role(ag, role);
      int rc;
      if (use_tools)
         rc = agent_execute_with_tools(db, ag, &cfg->network, system_prompt, user_prompt,
                                       max_tokens, 0.3, out);
      else
         rc = agent_execute(db, ag, system_prompt, user_prompt, max_tokens, 0.3, out);

      if (rc == 0)
      {
         if (db)
         {
            agent_log_call(db, out, role);
            agent_store_feedback(db, out, role, user_prompt);
         }
         agent_cache_put(db, role, user_prompt, out);
         return 0;
      }
      /* Failed, try next */
      free(out->response);
      out->response = NULL;
   }

   /* Try any available agent with this role (cheapest first) */
   agent_t *ag = agent_route(cfg, role);
   if (ag)
   {
      int use_tools = ag->tools_enabled && agent_is_exec_role(ag, role);

      /* Build enhanced prompt with hint from past successes (#5) */
      char *hint = agent_find_hint(db, role, user_prompt);
      const char *effective_prompt = user_prompt;
      char *enhanced = NULL;
      if (hint && use_tools)
      {
         size_t elen = strlen(user_prompt) + strlen(hint) + 4;
         enhanced = malloc(elen);
         if (enhanced)
         {
            snprintf(enhanced, elen, "%s\n\n%s", user_prompt, hint);
            effective_prompt = enhanced;
         }
      }
      free(hint);

      int rc;
      if (use_tools)
         rc = agent_execute_with_tools(db, ag, &cfg->network, system_prompt, effective_prompt,
                                       max_tokens, 0.3, out);
      else
         rc = agent_execute(db, ag, system_prompt, effective_prompt, max_tokens, 0.3, out);

      free(enhanced);

      if (rc == 0)
      {
         if (db)
         {
            agent_log_call(db, out, role);
            agent_store_feedback(db, out, role, user_prompt);

            agent_outcome_t oc;
            classify_outcome(out, ag->max_turns, &oc);
            record_outcome(db, out->agent_name, role, &oc);
         }
         agent_cache_put(db, role, user_prompt, out);
         return 0;
      }
   }

   /* Store failure feedback and outcome */
   if (db)
   {
      agent_store_feedback(db, out, role, user_prompt);

      agent_outcome_t oc;
      classify_outcome(out, 0, &oc);
      record_outcome(db, out->agent_name, role, &oc);
   }

   if (!out->error[0])
      snprintf(out->error, sizeof(out->error), "no agent available for role '%s'", role);
   return -1;
}

/* Like agent_run but always uses tool execution, even if the agent config
 * does not have tools_enabled set. Used by the delegate command. */
int agent_run_with_tools(sqlite3 *db, agent_config_t *cfg, const char *role,
                         const char *system_prompt, const char *user_prompt, int max_tokens,
                         agent_result_t *out)
{
   memset(out, 0, sizeof(*out));

   agent_t *ag = agent_route(cfg, role);
   if (!ag)
   {
      snprintf(out->error, sizeof(out->error), "no agent available for role '%s'", role);
      return -1;
   }

   /* Build enhanced prompt with hint from past successes */
   char *hint = agent_find_hint(db, role, user_prompt);
   const char *effective_prompt = user_prompt;
   char *enhanced = NULL;
   if (hint)
   {
      size_t elen = strlen(user_prompt) + strlen(hint) + 4;
      enhanced = malloc(elen);
      if (enhanced)
      {
         snprintf(enhanced, elen, "%s\n\n%s", user_prompt, hint);
         effective_prompt = enhanced;
      }
   }
   free(hint);

   /* Always use tool execution -- that is the contract of this function.
    * agent_run() already handles the tools_enabled gate for normal calls. */
   int rc = agent_execute_with_tools(db, ag, &cfg->network, system_prompt, effective_prompt,
                                     max_tokens, 0.3, out);
   free(enhanced);

   if (db)
   {
      if (rc == 0)
         agent_log_call(db, out, role);
      agent_store_feedback(db, out, role, user_prompt);
   }
   if (rc == 0)
      agent_cache_put(db, role, user_prompt, out);

   return rc;
}

/* --- Parallel execution --- */

typedef struct
{
   sqlite3 *db;
   agent_config_t *cfg;
   agent_task_t *task;
   agent_result_t *result;
} parallel_ctx_t;

#ifdef _WIN32
static unsigned __stdcall parallel_worker(void *arg)
#else
static void *parallel_worker(void *arg)
#endif
{
   parallel_ctx_t *ctx = (parallel_ctx_t *)arg;
   /* Open a per-thread db connection to avoid shared sqlite3 state.
    * SQLite is not safe to share across threads without serialized mode. */
   sqlite3 *thread_db = db_open(NULL);
   agent_run(thread_db, ctx->cfg, ctx->task->role, ctx->task->system_prompt, ctx->task->user_prompt,
             ctx->task->max_tokens, ctx->result);
   if (thread_db)
   {
      db_stmt_cache_clear();
      db_close(thread_db);
   }
#ifdef _WIN32
   return 0;
#else
   return NULL;
#endif
}

int agent_run_parallel(sqlite3 *db, agent_config_t *cfg, agent_task_t *tasks, int task_count,
                       agent_result_t *out)
{
   if (task_count <= 0)
      return 0;
   if (task_count == 1)
   {
      /* Single task: no thread needed */
      int rc = agent_run(db, cfg, tasks[0].role, tasks[0].system_prompt, tasks[0].user_prompt,
                         tasks[0].max_tokens, &out[0]);
      return rc == 0 ? 1 : 0;
   }

   parallel_ctx_t *ctxs = calloc((size_t)task_count, sizeof(parallel_ctx_t));
   if (!ctxs)
      return 0;

#ifdef _WIN32
   HANDLE *threads = calloc((size_t)task_count, sizeof(HANDLE));
#else
   pthread_t *threads = calloc((size_t)task_count, sizeof(pthread_t));
#endif
   if (!threads)
   {
      free(ctxs);
      return 0;
   }

   /* Spawn threads (track which ones actually started) */
   int *thread_ok = calloc((size_t)task_count, sizeof(int));
   for (int i = 0; i < task_count; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      ctxs[i].db = db;
      ctxs[i].cfg = cfg;
      ctxs[i].task = &tasks[i];
      ctxs[i].result = &out[i];

#ifdef _WIN32
      threads[i] = (HANDLE)_beginthreadex(NULL, 0, parallel_worker, &ctxs[i], 0, NULL);
      if (threads[i])
      {
         thread_ok[i] = 1;
      }
#else
      if (pthread_create(&threads[i], NULL, parallel_worker, &ctxs[i]) == 0)
      {
         thread_ok[i] = 1;
      }
#endif
   }

   /* Join only threads that were actually created */
   for (int i = 0; i < task_count; i++)
   {
      if (!thread_ok[i])
         continue;
#ifdef _WIN32
      WaitForSingleObject(threads[i], INFINITE);
      CloseHandle(threads[i]);
#else
      pthread_join(threads[i], NULL);
#endif
   }
   free(thread_ok);

   free(threads);
   free(ctxs);

   /* Count successes */
   int success_count = 0;
   for (int i = 0; i < task_count; i++)
   {
      if (out[i].success)
         success_count++;
   }
   return success_count;
}

/* --- Post-execution feedback (Item 4) --- */

static void agent_store_feedback(sqlite3 *db, const agent_result_t *result, const char *role,
                                 const char *prompt_summary)
{
   if (!db)
      return;
   /* For successes, require a response; for failures, always store */
   if (result->success && (!result->response || !result->response[0]))
      return;

   const char *r = (role && role[0]) ? role : "unknown";

   /* Store outcome as L1 episode memory for future context */
   char key[256];
   snprintf(key, sizeof(key), "delegate_%s_%s_%s", result->agent_name, r,
            result->success ? "ok" : "fail");

   /* Build a prompt excerpt (first 200 chars) */
   char excerpt[256] = "";
   if (prompt_summary && prompt_summary[0])
   {
      snprintf(excerpt, sizeof(excerpt), "%.200s", prompt_summary);
      /* Truncate at last space to avoid mid-word cuts */
      if (strlen(prompt_summary) > 200)
      {
         char *last_sp = strrchr(excerpt, ' ');
         if (last_sp && last_sp > excerpt + 100)
            *last_sp = '\0';
      }
   }

   char content[2048];
   if (result->success)
   {
      snprintf(content, sizeof(content),
               "Delegation [%s] via %s succeeded. %d turns, %d tool calls, %dms. "
               "Confidence: %d. Task: %s",
               r, result->agent_name, result->turns, result->tool_calls, result->latency_ms,
               result->confidence, excerpt);
   }
   else
   {
      snprintf(content, sizeof(content),
               "Delegation [%s] via %s failed. %d turns, %d tool calls, %dms. "
               "Error: %.512s. Task: %s",
               r, result->agent_name, result->turns, result->tool_calls, result->latency_ms,
               result->error, excerpt);
   }

   static const char *sql =
       "INSERT INTO memories (tier, kind, key, content, confidence, use_count,"
       " created_at, updated_at, last_used_at)"
       " VALUES ('L1', 'episode', ?, ?, ?, 1, datetime('now'), datetime('now'), datetime('now'))";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, 3, result->success ? 0.8 : 0.4);
   DB_STEP_LOG(stmt, "agent_store_feedback");
}
