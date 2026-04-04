/* test_server_auth.c: tests for method registry completeness and capability lookup */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "server.h"

/* Stubs for server functions referenced by server_auth.o but not needed by tests */
int server_send_response(server_conn_t *conn, cJSON *resp)
{
   (void)conn;
   (void)resp;
   return 0;
}
int server_send_error(server_conn_t *conn, const char *message, const char *request_id)
{
   (void)conn;
   (void)message;
   (void)request_id;
   return 0;
}

/* All methods that appear in server_dispatch(). Keep this list in sync
 * with the dispatch table in server.c. The registry completeness test
 * will fail loudly if a method is added to dispatch but not here AND
 * not in the method_registry. */
static const char *dispatch_methods[] = {"server.info",
                                         "server.health",
                                         "auth",
                                         "hooks.pre",
                                         "hooks.post",
                                         "session.create",
                                         "session.list",
                                         "session.get",
                                         "session.close",
                                         "memory.search",
                                         "memory.store",
                                         "memory.list",
                                         "memory.get",
                                         "index.find",
                                         "index.blast_radius",
                                         "index.list",
                                         "rules.list",
                                         "rules.generate",
                                         "wm.set",
                                         "wm.get",
                                         "wm.list",
                                         "wm.context",
                                         "dashboard.metrics",
                                         "dashboard.delegations",
                                         "workspace.context",
                                         "tool.execute",
                                         "delegate",
                                         "chat.send_stream",
                                         "cli.forward",
                                         NULL};

/* Test: every dispatch method is covered by the registry (not CAPS_ALL) */
static void test_registry_completeness(void)
{
   int failures = 0;
   for (int i = 0; dispatch_methods[i]; i++)
   {
      const method_policy_t *policy = server_policy_for_method(dispatch_methods[i]);
      if (!policy)
      {
         fprintf(stderr,
                 "FAIL: dispatch method '%s' has no registry entry "
                 "(would require CAPS_ALL)\n",
                 dispatch_methods[i]);
         failures++;
      }
   }
   assert(failures == 0);
   printf("  registry_completeness: OK (%d methods checked)\n",
          (int)(sizeof(dispatch_methods) / sizeof(dispatch_methods[0]) - 1));
}

/* Test: exact method lookups return expected capabilities */
static void test_exact_capability_lookup(void)
{
   /* No-auth methods */
   assert(server_capability_for_method("server.info") == 0);
   assert(server_capability_for_method("server.health") == 0);
   assert(server_capability_for_method("auth") == 0);

   /* Hooks */
   assert(server_capability_for_method("hooks.pre") == CAP_TOOL_EXECUTE);
   assert(server_capability_for_method("hooks.post") == CAP_TOOL_EXECUTE);

   /* Memory: exact match for store, prefix for others */
   assert(server_capability_for_method("memory.store") == CAP_MEMORY_WRITE);
   assert(server_capability_for_method("memory.search") == CAP_MEMORY_READ);
   assert(server_capability_for_method("memory.list") == CAP_MEMORY_READ);
   assert(server_capability_for_method("memory.get") == CAP_MEMORY_READ);

   /* Sessions (prefix) */
   assert(server_capability_for_method("session.create") == CAP_SESSION_READ);
   assert(server_capability_for_method("session.list") == CAP_SESSION_READ);

   /* Index (prefix) */
   assert(server_capability_for_method("index.find") == CAP_INDEX_READ);
   assert(server_capability_for_method("index.blast_radius") == CAP_INDEX_READ);

   /* Rules (prefix) */
   assert(server_capability_for_method("rules.list") == CAP_RULES_READ);
   assert(server_capability_for_method("rules.generate") == CAP_RULES_READ);

   /* Working memory (prefix) */
   assert(server_capability_for_method("wm.set") == CAP_SESSION_READ);
   assert(server_capability_for_method("wm.get") == CAP_SESSION_READ);

   /* Dashboard (prefix) */
   assert(server_capability_for_method("dashboard.metrics") == CAP_DASHBOARD_READ);

   /* Workspace */
   assert(server_capability_for_method("workspace.context") == CAP_INDEX_READ);

   /* Compute */
   assert(server_capability_for_method("tool.execute") == CAP_TOOL_EXECUTE);
   assert(server_capability_for_method("delegate") == CAP_DELEGATE);
   assert(server_capability_for_method("chat.send_stream") == CAP_CHAT);
   assert(server_capability_for_method("cli.forward") == CAP_TOOL_EXECUTE);

   printf("  exact_capability_lookup: OK\n");
}

/* Test: unknown methods require CAPS_ALL (deny-by-default) */
static void test_unknown_method_requires_all(void)
{
   assert(server_capability_for_method("nonexistent.method") == CAPS_ALL);
   assert(server_capability_for_method("admin.shutdown") == CAPS_ALL);
   assert(server_capability_for_method("") == CAPS_ALL);
   printf("  unknown_method_requires_all: OK\n");
}

/* Test: policy lookup returns correct description */
static void test_policy_lookup_description(void)
{
   const method_policy_t *p = server_policy_for_method("memory.store");
   assert(p != NULL);
   assert(strcmp(p->description, "store memory") == 0);
   assert(p->required_caps == CAP_MEMORY_WRITE);

   /* Prefix match */
   p = server_policy_for_method("dashboard.metrics");
   assert(p != NULL);
   assert(strcmp(p->description, "dashboard operation") == 0);

   /* Unknown method */
   p = server_policy_for_method("nonexistent");
   assert(p == NULL);

   printf("  policy_lookup_description: OK\n");
}

/* Test: registry has no duplicate exact entries */
static void test_no_duplicate_entries(void)
{
   for (int i = 0; i < method_registry_count; i++)
   {
      const char *a = method_registry[i].method;
      size_t alen = strlen(a);
      /* Only check exact entries (not prefixes) */
      if (alen > 0 && a[alen - 1] == '*')
         continue;

      for (int j = i + 1; j < method_registry_count; j++)
      {
         const char *b = method_registry[j].method;
         size_t blen = strlen(b);
         if (blen > 0 && b[blen - 1] == '*')
            continue;
         if (strcmp(a, b) == 0)
         {
            fprintf(stderr, "FAIL: duplicate registry entry '%s' at indices %d and %d\n", a, i, j);
            assert(0);
         }
      }
   }
   printf("  no_duplicate_entries: OK\n");
}

int main(void)
{
   printf("test_server_auth:\n");
   test_registry_completeness();
   test_exact_capability_lookup();
   test_unknown_method_requires_all();
   test_policy_lookup_description();
   test_no_duplicate_entries();
   printf("All server_auth tests passed.\n");
   return 0;
}
