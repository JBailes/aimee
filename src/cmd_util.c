/* cmd_util.c: command handler utilities (db open/close, subcommand dispatch) */
#include "aimee.h"
#include "commands.h"

/* --- subcmd_usage --- */

void subcmd_usage(const char *parent, const subcmd_t *table)
{
   fprintf(stderr, "Usage: aimee %s <subcommand> [args...]\n\nSubcommands:\n", parent);
   for (int i = 0; table[i].name; i++)
   {
      if (table[i].help)
         fprintf(stderr, "  %-16s %s\n", table[i].name, table[i].help);
      else
         fprintf(stderr, "  %s\n", table[i].name);
   }
}

/* --- subcmd_dispatch --- */

int subcmd_dispatch(const subcmd_t *table, const char *name, app_ctx_t *ctx, sqlite3 *db, int argc,
                    char **argv)
{
   if (!name || strcmp(name, "help") == 0 || strcmp(name, "--help") == 0)
   {
      /* Derive parent name from table context is not possible here,
       * so just print a generic header. Callers that want a specific
       * parent name should call subcmd_usage() directly. */
      fprintf(stderr, "Subcommands:\n");
      for (int i = 0; table[i].name; i++)
      {
         if (table[i].help)
            fprintf(stderr, "  %-16s %s\n", table[i].name, table[i].help);
         else
            fprintf(stderr, "  %s\n", table[i].name);
      }
      return 0;
   }

   for (int i = 0; table[i].name; i++)
   {
      if (strcmp(name, table[i].name) == 0)
      {
         table[i].handler(ctx, db, argc, argv);
         return 0;
      }
   }
   return -1;
}

/* --- ctx_db_open / ctx_db_open_fast / ctx_db_close --- */

sqlite3 *ctx_db_open(app_ctx_t *ctx)
{
   if (!ctx->db)
      ctx->db = db_open(NULL);
   return ctx->db;
}

sqlite3 *ctx_db_open_fast(app_ctx_t *ctx)
{
   config_t cfg;
   config_load(&cfg);
   if (!ctx->db)
      ctx->db = db_open_fast(cfg.db_path);
   return ctx->db;
}

void ctx_db_close(app_ctx_t *ctx)
{
   if (ctx->db)
   {
      db_stmt_cache_clear();
      db_close(ctx->db);
      ctx->db = NULL;
   }
}
