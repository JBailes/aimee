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
