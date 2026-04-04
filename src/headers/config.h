#ifndef DEC_CONFIG_H
#define DEC_CONFIG_H 1

typedef struct config
{
   char db_path[MAX_PATH_LEN];
   char workspace_root[MAX_PATH_LEN]; /* DEPRECATED — migrated to absolute paths on save */
   char workspaces[64][MAX_PATH_LEN]; /* workspace root directories (absolute at runtime) */
   int workspace_count;
   char guardrail_mode[16];
   char provider[16];
   int use_builtin_cli; /* 1 = use aimee's built-in chat; 0 = launch native CLI */

   /* OpenAI-compatible primary CLI settings */
   char openai_endpoint[512]; /* e.g. "https://api.openai.com/v1" */
   char openai_model[128];    /* e.g. "gpt-4o" */
   char openai_key_cmd[512];  /* command that prints the API key */

   /* Embedding command: piped text on stdin, returns JSON float array on stdout */
   char embedding_command[512];

   /* Block raw git/gh commands from primary agents, redirecting to MCP git tools */
   int block_raw_git;

   /* Autonomous mode: launch agent CLIs with skip-permissions flags,
    * relying solely on aimee guardrails for safety */
   int autonomous;

   /* Cross-verification: delegates verify tool fixes, tool verifies delegate fixes */
   int cross_verify;
   char verify_cmd[512];
   char verify_role[32];
   char verify_prompt[2048];
} config_t;

/* Config schema types for validation */
typedef enum
{
   SCHEMA_STRING,
   SCHEMA_INT,
   SCHEMA_BOOL,
   SCHEMA_ARRAY,
   SCHEMA_OBJECT
} schema_type_t;

typedef struct
{
   const char *key;
   schema_type_t type;
   int required;
} config_schema_entry_t;

/* Global strict mode flag (set via --strict or AIMEE_STRICT=1) */
extern int g_config_strict;

/* Load config from default path. Returns defaults if missing.
 * In strict mode, returns -1 on validation errors. */
int config_load(config_t *cfg);

/* Save config to default path (atomic write via rename). */
int config_save(const config_t *cfg);

/* Default config directory: ~/.config/aimee/ */
const char *config_default_dir(void);

/* Default config path: ~/.config/aimee/config.json */
const char *config_default_path(void);

/* Output directory (same as config dir). */
const char *config_output_dir(void);

/* Effective guardrail mode (defaults to "approve"). */
const char *config_guardrail_mode(const config_t *cfg);

/* Conversation directories for the configured provider. */
int config_conversation_dirs(const config_t *cfg, char dirs[][MAX_PATH_LEN], int max_dirs);

/* Provider config path (e.g., ~/.claude/settings.json). */
const char *config_provider_path(const config_t *cfg);

/* Session ID for the current process. Reads CLAUDE_SESSION_ID from env,
 * falls back to a random UUID generated once per process. */
const char *session_id(void);

/* Per-thread override for work running on behalf of another session. */
void session_id_set_override(const char *sid);
void session_id_clear_override(void);

/* Thread-local DB pointer for MCP handlers (set by dispatch, used by handlers). */
void mcp_db_set(sqlite3 *db);
sqlite3 *mcp_db_get(void);
void mcp_db_clear(void);

/* Build the per-session state file path into buf. */
void session_state_path(char *buf, size_t len);

#endif /* DEC_CONFIG_H */
