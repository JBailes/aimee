/* db.c: SQLite database, migrations, prepared statement cache (FNV-1a keyed) */
#include "aimee.h"
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Prepared statement cache (FNV-1a hash keyed) --- */

typedef struct
{
   uint32_t hash;
   sqlite3 *db; /* the connection this stmt belongs to */
   sqlite3_stmt *stmt;
} stmt_entry_t;

static stmt_entry_t stmt_cache[MAX_STMT_CACHE];
static int stmt_cache_count = 0;
static pthread_mutex_t stmt_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t fnv1a(const char *s)
{
   uint32_t h = 2166136261u;
   for (; *s; s++)
      h = (h ^ (uint8_t)*s) * 16777619u;
   return h;
}

/* Statement cache: keyed by SQL string hash + db pointer.
 * Threading model: the cache mutex protects lookup/insertion, but the returned
 * statement is used without holding the lock. This is safe because each server
 * request uses its own db connection (and thus its own cached statements).
 * Do NOT share a db connection across threads with this cache. */
sqlite3_stmt *db_prepare(sqlite3 *db, const char *sql)
{
   uint32_t h = fnv1a(sql);

   pthread_mutex_lock(&stmt_cache_mutex);

   for (int i = 0; i < stmt_cache_count; i++)
   {
      if (stmt_cache[i].hash == h && stmt_cache[i].db == db &&
          strcmp(sqlite3_sql(stmt_cache[i].stmt), sql) == 0)
      {
         sqlite3_reset(stmt_cache[i].stmt);
         sqlite3_clear_bindings(stmt_cache[i].stmt);
         pthread_mutex_unlock(&stmt_cache_mutex);
         return stmt_cache[i].stmt;
      }
   }

   sqlite3_stmt *stmt = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
   {
      pthread_mutex_unlock(&stmt_cache_mutex);
      fprintf(stderr, "db_prepare: %s\n  SQL: %.200s\n", sqlite3_errmsg(db), sql);
      return NULL;
   }

   if (stmt_cache_count < MAX_STMT_CACHE)
   {
      stmt_cache[stmt_cache_count].hash = h;
      stmt_cache[stmt_cache_count].db = db;
      stmt_cache[stmt_cache_count].stmt = stmt;
      stmt_cache_count++;
   }

   pthread_mutex_unlock(&stmt_cache_mutex);
   return stmt;
}

void db_stmt_cache_clear(void)
{
   pthread_mutex_lock(&stmt_cache_mutex);
   for (int i = 0; i < stmt_cache_count; i++)
   {
      if (stmt_cache[i].stmt)
         sqlite3_finalize(stmt_cache[i].stmt);
      stmt_cache[i].stmt = NULL;
      stmt_cache[i].hash = 0;
      stmt_cache[i].db = NULL;
   }
   stmt_cache_count = 0;
   pthread_mutex_unlock(&stmt_cache_mutex);
}

void db_stmt_cache_clear_for(sqlite3 *db)
{
   pthread_mutex_lock(&stmt_cache_mutex);
   int write_idx = 0;
   for (int i = 0; i < stmt_cache_count; i++)
   {
      if (stmt_cache[i].db == db)
      {
         if (stmt_cache[i].stmt)
            sqlite3_finalize(stmt_cache[i].stmt);
      }
      else
      {
         if (write_idx != i)
            stmt_cache[write_idx] = stmt_cache[i];
         write_idx++;
      }
   }
   for (int i = write_idx; i < stmt_cache_count; i++)
   {
      stmt_cache[i].stmt = NULL;
      stmt_cache[i].hash = 0;
      stmt_cache[i].db = NULL;
   }
   stmt_cache_count = write_idx;
   pthread_mutex_unlock(&stmt_cache_mutex);
}

/* --- Default path --- */

const char *db_default_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   const char *home = getenv("HOME");
   if (!home)
      home = "/tmp";
   snprintf(path, sizeof(path), "%s/.config/aimee/aimee.db", home);
   return path;
}

/* --- FTS5 check --- */

int db_fts5_available(sqlite3 *db)
{
   sqlite3_stmt *stmt = NULL;
   int found = 0;

   if (sqlite3_prepare_v2(db, "PRAGMA compile_options", -1, &stmt, NULL) != SQLITE_OK)
      return 0;

   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      const char *opt = (const char *)sqlite3_column_text(stmt, 0);
      if (opt && strcmp(opt, "ENABLE_FTS5") == 0)
      {
         found = 1;
         break;
      }
   }
   sqlite3_finalize(stmt);
   return found;
}

/* --- Migrations --- */

static const struct
{
   int version;
   const char *sql;
} migrations[] = {
    /* 1: core tables */
    {1, "CREATE TABLE IF NOT EXISTS rules ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  polarity TEXT NOT NULL,"
        "  title TEXT NOT NULL,"
        "  description TEXT NOT NULL DEFAULT '',"
        "  weight INTEGER NOT NULL DEFAULT 5,"
        "  domain TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS projects ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  root TEXT NOT NULL,"
        "  scanned_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS files ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
        "  path TEXT NOT NULL,"
        "  purpose TEXT NOT NULL DEFAULT '',"
        "  hash TEXT NOT NULL DEFAULT '',"
        "  scanned_at TEXT NOT NULL,"
        "  UNIQUE(project_id, path)"
        ");"
        "CREATE TABLE IF NOT EXISTS file_exports ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        "  name TEXT NOT NULL,"
        "  kind TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS file_imports ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        "  name TEXT NOT NULL,"
        "  kind TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS aliases ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  target TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS terms ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        "  name TEXT NOT NULL,"
        "  kind TEXT NOT NULL DEFAULT '',"
        "  line INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS windows ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id TEXT NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  summary TEXT NOT NULL DEFAULT '',"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS decisions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  window_id INTEGER NOT NULL REFERENCES windows(id) ON DELETE CASCADE,"
        "  description TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS window_terms ("
        "  window_id INTEGER NOT NULL REFERENCES windows(id) ON DELETE CASCADE,"
        "  term TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS window_files ("
        "  window_id INTEGER NOT NULL REFERENCES windows(id) ON DELETE CASCADE,"
        "  file_path TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS window_projects ("
        "  window_id INTEGER NOT NULL REFERENCES windows(id) ON DELETE CASCADE,"
        "  project_name TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS stopwords ("
        "  word TEXT PRIMARY KEY"
        ");"
        "CREATE TABLE IF NOT EXISTS schemas ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,"
        "  name TEXT NOT NULL,"
        "  definition TEXT NOT NULL,"
        "  scanned_at TEXT NOT NULL,"
        "  UNIQUE(project_id, name)"
        ");"},

    /* 2: indexes on window/file/term tables */
    {2, "CREATE INDEX IF NOT EXISTS idx_wt_term ON window_terms(term);"
        "CREATE INDEX IF NOT EXISTS idx_wf_path ON window_files(file_path);"
        "CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);"
        "CREATE INDEX IF NOT EXISTS idx_terms_name ON terms(name);"},

    /* 3: import/export name indexes */
    {3, "CREATE INDEX IF NOT EXISTS idx_fi_name ON file_imports(name);"
        "CREATE INDEX IF NOT EXISTS idx_fe_name ON file_exports(name);"},

    /* 4: window tier column */
    {4, "ALTER TABLE windows ADD COLUMN tier TEXT NOT NULL DEFAULT 'raw';"},

    /* 5: entity edges */
    {5, "CREATE TABLE IF NOT EXISTS entity_edges ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  source TEXT NOT NULL,"
        "  relation TEXT NOT NULL,"
        "  target TEXT NOT NULL,"
        "  weight INTEGER NOT NULL DEFAULT 1,"
        "  window_id INTEGER REFERENCES windows(id) ON DELETE SET NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ee_source ON entity_edges(source);"
        "CREATE INDEX IF NOT EXISTS idx_ee_target ON entity_edges(target);"},

    /* 6: memories + provenance + conflicts */
    {6, "CREATE TABLE IF NOT EXISTS memories ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  tier TEXT NOT NULL DEFAULT 'L0',"
        "  kind TEXT NOT NULL DEFAULT 'fact',"
        "  key TEXT NOT NULL,"
        "  content TEXT NOT NULL DEFAULT '',"
        "  confidence REAL NOT NULL DEFAULT 1.0,"
        "  use_count INTEGER NOT NULL DEFAULT 0,"
        "  last_used_at TEXT,"
        "  source_session TEXT,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_mem_tier ON memories(tier);"
        "CREATE INDEX IF NOT EXISTS idx_mem_kind ON memories(kind);"
        "CREATE INDEX IF NOT EXISTS idx_mem_key ON memories(key);"
        "CREATE TABLE IF NOT EXISTS memory_provenance ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
        "  session_id TEXT NOT NULL,"
        "  action TEXT NOT NULL,"
        "  details TEXT,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_mp_mem ON memory_provenance(memory_id);"
        "CREATE TABLE IF NOT EXISTS memory_conflicts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  memory_a INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
        "  memory_b INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
        "  detected_at TEXT NOT NULL,"
        "  resolved INTEGER NOT NULL DEFAULT 0,"
        "  resolution TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_mc_unresolved "
        "  ON memory_conflicts(resolved) WHERE resolved = 0;"},

    /* 7: context cache */
    {7, "CREATE TABLE IF NOT EXISTS context_cache ("
        "  hash TEXT PRIMARY KEY,"
        "  output TEXT NOT NULL,"
        "  created_at TEXT NOT NULL"
        ");"},

    /* 8: tasks + task edges + decision log */
    {8, "CREATE TABLE IF NOT EXISTS tasks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  parent_id INTEGER DEFAULT 0,"
        "  title TEXT NOT NULL,"
        "  state TEXT NOT NULL DEFAULT 'todo',"
        "  confidence REAL NOT NULL DEFAULT 1.0,"
        "  session_id TEXT,"
        "  created_at TEXT NOT NULL,"
        "  updated_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_tasks_state ON tasks(state);"
        "CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks(parent_id);"
        "CREATE TABLE IF NOT EXISTS task_edges ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  source_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,"
        "  target_id INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,"
        "  relation TEXT NOT NULL DEFAULT 'depends_on'"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_te_source ON task_edges(source_id);"
        "CREATE INDEX IF NOT EXISTS idx_te_target ON task_edges(target_id);"
        "CREATE TABLE IF NOT EXISTS decision_log ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  task_id INTEGER DEFAULT 0,"
        "  options TEXT NOT NULL,"
        "  chosen TEXT NOT NULL,"
        "  rationale TEXT NOT NULL DEFAULT '',"
        "  assumptions TEXT NOT NULL DEFAULT '',"
        "  outcome TEXT,"
        "  created_at TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_dl_task ON decision_log(task_id);"},

    /* 9: anti-patterns */
    {9, "CREATE TABLE IF NOT EXISTS anti_patterns ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  pattern TEXT NOT NULL,"
        "  description TEXT NOT NULL DEFAULT '',"
        "  source TEXT NOT NULL DEFAULT '',"
        "  source_ref TEXT NOT NULL DEFAULT '',"
        "  hit_count INTEGER NOT NULL DEFAULT 0,"
        "  confidence REAL NOT NULL DEFAULT 1.0"
        ");"},

    /* 10: checkpoints */
    {10, "CREATE TABLE IF NOT EXISTS checkpoints ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  task_id INTEGER DEFAULT 0,"
         "  session_id TEXT,"
         "  label TEXT NOT NULL,"
         "  snapshot TEXT NOT NULL DEFAULT '',"
         "  created_at TEXT NOT NULL"
         ");"},

    /* 11: memory temporal validity */
    {11, "ALTER TABLE memories ADD COLUMN valid_from TEXT;"
         "ALTER TABLE memories ADD COLUMN valid_until TEXT;"},

    /* 12: agent log */
    {12, "CREATE TABLE IF NOT EXISTS agent_log ("
         "  id INTEGER PRIMARY KEY,"
         "  agent_name TEXT NOT NULL,"
         "  role TEXT NOT NULL,"
         "  prompt_tokens INTEGER DEFAULT 0,"
         "  completion_tokens INTEGER DEFAULT 0,"
         "  latency_ms INTEGER DEFAULT 0,"
         "  success INTEGER NOT NULL DEFAULT 0,"
         "  error TEXT,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_agent_log_name ON agent_log(agent_name);"
         "CREATE INDEX IF NOT EXISTS idx_agent_log_role ON agent_log(role);"},

    /* 13: agent log turns + tool calls */
    {13, "ALTER TABLE agent_log ADD COLUMN turns INTEGER DEFAULT 0;"
         "ALTER TABLE agent_log ADD COLUMN tool_calls INTEGER DEFAULT 0;"},

    /* 14: tool registry */
    {14, "CREATE TABLE IF NOT EXISTS tool_registry ("
         "  name TEXT PRIMARY KEY,"
         "  description TEXT NOT NULL,"
         "  input_schema TEXT NOT NULL,"
         "  output_schema TEXT,"
         "  side_effect TEXT NOT NULL DEFAULT 'read',"
         "  idempotent INTEGER NOT NULL DEFAULT 0,"
         "  enabled INTEGER NOT NULL DEFAULT 1"
         ");"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('bash', 'Run a shell command',"
         " '{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
         "\"required\":[\"command\"]}', 'destructive', 0);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('read_file', 'Read a file',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
         "\"required\":[\"path\"]}', 'read', 1);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('write_file', 'Write to a file',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
         "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}', 'write', 0);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('list_files', 'List files in a directory',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
         "\"required\":[\"path\"]}', 'read', 1);"},

    /* 15: plan IR + execution transactions */
    {15, "CREATE TABLE IF NOT EXISTS execution_plans ("
         "  id INTEGER PRIMARY KEY,"
         "  agent_name TEXT NOT NULL,"
         "  task TEXT NOT NULL,"
         "  status TEXT NOT NULL DEFAULT 'pending',"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE TABLE IF NOT EXISTS plan_steps ("
         "  id INTEGER PRIMARY KEY,"
         "  plan_id INTEGER NOT NULL REFERENCES execution_plans(id),"
         "  seq INTEGER NOT NULL,"
         "  action TEXT NOT NULL,"
         "  precondition TEXT,"
         "  success_predicate TEXT,"
         "  rollback TEXT,"
         "  status TEXT NOT NULL DEFAULT 'pending',"
         "  output TEXT,"
         "  checkpoint TEXT,"
         "  started_at TEXT,"
         "  finished_at TEXT"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_plan_steps_plan ON plan_steps(plan_id);"},

    /* 16: execution trace / replay */
    {16, "CREATE TABLE IF NOT EXISTS execution_trace ("
         "  id INTEGER PRIMARY KEY,"
         "  plan_id INTEGER,"
         "  turn INTEGER NOT NULL,"
         "  direction TEXT NOT NULL,"
         "  content TEXT NOT NULL,"
         "  tool_name TEXT,"
         "  tool_args TEXT,"
         "  tool_result TEXT,"
         "  context_hash TEXT,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_trace_plan ON execution_trace(plan_id);"},

    /* 17: eval harness */
    {17, "CREATE TABLE IF NOT EXISTS eval_results ("
         "  id INTEGER PRIMARY KEY,"
         "  suite TEXT NOT NULL,"
         "  task_name TEXT NOT NULL,"
         "  agent_name TEXT NOT NULL,"
         "  success INTEGER NOT NULL DEFAULT 0,"
         "  turns INTEGER DEFAULT 0,"
         "  tool_calls INTEGER DEFAULT 0,"
         "  prompt_tokens INTEGER DEFAULT 0,"
         "  completion_tokens INTEGER DEFAULT 0,"
         "  latency_ms INTEGER DEFAULT 0,"
         "  response TEXT,"
         "  error TEXT,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_eval_suite ON eval_results(suite);"},

    /* 18: confidence + artifact-aware memory */
    {18, "ALTER TABLE agent_log ADD COLUMN confidence INTEGER DEFAULT -1;"
         "ALTER TABLE memories ADD COLUMN artifact_type TEXT;"
         "ALTER TABLE memories ADD COLUMN artifact_ref TEXT;"
         "ALTER TABLE memories ADD COLUMN artifact_hash TEXT;"},

    /* 19: long-running sessions (agent jobs) */
    {19, "CREATE TABLE IF NOT EXISTS agent_jobs ("
         "  id INTEGER PRIMARY KEY,"
         "  role TEXT NOT NULL,"
         "  prompt TEXT NOT NULL,"
         "  agent_name TEXT NOT NULL,"
         "  status TEXT NOT NULL DEFAULT 'pending',"
         "  cursor TEXT,"
         "  heartbeat_at TEXT,"
         "  lease_owner TEXT,"
         "  result TEXT,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
         "  updated_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_jobs_status ON agent_jobs(status);"},

    /* 20: environment introspection */
    {20, "CREATE TABLE IF NOT EXISTS env_capabilities ("
         "  key TEXT PRIMARY KEY,"
         "  value TEXT NOT NULL,"
         "  detected_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"},

    /* 21: preference controls */
    {21, "ALTER TABLE rules ADD COLUMN directive_type TEXT DEFAULT 'soft';"
         "ALTER TABLE rules ADD COLUMN expires_at TEXT;"},

    /* 22: verify + git_log tools */
    {22, "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('verify', 'Verify an assertion (HTTP status, file contents, command exit)',"
         " '{\"type\":\"object\",\"properties\":{\"check_type\":{\"type\":\"string\"},"
         "\"target\":{\"type\":\"string\"},\"expected\":{\"type\":\"string\"}},"
         "\"required\":[\"check_type\",\"target\"]}', 'read', 1);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('git_log', 'Show recent git commits',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
         "\"count\":{\"type\":\"integer\"}},\"required\":[\"path\"]}', 'read', 1);"},

    /* 23: result cache for delegations */
    {23, "CREATE TABLE IF NOT EXISTS agent_cache ("
         "  id INTEGER PRIMARY KEY,"
         "  role TEXT NOT NULL,"
         "  prompt_hash TEXT NOT NULL,"
         "  response TEXT NOT NULL,"
         "  confidence INTEGER DEFAULT 0,"
         "  turns INTEGER DEFAULT 0,"
         "  tool_calls INTEGER DEFAULT 0,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE UNIQUE INDEX IF NOT EXISTS idx_cache_lookup"
         " ON agent_cache(role, prompt_hash);"},

    /* 24: session isolation columns */
    {24, "ALTER TABLE agent_log ADD COLUMN session_id TEXT;"
         "ALTER TABLE context_cache ADD COLUMN session_id TEXT;"},

    /* 25: working memory (session-scoped key-value scratch space) */
    {25, "CREATE TABLE IF NOT EXISTS working_memory ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  session_id TEXT NOT NULL,"
         "  key TEXT NOT NULL,"
         "  value TEXT NOT NULL,"
         "  category TEXT DEFAULT 'general',"
         "  created_at TEXT NOT NULL,"
         "  updated_at TEXT NOT NULL,"
         "  expires_at TEXT,"
         "  UNIQUE(session_id, key)"
         ");"},

    /* 26: task success criteria + new tool registry entries */
    {26, "ALTER TABLE tasks ADD COLUMN success_criteria TEXT DEFAULT '';"
         "ALTER TABLE tasks ADD COLUMN description TEXT DEFAULT '';"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('grep', 'Search for patterns in files',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
         "\"pattern\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\"}},"
         "\"required\":[\"path\",\"pattern\"]}', 'read', 1);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('git_diff', 'Show git diff',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
         "\"ref\":{\"type\":\"string\"}},\"required\":[\"path\"]}', 'read', 1);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('git_status', 'Show git status',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
         "\"required\":[\"path\"]}', 'read', 1);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('env_get', 'Get environment variable',"
         " '{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"}},"
         "\"required\":[\"name\"]}', 'read', 1);"
         "INSERT OR IGNORE INTO tool_registry (name, description, input_schema, side_effect, "
         "idempotent)"
         " VALUES ('test', 'Check file existence and permissions',"
         " '{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
         "\"check\":{\"type\":\"string\"}},\"required\":[\"path\"]}', 'read', 1);"},

    /* 27: server sessions table */
    {27, "CREATE TABLE IF NOT EXISTS server_sessions ("
         "  id TEXT PRIMARY KEY,"
         "  client_type TEXT NOT NULL,"
         "  principal TEXT NOT NULL,"
         "  title TEXT DEFAULT '',"
         "  created_at TEXT NOT NULL,"
         "  last_activity_at TEXT NOT NULL,"
         "  claude_session_id TEXT DEFAULT '',"
         "  metadata TEXT DEFAULT '{}'"
         ");"},

    /* 28: FTS5 index on memories for full-text search */
    {28, "CREATE VIRTUAL TABLE IF NOT EXISTS memories_fts "
         "USING fts5(key, content, content='memories', content_rowid='id', "
         "tokenize='porter unicode61');"
         "CREATE TRIGGER IF NOT EXISTS memories_fts_ai AFTER INSERT ON memories BEGIN"
         "  INSERT INTO memories_fts(rowid, key, content)"
         "  VALUES (new.id, new.key, new.content);"
         "END;"
         "CREATE TRIGGER IF NOT EXISTS memories_fts_ad AFTER DELETE ON memories BEGIN"
         "  INSERT INTO memories_fts(memories_fts, rowid, key, content)"
         "  VALUES('delete', old.id, old.key, old.content);"
         "END;"
         "CREATE TRIGGER IF NOT EXISTS memories_fts_au AFTER UPDATE ON memories BEGIN"
         "  INSERT INTO memories_fts(memories_fts, rowid, key, content)"
         "  VALUES('delete', old.id, old.key, old.content);"
         "  INSERT INTO memories_fts(rowid, key, content)"
         "  VALUES (new.id, new.key, new.content);"
         "END;"},

    /* 29: memory health metrics + contradiction audit log */
    {29, "CREATE TABLE IF NOT EXISTS memory_health ("
         "  id INTEGER PRIMARY KEY,"
         "  cycle_at TEXT NOT NULL,"
         "  total_memories INTEGER NOT NULL,"
         "  contradictions_detected INTEGER NOT NULL DEFAULT 0,"
         "  promotions INTEGER NOT NULL DEFAULT 0,"
         "  demotions INTEGER NOT NULL DEFAULT 0,"
         "  expirations INTEGER NOT NULL DEFAULT 0"
         ");"
         "CREATE TABLE IF NOT EXISTS contradiction_log ("
         "  id INTEGER PRIMARY KEY,"
         "  detected_at TEXT NOT NULL,"
         "  memory_a_id INTEGER NOT NULL,"
         "  memory_b_id INTEGER NOT NULL,"
         "  resolution TEXT NOT NULL,"
         "  details TEXT"
         ");"},

    /* 30: cross-workspace memory scoping */
    {30, "CREATE TABLE IF NOT EXISTS memory_workspaces ("
         "  memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
         "  workspace TEXT NOT NULL,"
         "  PRIMARY KEY (memory_id, workspace)"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_mw_workspace"
         " ON memory_workspaces(workspace);"},

    /* 31: embedding-based memory retrieval */
    {31, "CREATE TABLE IF NOT EXISTS memory_embeddings ("
         "  memory_id INTEGER PRIMARY KEY REFERENCES memories(id) ON DELETE CASCADE,"
         "  embedding BLOB NOT NULL,"
         "  model TEXT NOT NULL DEFAULT '',"
         "  created_at TEXT NOT NULL"
         ");"},

    /* 32: kind-specific lifecycle rules */
    {32, "CREATE TABLE IF NOT EXISTS kind_lifecycle ("
         "  kind TEXT PRIMARY KEY,"
         "  promote_use_count INTEGER NOT NULL DEFAULT 3,"
         "  promote_confidence REAL NOT NULL DEFAULT 0.9,"
         "  demote_days INTEGER NOT NULL DEFAULT 60,"
         "  demote_confidence REAL NOT NULL DEFAULT 0.7,"
         "  expire_days INTEGER NOT NULL DEFAULT 30,"
         "  demotion_resistance REAL NOT NULL DEFAULT 1.0"
         ");"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('fact',       3,  0.9,  60,  0.7,  30,  1.0);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('preference', 2,  0.8,  90,  0.6,  30,  1.5);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('decision',   3,  0.9,  90,  0.7,  45,  1.5);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('episode',    5,  0.9,  30,  0.7,  14,  0.5);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('task',       3,  0.9,  14,  0.7,   7,  0.5);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('scratch',    5,  0.95, 7,   0.7,   3,  0.25);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('procedure',  2,  0.8,  180, 0.5,  90,  3.0);"
         "INSERT OR IGNORE INTO kind_lifecycle VALUES"
         " ('policy',     1,  0.7,  365, 0.3,  180, 5.0);"},

    /* 33: worktree registry for janitor/gc */
    {33, "CREATE TABLE IF NOT EXISTS worktrees ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  session_id TEXT NOT NULL,"
         "  workspace TEXT NOT NULL,"
         "  path TEXT NOT NULL,"
         "  created_at INTEGER NOT NULL,"
         "  last_accessed_at INTEGER NOT NULL,"
         "  size_bytes INTEGER NOT NULL DEFAULT 0,"
         "  state TEXT NOT NULL DEFAULT 'active'"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_worktrees_session ON worktrees(session_id);"
         "CREATE INDEX IF NOT EXISTS idx_worktrees_state ON worktrees(state);"},

    /* 34: inter-session work queue */
    {34, "CREATE TABLE IF NOT EXISTS work_queue ("
         "  id TEXT PRIMARY KEY,"
         "  title TEXT NOT NULL,"
         "  description TEXT DEFAULT '',"
         "  source TEXT DEFAULT '',"
         "  priority INTEGER DEFAULT 0,"
         "  status TEXT NOT NULL DEFAULT 'pending',"
         "  claimed_by TEXT,"
         "  claimed_at TEXT,"
         "  completed_at TEXT,"
         "  result TEXT DEFAULT '',"
         "  created_by TEXT,"
         "  created_at TEXT NOT NULL,"
         "  metadata TEXT DEFAULT ''"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_work_queue_status ON work_queue(status);"
         "CREATE INDEX IF NOT EXISTS idx_work_queue_claimed ON work_queue(claimed_by);"},

    /* 35: agent execution outcomes for automated learning */
    {35, "CREATE TABLE IF NOT EXISTS agent_outcomes ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  agent_name TEXT NOT NULL,"
         "  role TEXT DEFAULT '',"
         "  outcome TEXT NOT NULL," /* success, partial, failure, error */
         "  reason TEXT DEFAULT '',"
         "  turns_used INTEGER DEFAULT 0,"
         "  tools_called INTEGER DEFAULT 0,"
         "  tokens_used INTEGER DEFAULT 0,"
         "  tool_error_pattern TEXT DEFAULT '',"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_agent_outcomes_created"
         " ON agent_outcomes(created_at);"
         "CREATE INDEX IF NOT EXISTS idx_agent_outcomes_pattern"
         " ON agent_outcomes(tool_error_pattern);"},

    /* 36: context snapshots and session outcomes for memory effectiveness tracking */
    {36, "CREATE TABLE IF NOT EXISTS context_snapshots ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  session_id TEXT NOT NULL,"
         "  memory_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
         "  relevance_score REAL,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_ctx_snap_session ON context_snapshots(session_id);"
         "CREATE INDEX IF NOT EXISTS idx_ctx_snap_memory ON context_snapshots(memory_id);"
         "ALTER TABLE server_sessions ADD COLUMN outcome TEXT DEFAULT NULL;"
         "ALTER TABLE server_sessions ADD COLUMN rule_violations INTEGER DEFAULT 0;"
         "ALTER TABLE memories ADD COLUMN effectiveness REAL DEFAULT NULL;"},

    /* 36: memory-to-memory linking */
    {36, "CREATE TABLE IF NOT EXISTS memory_links ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  source_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
         "  target_id INTEGER NOT NULL REFERENCES memories(id) ON DELETE CASCADE,"
         "  relation TEXT NOT NULL,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now')),"
         "  UNIQUE(source_id, target_id, relation)"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_ml_source ON memory_links(source_id);"
         "CREATE INDEX IF NOT EXISTS idx_ml_target ON memory_links(target_id);"},

    /* 36: delegation conversation messages */
    {36, "CREATE TABLE IF NOT EXISTS delegation_messages ("
         "  id INTEGER PRIMARY KEY,"
         "  delegation_id TEXT NOT NULL,"
         "  direction TEXT NOT NULL,"
         "  content TEXT NOT NULL,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_deleg_msg_id ON delegation_messages(delegation_id);"},

    /* 36: delegation checkpoint table for retry context */
    {36, "CREATE TABLE IF NOT EXISTS delegation_checkpoint ("
         "  delegation_id TEXT PRIMARY KEY,"
         "  job_id TEXT DEFAULT '',"
         "  steps_completed TEXT DEFAULT '[]',"
         "  last_output TEXT DEFAULT '',"
         "  error TEXT DEFAULT '',"
         "  attempt INTEGER DEFAULT 0,"
         "  failed_at INTEGER,"
         "  created_at INTEGER NOT NULL"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_deleg_ckpt_job ON delegation_checkpoint(job_id);"},

    /* 36: content safety sensitivity classification */
    {36, "ALTER TABLE memories ADD COLUMN sensitivity TEXT NOT NULL DEFAULT 'normal';"
         "CREATE INDEX IF NOT EXISTS idx_memories_sensitivity ON memories(sensitivity);"},

    /* 36: rule weight decay support */
    {36, "ALTER TABLE rules ADD COLUMN last_reinforced_at TEXT DEFAULT NULL;"
         "UPDATE rules SET last_reinforced_at = updated_at WHERE last_reinforced_at IS NULL;"},

    /* 36: trace mining log (tracks which traces have been mined) */
    {36, "CREATE TABLE IF NOT EXISTS trace_mining_log ("
         "  id INTEGER PRIMARY KEY,"
         "  last_trace_id INTEGER,"
         "  mined_at TEXT"
         ");"},

    /* 37: code call graph */
    {37, "CREATE TABLE IF NOT EXISTS code_calls ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
         "  caller TEXT NOT NULL DEFAULT '',"
         "  callee TEXT NOT NULL,"
         "  line INTEGER NOT NULL"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_cc_callee ON code_calls(callee);"
         "CREATE INDEX IF NOT EXISTS idx_cc_file ON code_calls(file_id);"},

    /* 37: work queue audit trail */
    {37, "CREATE TABLE IF NOT EXISTS work_queue_log ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  item_id TEXT NOT NULL,"
         "  old_status TEXT,"
         "  new_status TEXT NOT NULL,"
         "  session_id TEXT,"
         "  detail TEXT DEFAULT '',"
         "  created_at TEXT NOT NULL"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_wq_log_item ON work_queue_log(item_id);"},

    /* 37: token usage audit */
    {37, "CREATE TABLE IF NOT EXISTS token_audit ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  session_id TEXT NOT NULL,"
         "  project_name TEXT DEFAULT '',"
         "  tool_name TEXT NOT NULL,"
         "  role TEXT DEFAULT '',"
         "  prompt_tokens INTEGER DEFAULT 0,"
         "  completion_tokens INTEGER DEFAULT 0,"
         "  estimated_cost_usd REAL DEFAULT 0.0,"
         "  created_at TEXT NOT NULL DEFAULT (datetime('now'))"
         ");"
         "CREATE INDEX IF NOT EXISTS idx_audit_session ON token_audit(session_id);"
         "CREATE INDEX IF NOT EXISTS idx_audit_tool ON token_audit(tool_name);"},

    /* 37: file contents storage for FTS5 full-text code search */
    {37, "CREATE TABLE IF NOT EXISTS file_contents ("
         "  file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,"
         "  content TEXT NOT NULL"
         ");"},

    /* 37: work queue effort and tags for filtering */
    {37, "ALTER TABLE work_queue ADD COLUMN effort TEXT DEFAULT '';"
         "ALTER TABLE work_queue ADD COLUMN tags TEXT DEFAULT '';"},

    /* 38: track which workspace a project was discovered under */
    {38, "ALTER TABLE projects ADD COLUMN workspace TEXT DEFAULT '';"},

    /* 39: branch ownership tracking for multi-session safety */
    {39, "CREATE TABLE IF NOT EXISTS branch_ownership ("
         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
         "  repo_path TEXT NOT NULL,"
         "  branch_name TEXT NOT NULL,"
         "  session_id TEXT NOT NULL,"
         "  created_at TEXT DEFAULT (datetime('now')),"
         "  UNIQUE(repo_path, branch_name)"
         ");"},
};

#define MIGRATION_COUNT (int)(sizeof(migrations) / sizeof(migrations[0]))

int db_next_migration_version(void)
{
   int max_version = 0;
   for (int i = 0; i < MIGRATION_COUNT; i++)
   {
      if (migrations[i].version > max_version)
         max_version = migrations[i].version;
   }
   return max_version + 1;
}

int db_validate_migrations(char *err_buf, size_t err_len)
{
   int prev = 0;
   int seen[1024] = {0};

   for (int i = 0; i < MIGRATION_COUNT; i++)
   {
      int v = migrations[i].version;
      if (v <= 0 || v >= 1024)
      {
         if (err_buf)
            snprintf(err_buf, err_len, "migration %d: version out of range", v);
         return -1;
      }

      /* Check strict ordering (allow same version for multi-part migrations) */
      if (v < prev)
      {
         if (err_buf)
            snprintf(err_buf, err_len, "migration %d: out of order (after %d)", v, prev);
         return -1;
      }
      prev = v;

      /* Track first occurrence to detect non-contiguous duplicates */
      if (seen[v] && i > 0 && migrations[i - 1].version != v)
      {
         if (err_buf)
            snprintf(err_buf, err_len, "migration %d: non-contiguous duplicate", v);
         return -1;
      }
      seen[v] = 1;
   }
   return 0;
}

/* Back up the database using the SQLite online backup API.
 * Keeps the last 3 backups and prunes older ones. Returns 0 on success. */
static int backup_before_migrate(sqlite3 *db, const char *db_path, int current_version)
{
   char bak[MAX_PATH_LEN];
   snprintf(bak, sizeof(bak), "%s.bak.%d", db_path, current_version);

   sqlite3 *dst = NULL;
   if (sqlite3_open(bak, &dst) != SQLITE_OK)
   {
      if (dst)
         sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup *b = sqlite3_backup_init(dst, "main", db, "main");
   if (!b)
   {
      sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup_step(b, -1);
   int brc = sqlite3_backup_finish(b);
   sqlite3_close(dst);

   if (brc != SQLITE_OK)
   {
      unlink(bak);
      return -1;
   }

   chmod(bak, 0600);

   /* Prune old backups: keep only last 3 */
   for (int v = current_version - 3; v >= 0; v--)
   {
      char old[MAX_PATH_LEN];
      snprintf(old, sizeof(old), "%s.bak.%d", db_path, v);
      unlink(old);
   }
   return 0;
}

static int migrate(sqlite3 *db, const char *db_path)
{
   char *err = NULL;

   /* Fast-path: if user_version already matches MIGRATION_COUNT, all
    * migrations have been applied. Skip the per-version check loop. */
   int current_version = 0;
   {
      sqlite3_stmt *uv = NULL;
      if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &uv, NULL) == SQLITE_OK &&
          sqlite3_step(uv) == SQLITE_ROW)
      {
         current_version = sqlite3_column_int(uv, 0);
         sqlite3_finalize(uv);
         if (current_version == MIGRATION_COUNT)
            return 0;
      }
      else if (uv)
         sqlite3_finalize(uv);
   }

   int rc = sqlite3_exec(db,
                         "CREATE TABLE IF NOT EXISTS schema_migrations ("
                         "  version INTEGER PRIMARY KEY,"
                         "  applied_at TEXT NOT NULL"
                         ");",
                         NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      fprintf(stderr, "migrate: create schema_migrations: %s\n", err);
      sqlite3_free(err);
      return -1;
   }

   /* Back up the database before running pending migrations.
    * If the backup fails (e.g. disk full), skip migrations entirely
    * rather than running without a safety net. A fresh (version 0) database
    * does not need a backup since there is no data to protect. */
   if (db_path && current_version > 0)
   {
      if (backup_before_migrate(db, db_path, current_version) != 0)
      {
         fprintf(stderr,
                 "aimee: WARNING: pre-migration backup failed (disk full?), "
                 "skipping migration.\n"
                 "       Database remains at schema version %d. "
                 "Free disk space and restart.\n",
                 current_version);
         return -1;
      }
   }

   for (int i = 0; i < MIGRATION_COUNT; i++)
   {
      int version = i + 1;

      sqlite3_stmt *check = NULL;
      sqlite3_prepare_v2(db, "SELECT 1 FROM schema_migrations WHERE version = ?", -1, &check, NULL);
      sqlite3_bind_int(check, 1, version);
      int exists = (sqlite3_step(check) == SQLITE_ROW);
      sqlite3_finalize(check);

      if (exists)
         continue;

      sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);

      rc = sqlite3_exec(db, migrations[i].sql, NULL, NULL, &err);
      if (rc != SQLITE_OK)
      {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         fprintf(stderr, "migrate: v%d: %s\n", version, err);
         sqlite3_free(err);
         return -1;
      }

      char ts[32];
      now_utc(ts, sizeof(ts));
      sqlite3_stmt *ins = NULL;
      sqlite3_prepare_v2(db, "INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)", -1,
                         &ins, NULL);
      sqlite3_bind_int(ins, 1, version);
      sqlite3_bind_text(ins, 2, ts, -1, SQLITE_TRANSIENT);
      int ins_rc = sqlite3_step(ins);
      sqlite3_finalize(ins);

      if (ins_rc != SQLITE_DONE)
      {
         sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
         fprintf(stderr, "migrate: v%d: failed to record version\n", version);
         return -1;
      }

      sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
   }

   /* Record the current schema version in the database header so future
    * opens can skip the per-migration check loop entirely. */
   {
      char pragma_buf[64];
      snprintf(pragma_buf, sizeof(pragma_buf), "PRAGMA user_version = %d", MIGRATION_COUNT);
      sqlite3_exec(db, pragma_buf, NULL, NULL, NULL);
   }

   return 0;
}

static int create_fts_tables(sqlite3 *db)
{
   char *err = NULL;
   int rc = sqlite3_exec(db,
                         "CREATE VIRTUAL TABLE IF NOT EXISTS window_fts "
                         "USING fts5(summary, content='windows', content_rowid='id', "
                         "tokenize='porter unicode61');",
                         NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      fprintf(stderr, "create_fts: %s\n", err);
      sqlite3_free(err);
      return -1;
   }

   /* Populate memories_fts from existing data only if out of sync.
    * Skip the full-table-scan INSERT when the FTS index is already current. */
   {
      sqlite3_stmt *chk = NULL;
      sqlite3_prepare_v2(db,
                         "SELECT COUNT(*) FROM memories"
                         " WHERE id NOT IN (SELECT rowid FROM memories_fts)",
                         -1, &chk, NULL);
      int need_populate = 0;
      if (chk)
      {
         if (sqlite3_step(chk) == SQLITE_ROW)
            need_populate = sqlite3_column_int(chk, 0);
         sqlite3_finalize(chk);
      }
      if (need_populate > 0)
      {
         sqlite3_exec(db,
                      "INSERT OR IGNORE INTO memories_fts(rowid, key, content)"
                      " SELECT id, key, content FROM memories"
                      " WHERE id NOT IN (SELECT rowid FROM memories_fts)",
                      NULL, NULL, NULL);
      }
   }

   /* Code full-text search index */
   rc = sqlite3_exec(db,
                     "CREATE VIRTUAL TABLE IF NOT EXISTS code_fts "
                     "USING fts5(content, content='file_contents', content_rowid='file_id', "
                     "tokenize='unicode61');",
                     NULL, NULL, &err);
   if (rc != SQLITE_OK)
   {
      /* Non-fatal: file_contents table may not exist yet (pre-migration) */
      sqlite3_free(err);
   }

   return 0;
}

/* --- Open / Close --- */

static void ensure_dir(const char *path)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", path);
   char *last_slash = strrchr(dir, '/');
   if (last_slash)
   {
      *last_slash = '\0';
      /* Recursive mkdir with parents */
      for (char *p = dir + 1; *p; p++)
      {
         if (*p == '/')
         {
            *p = '\0';
            mkdir(dir, 0700);
            *p = '/';
         }
      }
      mkdir(dir, 0700);
   }
}

/* --- Pragma profiles --- */

void db_apply_pragmas(sqlite3 *db, db_mode_t mode)
{
   /* Common pragmas for all modes */
   sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
   sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
   sqlite3_exec(db, "PRAGMA wal_autocheckpoint=1000", NULL, NULL, NULL);

   if (mode == DB_MODE_SERVER)
   {
      /* Server: long-lived, concurrent queries, trades crash durability for throughput.
       * Lower busy timeout since server connections use transactions and retry at
       * a higher level. */
      sqlite3_busy_timeout(db, 5000);
      sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA cache_size=-8192", NULL, NULL, NULL);   /* 8MB */
      sqlite3_exec(db, "PRAGMA mmap_size=67108864", NULL, NULL, NULL); /* 64MB */
   }
   else
   {
      /* CLI: short-lived, full durability. Higher busy timeout because multiple
       * concurrent sessions (hooks, agents) contend on the same DB file and CLI
       * callers can afford to block. */
      sqlite3_busy_timeout(db, 15000);
      sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
      sqlite3_exec(db, "PRAGMA cache_size=-2048", NULL, NULL, NULL); /* 2MB */
   }
}

sqlite3 *db_open(const char *path)
{
   if (!path)
      path = db_default_path();

   ensure_dir(path);

   sqlite3 *db = NULL;
   int rc = sqlite3_open(path, &db);
   if (rc != SQLITE_OK)
   {
      fprintf(stderr, "db_open: %s\n", sqlite3_errmsg(db));
      sqlite3_close(db);
      return NULL;
   }

   /* Restrict database file permissions (contains API keys, session tokens) */
   chmod(path, 0600);

   db_apply_pragmas(db, DB_MODE_CLI);

   /* Run migrations */
   if (migrate(db, path) != 0)
   {
      sqlite3_close(db);
      return NULL;
   }

   /* FTS5 tables */
   if (!db_fts5_available(db))
   {
      fprintf(stderr, "db_open: FTS5 extension not available\n");
      sqlite3_close(db);
      return NULL;
   }

   if (create_fts_tables(db) != 0)
   {
      sqlite3_close(db);
      return NULL;
   }

   return db;
}

sqlite3 *db_open_fast(const char *path)
{
   if (!path)
      path = db_default_path();

   /* Try fast open first; if table doesn't exist, fall back to full open with migrations */
   sqlite3 *db = NULL;
   int rc = sqlite3_open(path, &db);
   if (rc != SQLITE_OK)
   {
      fprintf(stderr, "db_open_fast: %s\n", sqlite3_errmsg(db));
      sqlite3_close(db);
      return NULL;
   }

   /* Check if schema_migrations table exists to determine if DB is initialized */
   sqlite3_stmt *check = NULL;
   rc = sqlite3_prepare_v2(
       db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='schema_migrations'", -1,
       &check, NULL);
   int initialized = 0;
   if (rc == SQLITE_OK && sqlite3_step(check) == SQLITE_ROW)
      initialized = 1;
   if (check)
      sqlite3_finalize(check);

   if (!initialized)
   {
      sqlite3_close(db);
      return db_open(path);
   }

   /* Check FTS table exists — server connections use db_open_fast() for speed,
    * but after a schema upgrade FTS tables may not exist yet. Fall back to full
    * db_open() which runs migrations and creates FTS tables. */
   sqlite3_stmt *fts_check = NULL;
   rc = sqlite3_prepare_v2(db,
                           "SELECT 1 FROM sqlite_master WHERE type='table' AND name='memories_fts'",
                           -1, &fts_check, NULL);
   int has_fts = (rc == SQLITE_OK && sqlite3_step(fts_check) == SQLITE_ROW);
   if (fts_check)
      sqlite3_finalize(fts_check);

   if (!has_fts)
   {
      sqlite3_close(db);
      return db_open(path);
   }

   db_apply_pragmas(db, DB_MODE_CLI);

   return db;
}

void db_close(sqlite3 *db)
{
   if (!db)
      return;
   db_stmt_cache_clear_for(db);
   sqlite3_close(db);
}

/* --- Public backup / check / recover --- */

int db_backup(const char *db_path, const char *out_path)
{
   if (!db_path)
      db_path = db_default_path();

   sqlite3 *src = NULL;
   if (sqlite3_open_v2(db_path, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      if (src)
         sqlite3_close(src);
      return -1;
   }

   /* Generate output path if not provided */
   char auto_path[MAX_PATH_LEN];
   if (!out_path)
   {
      char ts[32];
      now_utc(ts, sizeof(ts));
      /* Replace colons and spaces for filesystem safety */
      for (char *p = ts; *p; p++)
         if (*p == ':' || *p == ' ')
            *p = '-';
      snprintf(auto_path, sizeof(auto_path), "%s.manual.%s", db_path, ts);
      out_path = auto_path;
   }

   sqlite3 *dst = NULL;
   if (sqlite3_open(out_path, &dst) != SQLITE_OK)
   {
      sqlite3_close(src);
      if (dst)
         sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
   if (!b)
   {
      sqlite3_close(src);
      sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup_step(b, -1);
   int rc = sqlite3_backup_finish(b);
   sqlite3_close(src);
   sqlite3_close(dst);

   if (rc != SQLITE_OK)
   {
      unlink(out_path);
      return -1;
   }

   chmod(out_path, 0600);
   fprintf(stderr, "backup saved to %s\n", out_path);
   return 0;
}

int db_check(const char *db_path, int full)
{
   if (!db_path)
      db_path = db_default_path();

   sqlite3 *db = NULL;
   if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      fprintf(stderr, "db check: cannot open %s\n", db_path);
      if (db)
         sqlite3_close(db);
      return -1;
   }

   const char *pragma = full ? "PRAGMA integrity_check" : "PRAGMA quick_check";
   sqlite3_stmt *stmt = NULL;
   int ok = 1;

   if (sqlite3_prepare_v2(db, pragma, -1, &stmt, NULL) == SQLITE_OK)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *result = (const char *)sqlite3_column_text(stmt, 0);
         if (result && strcmp(result, "ok") != 0)
         {
            fprintf(stderr, "%s\n", result);
            ok = 0;
         }
      }
      sqlite3_finalize(stmt);
   }
   else
   {
      fprintf(stderr, "db check: failed to run %s\n", pragma);
      ok = 0;
   }

   sqlite3_close(db);

   if (ok)
      fprintf(stderr, "ok\n");

   return ok ? 0 : -1;
}

int db_quick_check(sqlite3 *db)
{
   sqlite3_stmt *stmt = NULL;
   int ok = 1;

   if (sqlite3_prepare_v2(db, "PRAGMA quick_check", -1, &stmt, NULL) == SQLITE_OK)
   {
      while (sqlite3_step(stmt) == SQLITE_ROW)
      {
         const char *result = (const char *)sqlite3_column_text(stmt, 0);
         if (result && strcmp(result, "ok") != 0)
         {
            ok = 0;
            break;
         }
      }
      sqlite3_finalize(stmt);
   }
   else
      ok = 0;

   return ok ? 0 : -1;
}

int db_recover(const char *db_path, int force)
{
   if (!db_path)
      db_path = db_default_path();

   /* Search for backup files: .bak.N where N is highest first */
   char bak[MAX_PATH_LEN];
   int found_version = -1;

   for (int v = 999; v >= 0; v--)
   {
      snprintf(bak, sizeof(bak), "%s.bak.%d", db_path, v);
      struct stat st;
      if (stat(bak, &st) == 0)
      {
         /* Validate the backup with quick_check */
         sqlite3 *bdb = NULL;
         if (sqlite3_open_v2(bak, &bdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
         {
            if (db_quick_check(bdb) == 0)
            {
               found_version = v;
               sqlite3_close(bdb);
               break;
            }
            sqlite3_close(bdb);
         }
      }
   }

   if (found_version < 0)
   {
      if (!force)
      {
         fprintf(stderr, "aimee: database corrupted and no valid backup found.\n"
                         "To start fresh (all memories, rules, and tasks will be lost):\n"
                         "  aimee db recover --force\n");
         return -1;
      }

      /* Force: remove the corrupted database so db_open creates a fresh one */
      unlink(db_path);
      char wal[MAX_PATH_LEN], shm[MAX_PATH_LEN];
      snprintf(wal, sizeof(wal), "%s-wal", db_path);
      snprintf(shm, sizeof(shm), "%s-shm", db_path);
      unlink(wal);
      unlink(shm);
      fprintf(stderr, "aimee: created fresh database (previous data lost)\n");
      return 0;
   }

   /* Restore from backup using the backup API for atomicity */
   snprintf(bak, sizeof(bak), "%s.bak.%d", db_path, found_version);

   sqlite3 *src = NULL;
   if (sqlite3_open_v2(bak, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
   {
      if (src)
         sqlite3_close(src);
      return -1;
   }

   /* Remove existing corrupted db first */
   unlink(db_path);
   char wal[MAX_PATH_LEN], shm[MAX_PATH_LEN];
   snprintf(wal, sizeof(wal), "%s-wal", db_path);
   snprintf(shm, sizeof(shm), "%s-shm", db_path);
   unlink(wal);
   unlink(shm);

   sqlite3 *dst = NULL;
   if (sqlite3_open(db_path, &dst) != SQLITE_OK)
   {
      sqlite3_close(src);
      if (dst)
         sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
   if (!b)
   {
      sqlite3_close(src);
      sqlite3_close(dst);
      return -1;
   }

   sqlite3_backup_step(b, -1);
   int rc = sqlite3_backup_finish(b);
   sqlite3_close(src);
   sqlite3_close(dst);

   if (rc != SQLITE_OK)
      return -1;

   chmod(db_path, 0600);
   fprintf(stderr, "aimee: recovered from backup version %d\n", found_version);
   return 0;
}
