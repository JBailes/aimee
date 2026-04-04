#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"

int main(void)
{
   printf("config: ");

   /* Use isolated temp HOME */
   char tmpdir[] = "/tmp/aimee-test-config-XXXXXX";
   assert(mkdtemp(tmpdir) != NULL);
   setenv("HOME", tmpdir, 1);

   /* --- config_load: missing file returns defaults --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      assert(strcmp(cfg.provider, "claude") == 0);
      assert(strcmp(cfg.guardrail_mode, "approve") == 0);
      assert(cfg.db_path[0] != '\0');
   }

   /* --- config_save + config_load round-trip --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      snprintf(cfg.provider, sizeof(cfg.provider), "gemini");
      config_save(&cfg);

      config_t cfg2;
      memset(&cfg2, 0, sizeof(cfg2));
      config_load(&cfg2);
      assert(strcmp(cfg2.provider, "gemini") == 0);
   }

   /* --- config_guardrail_mode --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      config_load(&cfg);
      const char *mode = config_guardrail_mode(&cfg);
      assert(mode != NULL);
      assert(strcmp(mode, "approve") == 0 || strcmp(mode, "prompt") == 0 ||
             strcmp(mode, "deny") == 0);
   }

   /* --- session_id: returns non-empty, stable across calls --- */
   {
      setenv("CLAUDE_SESSION_ID", "test-session-42", 1);
      /* Note: session_id() caches on first call, so this only works
       * if it hasn't been called yet in this process. Since we set the
       * env before any call, it should pick it up. */
   }

   /* --- config_default_dir: contains .config/aimee --- */
   {
      const char *dir = config_default_dir();
      assert(dir != NULL);
      assert(strstr(dir, ".config/aimee") != NULL);
   }

   /* --- schema validation: valid config passes --- */
   {
      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 0;
      setenv("AIMEE_NO_CACHE", "1", 1); /* force re-parse */
      int rc = config_load(&cfg);
      assert(rc == 0);
      assert(strcmp(cfg.provider, "gemini") == 0); /* from earlier save */
   }

   /* --- schema validation: unknown key produces warning (non-strict) --- */
   {
      /* Write config with unknown key */
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/config.json", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "{\"provider\":\"claude\",\"bogus_key\":\"value\"}");
      fclose(f);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 0;
      int rc = config_load(&cfg);
      assert(rc == 0); /* warnings only, does not fail */
      assert(strcmp(cfg.provider, "claude") == 0);
   }

   /* --- schema validation: strict mode rejects unknown key --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/config.json", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "{\"provider\":\"claude\",\"bogus_key\":\"value\"}");
      fclose(f);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == -1); /* strict mode rejects */
      g_config_strict = 0;
   }

   /* --- schema validation: type mismatch detected --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/config.json", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "{\"provider\":123}"); /* should be string, not number */
      fclose(f);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == -1); /* type mismatch in strict mode */
      g_config_strict = 0;
   }

   /* --- schema validation: valid config passes strict mode --- */
   {
      char cpath[512];
      snprintf(cpath, sizeof(cpath), "%s/.config/aimee/config.json", tmpdir);
      FILE *f = fopen(cpath, "w");
      assert(f);
      fprintf(f, "{\"provider\":\"claude\",\"use_builtin_cli\":true}");
      fclose(f);

      config_t cfg;
      memset(&cfg, 0, sizeof(cfg));
      g_config_strict = 1;
      int rc = config_load(&cfg);
      assert(rc == 0); /* all keys valid */
      assert(strcmp(cfg.provider, "claude") == 0);
      assert(cfg.use_builtin_cli == 1);
      g_config_strict = 0;
   }

   unsetenv("AIMEE_NO_CACHE");

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
   (void)system(cmd);

   printf("all tests passed\n");
   return 0;
}
