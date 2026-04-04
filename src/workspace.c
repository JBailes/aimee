/* workspace.c: directory-based workspace discovery and context generation */
#include "aimee.h"
#include "workspace.h"
#include "index.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- recursive git discovery --- */

/* Directories to skip during discovery */
static int is_skip_dir(const char *name)
{
   static const char *skip[] = {
       "node_modules", ".git", "vendor", "__pycache__", "build", "dist", "target",   ".worktrees",
       "bin",          "obj",  ".cache", ".venv",       "venv",  ".tox", "coverage", NULL};
   for (int i = 0; skip[i]; i++)
   {
      if (strcmp(name, skip[i]) == 0)
         return 1;
   }
   return 0;
}

static int is_git_dir(const char *path)
{
   char git_path[MAX_PATH_LEN];
   struct stat st;
   snprintf(git_path, sizeof(git_path), "%s/.git", path);
   return (stat(git_path, &st) == 0);
}

static void discover_recursive(const char *dir, int depth, int max_depth,
                               char projects[][MAX_PATH_LEN], int max, int *count)
{
   if (depth > max_depth || *count >= max)
      return;

   /* Check if this directory itself is a git repo */
   if (is_git_dir(dir))
   {
      char abs[MAX_PATH_LEN];
      if (realpath(dir, abs))
         snprintf(projects[(*count)++], MAX_PATH_LEN, "%s", abs);
      else
         snprintf(projects[(*count)++], MAX_PATH_LEN, "%s", dir);
      /* Don't recurse into a git repo's subdirectories */
      return;
   }

   DIR *d = opendir(dir);
   if (!d)
      return;

   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && *count < max)
   {
      if (ent->d_name[0] == '.')
         continue;
      if (is_skip_dir(ent->d_name))
         continue;

      char sub[MAX_PATH_LEN];
      snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name);

      struct stat st;
      if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      discover_recursive(sub, depth + 1, max_depth, projects, max, count);
   }
   closedir(d);
}

int workspace_discover_projects(const char *root, int max_depth, char projects[][MAX_PATH_LEN],
                                int max)
{
   if (!root || !root[0])
      return -1;

   char abs_root[MAX_PATH_LEN];
   if (!realpath(root, abs_root))
      return -1;

   int count = 0;
   discover_recursive(abs_root, 0, max_depth, projects, max, &count);
   return count;
}

/* --- context generation --- */

/* Forward declaration: reads project description from ~/.config/aimee/projects/<name>.md */
char *describe_read(const char *project_name);

char *style_read(const char *project_name)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/projects/%s.style.md", config_default_dir(), project_name);

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (sz <= 0 || sz > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

/* Strip frontmatter (--- ... ---) from a description/style string.
 * Returns pointer into the same buffer (does not allocate). */
static const char *skip_frontmatter(const char *content)
{
   if (strncmp(content, "---", 3) != 0)
      return content;
   const char *end = strstr(content + 3, "---");
   if (!end)
      return content;
   end += 3;
   while (*end == '\n' || *end == '\r')
      end++;
   return end;
}

char *workspace_build_context_from_config(sqlite3 *db, const config_t *cfg)
{
   size_t bufsize = 64 * 1024;
   char *buf = malloc(bufsize);
   if (!buf)
      return NULL;

   size_t pos = 0;
   /* Safe snprintf helper: advance pos but never past bufsize-1 */
#define BUF_PRINTF(...)                                                                            \
   do                                                                                              \
   {                                                                                               \
      if (pos < bufsize - 1)                                                                       \
      {                                                                                            \
         int _n = snprintf(buf + pos, bufsize - pos, __VA_ARGS__);                                 \
         if (_n > 0)                                                                               \
            pos += ((size_t)_n < bufsize - pos) ? (size_t)_n : bufsize - pos - 1;                  \
      }                                                                                            \
   } while (0)

   /* Get all indexed projects */
   project_info_t projects[256];
   int pcount = db ? index_list_projects(db, projects, 256) : 0;

   /* Group projects by workspace and emit context */
   for (int w = 0; w < cfg->workspace_count; w++)
   {
      const char *ws_root = cfg->workspaces[w];
      size_t ws_len = strlen(ws_root);

      /* Find projects belonging to this workspace */
      int first = 1;
      for (int p = 0; p < pcount; p++)
      {
         /* Match: project root starts with workspace root */
         if (strncmp(projects[p].root, ws_root, ws_len) != 0)
            continue;
         /* Must be exact match or followed by '/' */
         if (projects[p].root[ws_len] != '/' && projects[p].root[ws_len] != '\0')
            continue;

         if (first)
         {
            BUF_PRINTF("# Workspace: %s\n\n", ws_root);
            first = 0;
         }

         /* Read and include project description */
         char *desc = describe_read(projects[p].name);
         if (desc)
         {
            const char *content = skip_frontmatter(desc);
            size_t desc_len = strlen(content);
            if (pos + desc_len + 4 < bufsize)
            {
               memcpy(buf + pos, content, desc_len);
               pos += desc_len;
               if (pos > 0 && buf[pos - 1] != '\n')
                  buf[pos++] = '\n';
               buf[pos++] = '\n';
            }
            free(desc);
         }
         else
         {
            BUF_PRINTF("## %s\n(no description)\n\n", projects[p].name);
         }

         /* Read and include style guide */
         char *style = style_read(projects[p].name);
         if (style)
         {
            const char *content = skip_frontmatter(style);
            size_t style_len = strlen(content);
            if (pos + style_len + 4 < bufsize)
            {
               memcpy(buf + pos, content, style_len);
               pos += style_len;
               if (pos > 0 && buf[pos - 1] != '\n')
                  buf[pos++] = '\n';
               buf[pos++] = '\n';
            }
            free(style);
         }
      }
   }

   /* Also include projects not belonging to any workspace */
   for (int p = 0; p < pcount; p++)
   {
      int found_ws = 0;
      for (int w = 0; w < cfg->workspace_count; w++)
      {
         size_t ws_len = strlen(cfg->workspaces[w]);
         if (strncmp(projects[p].root, cfg->workspaces[w], ws_len) == 0 &&
             (projects[p].root[ws_len] == '/' || projects[p].root[ws_len] == '\0'))
         {
            found_ws = 1;
            break;
         }
      }
      if (found_ws)
         continue;

      char *desc = describe_read(projects[p].name);
      if (desc)
      {
         const char *content = skip_frontmatter(desc);
         size_t desc_len = strlen(content);
         if (pos + desc_len + 4 < bufsize)
         {
            memcpy(buf + pos, content, desc_len);
            pos += desc_len;
            if (pos > 0 && buf[pos - 1] != '\n')
               buf[pos++] = '\n';
            buf[pos++] = '\n';
         }
         free(desc);
      }
   }

   if (pos >= bufsize)
      pos = bufsize - 1;
   buf[pos] = '\0';
#undef BUF_PRINTF
   return buf;
}

/* --- resolve_proposal_path --- */

char *resolve_proposal_path(const char *proposal)
{
   if (!proposal || !proposal[0])
      return NULL;

   /* 1. Try path as is (might be absolute or relative to CWD) */
   if (access(proposal, R_OK) == 0)
      return realpath(proposal, NULL);

   /* 2. Try from config workspaces */
   config_t cfg;
   if (config_load(&cfg) == 0)
   {
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         char path[MAX_PATH_LEN];
         snprintf(path, sizeof(path), "%s/%s", cfg.workspaces[w], proposal);
         if (access(path, R_OK) == 0)
            return realpath(path, NULL);
      }
   }

   /* 3. Search docs/proposals/ subdirectories for the filename */
   const char *filename = strrchr(proposal, '/');
   if (filename)
      filename++;
   else
      filename = proposal;

   static const char *subdirs[] = {"pending", "accepted", "done", "rejected",
                                   "reviews", "deferred", NULL};
   char search_path[MAX_PATH_LEN];

   for (int i = 0; subdirs[i]; i++)
   {
      /* Relative to CWD */
      snprintf(search_path, sizeof(search_path), "docs/proposals/%s/%s", subdirs[i], filename);
      if (access(search_path, R_OK) == 0)
         return realpath(search_path, NULL);

      /* Relative to each workspace root */
      for (int w = 0; w < cfg.workspace_count; w++)
      {
         snprintf(search_path, sizeof(search_path), "%s/docs/proposals/%s/%s", cfg.workspaces[w],
                  subdirs[i], filename);
         if (access(search_path, R_OK) == 0)
            return realpath(search_path, NULL);
      }
   }

   return NULL;
}
