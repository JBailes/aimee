/* agent_context.c: context assembly, SSH, request/response, logging, stats */
#include "aimee.h"
#include "agent.h"
#include "agent_tunnel.h"
#include "http_retry.h"
#include "workspace.h"
#include "working_memory.h"
#include "tasks.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* --- Request/response (simple, non-tool path) --- */

static int is_chatgpt_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "chatgpt") == 0;
}

static int is_anthropic_provider(const agent_t *agent)
{
   return strcmp(agent->provider, "anthropic") == 0;
}

static cJSON *build_request_openai(const agent_t *agent, const char *system_prompt,
                                   const char *user_prompt, int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   cJSON *messages = cJSON_CreateArray();
   if (system_prompt && system_prompt[0])
   {
      cJSON *sys = cJSON_CreateObject();
      cJSON_AddStringToObject(sys, "role", "system");
      cJSON_AddStringToObject(sys, "content", system_prompt);
      cJSON_AddItemToArray(messages, sys);
   }
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", user_prompt);
   cJSON_AddItemToArray(messages, user);

   cJSON_AddItemToObject(req, "messages", messages);

   if (max_tokens > 0)
      cJSON_AddNumberToObject(req, "max_tokens", max_tokens);
   if (temperature >= 0)
      cJSON_AddNumberToObject(req, "temperature", temperature);

   return req;
}

static cJSON *build_request_responses(const agent_t *agent, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature)
{
   (void)max_tokens;
   (void)temperature;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   /* ChatGPT Codex backend requires instructions, store=false, stream=true */
   if (system_prompt && system_prompt[0])
      cJSON_AddStringToObject(req, "instructions", system_prompt);
   else
      cJSON_AddStringToObject(req, "instructions", "You are a helpful assistant.");

   cJSON_AddBoolToObject(req, "store", 0);
   cJSON_AddBoolToObject(req, "stream", 1);

   /* Responses API uses "input" as an array of messages */
   cJSON *input = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", user_prompt);
   cJSON_AddItemToArray(input, user);

   cJSON_AddItemToObject(req, "input", input);

   return req;
}

static cJSON *build_request_anthropic(const agent_t *agent, const char *system_prompt,
                                      const char *user_prompt, int max_tokens, double temperature)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", agent->model);

   int tok = (max_tokens > 0) ? max_tokens : 4096;
   cJSON_AddNumberToObject(req, "max_tokens", tok);

   if (system_prompt && system_prompt[0])
   {
      /* Use content block array with cache_control for prompt caching.
       * This tells the API to cache the system prompt prefix across calls
       * with the same content, reducing cost for repeated delegate calls. */
      cJSON *sys_arr = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      cJSON_AddStringToObject(block, "text", system_prompt);
      cJSON *cc = cJSON_CreateObject();
      cJSON_AddStringToObject(cc, "type", "ephemeral");
      cJSON_AddItemToObject(block, "cache_control", cc);
      cJSON_AddItemToArray(sys_arr, block);
      cJSON_AddItemToObject(req, "system", sys_arr);
   }

   cJSON *messages = cJSON_CreateArray();
   cJSON *user = cJSON_CreateObject();
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", user_prompt);
   cJSON_AddItemToArray(messages, user);
   cJSON_AddItemToObject(req, "messages", messages);

   if (temperature >= 0)
      cJSON_AddNumberToObject(req, "temperature", temperature);

   return req;
}

static cJSON *build_request(const agent_t *agent, const char *system_prompt,
                            const char *user_prompt, int max_tokens, double temperature)
{
   if (is_chatgpt_provider(agent))
      return build_request_responses(agent, system_prompt, user_prompt, max_tokens, temperature);
   if (is_anthropic_provider(agent))
      return build_request_anthropic(agent, system_prompt, user_prompt, max_tokens, temperature);
   return build_request_openai(agent, system_prompt, user_prompt, max_tokens, temperature);
}

static int parse_error(cJSON *root, agent_result_t *out)
{
   cJSON *err = cJSON_GetObjectItem(root, "error");
   if (!err || cJSON_IsNull(err))
      return 0;

   if (cJSON_IsObject(err))
   {
      cJSON *msg = cJSON_GetObjectItem(err, "message");
      if (msg && cJSON_IsString(msg))
         snprintf(out->error, sizeof(out->error), "%s", msg->valuestring);
      else
         snprintf(out->error, sizeof(out->error), "API error");
   }
   else if (cJSON_IsString(err))
   {
      snprintf(out->error, sizeof(out->error), "%s", err->valuestring);
   }
   else
   {
      snprintf(out->error, sizeof(out->error), "API error");
   }
   return 1;
}

static void parse_response_openai(cJSON *root, agent_result_t *out)
{
   /* Extract content from choices[0].message.content */
   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (choices && cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0)
   {
      cJSON *choice = cJSON_GetArrayItem(choices, 0);
      cJSON *message = cJSON_GetObjectItem(choice, "message");
      if (message)
      {
         cJSON *content = cJSON_GetObjectItem(message, "content");
         if (content && cJSON_IsString(content))
         {
            out->response = strdup(content->valuestring);
            out->success = 1;
         }
      }
   }

   /* Extract usage */
   cJSON *usage = cJSON_GetObjectItem(root, "usage");
   if (usage)
   {
      cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
      cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
      if (pt && cJSON_IsNumber(pt))
         out->prompt_tokens = pt->valueint;
      if (ct && cJSON_IsNumber(ct))
         out->completion_tokens = ct->valueint;

      /* Anthropic prompt caching usage fields */
      cJSON *cache_create = cJSON_GetObjectItem(usage, "cache_creation_input_tokens");
      cJSON *cache_read = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
      if (cache_read && cJSON_IsNumber(cache_read) && cache_read->valueint > 0)
         fprintf(stderr, "aimee: prompt cache hit: %d tokens read from cache\n",
                 cache_read->valueint);
      else if (cache_create && cJSON_IsNumber(cache_create) && cache_create->valueint > 0)
         fprintf(stderr, "aimee: prompt cache miss: %d tokens written to cache\n",
                 cache_create->valueint);
   }
}

static void parse_response_object(cJSON *root, agent_result_t *out)
{
   /* Responses API: output[].content[].text where type == "output_text" */
   cJSON *output = cJSON_GetObjectItem(root, "output");
   if (output && cJSON_IsArray(output))
   {
      int n = cJSON_GetArraySize(output);
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_GetArrayItem(output, i);
         cJSON *type = cJSON_GetObjectItem(item, "type");
         if (!type || !cJSON_IsString(type) || strcmp(type->valuestring, "message") != 0)
            continue;

         cJSON *content = cJSON_GetObjectItem(item, "content");
         if (!content || !cJSON_IsArray(content))
            continue;

         int cn = cJSON_GetArraySize(content);
         for (int j = 0; j < cn; j++)
         {
            cJSON *part = cJSON_GetArrayItem(content, j);
            cJSON *pt = cJSON_GetObjectItem(part, "type");
            if (!pt || !cJSON_IsString(pt) || strcmp(pt->valuestring, "output_text") != 0)
               continue;

            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (text && cJSON_IsString(text))
            {
               out->response = strdup(text->valuestring);
               out->success = 1;
               break;
            }
         }
         if (out->success)
            break;
      }
   }

   /* Responses API usage: input_tokens, output_tokens */
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
}

/* Parse SSE stream body, extract the response.completed event's response object */
static void parse_response_responses(const char *body, agent_result_t *out)
{
   /* Find "event: response.completed" then extract the data line's "response" object */
   const char *completed = strstr(body, "event: response.completed\n");
   if (!completed)
   {
      /* Not SSE, try parsing as plain JSON */
      cJSON *root = cJSON_Parse(body);
      if (root)
      {
         if (parse_error(root, out))
         {
            cJSON_Delete(root);
            return;
         }
         parse_response_object(root, out);
         cJSON_Delete(root);
      }
      else
      {
         snprintf(out->error, sizeof(out->error), "no response.completed event in stream");
      }
      return;
   }

   /* Skip to the "data: " line after the event line */
   const char *data_line = strstr(completed, "data: ");
   if (!data_line)
   {
      snprintf(out->error, sizeof(out->error), "no data line after response.completed");
      return;
   }
   data_line += 6; /* skip "data: " */

   cJSON *event = cJSON_Parse(data_line);
   if (!event)
   {
      snprintf(out->error, sizeof(out->error), "invalid JSON in response.completed event");
      return;
   }

   /* The response object is nested under "response" in the event */
   cJSON *resp = cJSON_GetObjectItem(event, "response");
   if (resp)
   {
      if (parse_error(resp, out))
      {
         cJSON_Delete(event);
         return;
      }
      parse_response_object(resp, out);
   }
   else
   {
      snprintf(out->error, sizeof(out->error), "no response object in completed event");
   }

   cJSON_Delete(event);
}

static void parse_response_anthropic(cJSON *root, agent_result_t *out)
{
   /* Anthropic: {"content":[{"type":"text","text":"..."}],"usage":{...}} */
   cJSON *content = cJSON_GetObjectItem(root, "content");
   if (content && cJSON_IsArray(content))
   {
      int n = cJSON_GetArraySize(content);
      for (int i = 0; i < n; i++)
      {
         cJSON *block = cJSON_GetArrayItem(content, i);
         cJSON *type = cJSON_GetObjectItem(block, "type");
         if (type && cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0)
         {
            cJSON *text = cJSON_GetObjectItem(block, "text");
            if (text && cJSON_IsString(text))
            {
               out->response = strdup(text->valuestring);
               out->success = 1;
               break;
            }
         }
      }
   }

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
}

static void parse_response(const char *body, const agent_t *agent, agent_result_t *out)
{
   if (is_chatgpt_provider(agent))
   {
      /* SSE stream or plain JSON, handled internally */
      parse_response_responses(body, out);
      return;
   }

   cJSON *root = cJSON_Parse(body);
   if (!root)
   {
      snprintf(out->error, sizeof(out->error), "invalid JSON response");
      return;
   }

   if (parse_error(root, out))
   {
      cJSON_Delete(root);
      return;
   }

   if (is_anthropic_provider(agent))
      parse_response_anthropic(root, out);
   else
      parse_response_openai(root, out);
   cJSON_Delete(root);
}

/* --- Simple (non-tool) execution --- */

int agent_execute(sqlite3 *db, const agent_t *agent, const char *system_prompt,
                  const char *user_prompt, int max_tokens, double temperature, agent_result_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);

   if (!user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }

   /* Build URL */
   char url[MAX_ENDPOINT_LEN + 64];
   if (is_chatgpt_provider(agent))
      snprintf(url, sizeof(url), "%s/responses", agent->endpoint);
   else if (is_anthropic_provider(agent))
      snprintf(url, sizeof(url), "%s/messages", agent->endpoint);
   else
      snprintf(url, sizeof(url), "%s/chat/completions", agent->endpoint);

   /* Resolve auth */
   char auth_header[MAX_API_KEY_LEN + 32];
   if (agent_resolve_auth(agent, auth_header, sizeof(auth_header)) != 0)
   {
      snprintf(out->error, sizeof(out->error), "auth resolution failed");
      return -1;
   }

   /* Build request body */
   int tok = (max_tokens > 0) ? max_tokens : agent->max_tokens;
   cJSON *req = build_request(agent, system_prompt, user_prompt, tok, temperature);
   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
   {
      snprintf(out->error, sizeof(out->error), "failed to build request");
      return -1;
   }

   /* Measure latency */
   struct timespec start, end;
   clock_gettime(CLOCK_MONOTONIC, &start);

   /* HTTP POST with retry */
   char *response_body = NULL;
   config_t retry_cfg;
   config_load(&retry_cfg);
   int ra =
       retry_cfg.retry_max_attempts > 0 ? retry_cfg.retry_max_attempts : HTTP_RETRY_MAX_ATTEMPTS;
   int rb = retry_cfg.retry_base_ms > 0 ? retry_cfg.retry_base_ms : HTTP_RETRY_BASE_MS;
   int rm = retry_cfg.retry_max_ms > 0 ? retry_cfg.retry_max_ms : HTTP_RETRY_MAX_MS;

   int http_status = http_retry_post(url, auth_header, body, &response_body, agent->timeout_ms,
                                     agent->extra_headers, ra, rb, rm);
   free(body);

   /* Model fallback: if primary model fails with 400, retry with fallback_model */
   if (http_status == 400 && agent->fallback_model[0])
   {
      free(response_body);
      response_body = NULL;

      /* Rebuild request with fallback model */
      agent_t fb_agent;
      memcpy(&fb_agent, agent, sizeof(fb_agent));
      snprintf(fb_agent.model, MAX_MODEL_LEN, "%s", agent->fallback_model);
      fb_agent.fallback_model[0] = '\0';

      cJSON *fb_req = build_request(&fb_agent, system_prompt, user_prompt, tok, temperature);
      char *fb_body = cJSON_PrintUnformatted(fb_req);
      cJSON_Delete(fb_req);
      if (fb_body)
      {
         http_status = http_retry_post(url, auth_header, fb_body, &response_body, agent->timeout_ms,
                                       agent->extra_headers, ra, rb, rm);
         free(fb_body);
      }
   }

   clock_gettime(CLOCK_MONOTONIC, &end);
   out->latency_ms =
       (int)((end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000);

   if (http_status < 0 || !response_body)
   {
      snprintf(out->error, sizeof(out->error), "HTTP request failed (status %d)", http_status);
      free(response_body);
      return -1;
   }

   if (http_status != 200)
   {
      char snippet[256] = {0};
      if (response_body)
         snprintf(snippet, sizeof(snippet), "%.200s", response_body);
      snprintf(out->error, sizeof(out->error), "HTTP %d: %s", http_status, snippet);
      /* Try to parse error body */
      parse_response(response_body, agent, out);
      free(response_body);
      return -1;
   }

   parse_response(response_body, agent, out);
   free(response_body);

   if (!out->success && !out->error[0])
      snprintf(out->error, sizeof(out->error), "no content in response");

   /* Note: callers (agent_run, agent_run_with_tools) handle logging
    * with the correct role. Do not log here to avoid double-logging. */

   return out->success ? 0 : -1;
}

/* --- Task type classification --- */

typedef struct
{
   const char *word;
   task_type_t type;
} task_keyword_t;

static const task_keyword_t task_keywords[] = {
    {"fix", TASK_TYPE_BUG_FIX},         {"bug", TASK_TYPE_BUG_FIX},
    {"error", TASK_TYPE_BUG_FIX},       {"crash", TASK_TYPE_BUG_FIX},
    {"fail", TASK_TYPE_BUG_FIX},        {"broken", TASK_TYPE_BUG_FIX},
    {"regression", TASK_TYPE_BUG_FIX},  {"refactor", TASK_TYPE_REFACTOR},
    {"rename", TASK_TYPE_REFACTOR},     {"extract", TASK_TYPE_REFACTOR},
    {"reorganize", TASK_TYPE_REFACTOR}, {"clean", TASK_TYPE_REFACTOR},
    {"add", TASK_TYPE_FEATURE},         {"implement", TASK_TYPE_FEATURE},
    {"create", TASK_TYPE_FEATURE},      {"new", TASK_TYPE_FEATURE},
    {"build", TASK_TYPE_FEATURE},       {"support", TASK_TYPE_FEATURE},
    {"review", TASK_TYPE_REVIEW},       {"check", TASK_TYPE_REVIEW},
    {"audit", TASK_TYPE_REVIEW},        {"verify", TASK_TYPE_REVIEW},
    {"validate", TASK_TYPE_REVIEW},     {"test", TASK_TYPE_TEST},
    {"coverage", TASK_TYPE_TEST},       {"assert", TASK_TYPE_TEST},
    {"spec", TASK_TYPE_TEST},           {NULL, TASK_TYPE_GENERAL}};

task_type_t task_type_classify(const char *prompt)
{
   if (!prompt || !prompt[0])
      return TASK_TYPE_GENERAL;

   /* Lowercase copy for matching */
   char lower[512];
   size_t len = strlen(prompt);
   if (len >= sizeof(lower))
      len = sizeof(lower) - 1;
   for (size_t i = 0; i < len; i++)
      lower[i] = (char)((prompt[i] >= 'A' && prompt[i] <= 'Z') ? prompt[i] + 32 : prompt[i]);
   lower[len] = '\0';

   /* Scan for keyword matches (first match wins) */
   for (int i = 0; task_keywords[i].word; i++)
   {
      const char *kw = task_keywords[i].word;
      size_t kwlen = strlen(kw);
      const char *p = lower;
      while ((p = strstr(p, kw)) != NULL)
      {
         /* Check word boundary: must be at start or after non-alpha */
         int at_start = (p == lower) || (*(p - 1) == ' ' || *(p - 1) == '\t' || *(p - 1) == '-' ||
                                         *(p - 1) == '_' || *(p - 1) == '/');
         /* Check end boundary: must be at end or before non-alpha */
         char after = p[kwlen];
         int at_end = (after == '\0' || after == ' ' || after == '\t' || after == '-' ||
                       after == '_' || after == '/' || after == '.' || after == ',' ||
                       after == 'e' || after == 'i' || after == 's');
         if (at_start && at_end)
            return task_keywords[i].type;
         p += kwlen;
      }
   }

   return TASK_TYPE_GENERAL;
}

const char *task_type_name(task_type_t type)
{
   switch (type)
   {
   case TASK_TYPE_BUG_FIX:
      return "bug_fix";
   case TASK_TYPE_REFACTOR:
      return "refactor";
   case TASK_TYPE_FEATURE:
      return "feature";
   case TASK_TYPE_REVIEW:
      return "review";
   case TASK_TYPE_TEST:
      return "test";
   default:
      return "general";
   }
}

/* Context category indices */
enum
{
   CTX_CAT_ERRORS = 0, /* Recent errors/episodes */
   CTX_CAT_ARCH,       /* Architecture/structure (memory context) */
   CTX_CAT_CODE,       /* Related code symbols */
   CTX_CAT_PROCEDURES, /* Procedures/how-to (rules) */
   CTX_CAT_RECENT,     /* Recent changes/failures */
   CTX_CAT_COUNT
};

/* Weight table: [task_type][category] */
static const int ctx_weights[TASK_TYPE_COUNT][CTX_CAT_COUNT] = {
    /* GENERAL */ {CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_MED},
    /* BUG_FIX */
    {CTX_WEIGHT_HIGH, CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_MED, CTX_WEIGHT_HIGH},
    /* REFACTOR */
    {CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_HIGH, CTX_WEIGHT_LOW, CTX_WEIGHT_MED},
    /* FEATURE */ {CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_MED, CTX_WEIGHT_MED, CTX_WEIGHT_LOW},
    /* REVIEW */
    {CTX_WEIGHT_MED, CTX_WEIGHT_HIGH, CTX_WEIGHT_HIGH, CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH},
    /* TEST */ {CTX_WEIGHT_MED, CTX_WEIGHT_LOW, CTX_WEIGHT_HIGH, CTX_WEIGHT_HIGH, CTX_WEIGHT_MED},
};

/* Compute budget allocation for a category given the task type.
 * Returns max chars for that category from the total content budget. */
static size_t ctx_category_budget(task_type_t type, int category, size_t total_budget)
{
   int weight = ctx_weights[type][category];
   int total_weight = 0;
   for (int i = 0; i < CTX_CAT_COUNT; i++)
      total_weight += ctx_weights[type][i];
   if (total_weight == 0)
      return total_budget / CTX_CAT_COUNT;
   return (total_budget * (size_t)weight) / (size_t)total_weight;
}

/* --- Relevance-scored context (#3) --- */

char *agent_build_exec_context(sqlite3 *db, const agent_t *agent, const agent_network_t *network,
                               const char *custom_prompt)
{
   /* Classify the task type from the prompt */
   task_type_t task_type = task_type_classify(custom_prompt);
   if (task_type != TASK_TYPE_GENERAL)
      fprintf(stderr, "agent: context assembly: task_type=%s\n", task_type_name(task_type));

   /* Default execution instructions */
   static const char *default_exec_instructions =
       "# Instructions\n"
       "- Use the bash tool to run commands, including SSH to remote hosts.\n"
       "- Use read_file and write_file for file operations.\n"
       "- Use list_files to explore directories.\n"
       "- When you have completed the task, respond with a final summary.\n"
       "- If you encounter an error, try to diagnose and fix it.\n"
       "- Do not ask for confirmation. Execute the task directly.\n";

   /* Budget: AGENT_CONTEXT_BUDGET chars total */
   size_t cap = AGENT_CONTEXT_BUDGET + 4096; /* extra room for headers */
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   size_t pos = 0;

   /* Compute per-category budgets from the content budget (~12K of the 16K+4K total).
    * Fixed overhead (instructions, network, guardrails, etc.) sits outside these limits. */
   size_t content_budget = AGENT_CONTEXT_BUDGET;
   size_t budget_errors = ctx_category_budget(task_type, CTX_CAT_ERRORS, content_budget);
   size_t budget_arch = ctx_category_budget(task_type, CTX_CAT_ARCH, content_budget);
   size_t budget_code = ctx_category_budget(task_type, CTX_CAT_CODE, content_budget);
   size_t budget_procedures = ctx_category_budget(task_type, CTX_CAT_PROCEDURES, content_budget);
   size_t budget_recent = ctx_category_budget(task_type, CTX_CAT_RECENT, content_budget);

   pos += (size_t)snprintf(buf + pos, cap - pos,
                           "You are an execution agent. Complete the task using the provided "
                           "tools.\n\n");

   /* Custom prompt override */
   if (custom_prompt && custom_prompt[0])
   {
      pos += (size_t)snprintf(buf + pos, cap - pos, "%s\n\n", custom_prompt);
   }

   /* Rules (budget: procedures) */
   char *rules = rules_generate(db);
   if (rules && rules[0] && strcmp(rules, "No rules configured.") != 0)
   {
      size_t rules_start = pos;
      pos += (size_t)snprintf(buf + pos, cap - pos, "# Rules\n");
      /* Truncate rules content to budget */
      size_t rules_len = strlen(rules);
      size_t rules_avail =
          budget_procedures > (pos - rules_start) ? budget_procedures - (pos - rules_start) : 0;
      if (rules_len > rules_avail)
         rules_len = rules_avail;
      if (rules_len > 0)
      {
         size_t to_write = rules_len < cap - pos ? rules_len : cap - pos;
         memcpy(buf + pos, rules, to_write);
         pos += to_write;
      }
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n\n");
   }
   free(rules);

   /* Relevance-scored memory context (#3):
    * If we have a task prompt, search for relevant memories only.
    * Otherwise fall back to the generic context assembly. */
   if (custom_prompt && custom_prompt[0])
   {
      /* Extract keywords from the prompt for targeted search */
      memory_t mems[8];
      int mcount = 0;

      /* Search L2 facts matching keywords from the prompt */
      static const char *mem_sql = "SELECT key, content FROM memories"
                                   " WHERE tier = 'L2' AND kind = 'fact'"
                                   " AND (LOWER(content) LIKE '%' || LOWER(?) || '%'"
                                   " OR LOWER(key) LIKE '%' || LOWER(?) || '%')"
                                   " ORDER BY confidence DESC, use_count DESC LIMIT 5";

      sqlite3_stmt *stmt = db_prepare(db, mem_sql);
      if (stmt)
      {
         /* Try to extract a meaningful keyword from the prompt (first noun-ish word) */
         char keyword[64] = {0};
         const char *p = custom_prompt;
         /* Skip common prefixes */
         while (*p && (*p == ' ' || !strncmp(p, "Check ", 6) || !strncmp(p, "Deploy ", 7) ||
                       !strncmp(p, "Verify ", 7) || !strncmp(p, "List ", 5)))
         {
            while (*p && *p != ' ')
               p++;
            while (*p == ' ')
               p++;
         }
         /* Take up to 60 chars of the remaining prompt as search term */
         snprintf(keyword, sizeof(keyword), "%.*s", 60, p);
         /* Also try the first significant word */
         char first_word[32] = {0};
         sscanf(custom_prompt, "%*s %31s", first_word);

         sqlite3_reset(stmt);
         const char *search = keyword[0] ? keyword : custom_prompt;
         sqlite3_bind_text(stmt, 1, search, -1, SQLITE_TRANSIENT);
         sqlite3_bind_text(stmt, 2, search, -1, SQLITE_TRANSIENT);

         while (sqlite3_step(stmt) == SQLITE_ROW && mcount < 8)
         {
            snprintf(mems[mcount].key, sizeof(mems[mcount].key), "%s",
                     (const char *)sqlite3_column_text(stmt, 0));
            snprintf(mems[mcount].content, sizeof(mems[mcount].content), "%s",
                     (const char *)sqlite3_column_text(stmt, 1));
            mcount++;
         }
      }

      if (mcount > 0)
      {
         size_t arch_start = pos;
         pos += (size_t)snprintf(buf + pos, cap - pos, "# Relevant Context\n");
         for (int i = 0; i < mcount && pos < cap - 256 && (pos - arch_start) < budget_arch; i++)
         {
            pos +=
                (size_t)snprintf(buf + pos, cap - pos, "- %s: %s\n", mems[i].key, mems[i].content);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }
   else
   {
      /* Fallback: full context assembly (budget: architecture) */
      char *ctx = memory_assemble_context(db, NULL);
      if (ctx && ctx[0])
      {
         size_t ctx_len = strlen(ctx);
         if (ctx_len > budget_arch)
            ctx_len = budget_arch;
         size_t to_write = ctx_len < cap - pos ? ctx_len : cap - pos;
         memcpy(buf + pos, ctx, to_write);
         pos += to_write;
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n\n");
      }
      free(ctx);
   }

   /* Repo catalog (compact: only list names, not full paths) */
   project_info_t projects[32];
   int pcount = index_list_projects(db, projects, 32);
   if (pcount > 0)
   {
      pos += (size_t)snprintf(buf + pos, cap - pos, "# Repos: ");
      for (int i = 0; i < pcount && pos < cap - 64; i++)
      {
         pos += (size_t)snprintf(buf + pos, cap - pos, "%s%s", i > 0 ? ", " : "", projects[i].name);
      }
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n\n");
   }

   /* Relevant code symbols: query the index for terms matching the task prompt */
   if (custom_prompt && custom_prompt[0])
   {
      /* Extract words >3 chars from the prompt as search terms */
      char prompt_copy[512];
      snprintf(prompt_copy, sizeof(prompt_copy), "%s", custom_prompt);

      term_hit_t hits[16];
      int total_hits = 0;
      char *saveptr = NULL;
      char *word = strtok_r(prompt_copy, " \t\n,.;:!?()[]{}\"'", &saveptr);

      while (word && total_hits < 16)
      {
         if (strlen(word) > 3)
         {
            int found = index_find(db, word, hits + total_hits, 16 - total_hits);
            total_hits += found;
         }
         word = strtok_r(NULL, " \t\n,.;:!?()[]{}\"'", &saveptr);
      }

      if (total_hits > 0)
      {
         size_t code_start = pos;
         pos += (size_t)snprintf(buf + pos, cap - pos, "# Relevant Code\n");
         for (int i = 0; i < total_hits && pos < cap - 256 && (pos - code_start) < budget_code; i++)
         {
            pos += (size_t)snprintf(buf + pos, cap - pos, "- %s:%d (%s) [%s]\n", hits[i].file_path,
                                    hits[i].line, hits[i].kind, hits[i].project);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Project style guide: inject for current project if available */
   if (pcount > 0)
   {
      /* Determine current project from CWD */
      char cwd[MAX_PATH_LEN];
      if (getcwd(cwd, sizeof(cwd)))
      {
         for (int i = 0; i < pcount; i++)
         {
            if (strncmp(cwd, projects[i].root, strlen(projects[i].root)) == 0)
            {
               char *sg = style_read(projects[i].name);
               if (sg)
               {
                  /* Skip frontmatter */
                  const char *content = sg;
                  if (strncmp(content, "---", 3) == 0)
                  {
                     const char *end = strstr(content + 3, "---");
                     if (end)
                     {
                        content = end + 3;
                        while (*content == '\n' || *content == '\r')
                           content++;
                     }
                  }
                  size_t sg_len = strlen(content);
                  if (sg_len > 0 && pos + sg_len + 4 < cap)
                  {
                     memcpy(buf + pos, content, sg_len);
                     pos += sg_len;
                     if (pos > 0 && buf[pos - 1] != '\n')
                        buf[pos++] = '\n';
                     buf[pos++] = '\n';
                  }
                  free(sg);
               }
               break;
            }
         }
      }
   }

   /* Network access */
   if (network && network->ssh_entry[0])
   {
      pos += (size_t)snprintf(buf + pos, cap - pos, "# Network Access\n");
      pos += (size_t)snprintf(buf + pos, cap - pos, "Default entry: %s\n", network->ssh_entry);
      if (network->ssh_key[0])
         pos += (size_t)snprintf(buf + pos, cap - pos, "SSH key: %s\n", network->ssh_key);
      pos += (size_t)snprintf(buf + pos, cap - pos, "\n");

      if (network->host_count > 0)
      {
         pos += (size_t)snprintf(buf + pos, cap - pos, "Hosts:\n");
         for (int i = 0; i < network->host_count && pos < cap - 256; i++)
         {
            const agent_net_host_t *h = &network->hosts[i];

            /* Resolve per-host SSH entry (tunnel or fallback) */
            char host_entry[512] = {0};
            int via_tunnel = 0;
            if (network->tunnel_mgr)
               via_tunnel = agent_tunnel_resolve_entry(network->tunnel_mgr, network, h, host_entry,
                                                       sizeof(host_entry));

            if (via_tunnel && host_entry[0])
            {
               /* Tunnel-routed host: show SSH command with key */
               char ssh_cmd[768];
               if (network->ssh_key[0])
                  snprintf(ssh_cmd, sizeof(ssh_cmd), "%s -i %s", host_entry, network->ssh_key);
               else
                  snprintf(ssh_cmd, sizeof(ssh_cmd), "%s", host_entry);
               pos += (size_t)snprintf(buf + pos, cap - pos, "  %-16s %s %s@  %s\n", h->name,
                                       ssh_cmd, h->user[0] ? h->user : "root", h->desc);
            }
            else if (h->port > 0)
               pos += (size_t)snprintf(buf + pos, cap - pos, "  %-16s %s:%d  %-8s %s\n", h->name,
                                       h->ip, h->port, h->user, h->desc);
            else
               pos += (size_t)snprintf(buf + pos, cap - pos, "  %-16s %-20s %-8s %s\n", h->name,
                                       h->ip, h->user, h->desc);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }

      if (network->network_count > 0)
      {
         pos += (size_t)snprintf(buf + pos, cap - pos, "Networks:\n");
         for (int i = 0; i < network->network_count && pos < cap - 128; i++)
         {
            const agent_net_def_t *nd = &network->networks[i];
            pos += (size_t)snprintf(buf + pos, cap - pos, "  %-16s %-20s %s\n", nd->name, nd->cidr,
                                    nd->desc);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Working memory context */
   {
      char *wm_ctx = wm_assemble_context(db, session_id());
      if (wm_ctx && wm_ctx[0])
      {
         pos += (size_t)snprintf(buf + pos, cap - pos, "# Working Memory\n%s\n\n", wm_ctx);
      }
      free(wm_ctx);
   }

   /* Previous attempts (from delegation attempt log) */
   {
      wm_entry_t attempts[WM_MAX_RESULTS];
      int acount = wm_list(db, session_id(), "attempt", attempts, WM_MAX_RESULTS);
      if (acount > 0)
      {
         pos += (size_t)snprintf(buf + pos, cap - pos, "# Previous Attempts\n");
         for (int i = 0; i < acount && pos < cap - 256; i++)
         {
            cJSON *v = cJSON_Parse(attempts[i].value);
            if (!v)
               continue;

            cJSON *jtc = cJSON_GetObjectItemCaseSensitive(v, "task_context");
            cJSON *jap = cJSON_GetObjectItemCaseSensitive(v, "approach");
            cJSON *joc = cJSON_GetObjectItemCaseSensitive(v, "outcome");
            cJSON *jls = cJSON_GetObjectItemCaseSensitive(v, "lesson");

            const char *tc = cJSON_IsString(jtc) ? jtc->valuestring : "";
            const char *ap = cJSON_IsString(jap) ? jap->valuestring : "";
            const char *oc = cJSON_IsString(joc) ? joc->valuestring : "";
            const char *ls = cJSON_IsString(jls) ? jls->valuestring : "";

            /* Include if prompt matches the attempt context */
            if (custom_prompt && custom_prompt[0] && tc[0])
            {
               if (!strstr(custom_prompt, tc) && !strstr(tc, custom_prompt))
               {
                  cJSON_Delete(v);
                  continue;
               }
            }

            pos += (size_t)snprintf(buf + pos, cap - pos, "- Tried: %s\n  Result: %s\n", ap, oc);
            if (ls[0])
               pos += (size_t)snprintf(buf + pos, cap - pos, "  Lesson: %s\n", ls);

            cJSON_Delete(v);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Active tasks */
   {
      aimee_task_t active_tasks[8];
      int tcount = aimee_task_list(db, TASK_IN_PROGRESS, NULL, 8, active_tasks, 8);
      if (tcount > 0)
      {
         pos += (size_t)snprintf(buf + pos, cap - pos, "# Active Tasks\n");
         for (int i = 0; i < tcount && pos < cap - 256; i++)
         {
            pos += (size_t)snprintf(buf + pos, cap - pos, "- [%s] %s\n", active_tasks[i].state,
                                    active_tasks[i].title);
         }
         pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Recent failures (budget: recent + errors) */
   {
      size_t fail_budget = budget_recent + budget_errors;
      static const char *fail_sql = "SELECT role, error, created_at FROM agent_log"
                                    " WHERE success = 0 AND error IS NOT NULL"
                                    " AND created_at > datetime('now', '-5 minutes')"
                                    " ORDER BY id DESC LIMIT 3";
      sqlite3_stmt *fstmt = db_prepare(db, fail_sql);
      if (fstmt)
      {
         int has_failures = 0;
         size_t fail_start = pos;
         sqlite3_reset(fstmt);
         while (sqlite3_step(fstmt) == SQLITE_ROW && pos < cap - 256 &&
                (pos - fail_start) < fail_budget)
         {
            if (!has_failures)
            {
               pos += (size_t)snprintf(buf + pos, cap - pos, "# Recent Failures\n");
               has_failures = 1;
            }
            const char *role = (const char *)sqlite3_column_text(fstmt, 0);
            const char *err = (const char *)sqlite3_column_text(fstmt, 1);
            pos += (size_t)snprintf(buf + pos, cap - pos, "- [%s] %.120s\n", role ? role : "?",
                                    err ? err : "unknown");
         }
         if (has_failures)
            pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Recent execution history: skip. Execution traces from other delegates
    * pollute the context with irrelevant tool calls from unrelated projects.
    * Each delegate starts fresh — it has the task prompt and relevant context
    * from memory/index, which is sufficient. */

   /* Guardrail warnings */
   {
      const char *mode = MODE_APPROVE;
      config_t gcfg;
      if (config_load(&gcfg) == 0)
         mode = config_guardrail_mode(&gcfg);
      if (strcmp(mode, MODE_DENY) == 0)
      {
         pos += (size_t)snprintf(buf + pos, cap - pos,
                                 "# Guardrails\n"
                                 "Mode: strict. Writes to sensitive files and high blast-radius "
                                 "operations will be blocked.\n\n");
      }
      else if (strcmp(mode, MODE_APPROVE) == 0)
      {
         pos += (size_t)snprintf(
             buf + pos, cap - pos,
             "# Guardrails\n"
             "Mode: approve. Writes to sensitive files will trigger warnings.\n\n");
      }
   }

   /* Project contract */
   char *contract = agent_load_project_contract(NULL);
   if (contract)
   {
      pos += (size_t)snprintf(buf + pos, cap - pos, "%s\n", contract);
      free(contract);
   }

   /* Environment capabilities */
   {
      static const char *sql = "SELECT key, value FROM env_capabilities ORDER BY key LIMIT 20";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         int has_env = 0;
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW && pos < cap - 128)
         {
            if (!has_env)
            {
               pos += (size_t)snprintf(buf + pos, cap - pos, "# Environment\n");
               has_env = 1;
            }
            pos += (size_t)snprintf(buf + pos, cap - pos, "  %-20s %s\n",
                                    (const char *)sqlite3_column_text(stmt, 0),
                                    (const char *)sqlite3_column_text(stmt, 1));
         }
         if (has_env)
            pos += (size_t)snprintf(buf + pos, cap - pos, "\n");
      }
   }

   /* Instructions */
   pos += (size_t)snprintf(buf + pos, cap - pos, "%s",
                           agent->exec_system_prompt[0] ? agent->exec_system_prompt
                                                        : default_exec_instructions);

   return buf;
}

/* --- Ephemeral SSH --- */

#ifndef _WIN32

int agent_ssh_setup(const agent_network_t *network, char *key_path_out, size_t key_path_len,
                    char *session_id_out, size_t session_id_len)
{
   /* Generate session ID */
   snprintf(session_id_out, session_id_len, "aimee-session-%d", (int)getpid());

   /* Create temp directory */
   char tmpdir[] = "/tmp/aimee-agent-XXXXXX";
   if (!mkdtemp(tmpdir))
      return -1;

   /* Generate ephemeral Ed25519 key pair */
   char key_file[MAX_PATH_LEN];
   snprintf(key_file, sizeof(key_file), "%s/id_ed25519", tmpdir);
   snprintf(key_path_out, key_path_len, "%s", key_file);

   const char *keygen_argv[] = {"ssh-keygen", "-t", "ed25519",      "-f", key_file, "-N",
                                "",           "-C", session_id_out, "-q", NULL};
   char *keygen_out = NULL;
   if (safe_exec_capture(keygen_argv, &keygen_out, 4096) != 0)
   {
      free(keygen_out);
      return -1;
   }
   free(keygen_out);

   /* Read the public key */
   char pub_file[MAX_PATH_LEN];
   snprintf(pub_file, sizeof(pub_file), "%s.pub", key_file);
   FILE *f = fopen(pub_file, "r");
   if (!f)
      return -1;
   char pubkey[4096];
   pubkey[0] = '\0';
   if (fgets(pubkey, sizeof(pubkey), f))
   {
      size_t len = strlen(pubkey);
      while (len > 0 && (pubkey[len - 1] == '\n' || pubkey[len - 1] == '\r'))
         pubkey[--len] = '\0';
   }
   fclose(f);

   if (!pubkey[0])
      return -1;

   /* Authorize the key on the deploy host via existing SSH access.
    * Parse the deploy host from ssh_entry (e.g., "ssh -p 2222 deploy@host") */
   {
      /* Use single-quoted printf to prevent shell injection from pubkey content.
       * Single quotes prevent all shell interpretation; embedded single quotes
       * are handled by ending the quote, adding an escaped quote, and reopening. */
      char safe_pubkey[4096];
      size_t sp = 0;
      for (size_t pi = 0; pubkey[pi] && sp < sizeof(safe_pubkey) - 5; pi++)
      {
         if (pubkey[pi] == '\'')
         {
            safe_pubkey[sp++] = '\'';
            safe_pubkey[sp++] = '\\';
            safe_pubkey[sp++] = '\'';
            safe_pubkey[sp++] = '\'';
         }
         else
         {
            safe_pubkey[sp++] = pubkey[pi];
         }
      }
      safe_pubkey[sp] = '\0';
      char auth_script[8192];
      snprintf(auth_script, sizeof(auth_script), "printf '%%s\\n' '%s' >> ~/.ssh/authorized_keys",
               safe_pubkey);
      char *ssh_tokens[32];
      int stc = shlex_split(network->ssh_entry, ssh_tokens, 30);
      if (stc <= 0)
      {
         const char *rm_argv[] = {"rm", "-rf", tmpdir, NULL};
         char *rm_out = NULL;
         safe_exec_capture(rm_argv, &rm_out, 256);
         free(rm_out);
         return -1;
      }
      const char *ssh_argv[34];
      for (int si = 0; si < stc && si < 30; si++)
         ssh_argv[si] = ssh_tokens[si];
      ssh_argv[stc] = auth_script;
      ssh_argv[stc + 1] = NULL;
      char *ssh_out = NULL;
      int ssh_rc = safe_exec_capture(ssh_argv, &ssh_out, 4096);
      free(ssh_out);
      for (int si = 0; si < stc; si++)
         free(ssh_tokens[si]);
      if (ssh_rc != 0)
      {
         const char *rm_argv[] = {"rm", "-rf", tmpdir, NULL};
         char *rm_out = NULL;
         safe_exec_capture(rm_argv, &rm_out, 256);
         free(rm_out);
         return -1;
      }
   }

   return 0;
}

void agent_ssh_cleanup(const agent_network_t *network, const char *key_path, const char *session_id)
{
   if (!key_path || !key_path[0])
      return;

   /* Remove the ephemeral key from deploy's authorized_keys */
   if (network && network->ssh_entry[0] && session_id && session_id[0])
   {
      /* Sanitize session_id to prevent sed/shell injection: only allow
       * alphanumeric chars and hyphens in the pattern */
      char safe_sid[128];
      size_t si = 0;
      for (size_t k = 0; session_id[k] && si < sizeof(safe_sid) - 1; k++)
      {
         char c = session_id[k];
         if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '-' || c == '_')
            safe_sid[si++] = c;
      }
      safe_sid[si] = '\0';
      if (!safe_sid[0])
         return; /* nothing safe to match on */
      char sed_script[512];
      snprintf(sed_script, sizeof(sed_script), "sed -i '/%s/d' ~/.ssh/authorized_keys", safe_sid);
      char *ssh_tokens[32];
      int stc = shlex_split(network->ssh_entry, ssh_tokens, 30);
      if (stc > 0)
      {
         const char *ssh_argv[34];
         for (int si = 0; si < stc && si < 30; si++)
            ssh_argv[si] = ssh_tokens[si];
         ssh_argv[stc] = sed_script;
         ssh_argv[stc + 1] = NULL;
         char *ssh_out = NULL;
         int rc = safe_exec_capture(ssh_argv, &ssh_out, 256);
         free(ssh_out);
         if (rc != 0)
            fprintf(stderr, "aimee: warning: failed to revoke ephemeral key\n");
         for (int si = 0; si < stc; si++)
            free(ssh_tokens[si]);
      }
   }

   /* Delete the local key pair directory */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", key_path);
   /* key_path is like /tmp/aimee-agent-XXXXXX/id_ed25519, get the dir */
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      const char *rm_argv[] = {"rm", "-rf", dir, NULL};
      char *rm_out = NULL;
      safe_exec_capture(rm_argv, &rm_out, 256);
      free(rm_out);
   }
}

#endif /* !_WIN32 */

/* --- Context command (Item 10) --- */

void agent_print_context(sqlite3 *db, const agent_config_t *cfg)
{
   if (!cfg || cfg->agent_count == 0)
   {
      printf("No agents configured.\n");
      return;
   }

   agent_t *ag = &((agent_config_t *)cfg)->agents[0]; /* use first agent for context */
   const agent_network_t *net = cfg->network.ssh_entry[0] ? &cfg->network : NULL;

   char *ctx = agent_build_exec_context(db, ag, net, NULL);
   if (ctx)
   {
      printf("%s\n", ctx);
      free(ctx);
   }
   else
   {
      printf("Failed to assemble context.\n");
   }
}

/* --- Logging --- */

void agent_log_call(sqlite3 *db, const agent_result_t *result, const char *role)
{
   if (!db)
      return;

   static const char *sql = "INSERT INTO agent_log (agent_name, role, prompt_tokens,"
                            " completion_tokens, latency_ms, success, error,"
                            " turns, tool_calls, confidence, created_at)"
                            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return;

   char now[32];
   now_utc(now, sizeof(now));

   sqlite3_reset(stmt);
   sqlite3_bind_text(stmt, 1, result->agent_name, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, role ? role : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, result->prompt_tokens);
   sqlite3_bind_int(stmt, 4, result->completion_tokens);
   sqlite3_bind_int(stmt, 5, result->latency_ms);
   sqlite3_bind_int(stmt, 6, result->success);
   sqlite3_bind_text(stmt, 7, result->error[0] ? result->error : NULL, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 8, result->turns);
   sqlite3_bind_int(stmt, 9, result->tool_calls);
   sqlite3_bind_int(stmt, 10, result->confidence);
   sqlite3_bind_text(stmt, 11, now, -1, SQLITE_TRANSIENT);
   DB_STEP_LOG(stmt, "agent_log_call");
}

int agent_get_stats(sqlite3 *db, const char *name, agent_stats_t *out, int max)
{
   const char *sql;
   if (name && name[0])
   {
      sql = "SELECT agent_name, COUNT(*), SUM(prompt_tokens),"
            " SUM(completion_tokens), AVG(latency_ms),"
            " AVG(CASE WHEN success THEN 1.0 ELSE 0.0 END)"
            " FROM agent_log WHERE agent_name = ?"
            " GROUP BY agent_name";
   }
   else
   {
      sql = "SELECT agent_name, COUNT(*), SUM(prompt_tokens),"
            " SUM(completion_tokens), AVG(latency_ms),"
            " AVG(CASE WHEN success THEN 1.0 ELSE 0.0 END)"
            " FROM agent_log GROUP BY agent_name"
            " ORDER BY COUNT(*) DESC";
   }

   sqlite3_stmt *stmt = db_prepare(db, sql);
   if (!stmt)
      return 0;

   sqlite3_reset(stmt);
   if (name && name[0])
      sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);

   int count = 0;
   while (sqlite3_step(stmt) == SQLITE_ROW && count < max)
   {
      agent_stats_t *s = &out[count];
      memset(s, 0, sizeof(*s));
      snprintf(s->name, MAX_AGENT_NAME, "%s", (const char *)sqlite3_column_text(stmt, 0));
      s->total_calls = sqlite3_column_int(stmt, 1);
      s->total_prompt_tokens = sqlite3_column_int(stmt, 2);
      s->total_completion_tokens = sqlite3_column_int(stmt, 3);
      s->avg_latency_ms = sqlite3_column_int(stmt, 4);
      s->success_rate = sqlite3_column_double(stmt, 5);
      count++;
   }

   return count;
}
