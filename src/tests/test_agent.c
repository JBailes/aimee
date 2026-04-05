#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "agent.h"
#include "agent_protocol.h"
#include "cJSON.h"

/* --- Expose tool functions for testing via redeclaration --- */
char *tool_bash(const char *command, int timeout_ms);
char *tool_read_file(const char *path, int offset, int limit);
char *tool_write_file(const char *path, const char *content);
char *tool_list_files(const char *path, const char *pattern);
char *dispatch_tool_call(const char *name, const char *arguments_json, int timeout_ms);
char *dispatch_tool_call_ctx(const char *name, const char *arguments_json, int timeout_ms,
                             checkpoint_ctx_t *ctx);

static cJSON *parse_json_or_die(const char *text)
{
   cJSON *json = cJSON_Parse(text);
   assert(json != NULL);
   return json;
}

static void test_agent_expand_env(void)
{
   char dst[128];

   setenv("AIMEE_TEST_ENV", "expanded", 1);

   agent_expand_env("$AIMEE_TEST_ENV", dst, sizeof(dst));
   assert(strcmp(dst, "expanded") == 0);

   agent_expand_env("$AIMEE_NO_ENV", dst, sizeof(dst));
   assert(strcmp(dst, "$AIMEE_NO_ENV") == 0);

   agent_expand_env("", dst, sizeof(dst));
   assert(strcmp(dst, "") == 0);

   agent_expand_env("plain string", dst, sizeof(dst));
   assert(strcmp(dst, "plain string") == 0);
}

static void test_agent_has_role(void)
{
   agent_t agent;

   memset(&agent, 0, sizeof(agent));
   strcpy(agent.roles[0], "summarize");
   agent.role_count = 1;

   assert(agent_has_role(&agent, "summarize") == 1);
   assert(agent_has_role(&agent, "translate") == 0);

   agent.role_count = 0;
   assert(agent_has_role(&agent, "summarize") == 0);
}

static void test_agent_find(void)
{
   agent_config_t cfg;

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;
   strcpy(cfg.agents[0].name, "agent_one");
   strcpy(cfg.agents[1].name, "agent_two");

   assert(agent_find(&cfg, "agent_one") == &cfg.agents[0]);
   assert(agent_find(&cfg, "agent_two") == &cfg.agents[1]);
   assert(agent_find(&cfg, "missing") == NULL);
}

static void test_agent_route(void)
{
   agent_config_t cfg;

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.agents[0].name, "cheap");
   strcpy(cfg.agents[0].roles[0], "summarize");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   strcpy(cfg.agents[1].roles[0], "summarize");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;

   assert(agent_route(&cfg, "summarize") == &cfg.agents[0]);

   cfg.agents[0].enabled = 0;
   assert(agent_route(&cfg, "summarize") == &cfg.agents[1]);

   memset(&cfg, 0, sizeof(cfg));
   cfg.agent_count = 2;

   strcpy(cfg.default_agent, "cheap");
   strcpy(cfg.agents[0].name, "cheap");
   cfg.agents[0].cost_tier = 0;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;

   strcpy(cfg.agents[1].name, "expensive");
   cfg.agents[1].cost_tier = 1;
   cfg.agents[1].enabled = 1;
   cfg.agents[1].tools_enabled = 1;
   strcpy(cfg.agents[1].exec_roles[0], "custom_exec");
   cfg.agents[1].exec_role_count = 1;

   assert(agent_route(&cfg, "execute") == &cfg.agents[0]);
   assert(agent_route(&cfg, "custom_exec") == &cfg.agents[1]);

   assert(agent_route(&cfg, "no_role") == NULL);
}

static void test_agent_is_exec_role(void)
{
   agent_t agent;
   memset(&agent, 0, sizeof(agent));

   /* No explicit exec_roles: use defaults */
   assert(agent_is_exec_role(&agent, "deploy") == 1);
   assert(agent_is_exec_role(&agent, "validate") == 1);
   assert(agent_is_exec_role(&agent, "test") == 1);
   assert(agent_is_exec_role(&agent, "diagnose") == 1);
   assert(agent_is_exec_role(&agent, "execute") == 1);
   assert(agent_is_exec_role(&agent, "summarize") == 0);
   assert(agent_is_exec_role(&agent, "draft") == 0);

   /* With explicit exec_roles */
   strcpy(agent.exec_roles[0], "deploy");
   strcpy(agent.exec_roles[1], "custom_role");
   agent.exec_role_count = 2;

   assert(agent_is_exec_role(&agent, "deploy") == 1);
   assert(agent_is_exec_role(&agent, "custom_role") == 1);
   assert(agent_is_exec_role(&agent, "validate") == 0);
   assert(agent_is_exec_role(&agent, "execute") == 0);
}

static void test_tool_bash(void)
{
   /* Basic echo */
   char *result = tool_bash("echo hello", 5000);
   assert(result != NULL);
   cJSON *json = cJSON_Parse(result);
   assert(json != NULL);
   cJSON *out = cJSON_GetObjectItem(json, "stdout");
   assert(out && cJSON_IsString(out));
   assert(strstr(out->valuestring, "hello") != NULL);
   cJSON *ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 0);
   cJSON_Delete(json);
   free(result);

   /* Non-zero exit code */
   result = tool_bash("exit 42", 5000);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == 42);
   cJSON_Delete(json);
   free(result);

   /* Timeout */
   result = tool_bash("sleep 60", 200);
   json = cJSON_Parse(result);
   assert(json != NULL);
   ec = cJSON_GetObjectItem(json, "exit_code");
   assert(ec && ec->valueint == -1);
   cJSON_Delete(json);
   free(result);
}

static void test_tool_read_file(void)
{
   /* Write a temp file */
   char tmppath[] = "/tmp/aimee_test_read_XXXXXX";
   int fd = mkstemp(tmppath);
   assert(fd >= 0);
   const char *content = "line1\nline2\nline3\nline4\n";
   if (write(fd, content, strlen(content)) < 0)
   { /* ignore */
   }
   close(fd);

   /* Read entire file */
   char *result = tool_read_file(tmppath, 0, 0);
   assert(result != NULL);
   assert(strcmp(result, content) == 0);
   free(result);

   /* Read with offset */
   result = tool_read_file(tmppath, 1, 0);
   assert(result != NULL);
   assert(strcmp(result, "line2\nline3\nline4\n") == 0);
   free(result);

   /* Read with limit */
   result = tool_read_file(tmppath, 0, 2);
   assert(result != NULL);
   assert(strcmp(result, "line1\nline2\n") == 0);
   free(result);

   /* Nonexistent path */
   result = tool_read_file("/nonexistent/path/file.txt", 0, 0);
   assert(result != NULL);
   assert(strstr(result, "error: cannot open") != NULL);
   free(result);

   unlink(tmppath);
}

static void test_tool_write_file(void)
{
   char tmppath[] = "/tmp/aimee_test_write_XXXXXX";
   int fd = mkstemp(tmppath);
   close(fd);

   char *result = tool_write_file(tmppath, "hello world");
   assert(result != NULL);
   assert(strcmp(result, "ok") == 0);
   free(result);

   /* Verify contents */
   char *readback = tool_read_file(tmppath, 0, 0);
   assert(readback != NULL);
   assert(strcmp(readback, "hello world") == 0);
   free(readback);

   unlink(tmppath);
}

static void test_tool_list_files(void)
{
   char tmpdir[] = "/tmp/aimee_test_list_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char alpha[512], beta[512];
   snprintf(alpha, sizeof(alpha), "%s/alpha.txt", tmpdir);
   snprintf(beta, sizeof(beta), "%s/beta.log", tmpdir);

   FILE *f = fopen(alpha, "w");
   assert(f != NULL);
   fputs("alpha", f);
   fclose(f);

   f = fopen(beta, "w");
   assert(f != NULL);
   fputs("beta", f);
   fclose(f);

   char *result = tool_list_files(tmpdir, "*.txt");
   assert(result != NULL);
   assert(strstr(result, "alpha.txt") != NULL);
   assert(strstr(result, "beta.log") == NULL);
   free(result);

   /* List nonexistent dir */
   result = tool_list_files("/nonexistent_dir_12345", NULL);
   assert(result != NULL);
   assert(result[0] == '\0');
   free(result);

   unlink(alpha);
   unlink(beta);
   rmdir(tmpdir);
}

static void test_dispatch_tool_call(void)
{
   /* bash tool */
   char *result = dispatch_tool_call("bash", "{\"command\":\"echo dispatch_test\"}", 5000);
   assert(result != NULL);
   cJSON *json = parse_json_or_die(result);
   assert(strstr(cJSON_GetObjectItem(json, "stdout")->valuestring, "dispatch_test") != NULL);
   cJSON_Delete(json);
   free(result);

   /* Unknown tool */
   result = dispatch_tool_call("unknown_tool", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error: unknown tool") != NULL);
   free(result);

   /* Missing parameter */
   result = dispatch_tool_call("bash", "{}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error: missing 'command' parameter") != NULL);
   free(result);
}

static void test_parse_openai_tool_calls(void)
{
   /* Build a mock OpenAI response with tool_calls */
   const char *mock_json = "{"
                           "  \"choices\": [{"
                           "    \"finish_reason\": \"tool_calls\","
                           "    \"message\": {"
                           "      \"role\": \"assistant\","
                           "      \"content\": null,"
                           "      \"tool_calls\": [{"
                           "        \"id\": \"call_abc123\","
                           "        \"type\": \"function\","
                           "        \"function\": {"
                           "          \"name\": \"bash\","
                           "          \"arguments\": \"{\\\"command\\\":\\\"ls\\\"}\""
                           "        }"
                           "      }]"
                           "    }"
                           "  }],"
                           "  \"usage\": {\"prompt_tokens\": 100, \"completion_tokens\": 50}"
                           "}";

   /* We need to call the internal parse function. Since it's static,
    * we test indirectly by verifying the cJSON structure matches what
    * the parser expects. This validates the JSON format contract. */
   cJSON *root = cJSON_Parse(mock_json);
   assert(root != NULL);

   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   assert(choices && cJSON_GetArraySize(choices) == 1);

   cJSON *choice = cJSON_GetArrayItem(choices, 0);
   cJSON *finish = cJSON_GetObjectItem(choice, "finish_reason");
   assert(finish && strcmp(finish->valuestring, "tool_calls") == 0);

   cJSON *message = cJSON_GetObjectItem(choice, "message");
   cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
   assert(tool_calls && cJSON_GetArraySize(tool_calls) == 1);

   cJSON *tc = cJSON_GetArrayItem(tool_calls, 0);
   cJSON *tc_id = cJSON_GetObjectItem(tc, "id");
   assert(tc_id && strcmp(tc_id->valuestring, "call_abc123") == 0);

   cJSON *fn = cJSON_GetObjectItem(tc, "function");
   cJSON *fn_name = cJSON_GetObjectItem(fn, "name");
   assert(fn_name && strcmp(fn_name->valuestring, "bash") == 0);

   cJSON *fn_args = cJSON_GetObjectItem(fn, "arguments");
   assert(fn_args && cJSON_IsString(fn_args));
   cJSON *args_parsed = cJSON_Parse(fn_args->valuestring);
   assert(args_parsed != NULL);
   cJSON *cmd = cJSON_GetObjectItem(args_parsed, "command");
   assert(cmd && strcmp(cmd->valuestring, "ls") == 0);
   cJSON_Delete(args_parsed);

   cJSON_Delete(root);

   /* Test a text response (no tool calls) */
   const char *text_json = "{"
                           "  \"choices\": [{"
                           "    \"finish_reason\": \"stop\","
                           "    \"message\": {"
                           "      \"role\": \"assistant\","
                           "      \"content\": \"Task complete.\""
                           "    }"
                           "  }],"
                           "  \"usage\": {\"prompt_tokens\": 200, \"completion_tokens\": 10}"
                           "}";

   root = cJSON_Parse(text_json);
   assert(root != NULL);
   choices = cJSON_GetObjectItem(root, "choices");
   choice = cJSON_GetArrayItem(choices, 0);
   finish = cJSON_GetObjectItem(choice, "finish_reason");
   assert(finish && strcmp(finish->valuestring, "stop") == 0);
   message = cJSON_GetObjectItem(choice, "message");
   cJSON *content = cJSON_GetObjectItem(message, "content");
   assert(content && strcmp(content->valuestring, "Task complete.") == 0);
   cJSON_Delete(root);
}

/* --- Shared path-policy tests (traversal and sensitive-path rejection) --- */

static void test_path_traversal_rejected(void)
{
   /* read_file with traversal should be rejected */
   char *result = tool_read_file("/tmp/../etc/shadow", 0, 0);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* write_file with traversal */
   result = tool_write_file("/tmp/../etc/shadow", "hacked");
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* dispatch traversal via read_file */
   result = dispatch_tool_call("read_file", "{\"path\":\"/tmp/../../etc/shadow\"}", 5000);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);

   /* list_files with traversal in path */
   result = tool_list_files("/tmp/../etc", NULL);
   assert(result != NULL);
   assert(strstr(result, "error:") != NULL);
   free(result);
}

static void test_sensitive_path_rejected(void)
{
   /* read_file: .ssh directory */
   char ssh_path[256];
   const char *home = getenv("HOME");
   if (home)
   {
      snprintf(ssh_path, sizeof(ssh_path), "%s/.ssh/id_rsa", home);
      char *result = tool_read_file(ssh_path, 0, 0);
      assert(result != NULL);
      /* Should be rejected as sensitive or fail to open */
      assert(strstr(result, "error:") != NULL || strstr(result, "denied") != NULL);
      free(result);
   }
}

static void test_symlink_escape_rejected(void)
{
   /* Create a symlink pointing to /etc/shadow and try to read through it */
   char tmpdir[] = "/tmp/aimee_test_symlink_XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char link_path[512];
   snprintf(link_path, sizeof(link_path), "%s/sneaky_link", tmpdir);

   /* Create symlink to a sensitive path */
   if (symlink("/etc/shadow", link_path) == 0)
   {
      char *result = tool_read_file(link_path, 0, 0);
      assert(result != NULL);
      /* Should be blocked by the realpath-based check */
      assert(strstr(result, "error:") != NULL || strstr(result, "denied") != NULL);
      free(result);
      unlink(link_path);
   }

   rmdir(tmpdir);
}

/* --- Per-invocation checkpoint isolation tests --- */

static void test_checkpoint_ctx_isolation(void)
{
   /* Two independent checkpoint contexts should not interfere */
   checkpoint_ctx_t *ctx_a = checkpoint_ctx_new();
   checkpoint_ctx_t *ctx_b = checkpoint_ctx_new();
   assert(ctx_a != NULL);
   assert(ctx_b != NULL);
   assert(ctx_a != ctx_b);

   /* Create temp files for each context */
   char path_a[] = "/tmp/aimee_ckpt_a_XXXXXX";
   char path_b[] = "/tmp/aimee_ckpt_b_XXXXXX";
   int fd_a = mkstemp(path_a);
   int fd_b = mkstemp(path_b);
   assert(fd_a >= 0);
   assert(fd_b >= 0);
   if (write(fd_a, "original_a", 10) < 0)
   { /* ignore */
   }
   if (write(fd_b, "original_b", 10) < 0)
   { /* ignore */
   }
   close(fd_a);
   close(fd_b);

   /* Push checkpoints to separate contexts */
   checkpoint_ctx_push(ctx_a, path_a);
   checkpoint_ctx_push(ctx_b, path_b);

   assert(ctx_a->count == 1);
   assert(ctx_b->count == 1);

   /* Overwrite both files */
   FILE *f = fopen(path_a, "w");
   fputs("modified_a", f);
   fclose(f);
   f = fopen(path_b, "w");
   fputs("modified_b", f);
   fclose(f);

   /* Rollback only context A */
   checkpoint_ctx_rollback_all(ctx_a);

   /* Verify A is restored, B is still modified */
   char buf[64];
   f = fopen(path_a, "r");
   assert(f != NULL);
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   assert(strcmp(buf, "original_a") == 0);

   f = fopen(path_b, "r");
   assert(f != NULL);
   n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   assert(strcmp(buf, "modified_b") == 0);

   /* Cleanup */
   checkpoint_ctx_free(ctx_a);
   checkpoint_ctx_free(ctx_b);
   unlink(path_a);
   unlink(path_b);
}

static void test_checkpoint_ctx_new_file(void)
{
   /* Checkpoint a non-existent file, create it, then rollback should remove it */
   checkpoint_ctx_t *ctx = checkpoint_ctx_new();
   assert(ctx != NULL);

   char path[] = "/tmp/aimee_ckpt_new_XXXXXX";
   int fd = mkstemp(path);
   close(fd);
   unlink(path); /* ensure it doesn't exist */

   checkpoint_ctx_push(ctx, path);

   /* Create the file */
   FILE *f = fopen(path, "w");
   assert(f != NULL);
   fputs("new content", f);
   fclose(f);

   /* File exists now */
   struct stat st;
   assert(stat(path, &st) == 0);

   /* Rollback should remove it */
   checkpoint_ctx_rollback_all(ctx);
   assert(stat(path, &st) != 0); /* file should be gone */

   checkpoint_ctx_free(ctx);
}

static void test_dispatch_ctx_checkpoint(void)
{
   /* dispatch_tool_call_ctx with a checkpoint context should capture before write */
   checkpoint_ctx_t *ctx = checkpoint_ctx_new();
   assert(ctx != NULL);

   char path[] = "/tmp/aimee_dispatch_ckpt_XXXXXX";
   int fd = mkstemp(path);
   if (write(fd, "before", 6) < 0)
   { /* ignore */
   }
   close(fd);

   /* Write via dispatch_tool_call_ctx */
   char args[512];
   snprintf(args, sizeof(args), "{\"path\":\"%s\",\"content\":\"after\"}", path);
   char *result = dispatch_tool_call_ctx("write_file", args, 5000, ctx);
   assert(result != NULL);
   assert(strcmp(result, "ok") == 0);
   free(result);

   /* Checkpoint should have been captured */
   assert(ctx->count == 1);

   /* Rollback should restore original */
   checkpoint_ctx_rollback_all(ctx);

   char buf[64];
   FILE *f = fopen(path, "r");
   assert(f != NULL);
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);
   assert(strcmp(buf, "before") == 0);

   checkpoint_ctx_free(ctx);
   unlink(path);
}

/* --- messages_compact_consecutive tests --- */

static cJSON *make_msg(const char *role, const char *content)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", role);
   cJSON_AddStringToObject(msg, "content", content);
   return msg;
}

static void test_compact_empty(void)
{
   /* NULL and empty array */
   assert(messages_compact_consecutive(NULL) == 0);

   cJSON *arr = cJSON_CreateArray();
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 0);
   cJSON_Delete(arr);
}

static void test_compact_single(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 1);
   cJSON_Delete(arr);
}

static void test_compact_two_same_role(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   cJSON_AddItemToArray(arr, make_msg("user", "world"));
   assert(messages_compact_consecutive(arr) == 1);
   assert(cJSON_GetArraySize(arr) == 1);

   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(content, "hello\n\nworld") == 0);
   cJSON_Delete(arr);
}

static void test_compact_five_same_role(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "a"));
   cJSON_AddItemToArray(arr, make_msg("user", "b"));
   cJSON_AddItemToArray(arr, make_msg("user", "c"));
   cJSON_AddItemToArray(arr, make_msg("user", "d"));
   cJSON_AddItemToArray(arr, make_msg("user", "e"));
   assert(messages_compact_consecutive(arr) == 4);
   assert(cJSON_GetArraySize(arr) == 1);

   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(content, "a\n\nb\n\nc\n\nd\n\ne") == 0);
   cJSON_Delete(arr);
}

static void test_compact_mixed_roles(void)
{
   /* user-user-assistant-user-user */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "u1"));
   cJSON_AddItemToArray(arr, make_msg("user", "u2"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "a1"));
   cJSON_AddItemToArray(arr, make_msg("user", "u3"));
   cJSON_AddItemToArray(arr, make_msg("user", "u4"));
   assert(messages_compact_consecutive(arr) == 2);
   assert(cJSON_GetArraySize(arr) == 3);

   const char *c0 =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(c0, "u1\n\nu2") == 0);

   const char *r1 = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 1), "role"));
   assert(strcmp(r1, "assistant") == 0);

   const char *c2 =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 2), "content"));
   assert(strcmp(c2, "u3\n\nu4") == 0);
   cJSON_Delete(arr);
}

static void test_compact_no_consecutive(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "u1"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "a1"));
   cJSON_AddItemToArray(arr, make_msg("user", "u2"));
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON_Delete(arr);
}

static void test_compact_idempotent(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "a"));
   cJSON_AddItemToArray(arr, make_msg("user", "b"));
   messages_compact_consecutive(arr);
   assert(cJSON_GetArraySize(arr) == 1);

   /* Second call should be a no-op */
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 1);
   cJSON_Delete(arr);
}

static void test_compact_skips_structured_content(void)
{
   /* Messages with tool_calls or non-string content should not be merged */
   cJSON *arr = cJSON_CreateArray();
   cJSON *msg1 = cJSON_CreateObject();
   cJSON_AddStringToObject(msg1, "role", "assistant");
   cJSON_AddNullToObject(msg1, "content");
   cJSON_AddItemToArray(arr, msg1);

   cJSON_AddItemToArray(arr, make_msg("assistant", "text response"));

   /* These should not be merged because msg1 has null content */
   assert(messages_compact_consecutive(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 2);
   cJSON_Delete(arr);
}

static void test_compact_system_role(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("system", "rule1"));
   cJSON_AddItemToArray(arr, make_msg("system", "rule2"));
   assert(messages_compact_consecutive(arr) == 1);
   assert(cJSON_GetArraySize(arr) == 1);

   const char *content =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "content"));
   assert(strcmp(content, "rule1\n\nrule2") == 0);
   cJSON_Delete(arr);
}

/* --- message_history_repair tests --- */

/* Helper: make an OpenAI assistant message with tool_calls */
static cJSON *make_assistant_with_tools_openai(const char **ids, const char **names, int count)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON_AddNullToObject(msg, "content");
   cJSON *tcs = cJSON_AddArrayToObject(msg, "tool_calls");
   for (int i = 0; i < count; i++)
   {
      cJSON *tc = cJSON_CreateObject();
      cJSON_AddStringToObject(tc, "id", ids[i]);
      cJSON_AddStringToObject(tc, "type", "function");
      cJSON *fn = cJSON_AddObjectToObject(tc, "function");
      cJSON_AddStringToObject(fn, "name", names[i]);
      cJSON_AddStringToObject(fn, "arguments", "{}");
      cJSON_AddItemToArray(tcs, tc);
   }
   return msg;
}

/* Helper: make an OpenAI tool result message */
static cJSON *make_tool_result_openai(const char *tool_call_id, const char *content)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "tool");
   cJSON_AddStringToObject(msg, "tool_call_id", tool_call_id);
   cJSON_AddStringToObject(msg, "content", content);
   return msg;
}

/* Helper: make an Anthropic assistant message with tool_use blocks */
static cJSON *make_assistant_with_tools_anthropic(const char **ids, const char **names, int count)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "assistant");
   cJSON *content = cJSON_AddArrayToObject(msg, "content");
   for (int i = 0; i < count; i++)
   {
      cJSON *block = cJSON_CreateObject();
      cJSON_AddStringToObject(block, "type", "tool_use");
      cJSON_AddStringToObject(block, "id", ids[i]);
      cJSON_AddStringToObject(block, "name", names[i]);
      cJSON_AddItemToObject(block, "input", cJSON_CreateObject());
      cJSON_AddItemToArray(content, block);
   }
   return msg;
}

/* Helper: make an Anthropic user message with tool_result blocks */
static cJSON *make_tool_results_anthropic(const char **ids, const char **contents, int count)
{
   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "role", "user");
   cJSON *content = cJSON_AddArrayToObject(msg, "content");
   for (int i = 0; i < count; i++)
   {
      cJSON *tr = cJSON_CreateObject();
      cJSON_AddStringToObject(tr, "type", "tool_result");
      cJSON_AddStringToObject(tr, "tool_use_id", ids[i]);
      cJSON_AddStringToObject(tr, "content", contents[i]);
      cJSON_AddItemToArray(content, tr);
   }
   return msg;
}

static void test_repair_empty(void)
{
   assert(message_history_repair(NULL) == 0);
   cJSON *arr = cJSON_CreateArray();
   assert(message_history_repair(arr) == 0);
   cJSON_Delete(arr);
}

static void test_repair_no_tools(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("system", "You are helpful."));
   cJSON_AddItemToArray(arr, make_msg("user", "Hello"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "Hi there!"));
   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON_Delete(arr);
}

static void test_repair_consistent_openai(void)
{
   /* Complete tool call cycle — should not be modified */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"call_1"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 1));
   cJSON_AddItemToArray(arr, make_tool_result_openai("call_1", "file1.txt\nfile2.txt"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "Found 2 files."));

   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 4);
   cJSON_Delete(arr);
}

static void test_repair_orphaned_call_openai(void)
{
   /* Assistant made a tool call but result is missing (simulating crash mid-execution) */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"call_orphan"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 1));
   /* No tool result! */

   int repairs = message_history_repair(arr);
   assert(repairs == 1);
   assert(cJSON_GetArraySize(arr) == 3);

   /* Verify the synthetic result was inserted */
   cJSON *last = cJSON_GetArrayItem(arr, 2);
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(last, "role"));
   assert(strcmp(role, "tool") == 0);
   const char *tcid = cJSON_GetStringValue(cJSON_GetObjectItem(last, "tool_call_id"));
   assert(strcmp(tcid, "call_orphan") == 0);
   const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(last, "content"));
   assert(strstr(content, "cancelled") != NULL);

   cJSON_Delete(arr);
}

static void test_repair_orphaned_result_openai(void)
{
   /* Tool result exists but no matching call (state corruption) */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   cJSON_AddItemToArray(arr, make_tool_result_openai("call_ghost", "some result"));
   cJSON_AddItemToArray(arr, make_msg("assistant", "done"));

   int repairs = message_history_repair(arr);
   assert(repairs == 1);
   assert(cJSON_GetArraySize(arr) == 2); /* orphaned result removed */

   cJSON_Delete(arr);
}

static void test_repair_multiple_orphans_openai(void)
{
   /* Two tool calls, only one has a result */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "do stuff"));
   const char *ids[] = {"call_a", "call_b"};
   const char *names[] = {"bash", "read_file"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 2));
   cJSON_AddItemToArray(arr, make_tool_result_openai("call_a", "result_a"));
   /* call_b has no result */

   int repairs = message_history_repair(arr);
   assert(repairs == 1);

   /* Should now have: user, assistant, tool(call_a), tool(call_b synthetic) */
   int found_b = 0;
   cJSON *m;
   cJSON_ArrayForEach(m, arr)
   {
      const char *tcid = cJSON_GetStringValue(cJSON_GetObjectItem(m, "tool_call_id"));
      if (tcid && strcmp(tcid, "call_b") == 0)
      {
         found_b = 1;
         const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(m, "content"));
         assert(strstr(c, "cancelled") != NULL);
      }
   }
   assert(found_b == 1);
   cJSON_Delete(arr);
}

static void test_repair_orphaned_call_anthropic(void)
{
   /* Anthropic format: tool_use block with no matching tool_result */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"toolu_orphan"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_anthropic(ids, names, 1));
   /* No user message with tool_result */

   int repairs = message_history_repair(arr);
   assert(repairs == 1);

   /* Should have added a user message with tool_result */
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON *last = cJSON_GetArrayItem(arr, 2);
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(last, "role"));
   assert(strcmp(role, "user") == 0);

   cJSON *content = cJSON_GetObjectItem(last, "content");
   assert(cJSON_IsArray(content));
   cJSON *tr = cJSON_GetArrayItem(content, 0);
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "type"));
   assert(strcmp(type, "tool_result") == 0);
   const char *tuid = cJSON_GetStringValue(cJSON_GetObjectItem(tr, "tool_use_id"));
   assert(strcmp(tuid, "toolu_orphan") == 0);

   cJSON_Delete(arr);
}

static void test_repair_consistent_anthropic(void)
{
   /* Complete Anthropic tool cycle — should not be modified */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "list files"));
   const char *ids[] = {"toolu_1"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_anthropic(ids, names, 1));
   const char *rids[] = {"toolu_1"};
   const char *rcontents[] = {"file1.txt"};
   cJSON_AddItemToArray(arr, make_tool_results_anthropic(rids, rcontents, 1));

   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == 3);
   cJSON_Delete(arr);
}

static void test_repair_orphaned_result_anthropic(void)
{
   /* Anthropic: tool_result with no matching tool_use */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "hello"));
   /* Add an assistant with a tool_use to establish Anthropic format detection */
   const char *ids[] = {"toolu_real"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_anthropic(ids, names, 1));
   /* User message with both a valid and an orphaned tool_result */
   const char *rids[] = {"toolu_real", "toolu_ghost"};
   const char *rcontents[] = {"ok", "orphan"};
   cJSON_AddItemToArray(arr, make_tool_results_anthropic(rids, rcontents, 2));

   int repairs = message_history_repair(arr);
   assert(repairs == 1); /* orphaned toolu_ghost removed */

   /* The user message should still exist with one tool_result */
   cJSON *user_msg = cJSON_GetArrayItem(arr, 2);
   cJSON *content = cJSON_GetObjectItem(user_msg, "content");
   assert(cJSON_GetArraySize(content) == 1);
   const char *tuid =
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(content, 0), "tool_use_id"));
   assert(strcmp(tuid, "toolu_real") == 0);

   cJSON_Delete(arr);
}

static void test_repair_responses_api(void)
{
   /* Responses API: function_call with no function_call_output */
   cJSON *arr = cJSON_CreateArray();

   cJSON *fc = cJSON_CreateObject();
   cJSON_AddStringToObject(fc, "type", "function_call");
   cJSON_AddStringToObject(fc, "call_id", "fc_orphan");
   cJSON_AddStringToObject(fc, "name", "bash");
   cJSON_AddStringToObject(fc, "arguments", "{}");
   cJSON_AddItemToArray(arr, fc);

   int repairs = message_history_repair(arr);
   assert(repairs == 1);
   assert(cJSON_GetArraySize(arr) == 2);

   cJSON *output = cJSON_GetArrayItem(arr, 1);
   const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(output, "type"));
   assert(strcmp(type, "function_call_output") == 0);
   const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(output, "call_id"));
   assert(strcmp(cid, "fc_orphan") == 0);

   cJSON_Delete(arr);
}

static void test_repair_idempotent(void)
{
   /* Repair should be idempotent — running twice gives same result */
   cJSON *arr = cJSON_CreateArray();
   cJSON_AddItemToArray(arr, make_msg("user", "do stuff"));
   const char *ids[] = {"call_x"};
   const char *names[] = {"bash"};
   cJSON_AddItemToArray(arr, make_assistant_with_tools_openai(ids, names, 1));

   assert(message_history_repair(arr) == 1);
   int size_after_first = cJSON_GetArraySize(arr);

   assert(message_history_repair(arr) == 0);
   assert(cJSON_GetArraySize(arr) == size_after_first);

   cJSON_Delete(arr);
}

int main(void)
{
   test_agent_expand_env();
   test_agent_has_role();
   test_agent_find();
   test_agent_route();
   test_agent_is_exec_role();
   test_tool_bash();
   test_tool_read_file();
   test_tool_write_file();
   test_tool_list_files();
   test_dispatch_tool_call();
   test_parse_openai_tool_calls();
   test_path_traversal_rejected();
   test_sensitive_path_rejected();
   test_symlink_escape_rejected();
   test_checkpoint_ctx_isolation();
   test_checkpoint_ctx_new_file();
   test_dispatch_ctx_checkpoint();
   test_compact_empty();
   test_compact_single();
   test_compact_two_same_role();
   test_compact_five_same_role();
   test_compact_mixed_roles();
   test_compact_no_consecutive();
   test_compact_idempotent();
   test_compact_skips_structured_content();
   test_compact_system_role();
   test_repair_empty();
   test_repair_no_tools();
   test_repair_consistent_openai();
   test_repair_orphaned_call_openai();
   test_repair_orphaned_result_openai();
   test_repair_multiple_orphans_openai();
   test_repair_orphaned_call_anthropic();
   test_repair_consistent_anthropic();
   test_repair_orphaned_result_anthropic();
   test_repair_responses_api();
   test_repair_idempotent();
   printf("agent: all tests passed\n");
   return 0;
}
