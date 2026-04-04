#define _GNU_SOURCE
#define main aimee_mcp_embedded_main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"

#include "../mcp_server.c"
#undef main

static char *capture_request(sqlite3 *db, const char *json)
{
   fflush(stdout);
   int saved = dup(STDOUT_FILENO);
   assert(saved >= 0);

   FILE *tmp = tmpfile();
   assert(tmp != NULL);
   assert(dup2(fileno(tmp), STDOUT_FILENO) >= 0);

   cJSON *req = cJSON_Parse(json);
   assert(req != NULL);
   handle_request(db, req);
   cJSON_Delete(req);
   fflush(stdout);

   long end = ftell(tmp);
   assert(end >= 0);
   rewind(tmp);
   char *out = calloc((size_t)end + 1, 1);
   assert(out != NULL);
   fread(out, 1, (size_t)end, tmp);
   fclose(tmp);
   assert(dup2(saved, STDOUT_FILENO) >= 0);
   close(saved);
   return out;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   return -1;
}

static cJSON *parse_single_line(const char *out)
{
   cJSON *json = cJSON_Parse(out);
   assert(json != NULL);
   return json;
}

static void test_initialize(sqlite3 *db)
{
   char *out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
   cJSON *json = parse_single_line(out);
   assert(strcmp(cJSON_GetObjectItem(json, "jsonrpc")->valuestring, "2.0") == 0);
   cJSON *result = cJSON_GetObjectItem(json, "result");
   assert(strcmp(cJSON_GetObjectItem(result, "protocolVersion")->valuestring,
                 MCP_PROTOCOL_VERSION) == 0);
   assert(cJSON_GetObjectItem(result, "serverInfo") != NULL);
   cJSON_Delete(json);
   free(out);
}

static void test_tools_and_resources(sqlite3 *db)
{
   char *out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
   cJSON *json = parse_single_line(out);
   cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "result"), "tools");
   assert(cJSON_IsArray(tools));
   assert(cJSON_GetArraySize(tools) > 5);
   cJSON_Delete(json);
   free(out);

   out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"resources/list\"}");
   json = parse_single_line(out);
   cJSON *resources = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "result"), "resources");
   assert(cJSON_IsArray(resources));
   assert(cJSON_GetArraySize(resources) >= 6);
   cJSON_Delete(json);
   free(out);
}

static void test_tools_call(sqlite3 *db)
{
   memory_insert(db, TIER_L2, KIND_FACT, "fact.one", "stored fact", 1.0, session_id(), NULL);

   char *out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_facts\",\"arguments\":{}}}");
   cJSON *json = parse_single_line(out);
   cJSON *content = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "result"), "content");
   assert(cJSON_IsArray(content));
   assert(strstr(cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "text")->valuestring,
                 "fact.one") != NULL);
   cJSON_Delete(json);
   free(out);

   out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
                             "\"params\":{\"name\":\"preview_blast_radius\",\"arguments\":{}}}");
   json = parse_single_line(out);
   assert(cJSON_GetObjectItem(json, "error") != NULL);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "error"), "message")->valuestring,
                 "Invalid tool arguments") == 0);
   cJSON_Delete(json);
   free(out);

   out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
                             "\"params\":{\"name\":\"missing_tool\",\"arguments\":{}}}");
   json = parse_single_line(out);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "error"), "message")->valuestring,
                 "Unknown tool") == 0);
   cJSON_Delete(json);
   free(out);
}

static void test_resource_and_prompt_routing(sqlite3 *db)
{
   char *out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"resources/read\","
                                   "\"params\":{\"uri\":\"aimee://facts\"}}");
   cJSON *json = parse_single_line(out);
   cJSON *contents = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "result"), "contents");
   assert(cJSON_IsArray(contents));
   cJSON_Delete(json);
   free(out);

   out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/read\","
                             "\"params\":{\"uri\":\"aimee://missing\"}}");
   json = parse_single_line(out);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "error"), "message")->valuestring,
                 "Unknown resource URI") == 0);
   cJSON_Delete(json);
   free(out);

   out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"prompts/get\","
                             "\"params\":{\"name\":\"delegate-task\","
                             "\"arguments\":{\"role\":\"review\",\"prompt\":\"check this\"}}}");
   json = parse_single_line(out);
   cJSON *messages = cJSON_GetObjectItem(cJSON_GetObjectItem(json, "result"), "messages");
   assert(cJSON_IsArray(messages));
   assert(strstr(cJSON_GetObjectItem(
                     cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "content"), "text")
                     ->valuestring,
                 "review") != NULL);
   cJSON_Delete(json);
   free(out);

   out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"prompts/get\","
                             "\"params\":{\"name\":\"missing-prompt\"}}");
   json = parse_single_line(out);
   assert(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(json, "error"), "message")->valuestring,
                 "Unknown prompt") == 0);
   cJSON_Delete(json);
   free(out);
}

static void test_notification_behavior(sqlite3 *db)
{
   char *out = capture_request(db, "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\"}");
   assert(out[0] == '\0');
   free(out);
}

static void test_shell_escape_injection_payloads(void)
{
   /* Verify shell_escape handles injection payloads correctly */
   char *e;

   /* Single quote injection: '; rm -rf / # */
   e = shell_escape("'; rm -rf / #");
   assert(e != NULL);
   assert(strstr(e, "rm -rf") != NULL); /* content preserved */
   assert(e[0] == '\'');                /* leading quote is escaped */
   assert(strstr(e, "'\\''") != NULL);  /* quote properly escaped */
   free(e);

   /* Backtick injection */
   e = shell_escape("`whoami`");
   assert(strcmp(e, "`whoami`") == 0); /* backticks inside single quotes are safe */
   free(e);

   /* Dollar expansion */
   e = shell_escape("$(cat /etc/passwd)");
   assert(strcmp(e, "$(cat /etc/passwd)") == 0); /* $ inside single quotes is literal */
   free(e);

   /* Null input */
   e = shell_escape(NULL);
   assert(e != NULL);
   assert(strcmp(e, "") == 0);
   free(e);

   /* Empty input */
   e = shell_escape("");
   assert(e != NULL);
   assert(strcmp(e, "") == 0);
   free(e);
}

int main(void)
{
   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);
   test_initialize(db);
   test_tools_and_resources(db);
   test_tools_call(db);
   test_resource_and_prompt_routing(db);
   test_notification_behavior(db);
   test_shell_escape_injection_payloads();
   db_stmt_cache_clear();
   db_close(db);
   printf("mcp_server: all tests passed\n");
   return 0;
}
