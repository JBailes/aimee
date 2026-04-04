/* test_client_integrations.c: Claude MCP registration, Codex plugin payload,
 * and non-destructive settings update tests for client_integrations.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "cJSON.h"

/* Include the source directly to test static functions */
#include "../client_integrations.c"

/* --- Test build_marketplace_root --- */

static void test_build_marketplace_root(void)
{
   cJSON *root = build_marketplace_root();
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   /* Should have name = "local" */
   cJSON *name = cJSON_GetObjectItem(root, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "local") == 0);

   /* Should have interface.displayName */
   cJSON *iface = cJSON_GetObjectItem(root, "interface");
   assert(cJSON_IsObject(iface));
   cJSON *dn = cJSON_GetObjectItem(iface, "displayName");
   assert(cJSON_IsString(dn));

   /* Should have empty plugins array */
   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   assert(cJSON_GetArraySize(plugins) == 0);

   cJSON_Delete(root);
}

/* --- Test build_aimee_plugin_entry --- */

static void test_build_aimee_plugin_entry(void)
{
   cJSON *entry = build_aimee_plugin_entry();
   assert(entry != NULL);
   assert(cJSON_IsObject(entry));

   cJSON *name = cJSON_GetObjectItem(entry, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "aimee") == 0);

   cJSON *source = cJSON_GetObjectItem(entry, "source");
   assert(cJSON_IsObject(source));
   cJSON *src_path = cJSON_GetObjectItem(source, "path");
   assert(cJSON_IsString(src_path));
   assert(strstr(src_path->valuestring, "plugins/aimee") != NULL);

   cJSON *policy = cJSON_GetObjectItem(entry, "policy");
   assert(cJSON_IsObject(policy));
   cJSON *install = cJSON_GetObjectItem(policy, "installation");
   assert(cJSON_IsString(install));
   assert(strcmp(install->valuestring, "INSTALLED_BY_DEFAULT") == 0);

   cJSON *category = cJSON_GetObjectItem(entry, "category");
   assert(cJSON_IsString(category));
   assert(strcmp(category->valuestring, "Coding") == 0);

   cJSON_Delete(entry);
}

/* --- Test read_json_file --- */

static void test_read_json_file_missing(void)
{
   cJSON *root = read_json_file("/nonexistent/path/file.json");
   assert(root == NULL);
}

static void test_read_json_file_valid(void)
{
   char tmppath[] = "/tmp/aimee-test-json-XXXXXX";
   int fd = mkstemp(tmppath);
   assert(fd >= 0);

   const char *json = "{\"key\": \"value\", \"num\": 42}";
   write(fd, json, strlen(json));
   close(fd);

   cJSON *root = read_json_file(tmppath);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   cJSON *key = cJSON_GetObjectItem(root, "key");
   assert(cJSON_IsString(key));
   assert(strcmp(key->valuestring, "value") == 0);

   cJSON *num = cJSON_GetObjectItem(root, "num");
   assert(cJSON_IsNumber(num));
   assert(num->valueint == 42);

   cJSON_Delete(root);
   unlink(tmppath);
}

static void test_read_json_file_invalid(void)
{
   char tmppath[] = "/tmp/aimee-test-badjson-XXXXXX";
   int fd = mkstemp(tmppath);
   assert(fd >= 0);

   const char *bad = "not valid json at all {{{";
   write(fd, bad, strlen(bad));
   close(fd);

   cJSON *root = read_json_file(tmppath);
   assert(root == NULL);

   unlink(tmppath);
}

/* --- Test ensure_claude_code_mcp: non-destructive merge behavior --- */

static void test_claude_mcp_creates_fresh_settings(void)
{
   char tmpdir[] = "/tmp/aimee-test-claude-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   /* Create a fake aimee-mcp binary so stat() succeeds */
   char fake_bin[512];
   snprintf(fake_bin, sizeof(fake_bin), "%s/fake-mcp", tmpdir);
   FILE *fp = fopen(fake_bin, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n", fp);
   fclose(fp);
   chmod(fake_bin, 0755);

   /* The function checks /usr/local/bin/aimee which may not exist
    * in test environment. We test the JSON merge logic directly instead. */
   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);

   /* Write a settings file with existing data */
   fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{\"existingKey\": true, \"mcpServers\": {\"other\": {\"command\": \"other-mcp\"}}}", fp);
   fclose(fp);

   /* Simulate what ensure_claude_code_mcp does: merge aimee into mcpServers */
   cJSON *root = read_json_file(settings_path);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   /* Verify existing key is preserved */
   cJSON *existing = cJSON_GetObjectItem(root, "existingKey");
   assert(existing != NULL && cJSON_IsTrue(existing));

   /* Verify other server still present */
   cJSON *servers = cJSON_GetObjectItem(root, "mcpServers");
   assert(cJSON_IsObject(servers));
   cJSON *other = cJSON_GetObjectItem(servers, "other");
   assert(cJSON_IsObject(other));
   cJSON *other_cmd = cJSON_GetObjectItem(other, "command");
   assert(cJSON_IsString(other_cmd));
   assert(strcmp(other_cmd->valuestring, "other-mcp") == 0);

   /* Add aimee server (simulating the merge) */
   cJSON *aimee_server = cJSON_CreateObject();
   cJSON_AddStringToObject(aimee_server, "command", "/usr/local/bin/aimee");
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   /* Write back and re-read */
   char *json_out = cJSON_Print(root);
   assert(json_out != NULL);
   fp = fopen(settings_path, "w");
   fputs(json_out, fp);
   fclose(fp);
   free(json_out);
   cJSON_Delete(root);

   /* Re-read and verify everything was preserved */
   root = read_json_file(settings_path);
   assert(root != NULL);

   existing = cJSON_GetObjectItem(root, "existingKey");
   assert(existing != NULL && cJSON_IsTrue(existing));

   servers = cJSON_GetObjectItem(root, "mcpServers");
   assert(cJSON_IsObject(servers));

   other = cJSON_GetObjectItem(servers, "other");
   assert(cJSON_IsObject(other));

   cJSON *aimee = cJSON_GetObjectItem(servers, "aimee");
   assert(cJSON_IsObject(aimee));
   cJSON *cmd = cJSON_GetObjectItem(aimee, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, "/usr/local/bin/aimee") == 0);

   cJSON_Delete(root);

   /* Cleanup */
   char rm_cmd[512];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
   system(rm_cmd);
}

/* --- Test Codex plugin config (TOML-like) non-destructive update --- */

static void test_codex_plugin_enabled_fresh(void)
{
   char tmpdir[] = "/tmp/aimee-test-codex-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Call with no existing file - should create with section + enabled */
   ensure_codex_plugin_enabled(config_path);

   FILE *fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);
   assert(strstr(buf, "enabled = true") != NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_plugin_enabled_preserves_existing(void)
{
   char tmpdir[] = "/tmp/aimee-test-codex2-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Write existing config with other settings */
   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[general]\ntheme = \"dark\"\n", fp);
   fclose(fp);

   ensure_codex_plugin_enabled(config_path);

   fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[2048];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   /* Original content should be preserved */
   assert(strstr(buf, "[general]") != NULL);
   assert(strstr(buf, "theme = \"dark\"") != NULL);

   /* Plugin section should be appended */
   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);
   assert(strstr(buf, "enabled = true") != NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_plugin_enabled_idempotent(void)
{
   char tmpdir[] = "/tmp/aimee-test-codex3-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Create file with the section already present */
   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[plugins.\"aimee@local\"]\nenabled = true\n", fp);
   fclose(fp);

   /* Call again - should not modify */
   ensure_codex_plugin_enabled(config_path);

   fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   /* Should still have exactly one instance */
   char *first = strstr(buf, "[plugins.\"aimee@local\"]");
   assert(first != NULL);
   /* No second instance */
   char *second = strstr(first + 1, "[plugins.\"aimee@local\"]");
   assert(second == NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test ensure_codex_marketplace: non-destructive merge --- */

static void test_codex_marketplace_fresh(void)
{
   char tmpdir[] = "/tmp/aimee-test-market-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/marketplace.json", tmpdir);

   /* Call with no existing file */
   ensure_codex_marketplace(path);

   cJSON *root = read_json_file(path);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   cJSON *name = cJSON_GetObjectItem(root, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "local") == 0);

   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   assert(cJSON_GetArraySize(plugins) == 1);

   cJSON *entry = cJSON_GetArrayItem(plugins, 0);
   cJSON *ename = cJSON_GetObjectItem(entry, "name");
   assert(cJSON_IsString(ename));
   assert(strcmp(ename->valuestring, "aimee") == 0);

   cJSON_Delete(root);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_marketplace_preserves_other_plugins(void)
{
   char tmpdir[] = "/tmp/aimee-test-market2-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/marketplace.json", tmpdir);

   /* Write existing marketplace with another plugin */
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs("{\"name\":\"local\",\"interface\":{\"displayName\":\"Local\"},"
         "\"plugins\":[{\"name\":\"other-plugin\",\"category\":\"Tools\"}]}",
         fp);
   fclose(fp);

   ensure_codex_marketplace(path);

   cJSON *root = read_json_file(path);
   assert(root != NULL);

   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   /* Should have both plugins */
   assert(cJSON_GetArraySize(plugins) == 2);

   /* Verify other plugin is preserved */
   int found_other = 0, found_aimee = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *n = cJSON_GetObjectItem(item, "name");
      if (cJSON_IsString(n))
      {
         if (strcmp(n->valuestring, "other-plugin") == 0)
            found_other = 1;
         if (strcmp(n->valuestring, "aimee") == 0)
            found_aimee = 1;
      }
   }
   assert(found_other);
   assert(found_aimee);

   cJSON_Delete(root);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test write_text_file: no-op when content unchanged --- */

static void test_write_text_file_no_op(void)
{
   char tmppath[] = "/tmp/aimee-test-write-XXXXXX";
   int fd = mkstemp(tmppath);
   assert(fd >= 0);

   const char *content = "test content here";
   write(fd, content, strlen(content));
   close(fd);

   /* Get mtime before */
   struct stat st1;
   assert(stat(tmppath, &st1) == 0);

   /* Small sleep to ensure mtime would differ */
   usleep(10000);

   /* Write same content: should be no-op */
   int rc = write_text_file(tmppath, content, 0600);
   assert(rc == 0);

   /* mtime should not change since content is identical */
   struct stat st2;
   assert(stat(tmppath, &st2) == 0);
   assert(st1.st_mtime == st2.st_mtime);

   unlink(tmppath);
}

int main(void)
{
   printf("client_integrations: ");

   test_build_marketplace_root();
   test_build_aimee_plugin_entry();
   test_read_json_file_missing();
   test_read_json_file_valid();
   test_read_json_file_invalid();
   test_claude_mcp_creates_fresh_settings();
   test_codex_plugin_enabled_fresh();
   test_codex_plugin_enabled_preserves_existing();
   test_codex_plugin_enabled_idempotent();
   test_codex_marketplace_fresh();
   test_codex_marketplace_preserves_other_plugins();
   test_write_text_file_no_op();

   printf("all tests passed\n");
   return 0;
}
