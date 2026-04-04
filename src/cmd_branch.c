/* cmd_branch.c: cascading branch merge orchestration */
#include "aimee.h"
#include "commands.h"
#include "cmd_branch.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define BRANCH_BUF_SIZE 65536
#define MAX_LINE_BUF    4096

/* --- Helpers --- */

/* --- File-list retrieval --- */

/* Get list of files changed by a branch relative to main.
 * Returns count of files, populates files array. Caller must free each string. */
static int get_branch_files(const char *branch, char **files, int max_files)
{
   char cmd[MAX_LINE_BUF];
   snprintf(cmd, sizeof(cmd), "git diff --name-only main...%s 2>/dev/null", branch);

   int ec;
   char *out = run_cmd(cmd, &ec);
   if (!out || ec != 0)
   {
      free(out);
      return 0;
   }

   int count = 0;
   char *line = strtok(out, "\n");
   while (line && count < max_files)
   {
      if (line[0] != '\0')
         files[count++] = strdup(line);
      line = strtok(NULL, "\n");
   }
   free(out);
   return count;
}

/* --- Conflict graph construction --- */

void branch_build_conflict_graph(conflict_graph_t *g)
{
   /* Build edges: for each pair of branches, find shared files */
   g->edge_count = 0;
   for (int i = 0; i < g->branch_count; i++)
      g->branches[i].conflict_count = 0;

   for (int i = 0; i < g->branch_count; i++)
   {
      for (int j = i + 1; j < g->branch_count; j++)
      {
         if (g->edge_count >= BRANCH_MAX_EDGES)
            break;

         conflict_edge_t *e = &g->edges[g->edge_count];
         e->a = i;
         e->b = j;
         e->shared_file_count = 0;
         memset(e->shared_files, 0, sizeof(e->shared_files));

         for (int fi = 0; fi < g->branches[i].file_count; fi++)
         {
            for (int fj = 0; fj < g->branches[j].file_count; fj++)
            {
               if (strcmp(g->branches[i].files[fi], g->branches[j].files[fj]) == 0)
               {
                  if (e->shared_file_count < BRANCH_MAX_SHARED)
                  {
                     e->shared_files[e->shared_file_count] = strdup(g->branches[i].files[fi]);
                     e->shared_file_count++;
                  }
               }
            }
         }

         if (e->shared_file_count > 0)
         {
            g->branches[i].conflict_count++;
            g->branches[j].conflict_count++;
            g->edge_count++;
         }
      }
   }

   /* Determine merge order: greedy sort by conflict_count ascending */
   for (int i = 0; i < g->branch_count; i++)
      g->merge_order[i] = i;

   for (int i = 1; i < g->branch_count; i++)
   {
      int key = g->merge_order[i];
      int j = i - 1;
      while (j >= 0 &&
             g->branches[g->merge_order[j]].conflict_count > g->branches[key].conflict_count)
      {
         g->merge_order[j + 1] = g->merge_order[j];
         j--;
      }
      g->merge_order[j + 1] = key;
   }
}

void branch_free_edges(conflict_graph_t *g)
{
   for (int i = 0; i < g->edge_count; i++)
   {
      for (int f = 0; f < g->edges[i].shared_file_count; f++)
         free(g->edges[i].shared_files[f]);
   }
}

/* Populate a conflict graph from live git data. */
static void build_graph_from_git(conflict_graph_t *g, int argc, char **argv)
{
   g->branch_count = 0;

   if (argc == 0 || (argc == 1 && strcmp(argv[0], "all") == 0))
   {
      int ec;
      char *out = run_cmd("git worktree list --porcelain 2>/dev/null", &ec);
      if (out)
      {
         char *line = strtok(out, "\n");
         while (line && g->branch_count < BRANCH_MAX_BRANCHES)
         {
            if (strncmp(line, "branch refs/heads/", 18) == 0)
            {
               const char *bname = line + 18;
               if (strcmp(bname, "main") != 0 && strcmp(bname, "master") != 0)
               {
                  snprintf(g->branches[g->branch_count].name, 256, "%s", bname);
                  g->branches[g->branch_count].file_count =
                      get_branch_files(bname, g->branches[g->branch_count].files, BRANCH_MAX_FILES);
                  g->branch_count++;
               }
            }
            line = strtok(NULL, "\n");
         }
         free(out);
      }
   }
   else
   {
      for (int i = 0; i < argc && g->branch_count < BRANCH_MAX_BRANCHES; i++)
      {
         snprintf(g->branches[g->branch_count].name, 256, "%s", argv[i]);
         g->branches[g->branch_count].file_count =
             get_branch_files(argv[i], g->branches[g->branch_count].files, BRANCH_MAX_FILES);
         g->branch_count++;
      }
   }

   branch_build_conflict_graph(g);
}

static void free_graph(conflict_graph_t *g)
{
   branch_free_edges(g);
   for (int i = 0; i < g->branch_count; i++)
   {
      for (int f = 0; f < g->branches[i].file_count; f++)
         free(g->branches[i].files[f]);
   }
}

/* --- Auto-resolution patterns --- */

char *branch_resolve_migration_renumber(const char *content, int old_num, int new_num)
{
   char old_pat[32], new_pat[32];
   snprintf(old_pat, sizeof(old_pat), "{%d,", old_num);
   snprintf(new_pat, sizeof(new_pat), "{%d,", new_num);

   const char *pos = strstr(content, old_pat);
   if (!pos)
      return NULL;

   size_t prefix_len = (size_t)(pos - content);
   size_t old_len = strlen(old_pat);
   size_t new_len = strlen(new_pat);
   size_t rest_len = strlen(pos + old_len);
   size_t total = prefix_len + new_len + rest_len + 1;

   char *result = malloc(total);
   if (!result)
      return NULL;

   memcpy(result, content, prefix_len);
   memcpy(result + prefix_len, new_pat, new_len);
   memcpy(result + prefix_len + new_len, pos + old_len, rest_len);
   result[total - 1] = '\0';
   return result;
}

char *branch_resolve_additive_list(const char *base_line, const char *ours, const char *theirs)
{
   char *items[256];
   int count = 0;

   char *ours_copy = strdup(ours);
   char *tok = strtok(ours_copy, " \t");
   while (tok && count < 256)
   {
      items[count++] = strdup(tok);
      tok = strtok(NULL, " \t");
   }
   free(ours_copy);

   char *theirs_copy = strdup(theirs);
   tok = strtok(theirs_copy, " \t");
   while (tok && count < 256)
   {
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(items[i], tok) == 0)
         {
            found = 1;
            break;
         }
      }
      if (!found)
         items[count++] = strdup(tok);
      tok = strtok(NULL, " \t");
   }
   free(theirs_copy);

   (void)base_line;

   size_t total = 0;
   for (int i = 0; i < count; i++)
      total += strlen(items[i]) + 1;

   char *result = malloc(total + 1);
   if (!result)
   {
      for (int i = 0; i < count; i++)
         free(items[i]);
      return NULL;
   }

   size_t pos = 0;
   for (int i = 0; i < count; i++)
   {
      if (i > 0)
         pos += snprintf(result + pos, total + 1 - pos, " ");
      pos += snprintf(result + pos, total + 1 - pos, "%s", items[i]);
      free(items[i]);
   }

   return result;
}

/* --- Subcommands --- */

static void branch_conflicts(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;

   conflict_graph_t *g = calloc(1, sizeof(*g));
   if (!g)
      fatal("out of memory");

   build_graph_from_git(g, argc, argv);

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON *branches = cJSON_CreateArray();
      cJSON *edges = cJSON_CreateArray();

      for (int i = 0; i < g->branch_count; i++)
      {
         cJSON *b = cJSON_CreateObject();
         cJSON_AddStringToObject(b, "name", g->branches[i].name);
         cJSON_AddNumberToObject(b, "file_count", g->branches[i].file_count);
         cJSON_AddNumberToObject(b, "conflict_count", g->branches[i].conflict_count);
         cJSON_AddItemToArray(branches, b);
      }

      for (int i = 0; i < g->edge_count; i++)
      {
         cJSON *e = cJSON_CreateObject();
         cJSON_AddStringToObject(e, "branch_a", g->branches[g->edges[i].a].name);
         cJSON_AddStringToObject(e, "branch_b", g->branches[g->edges[i].b].name);
         cJSON_AddNumberToObject(e, "shared_files", g->edges[i].shared_file_count);
         cJSON *files = cJSON_CreateArray();
         for (int f = 0; f < g->edges[i].shared_file_count; f++)
            cJSON_AddItemToArray(files, cJSON_CreateString(g->edges[i].shared_files[f]));
         cJSON_AddItemToObject(e, "files", files);
         cJSON_AddItemToArray(edges, e);
      }

      cJSON *order = cJSON_CreateArray();
      for (int i = 0; i < g->branch_count; i++)
         cJSON_AddItemToArray(order, cJSON_CreateString(g->branches[g->merge_order[i]].name));

      cJSON_AddItemToObject(root, "branches", branches);
      cJSON_AddItemToObject(root, "conflicts", edges);
      cJSON_AddItemToObject(root, "merge_order", order);

      char *json = cJSON_PrintUnformatted(root);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(root);
   }
   else
   {
      printf("Branch Conflict Analysis\n");
      printf("========================\n\n");

      printf("Branches (%d):\n", g->branch_count);
      for (int i = 0; i < g->branch_count; i++)
         printf("  %-40s %d files, %d conflicts\n", g->branches[i].name, g->branches[i].file_count,
                g->branches[i].conflict_count);

      if (g->edge_count > 0)
      {
         printf("\nConflicts:\n");
         for (int i = 0; i < g->edge_count; i++)
         {
            printf("  %s <-> %s (%d shared files)\n", g->branches[g->edges[i].a].name,
                   g->branches[g->edges[i].b].name, g->edges[i].shared_file_count);
            for (int f = 0; f < g->edges[i].shared_file_count; f++)
               printf("    - %s\n", g->edges[i].shared_files[f]);
         }
      }
      else
      {
         printf("\nNo conflicts detected between branches.\n");
      }

      printf("\nSuggested merge order (least-conflicting first):\n");
      for (int i = 0; i < g->branch_count; i++)
         printf("  %d. %s\n", i + 1, g->branches[g->merge_order[i]].name);
   }

   free_graph(g);
   free(g);
}

static void branch_merge_all(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv)
{
   (void)db;

   int dry_run = 0;
   int branch_argc = 0;
   char *branch_argv[BRANCH_MAX_BRANCHES];

   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--dry-run") == 0)
         dry_run = 1;
      else if (branch_argc < BRANCH_MAX_BRANCHES)
         branch_argv[branch_argc++] = argv[i];
   }

   conflict_graph_t *g = calloc(1, sizeof(*g));
   if (!g)
      fatal("out of memory");

   build_graph_from_git(g, branch_argc, branch_argv);

   if (g->branch_count == 0)
   {
      printf("No branches to merge.\n");
      free_graph(g);
      free(g);
      return;
   }

   printf("Merge plan (%d branches):\n", g->branch_count);
   for (int i = 0; i < g->branch_count; i++)
      printf("  %d. %s (conflicts: %d)\n", i + 1, g->branches[g->merge_order[i]].name,
             g->branches[g->merge_order[i]].conflict_count);

   if (dry_run)
   {
      printf("\n[dry-run] No changes made.\n");
      free_graph(g);
      free(g);
      return;
   }

   printf("\n");

   int merged = 0;
   int failed = 0;

   for (int i = 0; i < g->branch_count; i++)
   {
      int idx = g->merge_order[i];
      const char *branch = g->branches[idx].name;

      printf("--- Merging %s (%d/%d) ---\n", branch, i + 1, g->branch_count);

      int ec;
      char *out;
      char cmd[MAX_LINE_BUF];

      snprintf(cmd, sizeof(cmd), "git checkout main 2>&1");
      out = run_cmd(cmd, &ec);
      if (ec != 0)
      {
         printf("FAIL: could not checkout main: %s\n", out ? out : "unknown error");
         free(out);
         failed++;
         break;
      }
      free(out);

      snprintf(cmd, sizeof(cmd), "git rebase main %s 2>&1", branch);
      out = run_cmd(cmd, &ec);
      if (ec != 0)
      {
         printf("WARN: rebase failed for %s, attempting abort\n", branch);
         free(out);
         out = run_cmd("git rebase --abort 2>&1", &ec);
         free(out);
         printf("SKIP: %s (rebase conflict -- needs manual resolution)\n", branch);
         failed++;
         continue;
      }
      free(out);

      snprintf(cmd, sizeof(cmd), "git checkout main 2>&1");
      out = run_cmd(cmd, &ec);
      free(out);

      snprintf(cmd, sizeof(cmd), "git merge --no-ff %s -m 'Merge branch %s' 2>&1", branch, branch);
      out = run_cmd(cmd, &ec);
      if (ec != 0)
      {
         printf("FAIL: merge of %s failed: %s\n", branch, out ? out : "unknown");
         free(out);
         out = run_cmd("git merge --abort 2>&1", &ec);
         free(out);
         failed++;
         continue;
      }
      free(out);

      printf("OK: merged %s\n", branch);
      merged++;
   }

   printf("\n--- Summary ---\n");
   printf("Merged: %d/%d\n", merged, g->branch_count);
   if (failed > 0)
      printf("Failed/skipped: %d\n", failed);

   if (ctx->json_output)
   {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddNumberToObject(root, "merged", merged);
      cJSON_AddNumberToObject(root, "failed", failed);
      cJSON_AddNumberToObject(root, "total", g->branch_count);
      char *json = cJSON_PrintUnformatted(root);
      printf("%s\n", json);
      free(json);
      cJSON_Delete(root);
   }

   free_graph(g);
   free(g);
}

/* --- Subcmd table --- */

static const subcmd_t branch_subcmds[] = {
    {"conflicts", "Analyze conflicts between branches", branch_conflicts},
    {"merge-all", "Merge branches in safe order", branch_merge_all},
    {NULL, NULL, NULL},
};

const subcmd_t *get_branch_subcmds(void)
{
   return branch_subcmds;
}

/* --- Main entry point --- */

void cmd_branch(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      subcmd_usage("branch", branch_subcmds);
      return;
   }

   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");

   subcmd_dispatch(branch_subcmds, argv[0], ctx, db, argc - 1, argv + 1);
   ctx_db_close(ctx);
}
