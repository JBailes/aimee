#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"

int main(void)
{
   printf("working_memory: ");

   sqlite3 *db = db_open(":memory:");
   assert(db != NULL);

   /* --- wm_set and wm_get --- */
   {
      int rc = wm_set(db, "sess-1", "plan", "implement feature X", "task", 0);
      assert(rc == 0);

      wm_entry_t entry;
      rc = wm_get(db, "sess-1", "plan", &entry);
      assert(rc == 0);
      assert(strcmp(entry.key, "plan") == 0);
      assert(strcmp(entry.value, "implement feature X") == 0);
      assert(strcmp(entry.category, "task") == 0);
   }

   /* --- wm_get: nonexistent key --- */
   {
      wm_entry_t entry;
      int rc = wm_get(db, "sess-1", "nonexistent", &entry);
      assert(rc != 0);
   }

   /* --- wm_set: upsert overwrites --- */
   {
      wm_set(db, "sess-1", "plan", "updated plan", "task", 0);

      wm_entry_t entry;
      int rc = wm_get(db, "sess-1", "plan", &entry);
      assert(rc == 0);
      assert(strcmp(entry.value, "updated plan") == 0);
   }

   /* --- wm_list --- */
   {
      wm_set(db, "sess-1", "key2", "value2", "general", 0);
      wm_set(db, "sess-1", "key3", "value3", "general", 0);

      wm_entry_t entries[16];
      int count = wm_list(db, "sess-1", NULL, entries, 16);
      assert(count >= 3); /* plan + key2 + key3 */

      /* Filter by category */
      count = wm_list(db, "sess-1", "task", entries, 16);
      assert(count == 1);
      assert(strcmp(entries[0].key, "plan") == 0);
   }

   /* --- wm_delete --- */
   {
      int rc = wm_delete(db, "sess-1", "key2");
      assert(rc == 0);

      wm_entry_t entry;
      rc = wm_get(db, "sess-1", "key2", &entry);
      assert(rc != 0); /* Should be gone */
   }

   /* --- session isolation --- */
   {
      wm_set(db, "sess-2", "other", "other value", "general", 0);

      wm_entry_t entries[16];
      int count = wm_list(db, "sess-1", NULL, entries, 16);
      /* sess-1 should not see sess-2's entries */
      for (int i = 0; i < count; i++)
         assert(strcmp(entries[i].key, "other") != 0);
   }

   /* --- wm_clear --- */
   {
      int rc = wm_clear(db, "sess-1");
      assert(rc == 0);

      wm_entry_t entries[16];
      int count = wm_list(db, "sess-1", NULL, entries, 16);
      assert(count == 0);

      /* sess-2 should still have its data */
      count = wm_list(db, "sess-2", NULL, entries, 16);
      assert(count == 1);
   }

   /* --- wm_gc: expired entries removed --- */
   {
      /* Set with TTL of 1 second */
      wm_set(db, "sess-gc", "ephemeral", "temp", "general", 1);

      wm_entry_t entry;
      int rc = wm_get(db, "sess-gc", "ephemeral", &entry);
      assert(rc == 0); /* Should exist right after creation */

      /* Sleep to let it expire */
      sleep(2);
      int removed = wm_gc(db);
      assert(removed >= 1);

      rc = wm_get(db, "sess-gc", "ephemeral", &entry);
      assert(rc != 0); /* Should be gone */
   }

   /* --- wm_assemble_context --- */
   {
      wm_set(db, "sess-ctx", "current_task", "testing", "task", 0);
      wm_set(db, "sess-ctx", "note", "important note", "general", 0);

      char *ctx = wm_assemble_context(db, "sess-ctx");
      assert(ctx != NULL);
      assert(strlen(ctx) > 0);
      assert(strstr(ctx, "current_task") != NULL);
      assert(strstr(ctx, "testing") != NULL);
      free(ctx);
   }

   /* --- attempt log: store and retrieve via 'attempt' category --- */
   {
      /* Record structured attempt data */
      const char *attempt_val =
          "{\"task_context\":\"fix auth\",\"approach\":\"changed token TTL\","
          "\"outcome\":\"tests still fail\",\"lesson\":\"TTL is not the issue\"}";
      int rc = wm_set(db, "sess-attempt", "attempt:1", attempt_val, "attempt", 14400);
      assert(rc == 0);

      /* Record another */
      const char *attempt_val2 =
          "{\"task_context\":\"fix auth\",\"approach\":\"regenerated certs\","
          "\"outcome\":\"cert mismatch error\",\"lesson\":\"need CA-signed certs\"}";
      rc = wm_set(db, "sess-attempt", "attempt:2", attempt_val2, "attempt", 14400);
      assert(rc == 0);

      /* List by category */
      wm_entry_t entries[16];
      int count = wm_list(db, "sess-attempt", "attempt", entries, 16);
      assert(count == 2);

      /* Verify structured content is retrievable */
      wm_entry_t entry;
      rc = wm_get(db, "sess-attempt", "attempt:1", &entry);
      assert(rc == 0);
      assert(strstr(entry.value, "changed token TTL") != NULL);
      assert(strcmp(entry.category, "attempt") == 0);

      /* Session isolation: other sessions don't see attempts */
      count = wm_list(db, "sess-other", "attempt", entries, 16);
      assert(count == 0);
   }

   db_stmt_cache_clear();
   db_close(db);

   printf("all tests passed\n");
   return 0;
}
