/* cli_mcp_serve.c: MCP stdio proxy -- forwards tool calls to aimee-server
 *
 * This replaces the standalone aimee-mcp binary. It handles MCP protocol
 * locally (initialize, tools/list, prompts) and forwards tools/call to
 * aimee-server over the Unix socket. If the server disconnects, it
 * reconnects transparently. */
#include "cli_client.h"
#include "cli_mcp_serve.h"
#include "mcp_tools.h"
#include "aimee.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MCP_LINE_MAX         65536
#define MCP_PROTOCOL_VERSION "2024-11-05"
#define MCP_VERSION          "0.2.0"

#define DEFAULT_TIMEOUT_MS  30000
#define DELEGATE_TIMEOUT_MS 300000
#define RECONNECT_RETRIES   3
#define RECONNECT_DELAY_US  500000 /* 500ms */

/* --- MCP JSON-RPC helpers --- */

static void mcp_send(cJSON *msg)
{
   char *s = cJSON_PrintUnformatted(msg);
   if (s)
   {
      fprintf(stdout, "%s\n", s);
      fflush(stdout);
      free(s);
   }
   cJSON_Delete(msg);
}

static void mcp_respond(cJSON *id, cJSON *result)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
   cJSON_AddItemToObject(resp, "result", result);
   mcp_send(resp);
}

static void mcp_error(cJSON *id, int code, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   if (id)
      cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
   else
      cJSON_AddNullToObject(resp, "id");
   cJSON *err = cJSON_CreateObject();
   cJSON_AddNumberToObject(err, "code", code);
   cJSON_AddStringToObject(err, "message", message);
   cJSON_AddItemToObject(resp, "error", err);
   mcp_send(resp);
}

/* --- Server connection with auto-reconnect --- */

static cli_conn_t g_conn = {.fd = -1};
static const char *g_sock_path = NULL;

static int ensure_connection(void)
{
   if (g_conn.fd >= 0)
      return 0;

   if (!g_sock_path)
      g_sock_path = cli_ensure_server();
   if (!g_sock_path)
      return -1;

   if (cli_connect(&g_conn, g_sock_path) != 0)
      return -1;
   if (cli_authenticate(&g_conn) != 0)
   {
      cli_close(&g_conn);
      return -1;
   }
   return 0;
}

static cJSON *forward_to_server(const char *tool, cJSON *args, int timeout_ms)
{
   for (int retry = 0; retry < RECONNECT_RETRIES; retry++)
   {
      if (ensure_connection() != 0)
      {
         usleep(RECONNECT_DELAY_US);
         continue;
      }

      cJSON *req = cJSON_CreateObject();
      cJSON_AddStringToObject(req, "method", "mcp.call");
      cJSON_AddStringToObject(req, "tool", tool);
      cJSON_AddStringToObject(req, "session_id", session_id());
      if (args)
         cJSON_AddItemToObject(req, "arguments", cJSON_Duplicate(args, 1));
      else
      {
         cJSON *empty = cJSON_CreateObject();
         cJSON_AddItemToObject(req, "arguments", empty);
      }

      cJSON *resp = cli_request(&g_conn, req, timeout_ms);
      cJSON_Delete(req);

      if (resp)
         return resp;

      /* Connection lost -- close and re-discover server on next attempt */
      cli_close(&g_conn);
      g_sock_path = NULL;
      if (retry < RECONNECT_RETRIES - 1)
         usleep(RECONNECT_DELAY_US);
   }
   return NULL;
}

/* --- Local protocol handlers --- */

static void handle_initialize(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION);

   cJSON *caps = cJSON_CreateObject();
   cJSON *tools_cap = cJSON_CreateObject();
   cJSON_AddBoolToObject(tools_cap, "listChanged", 0);
   cJSON_AddItemToObject(caps, "tools", tools_cap);
   cJSON *res_cap = cJSON_CreateObject();
   cJSON_AddBoolToObject(res_cap, "subscribe", 0);
   cJSON_AddBoolToObject(res_cap, "listChanged", 0);
   cJSON_AddItemToObject(caps, "resources", res_cap);
   cJSON *prompts_cap = cJSON_CreateObject();
   cJSON_AddBoolToObject(prompts_cap, "listChanged", 0);
   cJSON_AddItemToObject(caps, "prompts", prompts_cap);
   cJSON_AddItemToObject(result, "capabilities", caps);

   cJSON *info = cJSON_CreateObject();
   cJSON_AddStringToObject(info, "name", "aimee");
   cJSON_AddStringToObject(info, "version", MCP_VERSION);
   cJSON_AddItemToObject(result, "serverInfo", info);

   mcp_respond(id, result);
}

static void handle_tools_list(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "tools", mcp_build_tools_list());
   mcp_respond(id, result);
}

static void handle_tools_call(cJSON *id, cJSON *req)
{
   cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
   cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
   cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

   if (!cJSON_IsString(name))
   {
      mcp_error(id, -32602, "Missing tool name");
      return;
   }

   const char *tool = name->valuestring;
   int timeout = DEFAULT_TIMEOUT_MS;
   if (strcmp(tool, "delegate") == 0)
      timeout = DELEGATE_TIMEOUT_MS;

   cJSON *resp = forward_to_server(tool, args, timeout);
   if (!resp)
   {
      mcp_error(id, -32000, "aimee server unavailable after retries. Run 'aimee' to restart.");
      return;
   }

   /* Check server response status */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (status && cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      const char *errmsg = (msg && cJSON_IsString(msg)) ? msg->valuestring : "server error";
      /* Return as tool result with isError flag, not protocol error */
      cJSON *result = cJSON_CreateObject();
      cJSON *content = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      cJSON_AddStringToObject(block, "text", errmsg);
      cJSON_AddItemToArray(content, block);
      cJSON_AddItemToObject(result, "content", content);
      cJSON_AddBoolToObject(result, "isError", 1);
      mcp_respond(id, result);
      cJSON_Delete(resp);
      return;
   }

   /* Server returns {"status":"ok", "content":[...]} for synchronous tools.
    * For delegate (async), the server responds with the delegation result
    * which also has "status":"ok" and we wrap the response text. */
   cJSON *content = cJSON_DetachItemFromObjectCaseSensitive(resp, "content");
   if (!content)
   {
      /* Wrap the entire server response as text for non-standard formats */
      cJSON *response_text = cJSON_GetObjectItemCaseSensitive(resp, "response");
      content = cJSON_CreateArray();
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "text");
      if (response_text && cJSON_IsString(response_text))
         cJSON_AddStringToObject(block, "text", response_text->valuestring);
      else
      {
         char *raw = cJSON_PrintUnformatted(resp);
         cJSON_AddStringToObject(block, "text", raw ? raw : "{}");
         free(raw);
      }
      cJSON_AddItemToArray(content, block);
   }

   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "content", content);
   mcp_respond(id, result);
   cJSON_Delete(resp);
}

static void handle_resources_list(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON *resources = cJSON_CreateArray();

   const char *tiers[] = {"L0", "L1", "L2", "L3"};
   for (int i = 0; i < 4; i++)
   {
      cJSON *r = cJSON_CreateObject();
      char uri[64], rname[64];
      snprintf(uri, sizeof(uri), "aimee://memories/%s", tiers[i]);
      snprintf(rname, sizeof(rname), "Memories (%s)", tiers[i]);
      cJSON_AddStringToObject(r, "uri", uri);
      cJSON_AddStringToObject(r, "name", rname);
      cJSON_AddStringToObject(r, "mimeType", "application/json");
      cJSON_AddItemToArray(resources, r);
   }

   cJSON *facts = cJSON_CreateObject();
   cJSON_AddStringToObject(facts, "uri", "aimee://facts");
   cJSON_AddStringToObject(facts, "name", "All stored facts");
   cJSON_AddStringToObject(facts, "mimeType", "application/json");
   cJSON_AddItemToArray(resources, facts);

   cJSON *cfg_res = cJSON_CreateObject();
   cJSON_AddStringToObject(cfg_res, "uri", "aimee://config");
   cJSON_AddStringToObject(cfg_res, "name", "Current configuration");
   cJSON_AddStringToObject(cfg_res, "mimeType", "application/json");
   cJSON_AddItemToArray(resources, cfg_res);

   cJSON_AddItemToObject(result, "resources", resources);
   mcp_respond(id, result);
}

static void handle_resources_read(cJSON *id, cJSON *req)
{
   /* Forward resource reads to server via memory.list / memory.get */
   cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
   cJSON *juri = cJSON_GetObjectItemCaseSensitive(params, "uri");
   if (!cJSON_IsString(juri))
   {
      mcp_error(id, -32602, "Missing uri parameter");
      return;
   }

   /* For simplicity, forward as a search_memory or list_facts call */
   const char *uri = juri->valuestring;
   cJSON *args = cJSON_CreateObject();
   const char *tool = NULL;

   if (strncmp(uri, "aimee://memories/", 17) == 0 || strcmp(uri, "aimee://facts") == 0)
      tool = "list_facts";
   else if (strcmp(uri, "aimee://config") == 0)
   {
      /* Config is not a tool -- return minimal info */
      cJSON *contents = cJSON_CreateArray();
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "uri", uri);
      cJSON_AddStringToObject(item, "mimeType", "application/json");
      cJSON_AddStringToObject(item, "text", "{}");
      cJSON_AddItemToArray(contents, item);
      cJSON *result = cJSON_CreateObject();
      cJSON_AddItemToObject(result, "contents", contents);
      mcp_respond(id, result);
      cJSON_Delete(args);
      return;
   }

   if (!tool)
   {
      mcp_error(id, -32602, "Unknown resource URI");
      cJSON_Delete(args);
      return;
   }

   cJSON *resp = forward_to_server(tool, args, DEFAULT_TIMEOUT_MS);
   cJSON_Delete(args);

   /* Wrap server response as resource content */
   cJSON *contents = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "uri", uri);
   cJSON_AddStringToObject(item, "mimeType", "text/plain");

   if (resp)
   {
      cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
      if (content && cJSON_IsArray(content))
      {
         cJSON *first = cJSON_GetArrayItem(content, 0);
         cJSON *text = first ? cJSON_GetObjectItemCaseSensitive(first, "text") : NULL;
         cJSON_AddStringToObject(item, "text",
                                 (text && cJSON_IsString(text)) ? text->valuestring : "");
      }
      else
         cJSON_AddStringToObject(item, "text", "");
      cJSON_Delete(resp);
   }
   else
      cJSON_AddStringToObject(item, "text", "error: server unavailable");

   cJSON_AddItemToArray(contents, item);
   cJSON *result = cJSON_CreateObject();
   cJSON_AddItemToObject(result, "contents", contents);
   mcp_respond(id, result);
}

static void handle_prompts_list(cJSON *id)
{
   cJSON *result = cJSON_CreateObject();
   cJSON *prompts = cJSON_CreateArray();

   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", "search-and-summarize");
      cJSON_AddStringToObject(p, "description", "Search memories and summarize results");
      cJSON *a = cJSON_CreateArray();
      cJSON *a1 = cJSON_CreateObject();
      cJSON_AddStringToObject(a1, "name", "query");
      cJSON_AddStringToObject(a1, "description", "Search terms");
      cJSON_AddBoolToObject(a1, "required", 1);
      cJSON_AddItemToArray(a, a1);
      cJSON_AddItemToObject(p, "arguments", a);
      cJSON_AddItemToArray(prompts, p);
   }

   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", "delegate-task");
      cJSON_AddStringToObject(p, "description", "Delegate a task to a sub-agent");
      cJSON *a = cJSON_CreateArray();
      cJSON *a1 = cJSON_CreateObject();
      cJSON_AddStringToObject(a1, "name", "role");
      cJSON_AddStringToObject(a1, "description", "Agent role (execute, review, etc.)");
      cJSON_AddBoolToObject(a1, "required", 1);
      cJSON_AddItemToArray(a, a1);
      cJSON *a2 = cJSON_CreateObject();
      cJSON_AddStringToObject(a2, "name", "prompt");
      cJSON_AddStringToObject(a2, "description", "Task prompt for the delegate");
      cJSON_AddBoolToObject(a2, "required", 1);
      cJSON_AddItemToArray(a, a2);
      cJSON_AddItemToObject(p, "arguments", a);
      cJSON_AddItemToArray(prompts, p);
   }

   {
      cJSON *p = cJSON_CreateObject();
      cJSON_AddStringToObject(p, "name", "diagnose-issue");
      cJSON_AddStringToObject(p, "description", "Run a diagnostic workflow");
      cJSON *a = cJSON_CreateArray();
      cJSON *a1 = cJSON_CreateObject();
      cJSON_AddStringToObject(a1, "name", "description");
      cJSON_AddStringToObject(a1, "description", "Issue description");
      cJSON_AddBoolToObject(a1, "required", 1);
      cJSON_AddItemToArray(a, a1);
      cJSON_AddItemToObject(p, "arguments", a);
      cJSON_AddItemToArray(prompts, p);
   }

   cJSON_AddItemToObject(result, "prompts", prompts);
   mcp_respond(id, result);
}

/* --- Request dispatch --- */

static void handle_request(cJSON *req)
{
   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");

   if (!cJSON_IsString(method))
   {
      if (id)
         mcp_error(id, -32600, "Invalid request: missing method");
      return;
   }

   const char *m = method->valuestring;

   /* Notifications (no id) -- silently accept */
   if (!id)
   {
      if (strncmp(m, "notifications/", 14) == 0)
         return;
      return;
   }

   if (strcmp(m, "initialize") == 0)
      handle_initialize(id);
   else if (strcmp(m, "tools/list") == 0)
      handle_tools_list(id);
   else if (strcmp(m, "tools/call") == 0)
      handle_tools_call(id, req);
   else if (strcmp(m, "resources/list") == 0)
      handle_resources_list(id);
   else if (strcmp(m, "resources/read") == 0)
      handle_resources_read(id, req);
   else if (strcmp(m, "prompts/list") == 0)
      handle_prompts_list(id);
   else
      mcp_error(id, -32601, "Method not found");
}

/* --- Entry point --- */

int cli_mcp_serve(void)
{
   /* Unbuffered stdout for MCP protocol */
   setvbuf(stdout, NULL, _IONBF, 0);

   char *line = malloc(MCP_LINE_MAX);
   if (!line)
      return 1;

   while (fgets(line, MCP_LINE_MAX, stdin))
   {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      if (len == 0)
         continue;

      cJSON *req = cJSON_Parse(line);
      if (!req)
      {
         mcp_error(NULL, -32700, "Parse error");
         continue;
      }

      handle_request(req);
      cJSON_Delete(req);
   }

   free(line);
   cli_close(&g_conn);
   return 0;
}
