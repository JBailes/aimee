/* cmd_rules.c: rules and feedback CLI (list, generate, delete, +/- with directive flags) */
#include "aimee.h"
#include "commands.h"
#include "cJSON.h"

/* --- cmd_rules --- */

void cmd_rules(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("rules requires a subcommand: list, generate, delete");

   const char *sub = argv[0];
   argc--;
   argv++;

   sqlite3 *db = ctx_db_open_fast(ctx);
   if (!db)
      fatal("cannot open database");

   if (strcmp(sub, "list") == 0)
   {
      rule_t rules[256];
      int count = rules_list(db, rules, 256);
      if (ctx->json_output)
      {
         cJSON *arr = cJSON_CreateArray();
         for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(arr, rule_to_json(&rules[i]));
         emit_json_ctx(arr, ctx->json_fields, ctx->response_profile);
      }
   }
   else if (strcmp(sub, "generate") == 0)
   {
      char *md = rules_generate(db);
      if (ctx->json_output)
      {
         cJSON *j = cJSON_CreateObject();
         cJSON_AddStringToObject(j, "content", md ? md : "");
         emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
      }
      else if (md)
      {
         printf("%s", md);
      }
      free(md);
   }
   else if (strcmp(sub, "delete") == 0)
   {
      if (argc < 1)
         fatal("rules delete requires an id");
      int id = atoi(argv[0]);
      rules_delete(db, id);
      if (ctx->json_output)
         emit_ok_ctx(ctx->json_fields, ctx->response_profile);
   }
   else
   {
      fatal("unknown rules subcommand: %s", sub);
   }

   ctx_db_close(ctx);
}

/* --- cmd_feedback --- */

static void cmd_feedback_impl(app_ctx_t *ctx, int argc, char **argv, const char *polarity_hint)
{
   static const char *bool_flags[] = {"hard", "soft", "session", NULL};
   opt_parsed_t opts;
   opt_parse(argc, argv, bool_flags, &opts);
   int weight = opt_get_int(&opts, "weight", -1);
   const char *directive_type = NULL;
   if (opt_has(&opts, "hard"))
      directive_type = "hard";
   else if (opt_has(&opts, "soft"))
      directive_type = "soft";
   else if (opt_has(&opts, "session"))
      directive_type = "session";
   const char *polarity_str = polarity_hint ? polarity_hint : opt_pos(&opts, 0);
   const char *description = polarity_hint ? opt_pos(&opts, 0) : opt_pos(&opts, 1);

   if (!polarity_str)
      fatal("feedback requires a polarity (+, -, principle)");
   if (!description)
      fatal("feedback requires a description");

   const char *polarity = feedback_parse_polarity(polarity_str);
   if (!polarity)
      fatal("invalid polarity: %s", polarity_str);

   sqlite3 *db = ctx_db_open_fast(ctx);
   if (!db)
      fatal("cannot open database");

   int reinforced = 0;
   int id = feedback_record(db, polarity, description, "", weight, &reinforced);
   if (id < 0)
      fatal("failed to record feedback");

   /* Set directive type if specified (Feature 15) */
   if (directive_type && id > 0)
   {
      static const char *sql = "UPDATE rules SET directive_type = ? WHERE id = ?";
      sqlite3_stmt *stmt = db_prepare(db, sql);
      if (stmt)
      {
         sqlite3_reset(stmt);
         sqlite3_bind_text(stmt, 1, directive_type, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int(stmt, 2, id);
         DB_STEP_LOG(stmt, "cmd_feedback_impl");
      }
   }

   if (ctx->json_output)
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "status", "ok");
      cJSON_AddNumberToObject(j, "id", id);
      cJSON_AddBoolToObject(j, "reinforced", reinforced);
      if (directive_type)
         cJSON_AddStringToObject(j, "directive_type", directive_type);
      emit_json_ctx(j, ctx->json_fields, ctx->response_profile);
   }

   ctx_db_close(ctx);
}

void cmd_feedback_raw(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_feedback_impl(ctx, argc, argv, NULL);
}

void cmd_feedback_plus(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_feedback_impl(ctx, argc, argv, "+");
}

void cmd_feedback_minus(app_ctx_t *ctx, int argc, char **argv)
{
   cmd_feedback_impl(ctx, argc, argv, "-");
}
