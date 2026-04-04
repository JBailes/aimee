/* cmd_table.c: command table and help, shared by monolith and server */
#include "aimee.h"
#include "commands.h"

int command_is_alias(const char *name)
{
   return strcmp(name, "+") == 0 || strcmp(name, "-") == 0 || strcmp(name, "quickstart") == 0 ||
          strcmp(name, "queue") == 0;
}

int command_is_hidden_default(const char *name)
{
   return command_is_alias(name) || strcmp(name, "hooks") == 0 ||
          strcmp(name, "session-start") == 0 || strcmp(name, "launch") == 0 ||
          strcmp(name, "wrapup") == 0 || strcmp(name, "help") == 0;
}

void cmd_help(app_ctx_t *ctx, int argc, char **argv)
{
   (void)ctx;
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee help <command>\n");
      fprintf(stderr, "Common shortcuts: aimee use <provider>, aimee provider [name], "
                      "aimee verify on|off\n");
      return;
   }

   const char *target = argv[0];
   if (strcmp(target, "use") == 0 || strcmp(target, "provider") == 0)
   {
      fprintf(stderr, "aimee use <provider>\n"
                      "aimee provider [name]\n"
                      "\n"
                      "Shortcuts for reading or updating config.provider.\n"
                      "Examples:\n"
                      "  aimee use claude\n"
                      "  aimee provider codex\n"
                      "  aimee provider\n");
      return;
   }
   if (strcmp(target, "verify") == 0)
   {
      fprintf(stderr, "aimee verify [on|off|enable|disable|config]\n"
                      "\n"
                      "Human shortcut aliases:\n"
                      "  aimee verify on   -> aimee verify enable\n"
                      "  aimee verify off  -> aimee verify disable\n");
      return;
   }

   for (int i = 0; commands[i].name != NULL; i++)
   {
      if (strcmp(target, commands[i].name) == 0)
      {
         fprintf(stderr, "aimee %s: %s\n", commands[i].name, commands[i].help);

         const subcmd_t *subs = NULL;
         if (strcmp(target, "memory") == 0)
            subs = get_memory_subcmds();
         else if (strcmp(target, "agent") == 0)
            subs = get_agent_subcmds();
         else if (strcmp(target, "index") == 0)
            subs = get_index_subcmds();
         else if (strcmp(target, "wm") == 0)
            subs = get_wm_subcmds();
         else if (strcmp(target, "db") == 0)
            subs = get_db_subcmds();
         else if (strcmp(target, "worktree") == 0)
            subs = get_worktree_subcmds();
         else if (strcmp(target, "work") == 0)
            subs = get_work_subcmds();
         else if (strcmp(target, "branch") == 0)
            subs = get_branch_subcmds();

         if (subs)
         {
            fprintf(stderr, "\nSubcommands:\n");
            for (int j = 0; subs[j].name; j++)
            {
               if (subs[j].help)
                  fprintf(stderr, "  %-16s %s\n", subs[j].name, subs[j].help);
               else
                  fprintf(stderr, "  %s\n", subs[j].name);
            }
         }
         return;
      }
   }
   fprintf(stderr, "Unknown command: %s\n", target);
}

const command_t commands[] = {
    /* Core: session, hooks, memory, rules, config, index, delegate */
    {"init", "Initialize aimee database and config", cmd_init, CMD_TIER_CORE},
    {"setup", "Discover and index workspace projects", cmd_setup, CMD_TIER_CORE},
    {"quickstart", "Discover and index workspace projects", cmd_setup, CMD_TIER_CORE},
    {"wm", "Working memory (session-scoped scratch)", cmd_wm, CMD_TIER_CORE},
    {"index", "Code indexing (overview, map, find, ...)", cmd_index, CMD_TIER_CORE},
    {"memory", "Tiered memory management", cmd_memory, CMD_TIER_CORE},
    {"rules", "Rule management (list, generate, delete)", cmd_rules, CMD_TIER_CORE},
    {"feedback", "Record feedback (alias: +, -)", cmd_feedback_raw, CMD_TIER_CORE},
    {"+", "Record positive feedback", cmd_feedback_plus, CMD_TIER_CORE},
    {"-", "Record negative feedback", cmd_feedback_minus, CMD_TIER_CORE},
    {"hooks", "Pre/post tool hooks", cmd_hooks, CMD_TIER_CORE},
    {"session-start", "Start a new session", cmd_session_start, CMD_TIER_CORE},
    {"launch", "Session start + return launch metadata", cmd_launch, CMD_TIER_CORE},
    {"wrapup", "End-of-session processing", cmd_wrapup, CMD_TIER_CORE},
    {"mode", "Get/set session mode", cmd_mode, CMD_TIER_CORE},
    {"plan", "Switch to plan mode", cmd_plan, CMD_TIER_CORE},
    {"implement", "Switch to implement mode", cmd_implement, CMD_TIER_CORE},
    {"delegate", "Delegate a task to a sub-agent (for AI tools)", cmd_delegate, CMD_TIER_CORE},
    {"verify", "Cross-verify changes (delegate reviews tool, tool reviews delegate)", cmd_verify,
     CMD_TIER_CORE},
    {"config", "View and update configuration", cmd_config, CMD_TIER_CORE},
    {"version", "Print version", cmd_version, CMD_TIER_CORE},
    {"help", "Show help for a command", cmd_help, CMD_TIER_CORE},

    /* Advanced: workspace, worktree, agents, work queue, status */
    {"workspace", "Workspace management (add, list, remove)", cmd_workspace, CMD_TIER_ADVANCED},
    {"agent", "Sub-agent management and execution", cmd_agent, CMD_TIER_ADVANCED},
    {"context", "Print assembled execution context", cmd_context, CMD_TIER_ADVANCED},
    {"dispatch", "Run multiple tasks in parallel via agents", cmd_dispatch, CMD_TIER_ADVANCED},
    {"queue", "Deprecated alias for dispatch", cmd_queue, CMD_TIER_ADVANCED},
    {"work", "Inter-session work queue (distribute tasks across sessions)", cmd_work,
     CMD_TIER_ADVANCED},
    {"trace", "Execution trace viewer", cmd_trace, CMD_TIER_ADVANCED},
    {"jobs", "Agent job management", cmd_jobs, CMD_TIER_ADVANCED},
    {"plans", "Execution plan management (list, show, replay)", cmd_plans, CMD_TIER_ADVANCED},
    {"worktree", "Worktree management and garbage collection", cmd_worktree, CMD_TIER_ADVANCED},
    {"status", "System health overview", cmd_status, CMD_TIER_ADVANCED},
    {"usage", "Token usage statistics", cmd_usage, CMD_TIER_ADVANCED},
    {"manifest", "List or show manifests", cmd_manifest, CMD_TIER_ADVANCED},
    {"contract", "Show project contract for current directory", cmd_contract, CMD_TIER_ADVANCED},
    {"describe", "Auto-describe projects via agent analysis", cmd_describe, CMD_TIER_ADVANCED},
    {"env", "Detect environment capabilities", cmd_env, CMD_TIER_ADVANCED},
    {"doctor", "Run diagnostic checks on all subsystems", cmd_doctor, CMD_TIER_ADVANCED},

    /* Admin: dashboard, webchat, eval, import/export, db, branch, git */
    {"dashboard", "Serve the dashboard UI", cmd_dashboard, CMD_TIER_ADMIN},
    {"webchat", "Web chat + dashboard (HTTPS)", cmd_webchat, CMD_TIER_ADMIN},
    {"eval", "Eval harness (run, results)", cmd_eval, CMD_TIER_ADMIN},
    {"export", "Export state to portable JSONL format", cmd_export, CMD_TIER_ADMIN},
    {"import", "Import state from exported directory", cmd_import, CMD_TIER_ADMIN},
    {"db", "Database diagnostics and maintenance", cmd_db, CMD_TIER_ADMIN},
    {"branch", "Branch conflict analysis and cascading merge", cmd_branch, CMD_TIER_ADMIN},
    {"git", "Git and PR operations", cmd_git, CMD_TIER_ADMIN},
    {"clean", "Remove all aimee data and hooks (use --force)", NULL, CMD_TIER_ADMIN},

    {NULL, NULL, NULL, 0} /* sentinel */
};
