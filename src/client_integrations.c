#include "aimee.h"
#include "cJSON.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static void ensure_parent_dir(const char *path, mode_t mode)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", path);
   char *last_slash = strrchr(dir, '/');
   if (!last_slash)
      return;
   *last_slash = '\0';
   for (char *p = dir + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(dir, mode);
         *p = '/';
      }
   }
   mkdir(dir, mode);
}

static int write_text_file(const char *path, const char *content, mode_t mode)
{
   FILE *fp = fopen(path, "r");
   if (fp)
   {
      char buf[16384];
      size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
      fclose(fp);
      buf[n] = '\0';
      if (strcmp(buf, content) == 0)
         return 0;
   }

   ensure_parent_dir(path, 0700);

   /* Atomic write: write to a temp file then rename into place so readers
    * never see a truncated/empty file (avoids race with Claude Code reading
    * settings.json while we rewrite it). */
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
   fp = fopen(tmp, "w");
   if (!fp)
      return -1;
   fputs(content, fp);
   if (fclose(fp) != 0)
   {
      unlink(tmp);
      return -1;
   }
   chmod(tmp, mode);
   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static cJSON *build_marketplace_root(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "name", "local");
   cJSON *iface = cJSON_AddObjectToObject(root, "interface");
   cJSON_AddStringToObject(iface, "displayName", "Local Plugins");
   cJSON_AddItemToObject(root, "plugins", cJSON_CreateArray());
   return root;
}

static cJSON *build_aimee_plugin_entry(void)
{
   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "name", "aimee");
   cJSON *source = cJSON_AddObjectToObject(entry, "source");
   cJSON_AddStringToObject(source, "source", "local");
   cJSON_AddStringToObject(source, "path", "./plugins/aimee");
   cJSON *policy = cJSON_AddObjectToObject(entry, "policy");
   cJSON_AddStringToObject(policy, "installation", "INSTALLED_BY_DEFAULT");
   cJSON_AddStringToObject(policy, "authentication", "ON_USE");
   cJSON_AddStringToObject(entry, "category", "Coding");
   return entry;
}

static void ensure_codex_marketplace(const char *path)
{
   cJSON *root = NULL;
   FILE *fp = fopen(path, "r");
   if (fp)
   {
      fseek(fp, 0, SEEK_END);
      long sz = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (sz >= 0 && sz < (long)(1 << 20))
      {
         char *buf = malloc((size_t)sz + 1);
         if (buf)
         {
            if (fread(buf, 1, (size_t)sz, fp) == (size_t)sz)
            {
               buf[sz] = '\0';
               root = cJSON_Parse(buf);
            }
            free(buf);
         }
      }
      fclose(fp);
   }

   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = build_marketplace_root();
   }

   cJSON *plugins = cJSON_GetObjectItemCaseSensitive(root, "plugins");
   if (!cJSON_IsArray(plugins))
   {
      if (plugins)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "plugins");
      plugins = cJSON_CreateArray();
      cJSON_AddItemToObject(root, "plugins", plugins);
   }

   int replaced = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, "aimee") == 0)
      {
         cJSON *entry = build_aimee_plugin_entry();
         cJSON_ReplaceItemViaPointer(plugins, item, entry);
         replaced = 1;
         break;
      }
   }
   if (!replaced)
      cJSON_AddItemToArray(plugins, build_aimee_plugin_entry());

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_codex_plugin_enabled(const char *path)
{
   const char *section = "[plugins.\"aimee@local\"]";
   const char *enabled_true = "enabled = true";

   FILE *fp = fopen(path, "r");
   if (!fp)
   {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s\n%s\n", section, enabled_true);
      write_text_file(path, buf, 0600);
      return;
   }

   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';

   char *section_pos = strstr(buf, section);
   if (!section_pos)
   {
      size_t len = strlen(buf);
      int needs_nl = (len > 0 && buf[len - 1] != '\n');
      size_t extra = (needs_nl ? 1u : 0u) + (len > 0 ? 1u : 0u) + strlen(section) + 1u +
                     strlen(enabled_true) + 1u;
      char *out = malloc(len + extra + 1u);
      if (out)
      {
         size_t pos = 0;
         memcpy(out + pos, buf, len);
         pos += len;
         if (needs_nl)
            out[pos++] = '\n';
         if (len > 0)
            out[pos++] = '\n';
         memcpy(out + pos, section, strlen(section));
         pos += strlen(section);
         out[pos++] = '\n';
         memcpy(out + pos, enabled_true, strlen(enabled_true));
         pos += strlen(enabled_true);
         out[pos++] = '\n';
         out[pos] = '\0';
         write_text_file(path, out, 0600);
         free(out);
      }
      free(buf);
      return;
   }

   char *next_section = strstr(section_pos + strlen(section), "\n[");
   size_t section_len = next_section ? (size_t)(next_section - section_pos) : strlen(section_pos);
   char *enabled_pos = strstr(section_pos, enabled_true);
   if (enabled_pos && (size_t)(enabled_pos - section_pos) < section_len)
   {
      free(buf);
      return;
   }

   char *enabled_false = strstr(section_pos, "enabled = false");
   if (enabled_false && (size_t)(enabled_false - section_pos) < section_len)
   {
      size_t prefix_len = (size_t)(enabled_false - buf);
      size_t suffix_off = prefix_len + strlen("enabled = false");
      size_t suffix_len = strlen(buf + suffix_off);
      char *out = malloc(prefix_len + strlen(enabled_true) + suffix_len + 1u);
      if (out)
      {
         memcpy(out, buf, prefix_len);
         memcpy(out + prefix_len, enabled_true, strlen(enabled_true));
         memcpy(out + prefix_len + strlen(enabled_true), buf + suffix_off, suffix_len + 1u);
         write_text_file(path, out, 0600);
         free(out);
      }
      free(buf);
      return;
   }

   char *line_end = strchr(section_pos, '\n');
   if (!line_end)
   {
      free(buf);
      return;
   }

   size_t insert_off = (size_t)(line_end - buf) + 1u;
   size_t len = strlen(buf);
   size_t insert_len = strlen(enabled_true) + 1u;
   char *out = malloc(len + insert_len + 1u);
   if (out)
   {
      memcpy(out, buf, insert_off);
      memcpy(out + insert_off, enabled_true, strlen(enabled_true));
      out[insert_off + strlen(enabled_true)] = '\n';
      memcpy(out + insert_off + insert_len, buf + insert_off, len - insert_off + 1u);
      write_text_file(path, out, 0600);
      free(out);
   }
   free(buf);
}

static void ensure_codex_plugin_files(const char *home)
{
   const char *aimee_bin = "/usr/local/bin/aimee";
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char plugin_json[MAX_PATH_LEN];
   char marketplace_plugin_json[MAX_PATH_LEN];
   char installed_plugin_json[MAX_PATH_LEN];
   char mcp_json[MAX_PATH_LEN];
   char marketplace_mcp_json[MAX_PATH_LEN];
   char installed_mcp_json[MAX_PATH_LEN];
   char compat_plugin_json[MAX_PATH_LEN];
   char marketplace_compat_plugin_json[MAX_PATH_LEN];
   char installed_compat_plugin_json[MAX_PATH_LEN];
   char skill_md[MAX_PATH_LEN];
   char marketplace_skill_md[MAX_PATH_LEN];
   char installed_skill_md[MAX_PATH_LEN];
   char compat_mcp_json[MAX_PATH_LEN];
   char marketplace_compat_mcp_json[MAX_PATH_LEN];
   char installed_compat_mcp_json[MAX_PATH_LEN];
   char marketplace[MAX_PATH_LEN];
   char config_toml[MAX_PATH_LEN];
   snprintf(plugin_json, sizeof(plugin_json), "%s/plugins/aimee/.codex-plugin/plugin.json", home);
   snprintf(marketplace_plugin_json, sizeof(marketplace_plugin_json),
            "%s/.agents/plugins/plugins/aimee/.codex-plugin/plugin.json", home);
   snprintf(installed_plugin_json, sizeof(installed_plugin_json),
            "%s/.codex/plugins/cache/local/aimee/.codex-plugin/plugin.json", home);
   snprintf(mcp_json, sizeof(mcp_json), "%s/plugins/aimee/.mcp.json", home);
   snprintf(marketplace_mcp_json, sizeof(marketplace_mcp_json),
            "%s/.agents/plugins/plugins/aimee/.mcp.json", home);
   snprintf(installed_mcp_json, sizeof(installed_mcp_json),
            "%s/.codex/plugins/cache/local/aimee/.mcp.json", home);
   snprintf(compat_plugin_json, sizeof(compat_plugin_json),
            "%s/plugins/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(marketplace_compat_plugin_json, sizeof(marketplace_compat_plugin_json),
            "%s/.agents/plugins/plugins/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(installed_compat_plugin_json, sizeof(installed_compat_plugin_json),
            "%s/.codex/plugins/cache/local/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(skill_md, sizeof(skill_md), "%s/plugins/aimee/skills/aimee/SKILL.md", home);
   snprintf(marketplace_skill_md, sizeof(marketplace_skill_md),
            "%s/.agents/plugins/plugins/aimee/skills/aimee/SKILL.md", home);
   snprintf(installed_skill_md, sizeof(installed_skill_md),
            "%s/.codex/plugins/cache/local/aimee/skills/aimee/SKILL.md", home);
   snprintf(compat_mcp_json, sizeof(compat_mcp_json), "%s/plugins/aimee/skills/.mcp.json", home);
   snprintf(marketplace_compat_mcp_json, sizeof(marketplace_compat_mcp_json),
            "%s/.agents/plugins/plugins/aimee/skills/.mcp.json", home);
   snprintf(installed_compat_mcp_json, sizeof(installed_compat_mcp_json),
            "%s/.codex/plugins/cache/local/aimee/skills/.mcp.json", home);
   snprintf(marketplace, sizeof(marketplace), "%s/.agents/plugins/marketplace.json", home);
   snprintf(config_toml, sizeof(config_toml), "%s/.codex/config.toml", home);

   char plugin_buf[4096];
   snprintf(plugin_buf, sizeof(plugin_buf),
            "{\n"
            "  \"name\": \"aimee\",\n"
            "  \"version\": \"%s\",\n"
            "  \"description\": "
            "\"Persistent memory, code search, blast-radius preview, and delegation "
            "for local coding sessions.\",\n"
            "  \"author\": {\n"
            "    \"name\": \"aimee\",\n"
            "    \"email\": \"support@example.invalid\",\n"
            "    \"url\": \"https://github.com/JBailes/aimee\"\n"
            "  },\n"
            "  \"homepage\": \"https://github.com/JBailes/aimee\",\n"
            "  \"repository\": \"https://github.com/JBailes/aimee\",\n"
            "  \"license\": \"MIT\",\n"
            "  \"keywords\": [\"memory\", \"mcp\", \"coding\", \"search\", \"delegation\"],\n"
            "  \"skills\": \"./skills/\",\n"
            "  \"mcpServers\": \"./.mcp.json\",\n"
            "  \"interface\": {\n"
            "    \"displayName\": \"aimee\",\n"
            "    \"shortDescription\": \"Memory, search, and delegation for Codex\",\n"
            "    \"longDescription\": "
            "\"Expose aimee's MCP server to Codex so sessions can search memory, "
            "inspect indexed code, preview blast radius, and delegate sub-tasks "
            "through the same local backend.\",\n"
            "    \"developerName\": \"aimee\",\n"
            "    \"category\": \"Coding\",\n"
            "    \"capabilities\": [\"Interactive\", \"Write\"],\n"
            "    \"websiteURL\": \"https://github.com/JBailes/aimee\",\n"
            "    \"privacyPolicyURL\": \"https://github.com/JBailes/aimee\",\n"
            "    \"termsOfServiceURL\": \"https://github.com/JBailes/aimee\",\n"
            "    \"defaultPrompt\": [\n"
            "      \"Search aimee memory before answering repo-specific questions\",\n"
            "      \"Preview the blast radius before editing multiple files\",\n"
            "      \"Delegate a bounded task when local context is not enough\"\n"
            "    ],\n"
            "    \"brandColor\": \"#1F6FEB\",\n"
            "    \"screenshots\": []\n"
            "  }\n"
            "}\n",
            AIMEE_VERSION);

   char compat_plugin_buf[4096];
   snprintf(compat_plugin_buf, sizeof(compat_plugin_buf),
            "{\n"
            "  \"name\": \"aimee\",\n"
            "  \"version\": \"%s\",\n"
            "  \"description\": "
            "\"Persistent memory, code search, blast-radius preview, and delegation "
            "for local coding sessions.\",\n"
            "  \"author\": {\n"
            "    \"name\": \"aimee\",\n"
            "    \"email\": \"support@example.invalid\",\n"
            "    \"url\": \"https://github.com/JBailes/aimee\"\n"
            "  },\n"
            "  \"homepage\": \"https://github.com/JBailes/aimee\",\n"
            "  \"repository\": \"https://github.com/JBailes/aimee\",\n"
            "  \"license\": \"MIT\",\n"
            "  \"keywords\": [\"memory\", \"mcp\", \"coding\", \"search\", \"delegation\"],\n"
            "  \"skills\": \"./aimee/\",\n"
            "  \"mcpServers\": \"./.mcp.json\",\n"
            "  \"interface\": {\n"
            "    \"displayName\": \"aimee\",\n"
            "    \"shortDescription\": \"Memory, search, and delegation for Codex\",\n"
            "    \"longDescription\": "
            "\"Expose aimee's MCP server to Codex so sessions can search memory, "
            "inspect indexed code, preview blast radius, and delegate sub-tasks "
            "through the same local backend.\",\n"
            "    \"developerName\": \"aimee\",\n"
            "    \"category\": \"Coding\",\n"
            "    \"capabilities\": [\"Interactive\", \"Write\"],\n"
            "    \"websiteURL\": \"https://github.com/JBailes/aimee\",\n"
            "    \"privacyPolicyURL\": \"https://github.com/JBailes/aimee\",\n"
            "    \"termsOfServiceURL\": \"https://github.com/JBailes/aimee\",\n"
            "    \"defaultPrompt\": [\n"
            "      \"Search aimee memory before answering repo-specific questions\",\n"
            "      \"Preview the blast radius before editing multiple files\",\n"
            "      \"Delegate a bounded task when local context is not enough\"\n"
            "    ],\n"
            "    \"brandColor\": \"#1F6FEB\",\n"
            "    \"screenshots\": []\n"
            "  }\n"
            "}\n",
            AIMEE_VERSION);

   char mcp_buf[512];
   snprintf(mcp_buf, sizeof(mcp_buf),
            "{\n"
            "  \"mcpServers\": {\n"
            "    \"aimee\": {\n"
            "      \"command\": \"%s\",\n"
            "      \"args\": [\"mcp-serve\"]\n"
            "    }\n"
            "  }\n"
            "}\n",
            aimee_bin);

   const char *skill_buf =
       "---\n"
       "name: aimee\n"
       "description: Use aimee for repo memory, indexed symbol lookup, "
       "blast-radius preview, and delegated work.\n"
       "---\n"
       "\n"
       "# aimee\n"
       "\n"
       "Use this plugin when Codex needs repository memory or aimee-specific helpers.\n"
       "\n"
       "- Prefer local file inspection first for nearby code.\n"
       "- Use `search_memory` for stored project facts or prior decisions.\n"
       "- Use `find_symbol` when the local search surface is missing indexed context.\n"
       "- Use `preview_blast_radius` before broad multi-file edits.\n"
       "- Use `delegate` only for bounded sub-tasks that materially advance the current work.\n";

   write_text_file(plugin_json, plugin_buf, 0644);
   write_text_file(marketplace_plugin_json, plugin_buf, 0644);
   write_text_file(installed_plugin_json, plugin_buf, 0644);
   write_text_file(mcp_json, mcp_buf, 0644);
   write_text_file(marketplace_mcp_json, mcp_buf, 0644);
   write_text_file(installed_mcp_json, mcp_buf, 0644);
   write_text_file(compat_plugin_json, compat_plugin_buf, 0644);
   write_text_file(marketplace_compat_plugin_json, compat_plugin_buf, 0644);
   write_text_file(installed_compat_plugin_json, compat_plugin_buf, 0644);
   write_text_file(compat_mcp_json, mcp_buf, 0644);
   write_text_file(marketplace_compat_mcp_json, mcp_buf, 0644);
   write_text_file(installed_compat_mcp_json, mcp_buf, 0644);
   write_text_file(skill_md, skill_buf, 0644);
   write_text_file(marketplace_skill_md, skill_buf, 0644);
   write_text_file(installed_skill_md, skill_buf, 0644);
   ensure_codex_marketplace(marketplace);
   ensure_codex_plugin_enabled(config_toml);
}

/* --- Claude Code integration ---
 * Registers aimee MCP server in ~/.claude/settings.json and installs
 * custom slash commands to ~/.claude/commands/. */

static cJSON *read_json_file(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   return root;
}

static void ensure_claude_code_mcp(const char *settings_path)
{
   const char *aimee_bin = "/usr/local/bin/aimee";
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   /* Check if mcpServers.aimee already exists with the correct command */
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (cJSON_IsObject(servers))
   {
      cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
      if (cJSON_IsObject(aimee))
      {
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(aimee, "command");
         cJSON *cmd_args = cJSON_GetObjectItemCaseSensitive(aimee, "args");
         if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, aimee_bin) == 0 &&
             cJSON_IsArray(cmd_args) && cJSON_GetArraySize(cmd_args) == 1)
         {
            cJSON *arg0 = cJSON_GetArrayItem(cmd_args, 0);
            if (cJSON_IsString(arg0) && strcmp(arg0->valuestring, "mcp-serve") == 0)
            {
               cJSON_Delete(root);
               return; /* Already configured correctly */
            }
         }
      }
   }

   /* Ensure mcpServers object exists */
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   /* Create or replace aimee entry */
   cJSON *existing = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (existing)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = cJSON_CreateObject();
   cJSON_AddStringToObject(aimee_server, "command", aimee_bin);
   {
      cJSON *a = cJSON_CreateArray();
      cJSON_AddItemToArray(a, cJSON_CreateString("mcp-serve"));
      cJSON_AddItemToObject(aimee_server, "args", a);
   }
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(settings_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

/* Ensure PostToolUse hooks include EnterWorktree|ExitWorktree so that
 * aimee's CWD tracking file gets updated when the session enters/exits
 * a worktree. Without this, MCP git tools won't follow worktree changes. */
static void ensure_claude_code_hooks(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return; /* Don't create hooks from scratch — only patch existing */
   }

   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   if (!cJSON_IsObject(hooks))
   {
      cJSON_Delete(root);
      return;
   }

   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   if (!cJSON_IsArray(post))
   {
      cJSON_Delete(root);
      return;
   }

   /* Find the aimee hooks post entry and check its matcher */
   int dirty = 0;
   int n = cJSON_GetArraySize(post);
   for (int i = 0; i < n; i++)
   {
      cJSON *entry = cJSON_GetArrayItem(post, i);
      if (!cJSON_IsObject(entry))
         continue;
      cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
      if (!cJSON_IsString(matcher))
         continue;
      /* Check if this is an aimee hooks entry */
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
      if (!cJSON_IsArray(hook_arr))
         continue;
      int found_aimee = 0;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *h = cJSON_GetArrayItem(hook_arr, j);
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(h, "command");
         if (cJSON_IsString(cmd) && strstr(cmd->valuestring, "aimee hooks post"))
            found_aimee = 1;
      }
      if (!found_aimee)
         continue;

      /* This is the aimee PostToolUse entry — ensure EnterWorktree is in matcher */
      if (!strstr(matcher->valuestring, "EnterWorktree"))
      {
         char new_matcher[512];
         snprintf(new_matcher, sizeof(new_matcher), "%s|EnterWorktree|ExitWorktree",
                  matcher->valuestring);
         cJSON_SetValuestring(matcher, new_matcher);
         dirty = 1;
      }
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(settings_path, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_commands(const char *home)
{
   char path[MAX_PATH_LEN];

   snprintf(path, sizeof(path), "%s/.claude/commands/aimee-search.md", home);
   write_text_file(path,
                   "Search aimee memory for project facts, prior decisions, and stored context.\n"
                   "\n"
                   "Use the aimee MCP tool `search_memory` with the query: $ARGUMENTS\n"
                   "\n"
                   "If no query is provided, use `list_facts` to show all stored facts.\n",
                   0644);

   snprintf(path, sizeof(path), "%s/.claude/commands/aimee-delegate.md", home);
   write_text_file(path,
                   "Delegate a bounded sub-task to an aimee delegate agent.\n"
                   "\n"
                   "Use the aimee MCP tool `delegate` with the task: $ARGUMENTS\n"
                   "\n"
                   "The delegate will execute the task using the cheapest suitable model\n"
                   "and return the result. Only delegate bounded, well-defined tasks.\n",
                   0644);

   snprintf(path, sizeof(path), "%s/.claude/commands/aimee-blast-radius.md", home);
   write_text_file(path,
                   "Preview the blast radius of a multi-file edit before making changes.\n"
                   "\n"
                   "Use the aimee MCP tool `preview_blast_radius` for: $ARGUMENTS\n"
                   "\n"
                   "This shows which files and symbols would be affected by the change,\n"
                   "helping you understand the impact before editing.\n",
                   0644);
}

/* Ensure the "env" key in settings.json has required environment variables.
 * Currently sets CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR=0 so that cd
 * commands persist across Bash calls, enabling worktree workflows. */
static void ensure_claude_code_env(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return;
   }

   int dirty = 0;
   cJSON *env = cJSON_GetObjectItemCaseSensitive(root, "env");
   if (!cJSON_IsObject(env))
   {
      if (env)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "env");
      env = cJSON_AddObjectToObject(root, "env");
      dirty = 1;
   }

   cJSON *cwd_var =
       cJSON_GetObjectItemCaseSensitive(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR");
   if (!cJSON_IsString(cwd_var) || strcmp(cwd_var->valuestring, "0") != 0)
   {
      if (cwd_var)
         cJSON_DeleteItemFromObjectCaseSensitive(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR");
      cJSON_AddStringToObject(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR", "0");
      dirty = 1;
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(settings_path, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_integration(const char *home)
{
   const char *aimee_bin = "/usr/local/bin/aimee";
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char settings_path[MAX_PATH_LEN];
   snprintf(settings_path, sizeof(settings_path), "%s/.claude/settings.json", home);
   ensure_claude_code_mcp(settings_path);
   ensure_claude_code_hooks(settings_path);
   ensure_claude_code_env(settings_path);
   ensure_claude_code_commands(home);
}

static void ensure_gemini_integration(const char *home)
{
   const char *aimee_bin = "/usr/local/bin/aimee";
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.gemini/settings.json", home);

   cJSON *root = read_json_file(path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (aimee)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = cJSON_CreateObject();
   cJSON_AddStringToObject(aimee_server, "command", aimee_bin);
   {
      cJSON *a = cJSON_CreateArray();
      cJSON_AddItemToArray(a, cJSON_CreateString("mcp-serve"));
      cJSON_AddItemToObject(aimee_server, "args", a);
   }
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_copilot_integration(const char *home)
{
   const char *aimee_bin = "/usr/local/bin/aimee";
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.copilot/mcp-config.json", home);

   cJSON *root = read_json_file(path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (aimee)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = cJSON_CreateObject();
   cJSON_AddStringToObject(aimee_server, "command", aimee_bin);
   {
      cJSON *a = cJSON_CreateArray();
      cJSON_AddItemToArray(a, cJSON_CreateString("mcp-serve"));
      cJSON_AddItemToObject(aimee_server, "args", a);
   }
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

void ensure_client_integrations(void)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return;

   struct stat st;

   char codex_dir[MAX_PATH_LEN];
   snprintf(codex_dir, sizeof(codex_dir), "%s/.codex", home);
   if (stat(codex_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_codex_plugin_files(home);

   char claude_dir[MAX_PATH_LEN];
   snprintf(claude_dir, sizeof(claude_dir), "%s/.claude", home);
   if (stat(claude_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_claude_code_integration(home);

   char gemini_dir[MAX_PATH_LEN];
   snprintf(gemini_dir, sizeof(gemini_dir), "%s/.gemini", home);
   if (stat(gemini_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_gemini_integration(home);

   char copilot_dir[MAX_PATH_LEN];
   snprintf(copilot_dir, sizeof(copilot_dir), "%s/.copilot", home);
   if (stat(copilot_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_copilot_integration(home);
}
