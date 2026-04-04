/* cli_launch.c: parse __LAUNCH__ metadata from server output */
#include "cli_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int parse_launch_meta(const char *output, launch_meta_t *meta)
{
   memset(meta, 0, sizeof(*meta));
   snprintf(meta->provider, sizeof(meta->provider), "claude");

   const char *launch_marker = strstr(output, "__LAUNCH__");
   if (!launch_marker)
      return 0;

   meta->context_len = (size_t)(launch_marker - output);

   const char *json_start = launch_marker + strlen("__LAUNCH__");
   const char *line_end = strchr(json_start, '\n');
   size_t json_len = line_end ? (size_t)(line_end - json_start) : strlen(json_start);
   char *json_buf = malloc(json_len + 1);
   if (!json_buf)
      return 0;

   memcpy(json_buf, json_start, json_len);
   json_buf[json_len] = '\0';

   cJSON *jmeta = cJSON_Parse(json_buf);
   free(json_buf);
   if (!jmeta)
      return 0;

   cJSON *jprovider = cJSON_GetObjectItemCaseSensitive(jmeta, "provider");
   cJSON *jbuiltin = cJSON_GetObjectItemCaseSensitive(jmeta, "builtin");
   cJSON *jauto = cJSON_GetObjectItemCaseSensitive(jmeta, "autonomous");
   cJSON *jwt_cwd = cJSON_GetObjectItemCaseSensitive(jmeta, "worktree_cwd");

   if (cJSON_IsString(jprovider))
      snprintf(meta->provider, sizeof(meta->provider), "%s", jprovider->valuestring);
   meta->builtin = cJSON_IsTrue(jbuiltin);
   meta->autonomous = cJSON_IsTrue(jauto);
   if (cJSON_IsString(jwt_cwd))
      snprintf(meta->worktree_cwd, sizeof(meta->worktree_cwd), "%s", jwt_cwd->valuestring);

   cJSON_Delete(jmeta);
   return 1;
}
