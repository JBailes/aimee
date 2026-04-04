#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "cJSON.h"

int main(void)
{
   printf("render: ");

   /* --- memory_to_json --- */
   {
      memory_t m;
      memset(&m, 0, sizeof(m));
      m.id = 42;
      snprintf(m.tier, sizeof(m.tier), "L1");
      snprintf(m.kind, sizeof(m.kind), "fact");
      snprintf(m.key, sizeof(m.key), "test-key");
      snprintf(m.content, sizeof(m.content), "test content");
      m.confidence = 0.95;
      m.use_count = 3;
      snprintf(m.created_at, sizeof(m.created_at), "2026-01-01");

      cJSON *j = memory_to_json(&m);
      assert(j != NULL);
      assert(cJSON_GetObjectItem(j, "id") != NULL);
      assert(cJSON_GetObjectItem(j, "id")->valuedouble == 42.0);
      assert(strcmp(cJSON_GetObjectItem(j, "tier")->valuestring, "L1") == 0);
      assert(strcmp(cJSON_GetObjectItem(j, "kind")->valuestring, "fact") == 0);
      assert(strcmp(cJSON_GetObjectItem(j, "key")->valuestring, "test-key") == 0);
      assert(strcmp(cJSON_GetObjectItem(j, "content")->valuestring, "test content") == 0);
      assert(cJSON_GetObjectItem(j, "confidence")->valuedouble > 0.94);
      assert(cJSON_GetObjectItem(j, "use_count")->valuedouble == 3.0);
      cJSON_Delete(j);
   }

   /* --- rule_to_json --- */
   {
      rule_t r;
      memset(&r, 0, sizeof(r));
      r.id = 7;
      snprintf(r.polarity, sizeof(r.polarity), "negative");
      snprintf(r.title, sizeof(r.title), "do not force push");
      r.weight = 50;

      cJSON *j = rule_to_json(&r);
      assert(j != NULL);
      assert(cJSON_GetObjectItem(j, "id")->valuedouble == 7.0);
      assert(strcmp(cJSON_GetObjectItem(j, "polarity")->valuestring, "negative") == 0);
      assert(strcmp(cJSON_GetObjectItem(j, "title")->valuestring, "do not force push") == 0);
      assert(cJSON_GetObjectItem(j, "weight")->valuedouble == 50.0);
      cJSON_Delete(j);
   }

   /* --- aimee_task_to_json --- */
   {
      aimee_task_t t;
      memset(&t, 0, sizeof(t));
      t.id = 99;
      snprintf(t.title, sizeof(t.title), "implement feature");
      snprintf(t.state, sizeof(t.state), "in_progress");
      t.parent_id = 10;

      cJSON *j = aimee_task_to_json(&t);
      assert(j != NULL);
      assert(cJSON_GetObjectItem(j, "id")->valuedouble == 99.0);
      assert(strcmp(cJSON_GetObjectItem(j, "title")->valuestring, "implement feature") == 0);
      assert(strcmp(cJSON_GetObjectItem(j, "state")->valuestring, "in_progress") == 0);
      cJSON_Delete(j);
   }

   printf("all tests passed\n");
   return 0;
}
