/* agent_protocol.c: LLM request building, response parsing, message utilities */
#include "aimee.h"
#include "agent_protocol.h"
#include "cJSON.h"
#include <string.h>

/* --- Request builders --- */

cJSON *agent_build_request_openai(const agent_t *agent, cJSON *messages, cJSON *tools,
                                  int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);
   cJSON_AddItemReferenceToObject(req, "messages", messages);
   cJSON_AddItemReferenceToObject(req, "tools", tools);

   if (max_tokens > 0)
      cJSON_AddNumberToObject(req, "max_tokens", max_tokens);
   if (temperature >= 0)
      cJSON_AddNumberToObject(req, "temperature", temperature);

   return req;
}

cJSON *agent_build_request_responses(const agent_t *agent, cJSON *input, cJSON *tools,
                                     const char *system_prompt)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   if (system_prompt && system_prompt[0])
      cJSON_AddStringToObject(req, "instructions", system_prompt);
   else
      cJSON_AddStringToObject(req, "instructions", "You are an execution agent.");

   cJSON_AddBoolToObject(req, "store", 0);
   cJSON_AddBoolToObject(req, "stream", 1);
   cJSON_AddItemReferenceToObject(req, "input", input);
   cJSON_AddItemReferenceToObject(req, "tools", tools);

   return req;
}

cJSON *agent_build_request_anthropic(const agent_t *agent, cJSON *messages, cJSON *tools,
                                     const char *system_prompt, int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   int tok = (max_tokens > 0) ? max_tokens : 4096;
   cJSON_AddNumberToObject(req, "max_tokens", tok);

   if (system_prompt && system_prompt[0])
      cJSON_AddStringToObject(req, "system", system_prompt);

   cJSON_AddItemReferenceToObject(req, "messages", messages);
   cJSON_AddItemReferenceToObject(req, "tools", tools);

   if (temperature >= 0)
      cJSON_AddNumberToObject(req, "temperature", temperature);

   return req;
}

/* --- Response parsers --- */

void agent_parse_response_openai(cJSON *root, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Usage */
   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
      cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
      if (pt && cJSON_IsNumber(pt))
         out->prompt_tokens = pt->valueint;
      if (ct && cJSON_IsNumber(ct))
         out->completion_tokens = ct->valueint;
   }

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
      return;

   cJSON *choice = cJSON_GetArrayItem(choices, 0);
   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   cJSON *message = cJSON_GetObjectItem(choice, "message");
   if (!message)
      return;

   if (finish && cJSON_IsString(finish) && strcmp(finish->valuestring, "tool_calls") == 0)
   {
      out->is_tool_call = 1;
      out->assistant_message = cJSON_Duplicate(message, 1);

      cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
      if (tool_calls && cJSON_IsArray(tool_calls))
      {
         int n = cJSON_GetArraySize(tool_calls);
         if (n > AGENT_MAX_TOOL_CALLS)
            n = AGENT_MAX_TOOL_CALLS;
         for (int i = 0; i < n; i++)
         {
            cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
            cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
            cJSON *fn = cJSON_GetObjectItem(tc, "function");
            if (!fn)
               continue;
            cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
            cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");

            parsed_tool_call_t *call = &out->calls[out->call_count];
            if (tc_id && cJSON_IsString(tc_id))
               snprintf(call->id, sizeof(call->id), "%s", tc_id->valuestring);
            if (fn_name && cJSON_IsString(fn_name))
               snprintf(call->name, sizeof(call->name), "%s", fn_name->valuestring);
            if (fn_args && cJSON_IsString(fn_args))
               call->arguments = strdup(fn_args->valuestring);
            else
               call->arguments = strdup("{}");
            out->call_count++;
         }
      }
   }
   else
   {
      /* Text response */
      cJSON *content = cJSON_GetObjectItem(message, "content");
      if (content && cJSON_IsString(content) && content->valuestring[0])
         out->content = strdup(content->valuestring);

      /* Reasoning models (e.g. qwen3) may return empty content with
         reasoning_content holding the actual output. Fall back to it. */
      if (!out->content || !out->content[0])
      {
         cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
         if (reasoning && cJSON_IsString(reasoning) && reasoning->valuestring[0])
         {
            free(out->content);
            out->content = strdup(reasoning->valuestring);
         }
      }
   }
}

void agent_parse_response_responses(const char *body, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Find response.completed event in SSE stream */
   const char *completed = strstr(body, "event: response.completed\n");
   cJSON *resp = NULL;

   if (completed)
   {
      const char *data_line = strstr(completed, "data: ");
      if (data_line)
      {
         data_line += 6;
         cJSON *event = cJSON_Parse(data_line);
         if (event)
         {
            resp = cJSON_GetObjectItem(event, "response");
            if (resp)
               resp = cJSON_Duplicate(resp, 1);
            cJSON_Delete(event);
         }
      }
   }
   else
   {
      resp = cJSON_Parse(body);
   }

   if (!resp)
      return;

   /* Usage */
   cJSON *usage = cJSON_GetObjectItem(resp, "usage");
   if (usage)
   {
      cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
      cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
      if (it && cJSON_IsNumber(it))
         out->prompt_tokens = it->valueint;
      if (ot && cJSON_IsNumber(ot))
         out->completion_tokens = ot->valueint;
   }

   /* Check output array for tool calls vs text */
   cJSON *output = cJSON_GetObjectItem(resp, "output");
   if (output && cJSON_IsArray(output))
   {
      int n = cJSON_GetArraySize(output);
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_GetArrayItem(output, i);
         cJSON *type = cJSON_GetObjectItem(item, "type");
         if (!type || !cJSON_IsString(type))
            continue;

         if (strcmp(type->valuestring, "function_call") == 0)
         {
            out->is_tool_call = 1;
            if (out->call_count < AGENT_MAX_TOOL_CALLS)
            {
               parsed_tool_call_t *call = &out->calls[out->call_count];
               cJSON *cid = cJSON_GetObjectItem(item, "call_id");
               cJSON *nm = cJSON_GetObjectItem(item, "name");
               cJSON *args = cJSON_GetObjectItem(item, "arguments");
               if (cid && cJSON_IsString(cid))
                  snprintf(call->id, sizeof(call->id), "%s", cid->valuestring);
               if (nm && cJSON_IsString(nm))
                  snprintf(call->name, sizeof(call->name), "%s", nm->valuestring);
               if (args && cJSON_IsString(args))
                  call->arguments = strdup(args->valuestring);
               else
                  call->arguments = strdup("{}");
               out->call_count++;
            }
         }
         else if (strcmp(type->valuestring, "message") == 0)
         {
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (content && cJSON_IsArray(content))
            {
               int cn = cJSON_GetArraySize(content);
               for (int j = 0; j < cn; j++)
               {
                  cJSON *part = cJSON_GetArrayItem(content, j);
                  cJSON *pt = cJSON_GetObjectItem(part, "type");
                  if (pt && cJSON_IsString(pt) && strcmp(pt->valuestring, "output_text") == 0)
                  {
                     cJSON *text = cJSON_GetObjectItem(part, "text");
                     if (text && cJSON_IsString(text))
                        out->content = strdup(text->valuestring);
                  }
               }
            }
         }
      }
   }

   /* For Responses API, store the full output as assistant_message for multi-turn */
   if (out->is_tool_call)
      out->assistant_message = cJSON_Duplicate(output, 1);

   cJSON_Delete(resp);
}

void agent_parse_response_anthropic(cJSON *root, parsed_response_t *out)
{
   memset(out, 0, sizeof(*out));

   /* Usage */
   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *it = cJSON_GetObjectItem(usage, "input_tokens");
      cJSON *ot = cJSON_GetObjectItem(usage, "output_tokens");
      if (it && cJSON_IsNumber(it))
         out->prompt_tokens = it->valueint;
      if (ot && cJSON_IsNumber(ot))
         out->completion_tokens = ot->valueint;
   }

   /* Check stop_reason for tool_use */
   cJSON *stop = cJSON_GetObjectItem(root, "stop_reason");
   int has_tool_use = (stop && cJSON_IsString(stop) && strcmp(stop->valuestring, "tool_use") == 0);

   cJSON *content = cJSON_GetObjectItem(root, "content");
   if (!content || !cJSON_IsArray(content))
      return;

   /* Extract text and tool_use blocks from content array */
   int n = cJSON_GetArraySize(content);
   for (int i = 0; i < n; i++)
   {
      cJSON *block = cJSON_GetArrayItem(content, i);
      cJSON *type = cJSON_GetObjectItem(block, "type");
      if (!type || !cJSON_IsString(type))
         continue;

      if (strcmp(type->valuestring, "tool_use") == 0 && out->call_count < AGENT_MAX_TOOL_CALLS)
      {
         out->is_tool_call = 1;
         parsed_tool_call_t *call = &out->calls[out->call_count];
         cJSON *id = cJSON_GetObjectItem(block, "id");
         cJSON *nm = cJSON_GetObjectItem(block, "name");
         cJSON *input = cJSON_GetObjectItem(block, "input");

         if (id && cJSON_IsString(id))
            snprintf(call->id, sizeof(call->id), "%s", id->valuestring);
         if (nm && cJSON_IsString(nm))
            snprintf(call->name, sizeof(call->name), "%s", nm->valuestring);
         if (input)
         {
            char *s = cJSON_PrintUnformatted(input);
            call->arguments = s ? s : strdup("{}");
         }
         else
            call->arguments = strdup("{}");
         out->call_count++;
      }
      else if (strcmp(type->valuestring, "text") == 0 && !has_tool_use)
      {
         cJSON *text = cJSON_GetObjectItem(block, "text");
         if (text && cJSON_IsString(text))
            out->content = strdup(text->valuestring);
      }
   }

   /* Store the full content array as assistant_message for multi-turn */
   if (out->is_tool_call)
      out->assistant_message = cJSON_Duplicate(content, 1);
}

void agent_free_parsed_response(parsed_response_t *p)
{
   for (int i = 0; i < p->call_count; i++)
      free(p->calls[i].arguments);
   free(p->content);
   if (p->assistant_message)
      cJSON_Delete(p->assistant_message);
}

/* --- Message utilities --- */

/* Merge consecutive same-role messages in a cJSON messages array.
 * Returns the number of merges performed. Idempotent. */
int messages_compact_consecutive(cJSON *messages)
{
   if (!messages || !cJSON_IsArray(messages))
      return 0;

   int merged = 0;
   cJSON *cur = messages->child;

   while (cur && cur->next)
   {
      cJSON *next = cur->next;
      const char *cur_role = cJSON_GetStringValue(cJSON_GetObjectItem(cur, "role"));
      const char *next_role = cJSON_GetStringValue(cJSON_GetObjectItem(next, "role"));

      if (!cur_role || !next_role || strcmp(cur_role, next_role) != 0)
      {
         cur = next;
         continue;
      }

      /* Same role — merge next's content into cur */
      cJSON *cur_content = cJSON_GetObjectItem(cur, "content");
      cJSON *next_content = cJSON_GetObjectItem(next, "content");

      const char *cur_text = cJSON_GetStringValue(cur_content);
      const char *next_text = cJSON_GetStringValue(next_content);

      /* Only merge string content; skip messages with structured content (tool_calls, etc.) */
      if (!cur_text || !next_text)
      {
         cur = next;
         continue;
      }

      size_t new_len = strlen(cur_text) + 2 + strlen(next_text) + 1;
      char *merged_text = malloc(new_len);
      if (!merged_text)
      {
         cur = next;
         continue;
      }
      snprintf(merged_text, new_len, "%s\n\n%s", cur_text, next_text);

      cJSON_ReplaceItemInObject(cur, "content", cJSON_CreateString(merged_text));
      free(merged_text);

      /* Remove next from the array and free it */
      cJSON_DetachItemViaPointer(messages, next);
      cJSON_Delete(next);

      merged++;
      /* Don't advance cur — check if the next one also matches */
   }

   return merged;
}

/* --- Message history repair ---
 *
 * Scan message history for inconsistencies and repair them:
 * 1. Orphaned tool calls: assistant requested tool_use but no matching result exists.
 *    -> Insert synthetic cancellation result.
 * 2. Orphaned tool results: a tool result exists with no matching tool call.
 *    -> Remove the orphan.
 * 3. Trailing tool calls: conversation ends with unanswered tool calls.
 *    -> Fill with cancellation results.
 *
 * Handles OpenAI (tool_calls/tool_call_id), Anthropic (tool_use/tool_result),
 * and Responses API (function_call/function_call_output) message formats.
 *
 * Idempotent: running twice produces the same result.
 */

static const char *CANCEL_MSG = "[Tool call was cancelled or timed out]";

/* Collect all tool call IDs from a message array.
 * Returns a cJSON object used as a set: keys are IDs, values are true. */
static cJSON *collect_tool_call_ids(cJSON *messages)
{
   cJSON *ids = cJSON_CreateObject();
   cJSON *msg;

   cJSON_ArrayForEach(msg, messages)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));

      /* OpenAI: assistant message with tool_calls array */
      if (role && strcmp(role, "assistant") == 0)
      {
         cJSON *tcs = cJSON_GetObjectItem(msg, "tool_calls");
         if (tcs && cJSON_IsArray(tcs))
         {
            cJSON *tc;
            cJSON_ArrayForEach(tc, tcs)
            {
               const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
               if (id)
                  cJSON_AddBoolToObject(ids, id, 1);
            }
         }

         /* Anthropic: assistant message with content array containing tool_use blocks */
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (type && strcmp(type, "tool_use") == 0)
               {
                  const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
                  if (id)
                     cJSON_AddBoolToObject(ids, id, 1);
               }
            }
         }
      }

      /* Responses API: top-level function_call items */
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && strcmp(type, "function_call") == 0)
      {
         const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "call_id"));
         if (cid)
            cJSON_AddBoolToObject(ids, cid, 1);
      }
   }

   return ids;
}

/* Collect all tool result IDs from a message array. */
static cJSON *collect_tool_result_ids(cJSON *messages)
{
   cJSON *ids = cJSON_CreateObject();
   cJSON *msg;

   cJSON_ArrayForEach(msg, messages)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));

      /* OpenAI: role=tool with tool_call_id */
      if (role && strcmp(role, "tool") == 0)
      {
         const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "tool_call_id"));
         if (id)
            cJSON_AddBoolToObject(ids, id, 1);
      }

      /* Anthropic: user message with content array containing tool_result blocks */
      if (role && strcmp(role, "user") == 0)
      {
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (type && strcmp(type, "tool_result") == 0)
               {
                  const char *id =
                      cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_use_id"));
                  if (id)
                     cJSON_AddBoolToObject(ids, id, 1);
               }
            }
         }
      }

      /* Responses API: function_call_output with call_id */
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && strcmp(type, "function_call_output") == 0)
      {
         const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "call_id"));
         if (cid)
            cJSON_AddBoolToObject(ids, cid, 1);
      }
   }

   return ids;
}

/* Detect the message format: 0=OpenAI, 1=Anthropic, 2=Responses API */
static int detect_format(cJSON *messages)
{
   cJSON *msg;
   cJSON_ArrayForEach(msg, messages)
   {
      /* Responses API: top-level type=function_call or function_call_output */
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && (strcmp(type, "function_call") == 0 ||
                   strcmp(type, "function_call_output") == 0))
         return 2;

      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (!role)
         continue;

      /* Anthropic: assistant with content array containing tool_use blocks */
      if (strcmp(role, "assistant") == 0)
      {
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *btype = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (btype && strcmp(btype, "tool_use") == 0)
                  return 1;
            }
         }
      }

      /* OpenAI: role=tool messages */
      if (strcmp(role, "tool") == 0)
         return 0;
   }

   return 0; /* default to OpenAI */
}

/* Insert synthetic cancellation results for orphaned tool calls (OpenAI format) */
static int repair_orphans_openai(cJSON *messages, cJSON *call_ids, cJSON *result_ids)
{
   int repairs = 0;

   /* Find orphaned calls (call exists but no result) and insert cancellation */
   cJSON *id_item;
   cJSON_ArrayForEach(id_item, call_ids)
   {
      if (cJSON_GetObjectItem(result_ids, id_item->string))
         continue; /* has a matching result */

      /* Insert a synthetic tool result after the assistant message that made the call */
      /* Find the assistant message containing this call */
      cJSON *msg;
      cJSON *insert_after = NULL;
      cJSON_ArrayForEach(msg, messages)
      {
         const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
         if (!role || strcmp(role, "assistant") != 0)
            continue;
         cJSON *tcs = cJSON_GetObjectItem(msg, "tool_calls");
         if (!tcs || !cJSON_IsArray(tcs))
            continue;
         cJSON *tc;
         cJSON_ArrayForEach(tc, tcs)
         {
            const char *tc_id = cJSON_GetStringValue(cJSON_GetObjectItem(tc, "id"));
            if (tc_id && strcmp(tc_id, id_item->string) == 0)
            {
               insert_after = msg;
               break;
            }
         }
         if (insert_after)
            break;
      }

      /* Create synthetic result */
      cJSON *tool_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(tool_msg, "role", "tool");
      cJSON_AddStringToObject(tool_msg, "tool_call_id", id_item->string);
      cJSON_AddStringToObject(tool_msg, "content", CANCEL_MSG);

      if (insert_after)
      {
         /* Insert right after the assistant message (or after existing tool results) */
         cJSON *pos = insert_after->next;
         while (pos)
         {
            const char *r = cJSON_GetStringValue(cJSON_GetObjectItem(pos, "role"));
            if (!r || strcmp(r, "tool") != 0)
               break;
            pos = pos->next;
         }
         if (pos)
         {
            /* Insert before pos */
            tool_msg->next = pos;
            tool_msg->prev = pos->prev;
            if (pos->prev)
               pos->prev->next = tool_msg;
            pos->prev = tool_msg;
         }
         else
         {
            cJSON_AddItemToArray(messages, tool_msg);
         }
      }
      else
      {
         cJSON_AddItemToArray(messages, tool_msg);
      }
      repairs++;
   }

   /* Remove orphaned results (result exists but no matching call) */
   cJSON *msg = messages->child;
   while (msg)
   {
      cJSON *next = msg->next;
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (role && strcmp(role, "tool") == 0)
      {
         const char *tcid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "tool_call_id"));
         if (tcid && !cJSON_GetObjectItem(call_ids, tcid))
         {
            cJSON_DetachItemViaPointer(messages, msg);
            cJSON_Delete(msg);
            repairs++;
         }
      }
      msg = next;
   }

   return repairs;
}

/* Insert synthetic cancellation results for orphaned tool calls (Anthropic format) */
static int repair_orphans_anthropic(cJSON *messages, cJSON *call_ids, cJSON *result_ids)
{
   int repairs = 0;

   /* For each orphaned call, find the user message that should contain its result
    * and add a tool_result block, or create a new user message */
   cJSON *id_item;
   cJSON_ArrayForEach(id_item, call_ids)
   {
      if (cJSON_GetObjectItem(result_ids, id_item->string))
         continue;

      /* Find the assistant message with this tool_use, then look for the next user msg */
      int found_call = 0;
      cJSON *target_user = NULL;
      cJSON *msg;
      cJSON_ArrayForEach(msg, messages)
      {
         if (!found_call)
         {
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
            if (!role || strcmp(role, "assistant") != 0)
               continue;
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (!content || !cJSON_IsArray(content))
               continue;
            cJSON *block;
            cJSON_ArrayForEach(block, content)
            {
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               const char *bid = cJSON_GetStringValue(cJSON_GetObjectItem(block, "id"));
               if (type && strcmp(type, "tool_use") == 0 && bid &&
                   strcmp(bid, id_item->string) == 0)
               {
                  found_call = 1;
                  break;
               }
            }
         }
         else
         {
            /* Look for the next user message */
            const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
            if (role && strcmp(role, "user") == 0)
            {
               cJSON *content = cJSON_GetObjectItem(msg, "content");
               if (content && cJSON_IsArray(content))
               {
                  target_user = msg;
                  break;
               }
            }
         }
      }

      /* Create the tool_result block */
      cJSON *tr = cJSON_CreateObject();
      cJSON_AddStringToObject(tr, "type", "tool_result");
      cJSON_AddStringToObject(tr, "tool_use_id", id_item->string);
      cJSON_AddStringToObject(tr, "content", CANCEL_MSG);

      if (target_user)
      {
         /* Append to existing user message's content array */
         cJSON *content = cJSON_GetObjectItem(target_user, "content");
         cJSON_AddItemToArray(content, tr);
      }
      else
      {
         /* Create a new user message */
         cJSON *user_msg = cJSON_CreateObject();
         cJSON_AddStringToObject(user_msg, "role", "user");
         cJSON *content = cJSON_AddArrayToObject(user_msg, "content");
         cJSON_AddItemToArray(content, tr);
         cJSON_AddItemToArray(messages, user_msg);
      }
      repairs++;
   }

   /* Remove orphaned tool_result blocks */
   cJSON *msg = messages->child;
   while (msg)
   {
      cJSON *next_msg = msg->next;
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (role && strcmp(role, "user") == 0)
      {
         cJSON *content = cJSON_GetObjectItem(msg, "content");
         if (content && cJSON_IsArray(content))
         {
            cJSON *block = content->child;
            while (block)
            {
               cJSON *next_block = block->next;
               const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
               if (type && strcmp(type, "tool_result") == 0)
               {
                  const char *tuid =
                      cJSON_GetStringValue(cJSON_GetObjectItem(block, "tool_use_id"));
                  if (tuid && !cJSON_GetObjectItem(call_ids, tuid))
                  {
                     cJSON_DetachItemViaPointer(content, block);
                     cJSON_Delete(block);
                     repairs++;
                  }
               }
               block = next_block;
            }
            /* If the user message is now empty, remove it */
            if (cJSON_GetArraySize(content) == 0)
            {
               cJSON_DetachItemViaPointer(messages, msg);
               cJSON_Delete(msg);
            }
         }
      }
      msg = next_msg;
   }

   return repairs;
}

/* Insert synthetic cancellation results for orphaned calls (Responses API format) */
static int repair_orphans_responses(cJSON *messages, cJSON *call_ids, cJSON *result_ids)
{
   int repairs = 0;

   /* Add function_call_output for orphaned function_calls */
   cJSON *id_item;
   cJSON_ArrayForEach(id_item, call_ids)
   {
      if (cJSON_GetObjectItem(result_ids, id_item->string))
         continue;

      cJSON *out_item = cJSON_CreateObject();
      cJSON_AddStringToObject(out_item, "type", "function_call_output");
      cJSON_AddStringToObject(out_item, "call_id", id_item->string);
      cJSON_AddStringToObject(out_item, "output", CANCEL_MSG);
      cJSON_AddItemToArray(messages, out_item);
      repairs++;
   }

   /* Remove orphaned function_call_output items */
   cJSON *msg = messages->child;
   while (msg)
   {
      cJSON *next = msg->next;
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
      if (type && strcmp(type, "function_call_output") == 0)
      {
         const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "call_id"));
         if (cid && !cJSON_GetObjectItem(call_ids, cid))
         {
            cJSON_DetachItemViaPointer(messages, msg);
            cJSON_Delete(msg);
            repairs++;
         }
      }
      msg = next;
   }

   return repairs;
}

int message_history_repair(cJSON *messages)
{
   if (!messages || !cJSON_IsArray(messages))
      return 0;

   if (cJSON_GetArraySize(messages) == 0)
      return 0;

   cJSON *call_ids = collect_tool_call_ids(messages);
   cJSON *result_ids = collect_tool_result_ids(messages);

   /* If there are no tool calls at all, nothing to repair */
   if (!call_ids->child && !result_ids->child)
   {
      cJSON_Delete(call_ids);
      cJSON_Delete(result_ids);
      return 0;
   }

   int fmt = detect_format(messages);
   int repairs;

   switch (fmt)
   {
   case 1:
      repairs = repair_orphans_anthropic(messages, call_ids, result_ids);
      break;
   case 2:
      repairs = repair_orphans_responses(messages, call_ids, result_ids);
      break;
   default:
      repairs = repair_orphans_openai(messages, call_ids, result_ids);
      break;
   }

   cJSON_Delete(call_ids);
   cJSON_Delete(result_ids);
   return repairs;
}
