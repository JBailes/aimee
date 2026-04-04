#ifndef DEC_COMMANDS_H
#define DEC_COMMANDS_H 1

#include "aimee.h"

/* Subcommand dispatch table entry */
typedef struct
{
   const char *name;
   const char *help;
   void (*handler)(app_ctx_t *ctx, sqlite3 *db, int argc, char **argv);
} subcmd_t;

/* Look up and call a subcommand handler. Returns 0 on match, -1 if not found. */
int subcmd_dispatch(const subcmd_t *table, const char *name, app_ctx_t *ctx, sqlite3 *db, int argc,
                    char **argv);

/* Print subcommand usage for a parent command. */
void subcmd_usage(const char *parent, const subcmd_t *table);

/* cmd_util.c: database open/close helpers */
sqlite3 *ctx_db_open(app_ctx_t *ctx);
sqlite3 *ctx_db_open_fast(app_ctx_t *ctx);
void ctx_db_close(app_ctx_t *ctx);

/* cmd_core.c */
void ensure_mcp_json(const char *dir);
void ensure_client_integrations(void);
void cmd_init(app_ctx_t *ctx, int argc, char **argv);
void cmd_setup(app_ctx_t *ctx, int argc, char **argv);
void cmd_version(app_ctx_t *ctx, int argc, char **argv);
void cmd_mode(app_ctx_t *ctx, int argc, char **argv);
void cmd_plan(app_ctx_t *ctx, int argc, char **argv);
void cmd_implement(app_ctx_t *ctx, int argc, char **argv);
void cmd_dashboard(app_ctx_t *ctx, int argc, char **argv);
void cmd_env(app_ctx_t *ctx, int argc, char **argv);
void cmd_contract(app_ctx_t *ctx, int argc, char **argv);
void cmd_status(app_ctx_t *ctx, int argc, char **argv);
void cmd_export(app_ctx_t *ctx, int argc, char **argv);
void cmd_import(app_ctx_t *ctx, int argc, char **argv);
void cmd_workspace(app_ctx_t *ctx, int argc, char **argv);
void cmd_db(app_ctx_t *ctx, int argc, char **argv);
void cmd_worktree(app_ctx_t *ctx, int argc, char **argv);
void cmd_config(app_ctx_t *ctx, int argc, char **argv);
void cmd_git(app_ctx_t *ctx, int argc, char **argv);
void cmd_usage(app_ctx_t *ctx, int argc, char **argv);

/* cmd_memory.c */
void cmd_memory(app_ctx_t *ctx, int argc, char **argv);

/* cmd_index.c */
void cmd_index(app_ctx_t *ctx, int argc, char **argv);

/* cmd_rules.c */
void cmd_rules(app_ctx_t *ctx, int argc, char **argv);
void cmd_feedback_raw(app_ctx_t *ctx, int argc, char **argv);
void cmd_feedback_plus(app_ctx_t *ctx, int argc, char **argv);
void cmd_feedback_minus(app_ctx_t *ctx, int argc, char **argv);

/* cmd_hooks.c */
void cmd_hooks(app_ctx_t *ctx, int argc, char **argv);
void cmd_session_start(app_ctx_t *ctx, int argc, char **argv);
void cmd_launch(app_ctx_t *ctx, int argc, char **argv);
void cmd_wrapup(app_ctx_t *ctx, int argc, char **argv);

/* cmd_agent.c */
void cmd_agent(app_ctx_t *ctx, int argc, char **argv);

/* cmd_agent_delegate.c */
void cmd_delegate(app_ctx_t *ctx, int argc, char **argv);
void cmd_delegate_status(app_ctx_t *ctx, int argc, char **argv);
void cmd_verify(app_ctx_t *ctx, int argc, char **argv);

/* cmd_agent_trace.c */
void cmd_dispatch(app_ctx_t *ctx, int argc, char **argv);
void cmd_queue(app_ctx_t *ctx, int argc, char **argv);
void cmd_context(app_ctx_t *ctx, int argc, char **argv);
void cmd_manifest(app_ctx_t *ctx, int argc, char **argv);
void cmd_trace(app_ctx_t *ctx, int argc, char **argv);
void cmd_jobs(app_ctx_t *ctx, int argc, char **argv);
void cmd_plans(app_ctx_t *ctx, int argc, char **argv);
void cmd_eval(app_ctx_t *ctx, int argc, char **argv);

/* cmd_work.c */
void cmd_work(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_work_subcmds(void);
int work_queue_summary(sqlite3 *db, char *buf, size_t cap);

/* cmd_branch.c */
void cmd_branch(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_branch_subcmds(void);

/* cmd_chat.c */
void cmd_chat(app_ctx_t *ctx, int argc, char **argv);

/* cmd_core.c (webchat) */
void cmd_webchat(app_ctx_t *ctx, int argc, char **argv);

/* cmd_describe.c */
void cmd_describe(app_ctx_t *ctx, int argc, char **argv);

/* describe_read: read project description file. Caller owns returned string (or NULL). */
char *describe_read(const char *project_name);

/* cmd_wm.c */
void cmd_wm(app_ctx_t *ctx, int argc, char **argv);
const subcmd_t *get_wm_subcmds(void);

/* cmd_table.c: help command */
void cmd_help(app_ctx_t *ctx, int argc, char **argv);
int command_is_alias(const char *name);
int command_is_hidden_default(const char *name);

/* Subtable accessors for the help system */
const subcmd_t *get_memory_subcmds(void);
const subcmd_t *get_agent_subcmds(void);
const subcmd_t *get_index_subcmds(void);
const subcmd_t *get_db_subcmds(void);
const subcmd_t *get_worktree_subcmds(void);

/* Command table (defined in cmd_table.c) */
extern const command_t commands[];

/* Build aimee capabilities reference text. Caller owns the returned string. */
char *build_capabilities_text(void);

#endif
