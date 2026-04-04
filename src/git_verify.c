/* git_verify.c -- project verification runner
 *
 * Users define verify steps in .aimee/project.yaml:
 *   verify:
 *     - name: build
 *       run: cd src && make
 *     - name: tests
 *       run: cd src && make unit-tests
 *
 * `aimee git verify` runs all steps. On success, a file-mtime hash and
 * timestamp are recorded to .aimee/.last-verify.  The verify gate checks
 * this hash + a 1-hour TTL to decide whether re-verification is needed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#include "headers/git_verify.h"
#include "headers/aimee.h"
#include "headers/util.h"

/* --- Helpers --- */

static cJSON *mcp_text(const char *text)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *item = cJSON_CreateObject();
   cJSON_AddStringToObject(item, "type", "text");
   cJSON_AddStringToObject(item, "text", text);
   cJSON_AddItemToArray(arr, item);
   return arr;
}

/* --- Config loading --- */

int verify_load_config(const char *project_root, verify_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   char path[MAX_PATH_LEN];
   if (project_root && project_root[0])
      snprintf(path, sizeof(path), "%s/.aimee/project.yaml", project_root);
   else
      snprintf(path, sizeof(path), ".aimee/project.yaml");

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   int in_verify = 0;
   int pending_name = 0; /* saw a - name: line, expecting run: next */
   char line[1024];

   while (fgets(line, sizeof(line), f))
   {
      /* Strip trailing newline/cr */
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';

      /* Blank line or comment */
      if (len == 0 || line[0] == '#')
         continue;

      /* Non-indented line: either entering or leaving verify section */
      if (line[0] != ' ' && line[0] != '\t')
      {
         if (strncmp(line, "verify:", 7) == 0)
         {
            in_verify = 1;
            continue;
         }
         else
         {
            if (in_verify)
               break; /* left the verify section */
            continue;
         }
      }

      if (!in_verify)
         continue;

      /* Inside verify section: look for "  - name: X" and "    run: X" */
      char *trimmed = line;
      while (*trimmed == ' ' || *trimmed == '\t')
         trimmed++;

      if (strncmp(trimmed, "- name:", 7) == 0)
      {
         if (cfg->count >= MAX_VERIFY_STEPS)
            break;
         char *val = trimmed + 7;
         while (*val == ' ')
            val++;
         snprintf(cfg->steps[cfg->count].name, MAX_STEP_NAME, "%s", val);
         pending_name = 1;
      }
      else if (strncmp(trimmed, "run:", 4) == 0 && pending_name)
      {
         char *val = trimmed + 4;
         while (*val == ' ')
            val++;
         snprintf(cfg->steps[cfg->count].run, MAX_STEP_CMD, "%s", val);
         cfg->count++;
         pending_name = 0;
      }
   }

   fclose(f);
   return cfg->count > 0 ? 0 : -1;
}

/* --- File-mtime hash computation --- */

/* Simple DJB2-style hash accumulator */
static uint64_t hash_accum(uint64_t h, const void *data, size_t len)
{
   const unsigned char *p = data;
   for (size_t i = 0; i < len; i++)
      h = h * 5381 + p[i];
   return h;
}

char *verify_compute_file_hash(const char *project_root)
{
   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' ls-files 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git ls-files 2>/dev/null");

   int rc;
   char *file_list = run_cmd(cmd, &rc);
   if (rc != 0 || !file_list || !file_list[0])
   {
      free(file_list);
      return NULL;
   }

   uint64_t h = 0;
   char *line = file_list;
   while (line && *line)
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';

      if (line[0])
      {
         /* Hash the file path */
         h = hash_accum(h, line, strlen(line));

         /* Get file mtime */
         char full_path[MAX_PATH_LEN];
         if (project_root && project_root[0])
            snprintf(full_path, sizeof(full_path), "%s/%s", project_root, line);
         else
            snprintf(full_path, sizeof(full_path), "%s", line);

         struct stat st;
         if (stat(full_path, &st) == 0)
         {
            int64_t mtime = (int64_t)st.st_mtime;
            h = hash_accum(h, &mtime, sizeof(mtime));
         }
      }

      line = nl ? nl + 1 : NULL;
   }

   free(file_list);

   char *result = malloc(20);
   if (result)
      snprintf(result, 20, "%016llx", (unsigned long long)h);
   return result;
}

/* --- State file management --- */

static void get_state_path(const char *project_root, char *buf, size_t len)
{
   if (project_root && project_root[0])
      snprintf(buf, len, "%s/.aimee/.last-verify", project_root);
   else
      snprintf(buf, len, ".aimee/.last-verify");
}

/* State file format (two lines):
 *   <unix_timestamp>
 *   <file_mtime_hash>
 */

static int read_verify_state(const char *project_root, time_t *timestamp, char *hash, size_t hash_len)
{
   char path[MAX_PATH_LEN];
   get_state_path(project_root, path, sizeof(path));

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   char ts_line[32] = {0};
   char hash_line[32] = {0};

   if (!fgets(ts_line, sizeof(ts_line), f) || !fgets(hash_line, sizeof(hash_line), f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);

   /* Strip trailing whitespace */
   for (char *p = ts_line + strlen(ts_line) - 1; p >= ts_line && (*p == '\n' || *p == '\r' || *p == ' '); p--)
      *p = '\0';
   for (char *p = hash_line + strlen(hash_line) - 1; p >= hash_line && (*p == '\n' || *p == '\r' || *p == ' '); p--)
      *p = '\0';

   if (!ts_line[0] || !hash_line[0])
      return -1;

   *timestamp = (time_t)strtoll(ts_line, NULL, 10);
   snprintf(hash, hash_len, "%s", hash_line);
   return 0;
}

static int write_verify_state(const char *project_root, time_t timestamp, const char *hash)
{
   char path[MAX_PATH_LEN], tmp_path[MAX_PATH_LEN];
   get_state_path(project_root, path, sizeof(path));
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

   FILE *f = fopen(tmp_path, "w");
   if (!f)
      return -1;

   fprintf(f, "%lld\n%s\n", (long long)timestamp, hash);
   fclose(f);

   if (rename(tmp_path, path) != 0)
   {
      remove(tmp_path);
      return -1;
   }
   return 0;
}

/* --- Run verification --- */

int verify_run_all(const char *project_root, verify_config_t *cfg)
{
   if (!cfg || cfg->count == 0)
      return -1;

   /* Check working tree is clean */
   int rc;
   char *status = run_cmd("git status --porcelain 2>/dev/null", &rc);
   if (status && status[0])
   {
      fprintf(stderr, "warning: working tree has uncommitted changes\n");
      fprintf(stderr, "commit or stash changes before verifying for accurate results\n");
      /* Continue anyway -- user may want to verify before committing */
   }
   free(status);

   int all_passed = 1;

   for (int i = 0; i < cfg->count; i++)
   {
      fprintf(stderr, "[%d/%d] %s ... ", i + 1, cfg->count, cfg->steps[i].name);
      fflush(stderr);

      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);

      /* Run the step command, capturing output */
      char cmd[MAX_STEP_CMD + 16];
      snprintf(cmd, sizeof(cmd), "%s 2>&1", cfg->steps[i].run);
      char *out = run_cmd(cmd, &rc);

      clock_gettime(CLOCK_MONOTONIC, &t1);
      double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

      if (rc == 0)
      {
         fprintf(stderr, "PASS (%.1fs)\n", elapsed);
      }
      else
      {
         fprintf(stderr, "FAIL (exit %d, %.1fs)\n", rc, elapsed);
         /* Show output of failing step (truncated) */
         if (out && out[0])
         {
            size_t out_len = strlen(out);
            if (out_len > 4096)
            {
               fprintf(stderr, "... (truncated, showing last 4KB)\n");
               fprintf(stderr, "%s\n", out + out_len - 4096);
            }
            else
            {
               fprintf(stderr, "%s\n", out);
            }
         }
         free(out);
         all_passed = 0;
         break;
      }
      free(out);
   }

   if (!all_passed)
      return -1;

   /* Record file-mtime hash and timestamp */
   char *file_hash = verify_compute_file_hash(project_root);
   if (file_hash)
   {
      time_t now = time(NULL);
      if (write_verify_state(project_root, now, file_hash) == 0)
         fprintf(stderr, "verified at %s\n", file_hash);
      else
         fprintf(stderr, "warning: could not record verify state\n");
      free(file_hash);
   }
   else
   {
      fprintf(stderr, "warning: could not compute file hash\n");
   }

   return 0;
}

/* --- Check verification state --- */

int verify_check(const char *project_root, char *msg_buf, size_t msg_len)
{
   verify_config_t cfg;
   if (verify_load_config(project_root, &cfg) != 0)
   {
      /* No verify section -- no gate */
      if (msg_buf)
         snprintf(msg_buf, msg_len, "no verify steps configured");
      return 1;
   }

   time_t stored_ts = 0;
   char stored_hash[32] = {0};
   if (read_verify_state(project_root, &stored_ts, stored_hash, sizeof(stored_hash)) != 0)
   {
      if (msg_buf)
         snprintf(msg_buf, msg_len, "no verification recorded. Run 'aimee git verify' first.");
      return 0;
   }

   /* TTL check */
   time_t now = time(NULL);
   double elapsed = difftime(now, stored_ts);
   if (elapsed > VERIFY_TTL_SECS)
   {
      if (msg_buf)
         snprintf(msg_buf, msg_len,
                  "verification expired (%.0f minutes ago, TTL is %d minutes). "
                  "Run 'aimee git verify'.",
                  elapsed / 60.0, VERIFY_TTL_SECS / 60);
      return 0;
   }

   /* File-mtime hash check */
   char *current_hash = verify_compute_file_hash(project_root);
   if (!current_hash)
   {
      if (msg_buf)
         snprintf(msg_buf, msg_len, "could not compute file hash");
      return 0;
   }

   if (strcmp(stored_hash, current_hash) != 0)
   {
      if (msg_buf)
         snprintf(msg_buf, msg_len,
                  "tracked files have changed since last verification. "
                  "Run 'aimee git verify'.");
      free(current_hash);
      return 0;
   }

   if (msg_buf)
      snprintf(msg_buf, msg_len, "verified (%.0f minutes ago)", elapsed / 60.0);
   free(current_hash);
   return 1;
}

/* --- MCP tool handler --- */

cJSON *handle_git_verify(cJSON *args)
{
   (void)args;

   verify_config_t cfg;
   if (verify_load_config(NULL, &cfg) != 0)
      return mcp_text("error: no verify steps found in .aimee/project.yaml\n\n"
                      "Add a verify section:\n"
                      "  verify:\n"
                      "    - name: build\n"
                      "      run: make\n"
                      "    - name: tests\n"
                      "      run: make test");

   /* Build result string with step outcomes */
   char result[4096];
   size_t pos = 0;
   int all_passed = 1;
   int rc;

   /* Check working tree */
   char *status = run_cmd("git status --porcelain 2>/dev/null", &rc);
   if (status && status[0])
   {
      pos += (size_t)snprintf(result + pos, sizeof(result) - pos,
                              "warning: uncommitted changes in working tree\n\n");
   }
   free(status);

   for (int i = 0; i < cfg.count; i++)
   {
      struct timespec t0, t1;
      clock_gettime(CLOCK_MONOTONIC, &t0);

      char cmd[MAX_STEP_CMD + 16];
      snprintf(cmd, sizeof(cmd), "%s 2>&1", cfg.steps[i].run);
      char *out = run_cmd(cmd, &rc);

      clock_gettime(CLOCK_MONOTONIC, &t1);
      double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

      if (rc == 0)
      {
         pos += (size_t)snprintf(result + pos, sizeof(result) - pos, "[%d/%d] %s: PASS (%.1fs)\n",
                                 i + 1, cfg.count, cfg.steps[i].name, elapsed);
      }
      else
      {
         pos += (size_t)snprintf(result + pos, sizeof(result) - pos,
                                 "[%d/%d] %s: FAIL (exit %d, %.1fs)\n", i + 1, cfg.count,
                                 cfg.steps[i].name, rc, elapsed);
         /* Include truncated output */
         if (out && out[0])
         {
            size_t out_len = strlen(out);
            const char *show = out;
            if (out_len > 2048)
               show = out + out_len - 2048;
            pos += (size_t)snprintf(result + pos, sizeof(result) - pos, "%s\n", show);
         }
         free(out);
         all_passed = 0;
         break;
      }
      free(out);
   }

   if (all_passed)
   {
      char *file_hash = verify_compute_file_hash(NULL);
      if (file_hash)
      {
         time_t now = time(NULL);
         write_verify_state(NULL, now, file_hash);
         pos += (size_t)snprintf(result + pos, sizeof(result) - pos,
                                 "\nall %d steps passed -- verified (%s)", cfg.count, file_hash);
         free(file_hash);
      }
      else
      {
         pos += (size_t)snprintf(result + pos, sizeof(result) - pos,
                                 "\nall %d steps passed (could not record state)", cfg.count);
      }
   }

   (void)pos;
   return mcp_text(result);
}
