/* cmd_chat.c: interactive chat CLI with streaming and tool use (OpenAI + Anthropic) */
#include "aimee.h"
#include "agent_exec.h"
#include "agent_protocol.h"
#include "agent_tools.h"
#include "commands.h"
#include "cJSON.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* --- Constants --- */

#define CHAT_MAX_INPUT      8192
#define CHAT_SSE_BUF_SIZE   (128 * 1024)
#define CHAT_MAX_TOOL_CALLS 16
#define CHAT_TIMEOUT_MS     300000 /* 5 minutes */

/* --- Provider types --- */

typedef enum
{
   CHAT_PROVIDER_OPENAI,    /* OpenAI, Gemini (compat), Codex */
   CHAT_PROVIDER_ANTHROPIC, /* Claude via Anthropic Messages API */
   CHAT_PROVIDER_CLAUDE     /* Forward to Claude CLI */
} chat_provider_t;

/* --- Signal handling --- */

static volatile sig_atomic_t chat_interrupted = 0;

static void chat_sigint_handler(int sig)
{
   (void)sig;
   chat_interrupted = 1;
}

/* --- Tool call accumulator (for streaming deltas) --- */

typedef struct
{
   char id[64];
   char name[64];
   char *arguments; /* dynamically grown */
   size_t args_len;
   size_t args_cap;
} chat_tool_call_t;

/* --- Chat state --- */

typedef struct
{
   chat_provider_t provider;
   cJSON *messages;     /* conversation history */
   cJSON *tools;        /* tool definitions */
   char *system_prompt; /* stored separately for Anthropic */
   char url[1024];      /* full endpoint URL */
   char model[128];
   char auth_header[4128];
   char extra_headers[512];

   /* SSE parser state */
   char *sse_buf;
   size_t sse_len;

   /* Current response accumulators */
   char *content;
   size_t content_len;
   size_t content_cap;
   chat_tool_call_t tool_calls[CHAT_MAX_TOOL_CALLS];
   int tool_call_count;
   int finished;
   char finish_reason[32];

   /* Error buffer for non-streaming responses */
   char *error_buf;
   size_t error_buf_len;
   size_t error_buf_cap;

   /* Claude CLI session tracking */
   char claude_session_id[256];
} chat_state_t;

/* --- Helpers --- */

static void append_str(char **buf, size_t *len, size_t *cap, const char *data, size_t dlen)
{
   if (*len + dlen + 1 > *cap)
   {
      size_t new_cap = (*cap == 0) ? 1024 : *cap * 2;
      while (new_cap < *len + dlen + 1)
         new_cap *= 2;
      char *tmp = realloc(*buf, new_cap);
      if (!tmp)
         return;
      *buf = tmp;
      *cap = new_cap;
   }
   memcpy(*buf + *len, data, dlen);
   *len += dlen;
   (*buf)[*len] = '\0';
}

static void tool_call_append_args(chat_tool_call_t *tc, const char *data, size_t dlen)
{
   append_str(&tc->arguments, &tc->args_len, &tc->args_cap, data, dlen);
}

/* --- Auth resolution --- */

static int resolve_auth(const config_t *cfg, chat_provider_t provider, char *header,
                        size_t header_len)
{
   header[0] = '\0';
   char key[4096] = {0};

   if (cfg->openai_key_cmd[0])
   {
      /* Use safe argv execution instead of popen() to prevent shell injection */
      const char *argv[] = {"/bin/sh", "-c", cfg->openai_key_cmd, NULL};
      if (!has_shell_metachar(cfg->openai_key_cmd) || strncmp(cfg->openai_key_cmd, "cat ", 4) == 0)
      {
         char *out = NULL;
         int rc = safe_exec_capture(argv, &out, sizeof(key) - 1);
         if (rc == 0 && out)
         {
            snprintf(key, sizeof(key), "%s", out);
            size_t n = strlen(key);
            while (n > 0 && (key[n - 1] == '\n' || key[n - 1] == '\r' || key[n - 1] == ' '))
               key[--n] = '\0';
         }
         free(out);
      }
      else
      {
         fprintf(stderr, "warning: openai_key_cmd contains shell metacharacters; "
                         "only 'cat <path>' style commands are supported\n");
      }
   }

   if (!key[0])
   {
      const char *env = NULL;
      if (provider == CHAT_PROVIDER_ANTHROPIC)
         env = getenv("ANTHROPIC_API_KEY");
      if (!env || !env[0])
         env = getenv("GEMINI_API_KEY");
      if (!env || !env[0])
         env = getenv("OPENAI_API_KEY");
      if (env && env[0])
         snprintf(key, sizeof(key), "%s", env);
   }

   if (!key[0])
      return -1;

   if (provider == CHAT_PROVIDER_ANTHROPIC)
      snprintf(header, header_len, "x-api-key: %s", key);
   else
      snprintf(header, header_len, "Authorization: Bearer %s", key);
   return 0;
}

/* --- SSE parsing: OpenAI format --- */

static void process_sse_line_openai(chat_state_t *state, const char *line, size_t len)
{
   if (len == 0 || line[0] == ':')
      return;

   if (len < 6 || strncmp(line, "data: ", 6) != 0)
      return;

   const char *json_str = line + 6;

   if (strncmp(json_str, "[DONE]", 6) == 0)
   {
      state->finished = 1;
      return;
   }

   cJSON *root = cJSON_Parse(json_str);
   if (!root)
      return;

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0)
   {
      cJSON *err = cJSON_GetObjectItem(root, "error");
      if (err)
      {
         cJSON *msg = cJSON_GetObjectItem(err, "message");
         if (msg && cJSON_IsString(msg))
         {
            const char *m = msg->valuestring;
            append_str(&state->error_buf, &state->error_buf_len, &state->error_buf_cap, m,
                       strlen(m));
         }
      }
      cJSON_Delete(root);
      return;
   }

   cJSON *choice = cJSON_GetArrayItem(choices, 0);

   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   if (finish && cJSON_IsString(finish))
      snprintf(state->finish_reason, sizeof(state->finish_reason), "%s", finish->valuestring);

   cJSON *delta = cJSON_GetObjectItem(choice, "delta");
   if (!delta)
   {
      cJSON_Delete(root);
      return;
   }

   /* Text content */
   cJSON *content = cJSON_GetObjectItem(delta, "content");
   if (content && cJSON_IsString(content) && content->valuestring[0])
   {
      const char *text = content->valuestring;
      size_t tlen = strlen(text);
      append_str(&state->content, &state->content_len, &state->content_cap, text, tlen);
      fwrite(text, 1, tlen, stdout);
      fflush(stdout);
   }

   /* Tool call deltas */
   cJSON *tool_calls = cJSON_GetObjectItem(delta, "tool_calls");
   if (tool_calls && cJSON_IsArray(tool_calls))
   {
      int n = cJSON_GetArraySize(tool_calls);
      for (int i = 0; i < n; i++)
      {
         cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
         cJSON *idx_j = cJSON_GetObjectItem(tc, "index");
         int idx = (idx_j && cJSON_IsNumber(idx_j)) ? idx_j->valueint : 0;

         if (idx >= CHAT_MAX_TOOL_CALLS)
            continue;

         if (idx >= state->tool_call_count)
            state->tool_call_count = idx + 1;

         chat_tool_call_t *call = &state->tool_calls[idx];

         cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
         if (tc_id && cJSON_IsString(tc_id) && !call->id[0])
            snprintf(call->id, sizeof(call->id), "%s", tc_id->valuestring);

         cJSON *fn = cJSON_GetObjectItem(tc, "function");
         if (fn)
         {
            cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
            if (fn_name && cJSON_IsString(fn_name) && !call->name[0])
               snprintf(call->name, sizeof(call->name), "%s", fn_name->valuestring);

            cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
            if (fn_args && cJSON_IsString(fn_args) && fn_args->valuestring[0])
               tool_call_append_args(call, fn_args->valuestring, strlen(fn_args->valuestring));
         }
      }
   }

   cJSON_Delete(root);
}

/* --- SSE parsing: Anthropic format --- */

static void process_sse_line_anthropic(chat_state_t *state, const char *line, size_t len)
{
   if (len == 0 || line[0] == ':')
      return;

   /* Skip event: lines (we parse type from the data JSON) */
   if (len > 6 && strncmp(line, "event:", 6) == 0)
      return;

   if (len < 6 || strncmp(line, "data: ", 6) != 0)
      return;

   const char *json_str = line + 6;
   cJSON *root = cJSON_Parse(json_str);
   if (!root)
      return;

   cJSON *type_j = cJSON_GetObjectItem(root, "type");
   const char *t = (type_j && cJSON_IsString(type_j)) ? type_j->valuestring : "";

   if (strcmp(t, "content_block_start") == 0)
   {
      cJSON *block = cJSON_GetObjectItem(root, "content_block");
      if (block)
      {
         cJSON *btype = cJSON_GetObjectItem(block, "type");
         if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0)
         {
            int idx = state->tool_call_count;
            if (idx < CHAT_MAX_TOOL_CALLS)
            {
               chat_tool_call_t *tc = &state->tool_calls[idx];
               cJSON *id = cJSON_GetObjectItem(block, "id");
               if (id && cJSON_IsString(id))
                  snprintf(tc->id, sizeof(tc->id), "%s", id->valuestring);
               cJSON *name = cJSON_GetObjectItem(block, "name");
               if (name && cJSON_IsString(name))
                  snprintf(tc->name, sizeof(tc->name), "%s", name->valuestring);
               state->tool_call_count = idx + 1;
            }
         }
      }
   }
   else if (strcmp(t, "content_block_delta") == 0)
   {
      cJSON *delta = cJSON_GetObjectItem(root, "delta");
      if (delta)
      {
         cJSON *dtype = cJSON_GetObjectItem(delta, "type");
         const char *dt = (dtype && cJSON_IsString(dtype)) ? dtype->valuestring : "";

         if (strcmp(dt, "text_delta") == 0)
         {
            cJSON *text = cJSON_GetObjectItem(delta, "text");
            if (text && cJSON_IsString(text) && text->valuestring[0])
            {
               const char *s = text->valuestring;
               size_t slen = strlen(s);
               append_str(&state->content, &state->content_len, &state->content_cap, s, slen);
               fwrite(s, 1, slen, stdout);
               fflush(stdout);
            }
         }
         else if (strcmp(dt, "input_json_delta") == 0)
         {
            cJSON *pj = cJSON_GetObjectItem(delta, "partial_json");
            if (pj && cJSON_IsString(pj) && pj->valuestring[0])
            {
               int tc_idx = state->tool_call_count - 1;
               if (tc_idx >= 0 && tc_idx < CHAT_MAX_TOOL_CALLS)
                  tool_call_append_args(&state->tool_calls[tc_idx], pj->valuestring,
                                        strlen(pj->valuestring));
            }
         }
      }
   }
   else if (strcmp(t, "message_delta") == 0)
   {
      cJSON *delta = cJSON_GetObjectItem(root, "delta");
      if (delta)
      {
         cJSON *sr = cJSON_GetObjectItem(delta, "stop_reason");
         if (sr && cJSON_IsString(sr))
            snprintf(state->finish_reason, sizeof(state->finish_reason), "%s", sr->valuestring);
      }
   }
   else if (strcmp(t, "message_stop") == 0)
   {
      state->finished = 1;
   }
   else if (strcmp(t, "error") == 0)
   {
      cJSON *err = cJSON_GetObjectItem(root, "error");
      if (err)
      {
         cJSON *msg = cJSON_GetObjectItem(err, "message");
         if (msg && cJSON_IsString(msg))
            append_str(&state->error_buf, &state->error_buf_len, &state->error_buf_cap,
                       msg->valuestring, strlen(msg->valuestring));
      }
   }

   cJSON_Delete(root);
}

/* --- Stream callback --- */

static int chat_stream_cb(const char *data, size_t len, void *userdata)
{
   chat_state_t *state = (chat_state_t *)userdata;

   if (chat_interrupted)
      return 1; /* abort */

   /* Append to SSE buffer */
   if (state->sse_len + len + 1 > CHAT_SSE_BUF_SIZE)
      state->sse_len = 0;

   memcpy(state->sse_buf + state->sse_len, data, len);
   state->sse_len += len;
   state->sse_buf[state->sse_len] = '\0';

   /* Process complete lines */
   char *buf = state->sse_buf;
   char *nl;
   while ((nl = strchr(buf, '\n')) != NULL)
   {
      size_t line_len = (size_t)(nl - buf);
      if (line_len > 0 && buf[line_len - 1] == '\r')
         line_len--;

      if (state->provider == CHAT_PROVIDER_ANTHROPIC)
         process_sse_line_anthropic(state, buf, line_len);
      else
         process_sse_line_openai(state, buf, line_len);

      buf = nl + 1;
   }

   /* Move remaining partial data to front */
   size_t remaining = state->sse_len - (size_t)(buf - state->sse_buf);
   if (remaining > 0 && buf != state->sse_buf)
      memmove(state->sse_buf, buf, remaining);
   state->sse_len = remaining;

   return 0;
}

/* --- Reset per-turn state --- */

static void chat_reset_turn(chat_state_t *state)
{
   state->sse_len = 0;
   state->finished = 0;
   state->finish_reason[0] = '\0';

   free(state->content);
   state->content = NULL;
   state->content_len = 0;
   state->content_cap = 0;

   free(state->error_buf);
   state->error_buf = NULL;
   state->error_buf_len = 0;
   state->error_buf_cap = 0;

   for (int i = 0; i < state->tool_call_count; i++)
      free(state->tool_calls[i].arguments);
   memset(state->tool_calls, 0, sizeof(state->tool_calls));
   state->tool_call_count = 0;
}

/* --- Build and send request: OpenAI --- */

static int chat_send_openai(chat_state_t *state)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", state->model);
   cJSON_AddItemReferenceToObject(req, "messages", state->messages);
   cJSON_AddBoolToObject(req, "stream", 1);

   if (state->tools && cJSON_GetArraySize(state->tools) > 0)
      cJSON_AddItemReferenceToObject(req, "tools", state->tools);

   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   int rc = agent_http_post_stream(state->url, state->auth_header, body, chat_stream_cb, state,
                                   CHAT_TIMEOUT_MS, state->extra_headers);
   free(body);
   return rc;
}

/* --- Build and send request: Anthropic --- */

static int chat_send_anthropic(chat_state_t *state)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "model", state->model);
   cJSON_AddNumberToObject(req, "max_tokens", 8192);
   cJSON_AddBoolToObject(req, "stream", 1);

   if (state->system_prompt && state->system_prompt[0])
   {
      /* Use content block array with cache_control for prompt caching */
      cJSON *sys_arr = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      cJSON_AddStringToObject(block, "text", state->system_prompt);
      cJSON *cc = cJSON_CreateObject();
      cJSON_AddStringToObject(cc, "type", "ephemeral");
      cJSON_AddItemToObject(block, "cache_control", cc);
      cJSON_AddItemToArray(sys_arr, block);
      cJSON_AddItemToObject(req, "system", sys_arr);
   }

   cJSON_AddItemReferenceToObject(req, "messages", state->messages);

   if (state->tools && cJSON_GetArraySize(state->tools) > 0)
      cJSON_AddItemReferenceToObject(req, "tools", state->tools);

   char *body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!body)
      return -1;

   int rc = agent_http_post_stream(state->url, state->auth_header, body, chat_stream_cb, state,
                                   CHAT_TIMEOUT_MS, state->extra_headers);
   free(body);
   return rc;
}

static int chat_send(chat_state_t *state)
{
   if (state->provider == CHAT_PROVIDER_ANTHROPIC)
      return chat_send_anthropic(state);
   return chat_send_openai(state);
}

/* --- Build assistant message with tool calls for history: OpenAI --- */

static cJSON *build_tool_calls_message_openai(chat_state_t *state)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON_AddNullToObject(msg, "content");

   cJSON *tcs = cJSON_AddArrayToObject(msg, "tool_calls");
   for (int i = 0; i < state->tool_call_count; i++)
   {
      chat_tool_call_t *tc = &state->tool_calls[i];
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "id", tc->id);
      cJSON_AddStringToObject(item, "type", "function");

      cJSON *fn = cJSON_AddObjectToObject(item, "function");
      cJSON_AddStringToObject(fn, "name", tc->name);
      cJSON_AddStringToObject(fn, "arguments", tc->arguments ? tc->arguments : "{}");

      cJSON_AddItemToArray(tcs, item);
   }

   return msg;
}

/* --- Build assistant message with tool calls for history: Anthropic --- */

static cJSON *build_tool_calls_message_anthropic(chat_state_t *state)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");

   cJSON *content_arr = cJSON_AddArrayToObject(msg, "content");

   /* Include text block if any content was produced alongside tool calls */
   if (state->content && state->content_len > 0)
   {
      cJSON *text_block = cJSON_CreateObject();
      cJSON_AddStringToObject(text_block, "type", "text");
      cJSON_AddStringToObject(text_block, "text", state->content);
      cJSON_AddItemToArray(content_arr, text_block);
   }

   for (int i = 0; i < state->tool_call_count; i++)
   {
      chat_tool_call_t *tc = &state->tool_calls[i];
      cJSON *tool_block = cJSON_CreateObject();
      cJSON_AddStringToObject(tool_block, "type", "tool_use");
      cJSON_AddStringToObject(tool_block, "id", tc->id);
      cJSON_AddStringToObject(tool_block, "name", tc->name);

      cJSON *input = cJSON_Parse(tc->arguments ? tc->arguments : "{}");
      if (!input)
         input = cJSON_CreateObject();
      cJSON_AddItemToObject(tool_block, "input", input);

      cJSON_AddItemToArray(content_arr, tool_block);
   }

   return msg;
}

/* --- Execute tool calls: OpenAI format --- */

static int chat_execute_tools_openai(chat_state_t *state)
{
   cJSON *assist_msg = build_tool_calls_message_openai(state);
   cJSON_AddItemToArray(state->messages, assist_msg);

   for (int i = 0; i < state->tool_call_count; i++)
   {
      chat_tool_call_t *tc = &state->tool_calls[i];

      fprintf(stderr, "\033[2m[tool: %s]\033[0m ", tc->name);
      if (tc->arguments)
      {
         cJSON *args = cJSON_Parse(tc->arguments);
         if (args)
         {
            cJSON *cmd = cJSON_GetObjectItem(args, "command");
            cJSON *path = cJSON_GetObjectItem(args, "path");
            if (cmd && cJSON_IsString(cmd))
               fprintf(stderr, "%s", cmd->valuestring);
            else if (path && cJSON_IsString(path))
               fprintf(stderr, "%s", path->valuestring);
            cJSON_Delete(args);
         }
      }
      fprintf(stderr, "\n");

      char *result =
          dispatch_tool_call(tc->name, tc->arguments ? tc->arguments : "{}", CHAT_TIMEOUT_MS);

      cJSON *tool_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(tool_msg, "role", "tool");
      cJSON_AddStringToObject(tool_msg, "tool_call_id", tc->id);
      cJSON_AddStringToObject(tool_msg, "content",
                              result ? result : "error: tool execution failed");
      cJSON_AddItemToArray(state->messages, tool_msg);

      free(result);

      if (chat_interrupted)
         return -1;
   }

   return 0;
}

/* --- Execute tool calls: Anthropic format --- */

static int chat_execute_tools_anthropic(chat_state_t *state)
{
   cJSON *assist_msg = build_tool_calls_message_anthropic(state);
   cJSON_AddItemToArray(state->messages, assist_msg);

   /* Anthropic: all tool results go in a single user message with tool_result blocks */
   cJSON *user_msg = cJSON_CreateObject();
   cJSON_AddStringToObject(user_msg, "role", "user");
   cJSON *results = cJSON_AddArrayToObject(user_msg, "content");

   for (int i = 0; i < state->tool_call_count; i++)
   {
      chat_tool_call_t *tc = &state->tool_calls[i];

      fprintf(stderr, "\033[2m[tool: %s]\033[0m ", tc->name);
      if (tc->arguments)
      {
         cJSON *args = cJSON_Parse(tc->arguments);
         if (args)
         {
            cJSON *cmd = cJSON_GetObjectItem(args, "command");
            cJSON *path = cJSON_GetObjectItem(args, "path");
            if (cmd && cJSON_IsString(cmd))
               fprintf(stderr, "%s", cmd->valuestring);
            else if (path && cJSON_IsString(path))
               fprintf(stderr, "%s", path->valuestring);
            cJSON_Delete(args);
         }
      }
      fprintf(stderr, "\n");

      char *result =
          dispatch_tool_call(tc->name, tc->arguments ? tc->arguments : "{}", CHAT_TIMEOUT_MS);

      cJSON *tr = cJSON_CreateObject();
      cJSON_AddStringToObject(tr, "type", "tool_result");
      cJSON_AddStringToObject(tr, "tool_use_id", tc->id);
      cJSON_AddStringToObject(tr, "content", result ? result : "error: tool execution failed");
      cJSON_AddItemToArray(results, tr);

      free(result);

      if (chat_interrupted)
      {
         cJSON_AddItemToArray(state->messages, user_msg);
         return -1;
      }
   }

   cJSON_AddItemToArray(state->messages, user_msg);
   return 0;
}

static int chat_execute_tools(chat_state_t *state)
{
   if (state->provider == CHAT_PROVIDER_ANTHROPIC)
      return chat_execute_tools_anthropic(state);
   return chat_execute_tools_openai(state);
}

/* --- Read user input --- */

static char *chat_read_input(void)
{
   static char buf[CHAT_MAX_INPUT];
   size_t total = 0;

   fprintf(stdout, "\033[1m> \033[0m");
   fflush(stdout);

   while (total < sizeof(buf) - 2)
   {
      if (!fgets(buf + total, (int)(sizeof(buf) - total), stdin))
         return NULL; /* EOF */

      size_t len = strlen(buf + total);
      total += len;

      /* Check for line continuation */
      if (total >= 2 && buf[total - 2] == '\\' && buf[total - 1] == '\n')
      {
         buf[total - 2] = '\n';
         buf[total - 1] = '\0';
         total--;
         fprintf(stdout, "\033[2m. \033[0m");
         fflush(stdout);
         continue;
      }
      break;
   }

   /* Trim trailing newline */
   while (total > 0 && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
      buf[--total] = '\0';

   if (total == 0)
      return NULL;

   return buf;
}

/* --- System prompt --- */

static char *build_system_prompt(void)
{
   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      snprintf(cwd, sizeof(cwd), ".");

   char *prompt = malloc(8192);
   if (!prompt)
      return NULL;

   snprintf(prompt, 8192,
            "You are an AI coding assistant running in the terminal.\n"
            "Working directory: %s\n\n"
            "# Instructions\n"
            "- Use the bash tool to run shell commands.\n"
            "- Use read_file and write_file for file operations.\n"
            "- Use list_files to explore directories.\n"
            "- Use grep to search file contents.\n"
            "- Use git_status, git_log, git_diff for git operations.\n"
            "- Be concise and direct in your responses.\n"
            "- When editing code, show the changes you make.\n\n"
            "# Delegation\n"
            "You can delegate work to sub-agents via the bash tool:\n"
            "  aimee delegate <role> \"prompt\" [--tools] [--background]\n"
            "Roles are defined in the aimee config. Use --tools to give the delegate "
            "access to tools (bash, read_file, etc). Use --background to run "
            "asynchronously and get a task_id; poll with:\n"
            "  aimee delegate status <task_id>\n"
            "Delegates are ideal for offloading expensive or independent work "
            "(analysis, reviews, tests) in parallel while you continue.\n\n"
            "# Work Queue\n"
            "You can coordinate with other aimee sessions via a shared work queue.\n"
            "  aimee work claim              # pick up next pending item\n"
            "  aimee work complete --result \"summary\"  # mark done\n"
            "  aimee work fail --result \"reason\"       # mark failed\n"
            "  aimee work list               # see all items\n"
            "When you claim an item, read its source to understand what to do.\n"
            "If the source is a proposal (e.g., \"proposal:foo.md\"), read the file,\n"
            "implement it, and create a PR. Then mark the item complete with the PR URL.\n",
            cwd);
   return prompt;
}

/* --- Initialize system prompt into conversation --- */

static void chat_init_system(chat_state_t *state)
{
   char *sys_prompt = build_system_prompt();
   if (!sys_prompt)
      return;

   if (state->provider == CHAT_PROVIDER_ANTHROPIC)
   {
      free(state->system_prompt);
      state->system_prompt = sys_prompt; /* ownership transferred */
   }
   else
   {
      cJSON *sys_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(sys_msg, "role", "system");
      cJSON_AddStringToObject(sys_msg, "content", sys_prompt);
      cJSON_AddItemToArray(state->messages, sys_msg);
      free(sys_prompt);
   }
}

/* --- Add assistant text response to history --- */

static void chat_add_assistant_text(chat_state_t *state)
{
   cJSON *assist = cJSON_CreateObject();
   cJSON_AddStringToObject(assist, "role", "assistant");

   if (state->provider == CHAT_PROVIDER_ANTHROPIC)
   {
      cJSON *ca = cJSON_AddArrayToObject(assist, "content");
      cJSON *tb = cJSON_CreateObject();
      cJSON_AddStringToObject(tb, "type", "text");
      cJSON_AddStringToObject(tb, "text", state->content);
      cJSON_AddItemToArray(ca, tb);
   }
   else
   {
      cJSON_AddStringToObject(assist, "content", state->content);
   }

   cJSON_AddItemToArray(state->messages, assist);
}

/* --- Claude CLI forwarding --- */

#define CLAUDE_LINE_MAX (256 * 1024)

static void chat_via_claude(chat_state_t *state, const char *message)
{
   /* Build command */
   char cmd[1024];
   if (state->claude_session_id[0])
   {
      /* Sanitize session ID to prevent shell injection */
      char safe_sid[256];
      snprintf(safe_sid, sizeof(safe_sid), "%s", state->claude_session_id);
      sanitize_shell_token(safe_sid);
      snprintf(cmd, sizeof(cmd),
               "claude -p --output-format stream-json --verbose --include-partial-messages "
               "--resume '%s'",
               safe_sid);
   }
   else
      snprintf(cmd, sizeof(cmd),
               "claude -p --output-format stream-json --verbose --include-partial-messages");

   int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
   if (pipe(in_pipe) < 0)
   {
      fprintf(stderr, "\033[31mError: pipe failed\033[0m\n\n");
      return;
   }
   if (pipe(out_pipe) < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      fprintf(stderr, "\033[31mError: pipe failed\033[0m\n\n");
      return;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      fprintf(stderr, "\033[31mError: fork failed\033[0m\n\n");
      return;
   }

   if (pid == 0)
   {
      close(in_pipe[1]);
      close(out_pipe[0]);
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[0]);
      close(out_pipe[1]);
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0)
      {
         dup2(devnull, STDERR_FILENO);
         close(devnull);
      }
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   close(in_pipe[0]);
   close(out_pipe[1]);

   /* Write message via stdin */
   size_t msg_len = strlen(message);
   size_t written = 0;
   while (written < msg_len)
   {
      ssize_t w = write(in_pipe[1], message + written, msg_len - written);
      if (w <= 0)
         break;
      written += (size_t)w;
   }
   close(in_pipe[1]);

   /* Read stream-json output */
   FILE *fp = fdopen(out_pipe[0], "r");
   if (!fp)
   {
      close(out_pipe[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      fprintf(stderr, "\033[31mError: fdopen failed\033[0m\n\n");
      return;
   }

   char *line = malloc(CLAUDE_LINE_MAX);
   if (!line)
   {
      fclose(fp);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return;
   }

   int in_response = 0;

   while (fgets(line, CLAUDE_LINE_MAX, fp))
   {
      if (chat_interrupted)
      {
         kill(pid, SIGTERM);
         break;
      }

      cJSON *obj = cJSON_Parse(line);
      if (!obj)
         continue;

      cJSON *type_j = cJSON_GetObjectItem(obj, "type");
      const char *type = (type_j && cJSON_IsString(type_j)) ? type_j->valuestring : "";

      if (strcmp(type, "stream_event") == 0)
      {
         /* Real-time streaming deltas */
         cJSON *event = cJSON_GetObjectItem(obj, "event");
         if (event)
         {
            cJSON *etype = cJSON_GetObjectItem(event, "type");
            const char *et = (etype && cJSON_IsString(etype)) ? etype->valuestring : "";

            if (strcmp(et, "content_block_delta") == 0)
            {
               cJSON *delta = cJSON_GetObjectItem(event, "delta");
               cJSON *dt = delta ? cJSON_GetObjectItem(delta, "type") : NULL;
               const char *dts = (dt && cJSON_IsString(dt)) ? dt->valuestring : "";

               if (strcmp(dts, "text_delta") == 0)
               {
                  cJSON *text = cJSON_GetObjectItem(delta, "text");
                  if (text && cJSON_IsString(text) && text->valuestring[0])
                  {
                     if (!in_response)
                     {
                        fprintf(stdout, "\033[1m");
                        in_response = 1;
                     }
                     fputs(text->valuestring, stdout);
                     fflush(stdout);
                  }
               }
            }
            else if (strcmp(et, "content_block_start") == 0)
            {
               cJSON *cb = cJSON_GetObjectItem(event, "content_block");
               cJSON *cbtype = cb ? cJSON_GetObjectItem(cb, "type") : NULL;
               if (cbtype && cJSON_IsString(cbtype) && strcmp(cbtype->valuestring, "tool_use") == 0)
               {
                  cJSON *name = cJSON_GetObjectItem(cb, "name");
                  if (in_response)
                  {
                     fprintf(stdout, "\033[0m");
                     in_response = 0;
                  }
                  fprintf(stderr, "\n\033[2m[tool: %s]\033[0m\n",
                          name && cJSON_IsString(name) ? name->valuestring : "?");
               }
            }
         }
      }
      else if (strcmp(type, "tool_result") == 0)
      {
         /* Next turn starts fresh */
      }
      else if (strcmp(type, "result") == 0)
      {
         cJSON *sid = cJSON_GetObjectItem(obj, "session_id");
         if (sid && cJSON_IsString(sid))
            snprintf(state->claude_session_id, sizeof(state->claude_session_id), "%s",
                     sid->valuestring);
      }

      cJSON_Delete(obj);
   }

   if (in_response)
      fprintf(stdout, "\033[0m\n\n");

   free(line);
   fclose(fp);
   waitpid(pid, NULL, 0);
}

/* --- Main entry point --- */

void cmd_chat(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;

   signal(SIGINT, chat_sigint_handler);

   config_t cfg;
   config_load(&cfg);

   /* Initialize HTTP */
   agent_http_init();

   /* Set up state */
   chat_state_t state;
   memset(&state, 0, sizeof(state));

   /* Detect provider and set defaults */
   if (strcmp(cfg.provider, "claude") == 0)
   {
      state.provider = CHAT_PROVIDER_CLAUDE;
      state.claude_session_id[0] = '\0';
   }
   else if (strcmp(cfg.provider, "gemini") == 0)
   {
      state.provider = CHAT_PROVIDER_OPENAI;
      if (strcmp(cfg.openai_endpoint, "https://api.openai.com/v1") == 0)
         snprintf(cfg.openai_endpoint, sizeof(cfg.openai_endpoint),
                  "https://generativelanguage.googleapis.com/v1beta/openai");
      if (strcmp(cfg.openai_model, "gpt-4o") == 0)
         snprintf(cfg.openai_model, sizeof(cfg.openai_model), "gemini-2.5-pro");
   }
   else if (strcmp(cfg.provider, "codex") == 0)
   {
      state.provider = CHAT_PROVIDER_OPENAI;
      if (strcmp(cfg.openai_model, "gpt-4o") == 0)
         snprintf(cfg.openai_model, sizeof(cfg.openai_model), "o3");
   }
   else if (strcmp(cfg.provider, "copilot") == 0)
   {
      state.provider = CHAT_PROVIDER_OPENAI;
      if (strcmp(cfg.openai_endpoint, "https://api.openai.com/v1") == 0)
         snprintf(cfg.openai_endpoint, sizeof(cfg.openai_endpoint),
                  "https://api.githubcopilot.com");
      if (strcmp(cfg.openai_model, "gpt-4o") == 0)
         snprintf(cfg.openai_model, sizeof(cfg.openai_model), "gpt-4o");
      if (!cfg.openai_key_cmd[0])
         snprintf(cfg.openai_key_cmd, sizeof(cfg.openai_key_cmd), "gh auth token");
   }
   else
   {
      state.provider = CHAT_PROVIDER_OPENAI;
   }

   /* Install signal handler */
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_handler = chat_sigint_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;
   sigaction(SIGINT, &sa, NULL);
   signal(SIGPIPE, SIG_IGN);

   if (state.provider == CHAT_PROVIDER_CLAUDE)
   {
      fprintf(stderr, "\033[1maimee chat\033[0m (claude)\n");
      fprintf(stderr, "Type /quit to exit, /clear to reset conversation.\n\n");

      for (;;)
      {
         chat_interrupted = 0;
         char *input = chat_read_input();
         if (!input)
         {
            if (feof(stdin))
            {
               fprintf(stderr, "\n");
               break;
            }
            continue;
         }
         if (strcmp(input, "/quit") == 0 || strcmp(input, "/exit") == 0)
            break;
         if (strcmp(input, "/clear") == 0)
         {
            state.claude_session_id[0] = '\0';
            fprintf(stderr, "Conversation cleared.\n\n");
            continue;
         }
         chat_via_claude(&state, input);
      }

      agent_http_cleanup();
      return;
   }

   snprintf(state.model, sizeof(state.model), "%s", cfg.openai_model);

   /* Build URL */
   if (state.provider == CHAT_PROVIDER_ANTHROPIC)
      snprintf(state.url, sizeof(state.url), "%s/messages", cfg.openai_endpoint);
   else
      snprintf(state.url, sizeof(state.url), "%s/chat/completions", cfg.openai_endpoint);

   /* Resolve auth */
   if (resolve_auth(&cfg, state.provider, state.auth_header, sizeof(state.auth_header)) < 0)
   {
      const char *env_hint = "OPENAI_API_KEY";
      if (state.provider == CHAT_PROVIDER_ANTHROPIC)
         env_hint = "ANTHROPIC_API_KEY";
      fprintf(stderr,
              "Error: no API key configured.\n"
              "Set openai_key_cmd in ~/.config/aimee/config.json or %s env var.\n",
              env_hint);
      agent_http_cleanup();
      return;
   }

   /* Allocate SSE buffer */
   state.sse_buf = malloc(CHAT_SSE_BUF_SIZE);
   if (!state.sse_buf)
   {
      agent_http_cleanup();
      return;
   }

   /* Build tool definitions */
   if (state.provider == CHAT_PROVIDER_ANTHROPIC)
      state.tools = build_tools_array_anthropic();
   else
      state.tools = build_tools_array();

   /* Initialize conversation */
   state.messages = cJSON_CreateArray();
   chat_init_system(&state);

   /* Banner */
   fprintf(stderr, "\033[1maimee chat\033[0m (%s @ %s)\n", state.model, cfg.openai_endpoint);
   fprintf(stderr, "Type /quit to exit, /clear to reset conversation.\n\n");

   /* REPL loop */
   for (;;)
   {
      chat_interrupted = 0;

      char *input = chat_read_input();
      if (!input)
      {
         if (feof(stdin))
         {
            fprintf(stderr, "\n");
            break;
         }
         continue;
      }

      /* Commands */
      if (strcmp(input, "/quit") == 0 || strcmp(input, "/exit") == 0)
         break;

      if (strcmp(input, "/clear") == 0)
      {
         cJSON_Delete(state.messages);
         state.messages = cJSON_CreateArray();
         chat_init_system(&state);
         fprintf(stderr, "Conversation cleared.\n\n");
         continue;
      }

      if (strncmp(input, "/model ", 7) == 0)
      {
         const char *new_model = input + 7;
         while (*new_model == ' ')
            new_model++;
         if (*new_model)
         {
            snprintf(state.model, sizeof(state.model), "%s", new_model);
            fprintf(stderr, "Model changed to: %s\n\n", state.model);
         }
         continue;
      }

      /* Add user message */
      cJSON *user_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(user_msg, "role", "user");
      cJSON_AddStringToObject(user_msg, "content", input);
      cJSON_AddItemToArray(state.messages, user_msg);

      /* Send and handle response (may loop for tool calls) */
      for (;;)
      {
         chat_reset_turn(&state);
         messages_compact_consecutive(state.messages);

         int http_rc = chat_send(&state);

         if (chat_interrupted)
         {
            fprintf(stderr, "\n\033[2m(interrupted)\033[0m\n\n");
            if (state.content && state.content_len > 0)
            {
               chat_add_assistant_text(&state);
            }
            break;
         }

         if (http_rc < 0)
         {
            fprintf(stderr, "\n\033[31mNetwork error\033[0m\n\n");
            break;
         }

         if (http_rc >= 400)
         {
            if (state.error_buf && state.error_buf_len > 0)
               fprintf(stderr, "\n\033[31mAPI error (%d): %s\033[0m\n\n", http_rc, state.error_buf);
            else
               fprintf(stderr, "\n\033[31mAPI error (%d)\033[0m\n\n", http_rc);
            break;
         }

         /* Check if it's a tool call response */
         int is_tool_call = 0;
         if (state.provider == CHAT_PROVIDER_ANTHROPIC)
            is_tool_call =
                (state.tool_call_count > 0 && strcmp(state.finish_reason, "tool_use") == 0);
         else
            is_tool_call =
                (state.tool_call_count > 0 && strcmp(state.finish_reason, "tool_calls") == 0);

         if (is_tool_call)
         {
            if (chat_execute_tools(&state) < 0)
            {
               fprintf(stderr, "\n\033[2m(interrupted during tool execution)\033[0m\n\n");
               break;
            }
            continue;
         }

         /* Normal text response */
         if (state.content && state.content_len > 0)
         {
            fprintf(stdout, "\n\n");
            chat_add_assistant_text(&state);
         }
         else
         {
            fprintf(stdout, "\n");
         }
         break;
      }
   }

   /* Cleanup */
   chat_reset_turn(&state);
   free(state.sse_buf);
   free(state.system_prompt);
   cJSON_Delete(state.messages);
   cJSON_Delete(state.tools);
   agent_http_cleanup();

   (void)ctx;
}
