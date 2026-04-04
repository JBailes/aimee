/* cmd_agent_trace.c: delegate, queue, context, manifest, trace, and jobs CLI commands */
#include "aimee.h"
#include "agent.h"
#include "platform_random.h"
#include "commands.h"
#include "cJSON.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Remove a delegate worktree by finding the workspace root from the original CWD
 * and running `git worktree remove --force`. */
static void delegate_worktree_cleanup(const char *wt_path, const char *orig_cwd, int keep)
{
   if (!wt_path[0] || !orig_cwd[0])
      return;
   (void)chdir(orig_cwd);
   if (keep)
   {
      fprintf(stderr, "aimee: keeping delegate worktree at %s\n", wt_path);
      return;
   }
   char ws_root[MAX_PATH_LEN];
   snprintf(ws_root, sizeof(ws_root), "%s", orig_cwd);
   for (int d = 0; d < 20; d++)
   {
      char gd[MAX_PATH_LEN];
      snprintf(gd, sizeof(gd), "%s/.git", ws_root);
      struct stat gs;
      if (stat(gd, &gs) == 0)
         break;
      char *sl = strrchr(ws_root, '/');
      if (!sl || sl == ws_root)
         break;
      *sl = '\0';
   }
   char *rm_out = NULL;
   const char *rm_argv[] = {"git", "-C", ws_root, "worktree", "remove", "--force", wt_path, NULL};
   safe_exec_capture(rm_argv, &rm_out, 256);
   free(rm_out);
   fprintf(stderr, "aimee: removed delegate worktree %s\n", wt_path);
}

/* --- cmd_delegate --- */
/* Called by AI tools to offload work to sub-agents.
 * The AI tool runs: aimee delegate <role> "prompt"
 * aimee routes to the cheapest agent, executes, returns result on stdout.
 * This allows Claude Code / Gemini / Codex to delegate expensive work
 * to cheaper or local LLMs. */

static void generate_task_id(char *buf, size_t len)
{
   unsigned char rand_bytes[8];
   if (platform_random_bytes(rand_bytes, sizeof(rand_bytes)) != 0)
      memset(rand_bytes, 0, sizeof(rand_bytes));
   snprintf(buf, len, "aimee-task-%02x%02x%02x%02x%02x%02x%02x%02x", rand_bytes[0], rand_bytes[1],
            rand_bytes[2], rand_bytes[3], rand_bytes[4], rand_bytes[5], rand_bytes[6],
            rand_bytes[7]);
}

static void write_result_json(const char *path, const agent_result_t *result)
{
   cJSON *obj = agent_result_to_json(result);
   char *json = cJSON_Print(obj);
   cJSON_Delete(obj);
   if (!json)
      return;
   FILE *f = fopen(path, "w");
   if (f)
   {
      fputs(json, f);
      fputc('\n', f);
      fclose(f);
   }
   free(json);
}

static void delegate_print_help(void)
{
   fprintf(stderr, "Usage: aimee delegate <role> \"prompt\" [options]\n"
                   "\n"
                   "Delegate a bounded task to a sub-agent.\n"
                   "\n"
                   "Options:\n"
                   "  --json             Output result as JSON\n"
                   "  --background       Run asynchronously (returns task ID)\n"
                   "  --durable          Persist result to disk\n"
                   "  --tools            Enable tool use for the delegate\n"
                   "  --files F          Preload comma-separated file contents\n"
                   "  --context-file F   Preload a specific file (repeatable)\n"
                   "  --prompt-file PATH Read prompt from file (avoids ARG_MAX)\n"
                   "  --context-dir DIR  Include directory contents as context\n"
                   "  --output PATH      Write response to file instead of stdout\n"
                   "  --system S         Override system prompt\n"
                   "  --max-tokens N     Limit response tokens\n"
                   "  --timeout N        Timeout in milliseconds\n"
                   "  --retry N          Retry on failure (up to N times)\n"
                   "  --verify CMD       Run verification command after completion\n"
                   "  --worktree BRANCH  Execute in an isolated git worktree\n"
                   "  --coordination     Enable multi-agent coordination\n"
                   "  --vote N           Run N agents and pick best result\n"
                   "  --plan             Use plan-mode execution\n"
                   "  --dry-run          Show what would be sent without executing\n"
                   "\n"
                   "Subcommands:\n"
                   "  aimee delegate status <task_id>   Check background task status\n"
                   "  aimee delegate --list-roles        List available roles\n"
                   "\n"
                   "Examples:\n"
                   "  aimee delegate code \"Add error handling to src/config.c\"\n"
                   "  aimee delegate review \"Review PR #85 for security issues\"\n"
                   "  aimee delegate execute --tools \"Check nginx status on wol-web\"\n");
}

static void delegate_list_roles(void)
{
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
   {
      fprintf(stderr, "No agents configured. Add agents to agents.json.\n");
      return;
   }

   fprintf(stderr, "Available roles (from agents.json):\n");
   for (int i = 0; i < cfg.agent_count; i++)
   {
      fprintf(stderr, "  %s [%s", cfg.agents[i].name, cfg.agents[i].provider);
      if (cfg.agents[i].model[0])
         fprintf(stderr, "/%s", cfg.agents[i].model);
      fprintf(stderr, "]: ");
      for (int r = 0; r < cfg.agents[i].role_count; r++)
      {
         if (r > 0)
            fprintf(stderr, ", ");
         fprintf(stderr, "%s", cfg.agents[i].roles[r]);
      }
      if (cfg.agents[i].tools_enabled)
         fprintf(stderr, " (tools)");
      fprintf(stderr, "\n");
   }
}

void cmd_delegate(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
   {
      delegate_print_help();
      return;
   }

   /* Handle --help */
   if (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0)
   {
      delegate_print_help();
      return;
   }

   /* Handle --list-roles */
   if (strcmp(argv[0], "--list-roles") == 0)
   {
      delegate_list_roles();
      return;
   }

   /* Handle subcommand: aimee delegate status <task_id> */
   if (strcmp(argv[0], "status") == 0)
   {
      cmd_delegate_status(ctx, argc - 1, argv + 1);
      return;
   }

   if (argc < 2)
   {
      fprintf(stderr, "error: missing prompt\n\n");
      delegate_print_help();
      return;
   }

   static const char *bool_flags[] = {"json",         "background",    "durable",
                                      "coordination", "plan",          "dry-run",
                                      "tools",        "keep-worktree", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   const char *role = opt_pos(&opts, 0);
   const char *prompt = opt_pos(&opts, 1);
   const char *sys_prompt = opt_get(&opts, "system");
   int max_tokens = opt_get_int(&opts, "max-tokens", 0);
   int json_output = opt_has(&opts, "json");
   int background = opt_has(&opts, "background");
   int durable = opt_has(&opts, "durable");
   int coordination = opt_has(&opts, "coordination");
   int vote_count = opt_get_int(&opts, "vote", 0);
   int plan_mode = opt_has(&opts, "plan");
   int dry_run = opt_has(&opts, "dry-run");
   int force_tools = opt_has(&opts, "tools");
   const char *files_arg = opt_get(&opts, "files");
   int timeout_ms = opt_get_int(&opts, "timeout", 0);
   const char *prompt_file = opt_get(&opts, "prompt-file");
   int retry_count = opt_get_int(&opts, "retry", 0);
   const char *verify_cmd = opt_get(&opts, "verify");
   const char *context_dir = opt_get(&opts, "context-dir");
   const char *output_path = opt_get(&opts, "output");
   const char *worktree_branch = opt_get(&opts, "worktree");
   int keep_worktree = opt_has(&opts, "keep-worktree");
   (void)keep_worktree;

   /* Collect repeatable --context-file flags */
   const char *context_files[OPT_MAX_FLAGS];
   int context_file_count = 0;
   for (int i = 0; i < opts.flag_count && context_file_count < OPT_MAX_FLAGS; i++)
   {
      if (strcmp(opts.flags[i].name, "context-file") == 0 && opts.flags[i].value)
         context_files[context_file_count++] = opts.flags[i].value;
   }

   /* --prompt-file: read prompt from a file (avoids ARG_MAX limits) */
   char *file_prompt = NULL;
   if (prompt_file && prompt_file[0])
   {
      FILE *pf = fopen(prompt_file, "r");
      if (!pf)
         fatal("cannot open prompt file: %s", prompt_file);
      fseek(pf, 0, SEEK_END);
      long psz = ftell(pf);
      fseek(pf, 0, SEEK_SET);
      if (psz <= 0 || psz > 2 * 1024 * 1024)
      {
         fclose(pf);
         fatal("prompt file too large or empty (max 2MB)");
      }
      file_prompt = malloc((size_t)psz + 1);
      if (!file_prompt)
      {
         fclose(pf);
         fatal("out of memory reading prompt file");
      }
      size_t pnread = fread(file_prompt, 1, (size_t)psz, pf);
      file_prompt[pnread] = '\0';
      fclose(pf);
      if (!prompt)
         prompt = file_prompt;
      else
      {
         size_t combined_len = strlen(prompt) + pnread + 4;
         char *combined = malloc(combined_len);
         if (combined)
         {
            snprintf(combined, combined_len, "%s\n\n%s", prompt, file_prompt);
            free(file_prompt);
            file_prompt = combined;
            prompt = file_prompt;
         }
      }
   }

   if (!role || !prompt)
      fatal("usage: aimee delegate <role> \"prompt\" [--tools] [--prompt-file PATH]");

   /* Build enhanced prompt with file contents if --files specified */
   char *effective_prompt = NULL;
   if (files_arg && files_arg[0])
   {
      /* Parse comma-separated file paths */
      size_t plen = strlen(prompt);
      size_t bufcap = plen + 256;
      char files_copy[MAX_PATH_LEN * 4];
      snprintf(files_copy, sizeof(files_copy), "%s", files_arg);

      /* First pass: accumulate file contents */
      char *file_block = malloc(128 * 1024); /* 128KB for file contents */
      size_t fpos = 0;
      if (file_block)
      {
         char *saveptr = NULL;
         char *path = strtok_r(files_copy, ",", &saveptr);
         while (path)
         {
            while (*path == ' ')
               path++;
            FILE *fp = fopen(path, "r");
            if (fp)
            {
               fpos += (size_t)snprintf(file_block + fpos, 128 * 1024 - fpos,
                                        "\n--- File: %s ---\n", path);
               size_t nread = fread(file_block + fpos, 1, 128 * 1024 - fpos - 1, fp);
               fpos += nread;
               file_block[fpos] = '\0';
               fclose(fp);
            }
            else
            {
               fpos += (size_t)snprintf(file_block + fpos, 128 * 1024 - fpos,
                                        "\n--- File: %s (not found) ---\n", path);
            }
            path = strtok_r(NULL, ",", &saveptr);
         }

         bufcap = plen + fpos + 128;
         effective_prompt = malloc(bufcap);
         if (effective_prompt)
         {
            snprintf(effective_prompt, bufcap, "%s\n\n# Pre-loaded File Contents\n%s", prompt,
                     file_block);
         }
         free(file_block);
      }
   }

   /* --context-dir: bundle all source files from a directory into the prompt */
   if (context_dir && context_dir[0] && !effective_prompt)
   {
      size_t plen = strlen(prompt);
      char *ctx_block = malloc(512 * 1024);
      size_t cpos = 0;
      if (ctx_block)
      {
         DIR *d = opendir(context_dir);
         if (d)
         {
            static const char *src_exts[] = {".c",   ".h",    ".js",   ".ts", ".py", ".go",
                                             ".rs",  ".java", ".cs",   ".rb", ".sh", ".yaml",
                                             ".yml", ".json", ".toml", ".md", NULL};
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && cpos < 480 * 1024)
            {
               if (ent->d_name[0] == '.')
                  continue;
               const char *dot = strrchr(ent->d_name, '.');
               if (!dot)
                  continue;
               int is_src = 0;
               for (int si = 0; src_exts[si]; si++)
               {
                  if (strcmp(dot, src_exts[si]) == 0)
                  {
                     is_src = 1;
                     break;
                  }
               }
               if (!is_src)
                  continue;

               char fpath[MAX_PATH_LEN];
               snprintf(fpath, sizeof(fpath), "%s/%s", context_dir, ent->d_name);
               struct stat fst;
               if (stat(fpath, &fst) != 0 || !S_ISREG(fst.st_mode) || fst.st_size > 64 * 1024)
                  continue;

               FILE *fp = fopen(fpath, "r");
               if (!fp)
                  continue;
               cpos += (size_t)snprintf(ctx_block + cpos, 512 * 1024 - cpos, "\n--- File: %s ---\n",
                                        ent->d_name);
               size_t nread = fread(ctx_block + cpos, 1, 512 * 1024 - cpos - 1, fp);
               cpos += nread;
               ctx_block[cpos] = '\0';
               fclose(fp);
            }
            closedir(d);
         }

         if (cpos > 0)
         {
            size_t bufcap = plen + cpos + 128;
            effective_prompt = malloc(bufcap);
            if (effective_prompt)
            {
               snprintf(effective_prompt, bufcap, "%s\n\n# Pre-loaded Directory Contents (%s)\n%s",
                        prompt, context_dir, ctx_block);
            }
         }
         free(ctx_block);
      }
   }

   /* --context-file: append individually-specified files to the prompt.
    * Applied after --files and --context-dir in the order provided. */
   if (context_file_count > 0)
   {
      const char *base = effective_prompt ? effective_prompt : prompt;
      size_t blen = strlen(base);
      char *cf_block = malloc(128 * 1024);
      size_t cfpos = 0;
      if (cf_block)
      {
         for (int i = 0; i < context_file_count && cfpos < 120 * 1024; i++)
         {
            FILE *fp = fopen(context_files[i], "r");
            if (fp)
            {
               cfpos += (size_t)snprintf(cf_block + cfpos, 128 * 1024 - cfpos,
                                         "\n--- File: %s ---\n", context_files[i]);
               size_t nread = fread(cf_block + cfpos, 1, 128 * 1024 - cfpos - 1, fp);
               cfpos += nread;
               cf_block[cfpos] = '\0';
               fclose(fp);
            }
            else
            {
               cfpos += (size_t)snprintf(cf_block + cfpos, 128 * 1024 - cfpos,
                                         "\n--- File: %s (not found) ---\n", context_files[i]);
            }
         }

         if (cfpos > 0)
         {
            size_t bufcap = blen + cfpos + 128;
            char *combined = malloc(bufcap);
            if (combined)
            {
               snprintf(combined, bufcap, "%s\n\n# Context Files\n%s", base, cf_block);
               free(effective_prompt);
               effective_prompt = combined;
            }
         }
         free(cf_block);
      }
   }

   const char *final_prompt = effective_prompt ? effective_prompt : prompt;

   /* --worktree BRANCH: create a temporary git worktree for isolated branch work.
    * The delegate runs with CWD set to the worktree so all tool_bash commands
    * (git rebase, git push, etc.) operate on the isolated copy.
    * Cleaned up after delegate completes unless --keep-worktree is set. */
   char worktree_path[MAX_PATH_LEN] = "";
   char original_cwd[MAX_PATH_LEN] = "";
   if (worktree_branch && worktree_branch[0])
   {
      if (!getcwd(original_cwd, sizeof(original_cwd)))
         fatal("cannot get current working directory");

      /* Generate a unique worktree directory under the aimee worktrees area */
      char wt_base[MAX_PATH_LEN];
      snprintf(wt_base, sizeof(wt_base), "%s/delegate-worktrees", config_default_dir());
      mkdir(wt_base, 0700);

      char wt_id[32];
      generate_task_id(wt_id, sizeof(wt_id));
      snprintf(worktree_path, sizeof(worktree_path), "%s/%s", wt_base, wt_id);

      /* Detect workspace root (nearest .git directory) */
      char ws_root[MAX_PATH_LEN];
      snprintf(ws_root, sizeof(ws_root), "%s", original_cwd);
      int found_git = 0;
      for (int depth = 0; depth < 20; depth++)
      {
         char git_dir[MAX_PATH_LEN];
         snprintf(git_dir, sizeof(git_dir), "%s/.git", ws_root);
         struct stat gst;
         if (stat(git_dir, &gst) == 0)
         {
            found_git = 1;
            break;
         }
         char *sl = strrchr(ws_root, '/');
         if (!sl || sl == ws_root)
            break;
         *sl = '\0';
      }
      if (!found_git)
         fatal("--worktree requires a git repository (no .git found above cwd)");

      /* Create the worktree on the specified branch */
      char *wt_out = NULL;
      const char *wt_argv[] = {"git", "-C",          ws_root,         "worktree",
                               "add", worktree_path, worktree_branch, NULL};
      int wt_rc = safe_exec_capture(wt_argv, &wt_out, 1024);
      if (wt_rc != 0)
      {
         /* Branch might not exist locally -- try creating from origin */
         free(wt_out);
         wt_out = NULL;
         char origin_ref[256];
         snprintf(origin_ref, sizeof(origin_ref), "origin/%s", worktree_branch);
         const char *wt_argv2[] = {"git",         "-C", ws_root,         "worktree", "add",
                                   worktree_path, "-b", worktree_branch, origin_ref, NULL};
         wt_rc = safe_exec_capture(wt_argv2, &wt_out, 1024);
         if (wt_rc != 0)
            fatal("failed to create worktree for branch '%s': %s", worktree_branch,
                  wt_out ? wt_out : "unknown error");
      }
      free(wt_out);
      fprintf(stderr, "aimee: created delegate worktree at %s (branch: %s)\n", worktree_path,
              worktree_branch);

      if (chdir(worktree_path) != 0)
         fatal("cannot chdir to worktree: %s", worktree_path);
   }

   /* Apply per-delegate timeout override */
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
      fatal("no agents configured. Run 'aimee agent add' first.");

   if (timeout_ms > 0)
   {
      for (int i = 0; i < cfg.agent_count; i++)
         cfg.agents[i].timeout_ms = timeout_ms;
   }

   if (dry_run)
   {
      agent_t *ag = agent_route(&cfg, role);
      if (!ag)
         fatal("no agent available for role '%s'", role);

      sqlite3 *db = db_open(NULL);
      if (!db)
         fatal("cannot open database");

      char *assembled = agent_build_exec_context(db, ag, &cfg.network, sys_prompt);
      fprintf(stderr, "--- Dry Run ---\n");
      fprintf(stderr, "Agent:  %s\n", ag->name);
      fprintf(stderr, "Model:  %s\n", ag->model);
      fprintf(stderr, "Role:   %s\n", role);
      fprintf(stderr, "\n--- System Prompt ---\n%s\n", assembled ? assembled : "(none)");
      fprintf(stderr, "\n--- User Prompt ---\n%s\n", final_prompt);
      fprintf(stderr, "\n--- Tools: %s ---\n", force_tools ? "FORCED" : "config-based");
      if (worktree_branch)
         fprintf(stderr, "\n--- Worktree: %s (branch: %s) ---\n", worktree_path, worktree_branch);
      free(assembled);
      db_stmt_cache_clear();
      db_close(db);
      return;
   }

   if (background)
   {
      /* Fork a child to run the agent, parent returns immediately with task ID */
      char tasks_dir[MAX_PATH_LEN];
      snprintf(tasks_dir, sizeof(tasks_dir), "%s/tasks", config_default_dir());
      mkdir(tasks_dir, 0700);

      char task_id[64];
      generate_task_id(task_id, sizeof(task_id));
      char result_path[MAX_PATH_LEN];
      snprintf(result_path, sizeof(result_path), "%s/%s.json", tasks_dir, task_id);

      pid_t pid = fork();
      if (pid < 0)
         fatal("fork failed");

      if (pid > 0)
      {
         /* Write .pid file so delegate status can distinguish running from not_found */
         char pid_path[MAX_PATH_LEN];
         snprintf(pid_path, sizeof(pid_path), "%s/%s.pid", tasks_dir, task_id);
         FILE *pf = fopen(pid_path, "w");
         if (pf)
         {
            fprintf(pf, "%d\n", pid);
            fclose(pf);
         }
         /* Parent: print task info and exit */
         if (json_output)
         {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "task_id", task_id);
            cJSON_AddStringToObject(obj, "result_path", result_path);
            cJSON_AddStringToObject(obj, "status", "running");
            char *json = cJSON_Print(obj);
            if (json)
            {
               printf("%s\n", json);
               free(json);
            }
            cJSON_Delete(obj);
         }
         else
         {
            printf("task_id: %s\nresult: %s\n", task_id, result_path);
         }
         return;
      }

      /* Child: detach, run agent, write result file */
      setsid();
      fclose(stdin);

      agent_http_init();
      sqlite3 *db = db_open(NULL);
      agent_result_t result;
      memset(&result, 0, sizeof(result));
      if (force_tools)
         agent_run_with_tools(db, &cfg, role, sys_prompt, final_prompt, max_tokens, &result);
      else
         agent_run(db, &cfg, role, sys_prompt, final_prompt, max_tokens, &result);
      agent_http_cleanup();
      write_result_json(result_path, &result);
      /* Clean up .pid file now that result is written */
      char pid_path[MAX_PATH_LEN];
      snprintf(pid_path, sizeof(pid_path), "%s/%s.pid", tasks_dir, task_id);
      unlink(pid_path);
      delegate_worktree_cleanup(worktree_path, original_cwd, keep_worktree);
      free(result.response);
      free(effective_prompt);
      free(file_prompt);
      db_stmt_cache_clear();
      db_close(db);
      _exit(0);
   }

   agent_http_init();
   ctx->db = db_open(NULL);
   if (!ctx->db)
      fatal("cannot open database");
   sqlite3 *db = ctx->db;

   /* Create durable job record if requested */
   int job_id = 0;
   if (durable)
   {
      job_id = agent_job_create(db, role, prompt, cfg.default_agent);
      if (job_id > 0)
      {
         agent_set_durable_job(db, job_id);
         if (json_output)
            fprintf(stderr, "aimee: durable job created: %d\n", job_id);
      }
   }

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = -1;
   int attempts = retry_count + 1;

   /* Generate a delegation ID for checkpoint tracking */
   char deleg_id[32];
   generate_task_id(deleg_id, sizeof(deleg_id));

   /* Working prompt that may be augmented with prior attempt context */
   char *working_prompt = NULL;

   for (int attempt = 0; attempt < attempts; attempt++)
   {
      if (attempt > 0)
      {
         fprintf(stderr, "aimee: retrying delegate (attempt %d/%d)...\n", attempt + 1, attempts);
         agent_http_cleanup();
         agent_http_init();

         /* Inject prior attempt context from checkpoint */
         char ckpt_steps[2048] = "";
         char ckpt_error[512] = "";
         char ckpt_output[2048] = "";
         if (delegation_checkpoint_load(db, deleg_id, ckpt_steps, sizeof(ckpt_steps), ckpt_error,
                                        sizeof(ckpt_error), ckpt_output, sizeof(ckpt_output)) == 0)
         {
            free(working_prompt);
            size_t cap = strlen(final_prompt) + 4096;
            working_prompt = malloc(cap);
            if (working_prompt)
            {
               snprintf(working_prompt, cap,
                        "%s\n\n"
                        "[Prior attempt %d failed. Error: %s. "
                        "Partial progress: %s. "
                        "You may resume from where it left off or restart.]",
                        final_prompt, attempt, ckpt_error, ckpt_steps);
            }
         }
         memset(&result, 0, sizeof(result));
      }

      const char *prompt_to_use =
          (working_prompt && working_prompt[0]) ? working_prompt : final_prompt;

      if (coordination)
      {
         rc = agent_coordinate(db, &cfg, prompt_to_use, &result);
      }
      else if (vote_count > 0)
      {
         rc = agent_vote(db, &cfg, role, prompt_to_use, vote_count, &result);
      }
      else if (plan_mode)
      {
         agent_t *ag = agent_route(&cfg, role);
         if (!ag)
            fatal("no agent available for role '%s'", role);
         rc = agent_execute_with_plan(db, ag, &cfg.network, sys_prompt, prompt_to_use, max_tokens,
                                      0.3, &result);
      }
      else if (force_tools)
      {
         rc = agent_run_with_tools(db, &cfg, role, sys_prompt, prompt_to_use, max_tokens, &result);
      }
      else
      {
         rc = agent_run(db, &cfg, role, sys_prompt, prompt_to_use, max_tokens, &result);
      }

      if (rc == 0)
         break;

      /* Save checkpoint on failure for potential retry */
      {
         char steps_json[2048];
         snprintf(steps_json, sizeof(steps_json), "[\"turns: %d\", \"tool_calls: %d\"]",
                  result.turns, result.tool_calls);
         const char *last_out = result.response ? result.response : result.error;
         /* Truncate last_output to 1024 chars for checkpoint */
         char truncated[1024];
         snprintf(truncated, sizeof(truncated), "%.1023s", last_out);
         delegation_checkpoint_save(db, deleg_id, "", attempt, steps_json, truncated, result.error);
      }

      if (strstr(result.error, "HTTP") || strstr(result.error, "timeout"))
      {
         if (attempt < attempts - 1)
         {
            free(result.response);
            result.response = NULL;
            continue;
         }
      }
      break;
   }

   free(working_prompt);

   agent_http_cleanup();

   /* Update durable job with result */
   if (durable && job_id > 0)
   {
      agent_job_update(db, job_id, rc == 0 ? "done" : "failed", result.turns,
                       result.response ? result.response : result.error);
      agent_set_durable_job(NULL, 0);
   }

   /* --verify: run a verification command after successful delegation */
   if (rc == 0 && verify_cmd && verify_cmd[0])
   {
      const char *verify_argv[] = {"/bin/sh", "-c", verify_cmd, NULL};
      char *verify_out = NULL;
      int verify_rc = safe_exec_capture(verify_argv, &verify_out, AGENT_TOOL_OUTPUT_MAX);
      free(verify_out);
      if (verify_rc != 0)
      {
         fprintf(stderr, "aimee: verify command failed (exit %d): %s\n", verify_rc, verify_cmd);
         if (json_output)
         {
            cJSON *obj = agent_result_to_json(&result);
            cJSON_AddBoolToObject(obj, "verify_passed", 0);
            cJSON_AddNumberToObject(obj, "verify_exit_code", verify_rc);
            char *json = cJSON_Print(obj);
            if (json)
            {
               printf("%s\n", json);
               free(json);
            }
            cJSON_Delete(obj);
         }
         else
         {
            if (result.response)
               printf("%s\n", result.response);
            fprintf(stderr, "aimee: WARN: delegate output above may contain errors "
                            "(verify failed)\n");
         }
         delegate_worktree_cleanup(worktree_path, original_cwd, keep_worktree);
         free(result.response);
         free(effective_prompt);
         free(file_prompt);
         ctx_db_close(ctx);
         exit(3);
      }
      else if (!json_output)
      {
         fprintf(stderr, "aimee: verify passed: %s\n", verify_cmd);
      }
   }
   else if (rc == 0)
   {
      /* Auto-verify from app config: tool verifies delegate's changes */
      config_t app_cfg;
      config_load(&app_cfg);
      if (app_cfg.cross_verify && app_cfg.verify_cmd[0])
      {
         fprintf(stderr, "aimee: cross-verify: running %s\n", app_cfg.verify_cmd);
         const char *cv_argv[] = {"/bin/sh", "-c", app_cfg.verify_cmd, NULL};
         char *cv_out = NULL;
         int cv_rc = safe_exec_capture(cv_argv, &cv_out, AGENT_TOOL_OUTPUT_MAX);
         free(cv_out);
         if (cv_rc != 0)
            fprintf(stderr, "aimee: cross-verify FAILED (exit %d)\n", cv_rc);
         else if (!json_output)
            fprintf(stderr, "aimee: cross-verify passed\n");
      }
   }

   if (json_output)
   {
      cJSON *obj = agent_result_to_json(&result);
      if (verify_cmd && verify_cmd[0] && rc == 0)
         cJSON_AddBoolToObject(obj, "verify_passed", 1);
      char *json = cJSON_Print(obj);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(obj);
   }
   else if (rc == 0 && result.response)
   {
      if (output_path && output_path[0])
      {
         /* --output: write response to file, creating parent dirs if needed */
         char parent[MAX_PATH_LEN];
         snprintf(parent, sizeof(parent), "%s", output_path);
         char *sl = strrchr(parent, '/');
         if (sl && sl != parent)
         {
            *sl = '\0';
            /* Simple recursive mkdir: walk forward creating each component */
            for (char *p = parent + 1; *p; p++)
            {
               if (*p == '/')
               {
                  *p = '\0';
                  mkdir(parent, 0755);
                  *p = '/';
               }
            }
            mkdir(parent, 0755);
         }
         FILE *of = fopen(output_path, "w");
         if (!of)
         {
            fprintf(stderr, "aimee: cannot write to --output path: %s\n", output_path);
            delegate_worktree_cleanup(worktree_path, original_cwd, keep_worktree);
            free(result.response);
            free(effective_prompt);
            free(file_prompt);
            ctx_db_close(ctx);
            exit(2);
         }
         fputs(result.response, of);
         fputc('\n', of);
         fclose(of);
         fprintf(stderr, "%s\n", output_path);
      }
      else
      {
         printf("%s\n", result.response);
      }
   }
   else
   {
      fprintf(stderr, "aimee delegate failed: %s\n", result.error);
      delegate_worktree_cleanup(worktree_path, original_cwd, keep_worktree);
      free(result.response);
      free(effective_prompt);
      free(file_prompt);
      ctx_db_close(ctx);
      exit(2);
   }

   delegate_worktree_cleanup(worktree_path, original_cwd, keep_worktree);

   free(result.response);
   free(effective_prompt);
   free(file_prompt);
   ctx_db_close(ctx);
}

/* --- cmd_verify: cross-verification ---
 * Two modes:
 *   aimee verify             -- delegate verifies AI tool's changes (uses config)
 *   aimee verify --cmd CMD   -- run CMD to verify delegate's changes
 *   aimee verify enable      -- enable cross-verification
 *   aimee verify disable     -- disable cross-verification
 *   aimee verify config      -- show current cross-verify config
 *   aimee verify config --verify-cmd CMD --role R --prompt P  -- configure
 */

void cmd_verify(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   config_t cfg;
   config_load(&cfg);

   if (argc >= 1 && strcmp(argv[0], "enable") == 0)
   {
      cfg.cross_verify = 1;
      config_save(&cfg);
      printf("cross-verification enabled\n");
      return;
   }

   if (argc >= 1 && strcmp(argv[0], "disable") == 0)
   {
      cfg.cross_verify = 0;
      config_save(&cfg);
      printf("cross-verification disabled\n");
      return;
   }

   if (argc >= 1 && strcmp(argv[0], "config") == 0)
   {
      if (argc == 1)
      {
         printf("cross_verify: %s\n", cfg.cross_verify ? "enabled" : "disabled");
         printf("verify_cmd: %s\n", cfg.verify_cmd[0] ? cfg.verify_cmd : "(not set)");
         printf("verify_role: %s\n", cfg.verify_role[0] ? cfg.verify_role : "review");
         printf("verify_prompt: %s\n", cfg.verify_prompt[0] ? cfg.verify_prompt : "(default)");
         return;
      }
      /* Configure: --verify-cmd, --role, --prompt */
      for (int i = 1; i < argc; i++)
      {
         if (strcmp(argv[i], "--verify-cmd") == 0 && i + 1 < argc)
            snprintf(cfg.verify_cmd, sizeof(cfg.verify_cmd), "%s", argv[++i]);
         else if (strcmp(argv[i], "--role") == 0 && i + 1 < argc)
            snprintf(cfg.verify_role, sizeof(cfg.verify_role), "%s", argv[++i]);
         else if (strcmp(argv[i], "--prompt") == 0 && i + 1 < argc)
            snprintf(cfg.verify_prompt, sizeof(cfg.verify_prompt), "%s", argv[++i]);
      }
      config_save(&cfg);
      printf("cross-verify config updated\n");
      return;
   }

   /* Default: delegate verifies current changes */
   if (!cfg.cross_verify)
   {
      fprintf(stderr, "cross-verification is disabled. Run: aimee verify enable\n");
      return;
   }

   /* Step 1: run verify_cmd if set (compilation/tests) */
   if (cfg.verify_cmd[0])
   {
      fprintf(stderr, "aimee: running verify command: %s\n", cfg.verify_cmd);
      const char *cmd_argv[] = {"/bin/sh", "-c", cfg.verify_cmd, NULL};
      char *cmd_out = NULL;
      int cmd_rc = safe_exec_capture(cmd_argv, &cmd_out, AGENT_TOOL_OUTPUT_MAX);
      if (cmd_rc != 0)
      {
         fprintf(stderr, "aimee: verify command failed (exit %d)\n", cmd_rc);
         if (cmd_out && cmd_out[0])
            fprintf(stderr, "%s\n", cmd_out);
         free(cmd_out);
         exit(3);
      }
      free(cmd_out);
      fprintf(stderr, "aimee: verify command passed\n");
   }

   /* Step 2: delegate a review to an agent */
   const char *role = cfg.verify_role[0] ? cfg.verify_role : "review";

   /* Build the review prompt: get recent git diff as context */
   const char *diff_argv[] = {"git", "diff", "HEAD~1", NULL};
   char *diff_out = NULL;
   safe_exec_capture(diff_argv, &diff_out, AGENT_TOOL_OUTPUT_MAX);

   char *review_prompt = NULL;
   const char *base_prompt =
       cfg.verify_prompt[0]
           ? cfg.verify_prompt
           : "Review these code changes for bugs, security issues, and correctness. "
             "If everything looks good, say LGTM. If you find problems, list them.";

   if (diff_out && diff_out[0])
   {
      size_t plen = strlen(base_prompt) + strlen(diff_out) + 32;
      review_prompt = malloc(plen);
      if (review_prompt)
         snprintf(review_prompt, plen, "%s\n\nDiff:\n%s", base_prompt, diff_out);
   }
   free(diff_out);

   const char *final_prompt = review_prompt ? review_prompt : base_prompt;

   agent_config_t acfg;
   if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
   {
      fprintf(stderr, "no agents configured, skipping delegate review\n");
      free(review_prompt);
      return;
   }

   agent_http_init();
   sqlite3 *db = db_open(NULL);

   agent_result_t result;
   memset(&result, 0, sizeof(result));
   int rc = agent_run(db, &acfg, role, NULL, final_prompt, 0, &result);
   agent_http_cleanup();

   if (rc == 0 && result.response)
   {
      printf("%s\n", result.response);
   }
   else
   {
      fprintf(stderr, "aimee: delegate review failed: %s\n", result.error);
   }

   free(result.response);
   free(review_prompt);
   db_stmt_cache_clear();
   if (db)
      db_close(db);
}

/* --- cmd_delegate_status --- */

void cmd_delegate_status(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
      fatal("usage: aimee delegate status <task_id>");

   const char *task_id = argv[0];
   char result_path[MAX_PATH_LEN];
   snprintf(result_path, sizeof(result_path), "%s/tasks/%s.json", config_default_dir(), task_id);

   struct stat st;
   if (stat(result_path, &st) != 0)
   {
      /* No result file. Check if a tasks directory entry exists to distinguish
       * "never existed" from "still running". Look for a .pid file. */
      char pid_path[MAX_PATH_LEN];
      snprintf(pid_path, sizeof(pid_path), "%s/tasks/%s.pid", config_default_dir(), task_id);
      if (stat(pid_path, &st) == 0)
         printf("status: running\ntask_id: %s\n", task_id);
      else
         printf("status: not_found\ntask_id: %s\n", task_id);
      return;
   }

   /* Result file exists, read and display it */
   FILE *f = fopen(result_path, "r");
   if (!f)
   {
      printf("status: unknown\ntask_id: %s\n", task_id);
      return;
   }

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      printf("status: error\ntask_id: %s\n", task_id);
      return;
   }

   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   data[nread] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(data);
   free(data);

   if (!root)
   {
      printf("status: error\ntask_id: %s\n", task_id);
      return;
   }

   cJSON *success = cJSON_GetObjectItem(root, "success");
   cJSON *turns = cJSON_GetObjectItem(root, "turns");
   cJSON *tool_calls = cJSON_GetObjectItem(root, "tool_calls");
   cJSON *response = cJSON_GetObjectItem(root, "response");
   cJSON *error = cJSON_GetObjectItem(root, "error");

   printf("status: done\ntask_id: %s\n", task_id);
   if (success)
      printf("success: %s\n", cJSON_IsTrue(success) ? "true" : "false");
   if (turns && cJSON_IsNumber(turns))
      printf("turns: %d\n", turns->valueint);
   if (tool_calls && cJSON_IsNumber(tool_calls))
      printf("tool_calls: %d\n", tool_calls->valueint);
   if (error && cJSON_IsString(error) && error->valuestring[0])
      printf("error: %s\n", error->valuestring);
   if (response && cJSON_IsString(response))
      printf("response: %.200s\n", response->valuestring);

   cJSON_Delete(root);
}

/* --- cmd_dispatch (formerly cmd_queue) --- */

void cmd_dispatch(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   /* Run multiple tasks in parallel via agents */
   if (argc < 1)
      fatal("usage: aimee dispatch '<json tasks array>' [--background]\n"
            "  format: [{\"role\":\"...\",\"prompt\":\"...\"}]");

   static const char *bool_flags[] = {"background", "tools", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   int background = opt_has(&opts, "background");
   int global_tools = opt_has(&opts, "tools");
   int global_timeout = opt_get_int(&opts, "timeout", 0);
   const char *tasks_arg = opt_pos(&opts, 0);

   if (!tasks_arg)
      fatal("usage: aimee queue '<json tasks array>'");

   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
      fatal("no agents configured");

   cJSON *tasks_json = cJSON_Parse(tasks_arg);
   if (!tasks_json || !cJSON_IsArray(tasks_json))
      fatal("invalid JSON tasks array");

   int n = cJSON_GetArraySize(tasks_json);
   if (n <= 0 || n > 512)
      fatal("task count must be 1-512");

   if (background)
   {
      /* Fire-and-forget: fork one child per task, return task IDs immediately */
      char tasks_dir[MAX_PATH_LEN];
      snprintf(tasks_dir, sizeof(tasks_dir), "%s/tasks", config_default_dir());
      mkdir(tasks_dir, 0700);

      cJSON *id_arr = cJSON_CreateArray();

      for (int i = 0; i < n; i++)
      {
         cJSON *t = cJSON_GetArrayItem(tasks_json, i);
         cJSON *r = cJSON_GetObjectItem(t, "role");
         cJSON *p = cJSON_GetObjectItem(t, "prompt");
         cJSON *s = cJSON_GetObjectItem(t, "system");
         cJSON *ft = cJSON_GetObjectItem(t, "tools");
         cJSON *to = cJSON_GetObjectItem(t, "timeout");
         const char *role = (r && cJSON_IsString(r)) ? r->valuestring : "execute";
         const char *prompt = (p && cJSON_IsString(p)) ? p->valuestring : "";
         const char *sys = (s && cJSON_IsString(s)) ? s->valuestring : NULL;
         int use_tools = global_tools || (ft && cJSON_IsTrue(ft));
         int task_timeout = (to && cJSON_IsNumber(to)) ? to->valueint : global_timeout;

         char task_id[64];
         generate_task_id(task_id, sizeof(task_id));
         char result_path[MAX_PATH_LEN];
         snprintf(result_path, sizeof(result_path), "%s/%s.json", tasks_dir, task_id);

         pid_t pid = fork();
         if (pid < 0)
         {
            fprintf(stderr, "aimee: fork failed for task %d\n", i);
            continue;
         }

         if (pid == 0)
         {
            /* Child: run agent and write result */
            setsid();
            fclose(stdin);
            agent_http_init();
            sqlite3 *db = db_open(NULL);

            /* Apply per-task timeout */
            if (task_timeout > 0)
            {
               for (int ai = 0; ai < cfg.agent_count; ai++)
                  cfg.agents[ai].timeout_ms = task_timeout;
            }

            agent_result_t result;
            memset(&result, 0, sizeof(result));
            if (use_tools)
               agent_run_with_tools(db, &cfg, role, sys, prompt, 0, &result);
            else
               agent_run(db, &cfg, role, sys, prompt, 0, &result);
            agent_http_cleanup();
            write_result_json(result_path, &result);
            free(result.response);
            db_stmt_cache_clear();
            db_close(db);
            _exit(0);
         }

         /* Parent: record task ID */
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "task_id", task_id);
         cJSON_AddStringToObject(entry, "role", role);
         cJSON_AddItemToArray(id_arr, entry);
      }

      char *json = cJSON_Print(id_arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(id_arr);
      cJSON_Delete(tasks_json);
      return;
   }

   /* Synchronous: run all tasks in parallel threads, wait for completion */
   agent_task_t *tasks = calloc((size_t)n, sizeof(agent_task_t));
   agent_result_t *results = calloc((size_t)n, sizeof(agent_result_t));
   if (!tasks || !results)
   {
      free(tasks);
      free(results);
      cJSON_Delete(tasks_json);
      fatal("memory allocation failed");
   }

   for (int i = 0; i < n; i++)
   {
      cJSON *t = cJSON_GetArrayItem(tasks_json, i);
      cJSON *r = cJSON_GetObjectItem(t, "role");
      cJSON *p = cJSON_GetObjectItem(t, "prompt");
      tasks[i].role = (r && cJSON_IsString(r)) ? r->valuestring : "execute";
      tasks[i].user_prompt = (p && cJSON_IsString(p)) ? p->valuestring : "";
      tasks[i].system_prompt = NULL;
      tasks[i].max_tokens = 0;
      tasks[i].temperature = 0.3;
   }

   agent_http_init();
   ctx->db = db_open(NULL);
   if (!ctx->db)
      fatal("cannot open database");
   sqlite3 *db = ctx->db;
   int successes = agent_run_parallel(db, &cfg, tasks, n, results);
   agent_http_cleanup();

   /* Output results */
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
   {
      cJSON *obj = agent_result_to_json(&results[i]);
      cJSON_AddStringToObject(obj, "role", tasks[i].role);
      cJSON_AddItemToArray(arr, obj);
      free(results[i].response);
   }

   char *json = cJSON_Print(arr);
   if (json)
   {
      printf("%s\n", json);
      free(json);
   }
   cJSON_Delete(arr);
   cJSON_Delete(tasks_json);
   free(tasks);
   free(results);
   ctx_db_close(ctx);
   printf("%d/%d tasks succeeded.\n", successes, n);
}

/* --- cmd_queue (deprecated alias for cmd_dispatch) --- */

void cmd_queue(app_ctx_t *ctx, int argc, char **argv)
{
   fprintf(stderr, "aimee: 'queue' is deprecated, use 'dispatch' instead\n");
   cmd_dispatch(ctx, argc, argv);
}

/* --- cmd_context --- */

void cmd_context(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   (void)argc;
   (void)argv;

   /* Print the assembled execution context */
   agent_config_t cfg;
   if (agent_load_config(&cfg) != 0 || cfg.agent_count == 0)
      fatal("no agents configured");
   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");
   agent_print_context(db, &cfg);
   ctx_db_close(ctx);
}

/* --- cmd_manifest --- */

void cmd_manifest(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
      fatal("usage: aimee manifest list|show <id>");
   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");
   if (strcmp(argv[0], "list") == 0)
   {
      /* List manifests from config dir */
      char manifest_dir[MAX_PATH_LEN];
      snprintf(manifest_dir, sizeof(manifest_dir), "%s/manifests", config_default_dir());
      printf("Manifests in %s:\n", manifest_dir);
      DIR *d = opendir(manifest_dir);
      if (d)
      {
         struct dirent *ent;
         while ((ent = readdir(d)) != NULL)
         {
            if (ent->d_name[0] != '.')
               printf("  %s\n", ent->d_name);
         }
         closedir(d);
      }
   }
   else if (strcmp(argv[0], "show") == 0 && argc >= 2)
   {
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/manifests/%s.json", config_default_dir(), argv[1]);
      FILE *f = fopen(path, "r");
      if (!f)
         fatal("manifest not found: %s", argv[1]);
      char buf[4096];
      while (fgets(buf, sizeof(buf), f))
         fputs(buf, stdout);
      fclose(f);
   }
   ctx_db_close(ctx);
}

/* --- cmd_trace --- */

void cmd_trace(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;

   if (argc < 1)
      fatal("usage: aimee trace list|show <turn>");
   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");

   if (strcmp(argv[0], "list") == 0)
   {
      static const char *sql = "SELECT id, turn, direction, tool_name, created_at"
                               " FROM execution_trace ORDER BY id DESC LIMIT 50";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         printf("%-6s %-5s %-12s %-16s %s\n", "ID", "Turn", "Direction", "Tool", "Time");
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            const char *tn = (const char *)sqlite3_column_text(stmt, 3);
            printf("%-6d %-5d %-12s %-16s %s\n", sqlite3_column_int(stmt, 0),
                   sqlite3_column_int(stmt, 1), (const char *)sqlite3_column_text(stmt, 2),
                   tn ? tn : "--", (const char *)sqlite3_column_text(stmt, 4));
         }
      }
   }
   else if (strcmp(argv[0], "show") == 0 && argc >= 2)
   {
      int trace_id = atoi(argv[1]);
      static const char *sql = "SELECT turn, direction, content, tool_name, tool_args, tool_result"
                               " FROM execution_trace WHERE id = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_int(stmt, 1, trace_id);
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            printf("Turn: %d\n", sqlite3_column_int(stmt, 0));
            printf("Direction: %s\n", (const char *)sqlite3_column_text(stmt, 1));
            const char *content = (const char *)sqlite3_column_text(stmt, 2);
            if (content && content[0])
               printf("Content:\n%.*s\n", 2000, content);
            const char *tn = (const char *)sqlite3_column_text(stmt, 3);
            if (tn)
               printf("Tool: %s\n", tn);
            const char *ta = (const char *)sqlite3_column_text(stmt, 4);
            if (ta)
               printf("Args: %s\n", ta);
            const char *tr = (const char *)sqlite3_column_text(stmt, 5);
            if (tr)
               printf("Result:\n%.*s\n", 2000, tr);
         }
         else
         {
            printf("Trace entry %d not found.\n", trace_id);
         }
      }
   }

   ctx_db_close(ctx);
}

/* --- cmd_jobs --- */

void cmd_jobs(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("usage: aimee jobs list|status <id>|cancel <id>");
   sqlite3 *db = ctx_db_open(ctx);
   if (!db)
      fatal("cannot open database");

   if (strcmp(argv[0], "list") == 0)
   {
      static const char *sql = "SELECT id, role, status, cursor, agent_name, created_at"
                               " FROM agent_jobs ORDER BY id DESC LIMIT 20";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         printf("%-6s %-12s %-10s %-6s %-12s %s\n", "ID", "Role", "Status", "Turn", "Agent",
                "Created");
         sqlite3_reset(stmt);
         while (sqlite3_step(stmt) == SQLITE_ROW)
         {
            const char *cur = (const char *)sqlite3_column_text(stmt, 3);
            printf("%-6d %-12s %-10s %-6s %-12s %s\n", sqlite3_column_int(stmt, 0),
                   (const char *)sqlite3_column_text(stmt, 1),
                   (const char *)sqlite3_column_text(stmt, 2), cur ? cur : "--",
                   (const char *)sqlite3_column_text(stmt, 4),
                   (const char *)sqlite3_column_text(stmt, 5));
         }
      }
   }
   else if (strcmp(argv[0], "status") == 0 && argc >= 2)
   {
      int jid = atoi(argv[1]);
      static const char *sql = "SELECT id, role, prompt, status, cursor, agent_name,"
                               " result, heartbeat_at, created_at, updated_at"
                               " FROM agent_jobs WHERE id = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_int(stmt, 1, jid);
         if (sqlite3_step(stmt) == SQLITE_ROW)
         {
            printf("Job #%d\n", sqlite3_column_int(stmt, 0));
            printf("Role:      %s\n", (const char *)sqlite3_column_text(stmt, 1));
            printf("Prompt:    %.*s\n", 200, (const char *)sqlite3_column_text(stmt, 2));
            printf("Status:    %s\n", (const char *)sqlite3_column_text(stmt, 3));
            const char *cur = (const char *)sqlite3_column_text(stmt, 4);
            if (cur)
               printf("Cursor:    turn %s\n", cur);
            printf("Agent:     %s\n", (const char *)sqlite3_column_text(stmt, 5));
            const char *res = (const char *)sqlite3_column_text(stmt, 6);
            if (res && res[0])
               printf("Result:    %.*s\n", 500, res);
            const char *hb = (const char *)sqlite3_column_text(stmt, 7);
            if (hb)
               printf("Heartbeat: %s\n", hb);
            printf("Created:   %s\n", (const char *)sqlite3_column_text(stmt, 8));
            printf("Updated:   %s\n", (const char *)sqlite3_column_text(stmt, 9));
         }
         else
         {
            printf("Job %d not found.\n", jid);
         }
      }
   }
   else if (strcmp(argv[0], "cancel") == 0 && argc >= 2)
   {
      int jid = atoi(argv[1]);
      agent_job_update(db, jid, "cancelled", 0, NULL);
      printf("Job %d cancelled.\n", jid);
   }
   else if (strcmp(argv[0], "resume") == 0 && argc >= 2)
   {
      int jid = atoi(argv[1]);
      agent_config_t acfg;
      if (agent_load_config(&acfg) != 0 || acfg.agent_count == 0)
         fatal("no agents configured");

      agent_http_init();
      agent_result_t result;
      memset(&result, 0, sizeof(result));
      int rc = agent_job_resume(db, &acfg, jid, &result);
      agent_http_cleanup();

      if (ctx->json_output)
      {
         cJSON *obj = agent_result_to_json(&result);
         char *json = cJSON_Print(obj);
         if (json)
         {
            printf("%s\n", json);
            free(json);
         }
         cJSON_Delete(obj);
      }
      else if (rc == 0 && result.response)
      {
         printf("Job %d resumed and completed.\n%s\n", jid, result.response);
      }
      else
      {
         fprintf(stderr, "Job %d resume failed: %s\n", jid, result.error);
      }
      free(result.response);
   }
   else if (strcmp(argv[0], "wait") == 0 && argc >= 2)
   {
      const char *task_id = argv[1];
      int timeout_sec = 120;
      for (int i = 2; i < argc; i++)
      {
         if (strncmp(argv[i], "--timeout=", 10) == 0)
            timeout_sec = atoi(argv[i] + 10);
      }

      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/tasks/%s.json", config_default_dir(), task_id);

      int elapsed = 0;
      int done = 0;
      while (elapsed < timeout_sec)
      {
         FILE *f = fopen(path, "r");
         if (f)
         {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > 2)
            {
               fseek(f, 0, SEEK_SET);
               char *buf = malloc((size_t)sz + 1);
               if (buf)
               {
                  size_t nread = fread(buf, 1, (size_t)sz, f);
                  buf[nread] = '\0';
                  fclose(f);
                  printf("%s\n", buf);
                  free(buf);
                  done = 1;
                  break;
               }
            }
            fclose(f);
         }
         usleep(500000);
         elapsed++;
      }
      if (!done)
      {
         fprintf(stderr, "timeout waiting for %s (%ds)\n", task_id, timeout_sec);
         ctx_db_close(ctx);
         exit(1);
      }
   }
   else if (strcmp(argv[0], "collect") == 0 && argc >= 2)
   {
      /* Collect results from multiple background tasks.
       * Usage: aimee jobs collect <id1> [id2] [id3] ... [--timeout=N]
       * Waits for all tasks to complete, returns JSON array of results. */
      int timeout_sec = 120;
      int task_count = 0;
      const char *task_ids[512];

      for (int i = 1; i < argc; i++)
      {
         if (strncmp(argv[i], "--timeout=", 10) == 0)
            timeout_sec = atoi(argv[i] + 10);
         else if (task_count < 512)
            task_ids[task_count++] = argv[i];
      }

      if (task_count == 0)
         fatal("usage: aimee jobs collect <id1> [id2...] [--timeout=N]");

      /* Poll all tasks until all complete or timeout */
      char *task_results[512];
      int task_done[512];
      memset(task_results, 0, sizeof(task_results));
      memset(task_done, 0, sizeof(task_done));

      int all_done = 0;
      int elapsed = 0;
      while (!all_done && elapsed < timeout_sec)
      {
         all_done = 1;
         for (int i = 0; i < task_count; i++)
         {
            if (task_done[i])
               continue;

            char path[MAX_PATH_LEN];
            snprintf(path, sizeof(path), "%s/tasks/%s.json", config_default_dir(), task_ids[i]);

            FILE *f = fopen(path, "r");
            if (f)
            {
               fseek(f, 0, SEEK_END);
               long sz = ftell(f);
               if (sz > 2)
               {
                  fseek(f, 0, SEEK_SET);
                  char *buf = malloc((size_t)sz + 1);
                  if (buf)
                  {
                     size_t nread = fread(buf, 1, (size_t)sz, f);
                     buf[nread] = '\0';
                     task_results[i] = buf;
                     task_done[i] = 1;
                  }
               }
               fclose(f);
            }

            if (!task_done[i])
               all_done = 0;
         }

         if (!all_done)
         {
            usleep(500000);
            elapsed++;
         }
      }

      /* Build JSON array of results */
      cJSON *arr = cJSON_CreateArray();
      int success_count = 0;
      for (int i = 0; i < task_count; i++)
      {
         cJSON *entry = cJSON_CreateObject();
         cJSON_AddStringToObject(entry, "task_id", task_ids[i]);

         if (task_results[i])
         {
            cJSON *result = cJSON_Parse(task_results[i]);
            if (result)
            {
               cJSON_AddItemToObject(entry, "result", result);
               cJSON_AddStringToObject(entry, "status", "done");
               success_count++;
            }
            else
            {
               cJSON_AddStringToObject(entry, "status", "error");
               cJSON_AddStringToObject(entry, "error", "failed to parse result");
            }
            free(task_results[i]);
         }
         else
         {
            cJSON_AddStringToObject(entry, "status", "timeout");
         }
         cJSON_AddItemToArray(arr, entry);
      }

      char *json = cJSON_Print(arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(arr);

      if (success_count < task_count)
      {
         fprintf(stderr, "%d/%d tasks completed (%d timed out)\n", success_count, task_count,
                 task_count - success_count);
      }
   }

   ctx_db_close(ctx);
}
