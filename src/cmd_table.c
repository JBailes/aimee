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
    {"init", "Initialize aimee database and config", cmd_init},
    {"setup", "Discover and index workspace projects", cmd_setup},
    {"quickstart", "Discover and index workspace projects", cmd_setup},
    {"wm", "Working memory (session-scoped scratch)", cmd_wm},
    {"workspace", "Workspace management (add, list, remove)", cmd_workspace},
    {"index", "Code indexing (overview, map, find, ...)", cmd_index},
    {"memory", "Tiered memory management", cmd_memory},
    {"rules", "Rule management (list, generate, delete)", cmd_rules},
    {"feedback", "Record feedback (alias: +, -)", cmd_feedback_raw},
    {"+", "Record positive feedback", cmd_feedback_plus},
    {"-", "Record negative feedback", cmd_feedback_minus},
    {"hooks", "Pre/post tool hooks", cmd_hooks},
    {"session-start", "Start a new session", cmd_session_start},
    {"launch", "Session start + return launch metadata", cmd_launch},
    {"wrapup", "End-of-session processing", cmd_wrapup},
    {"mode", "Get/set session mode", cmd_mode},
    {"plan", "Switch to plan mode", cmd_plan},
    {"implement", "Switch to implement mode", cmd_implement},
    {"agent", "Sub-agent management and execution", cmd_agent},
    {"delegate", "Delegate a task to a sub-agent (for AI tools)", cmd_delegate},
    {"verify", "Cross-verify changes (delegate reviews tool, tool reviews delegate)", cmd_verify},
    {"dashboard", "Serve the dashboard UI", cmd_dashboard},
    {"webchat", "Web chat + dashboard (HTTPS)", cmd_webchat},
    {"env", "Detect environment capabilities", cmd_env},
    {"manifest", "List or show manifests", cmd_manifest},
    {"context", "Print assembled execution context", cmd_context},
    {"dispatch", "Run multiple tasks in parallel via agents", cmd_dispatch},
    {"queue", "Deprecated alias for dispatch", cmd_queue},
    {"work", "Inter-session work queue (distribute tasks across sessions)", cmd_work},
    {"trace", "Execution trace viewer", cmd_trace},
    {"jobs", "Agent job management", cmd_jobs},
    {"describe", "Auto-describe projects via agent analysis", cmd_describe},
    {"contract", "Show project contract for current directory", cmd_contract},
    {"plans", "Execution plan management (list, show, replay)", cmd_plans},
    {"eval", "Eval harness (run, results)", cmd_eval},
    {"worktree", "Worktree management and garbage collection", cmd_worktree},
    {"export", "Export state to portable JSONL format", cmd_export},
    {"import", "Import state from exported directory", cmd_import},
    {"db", "Database diagnostics and maintenance", cmd_db},
    {"status", "System health overview", cmd_status},
    {"usage", "Token usage statistics", cmd_usage},
    {"config", "View and update configuration", cmd_config},
    {"branch", "Branch conflict analysis and cascading merge", cmd_branch},
    {"git", "Git and PR operations", cmd_git},
    {"version", "Print version", cmd_version},
    {"help", "Show help for a command", cmd_help},
    {NULL, NULL, NULL} /* sentinel */
};
