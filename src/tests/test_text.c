#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"

int main(void)
{
   printf("text: ");

   /* --- canonical_fingerprint --- */
   {
      char buf[512];

      /* Basic: sorts words, lowercases */
      canonical_fingerprint("Hello World", buf, sizeof(buf));
      assert(strlen(buf) > 0);

      /* Same content different order -> same fingerprint */
      char buf2[512];
      canonical_fingerprint("world hello", buf2, sizeof(buf2));
      assert(strcmp(buf, buf2) == 0);

      /* Empty string */
      canonical_fingerprint("", buf, sizeof(buf));
      assert(buf[0] == '\0');

      /* NULL */
      canonical_fingerprint(NULL, buf, sizeof(buf));
      assert(buf[0] == '\0');
   }

   /* --- word_similarity --- */
   {
      /* Identical strings -> 1.0 */
      double sim = word_similarity("hello world", "hello world");
      assert(sim > 0.99);

      /* Completely different -> 0.0 */
      sim = word_similarity("alpha beta", "gamma delta");
      assert(sim < 0.01);

      /* Partial overlap */
      sim = word_similarity("hello world foo", "hello world bar");
      assert(sim > 0.3 && sim < 0.9);

      /* Empty strings */
      sim = word_similarity("", "");
      assert(sim >= 0.0);

      sim = word_similarity("hello", "");
      assert(sim < 0.01);
   }

   /* --- expand_terms_for_fts --- */
   {
      char buf[1024];
      char *terms[] = {"camelCase", "snake_case"};
      char *result = expand_terms_for_fts(terms, 2, buf, sizeof(buf));
      assert(result != NULL);
      assert(strlen(result) > 0);
      /* Should contain the original terms */
      assert(strstr(result, "camelCase") != NULL || strstr(result, "camel") != NULL);
   }

   printf("all tests passed\n");
   return 0;
}
