/* dstr.c: lightweight dynamic string library */
#include "dstr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DSTR_INIT_CAP 64

void dstr_init(dstr_t *s)
{
   s->data = NULL;
   s->len = 0;
   s->cap = 0;
}

void dstr_free(dstr_t *s)
{
   free(s->data);
   s->data = NULL;
   s->len = 0;
   s->cap = 0;
}

void dstr_reserve(dstr_t *s, size_t additional)
{
   size_t needed = s->len + additional + 1; /* +1 for NUL */
   if (needed <= s->cap)
      return;

   size_t newcap = s->cap ? s->cap : DSTR_INIT_CAP;
   while (newcap < needed)
      newcap *= 2;

   char *p = realloc(s->data, newcap);
   if (!p)
      return; /* OOM: silently keep existing buffer */
   s->data = p;
   s->cap = newcap;
}

void dstr_append(dstr_t *s, const char *data, size_t len)
{
   if (!len)
      return;
   dstr_reserve(s, len);
   if (s->len + len + 1 > s->cap)
      return; /* reserve failed */
   memcpy(s->data + s->len, data, len);
   s->len += len;
   s->data[s->len] = '\0';
}

void dstr_append_str(dstr_t *s, const char *str)
{
   if (str)
      dstr_append(s, str, strlen(str));
}

void dstr_append_char(dstr_t *s, char c)
{
   dstr_append(s, &c, 1);
}

void dstr_vappendf(dstr_t *s, const char *fmt, va_list ap)
{
   va_list ap2;
   va_copy(ap2, ap);

   /* Try to format into existing free space */
   size_t avail = s->cap > s->len ? s->cap - s->len : 0;
   int n = vsnprintf(s->data ? s->data + s->len : NULL, avail, fmt, ap);
   if (n < 0)
   {
      va_end(ap2);
      return;
   }

   if ((size_t)n < avail)
   {
      /* Fit in existing buffer */
      s->len += (size_t)n;
      va_end(ap2);
      return;
   }

   /* Need more space */
   dstr_reserve(s, (size_t)n);
   avail = s->cap > s->len ? s->cap - s->len : 0;
   vsnprintf(s->data + s->len, avail, fmt, ap2);
   s->len += (size_t)n;
   va_end(ap2);
}

void dstr_appendf(dstr_t *s, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   dstr_vappendf(s, fmt, ap);
   va_end(ap);
}

void dstr_reset(dstr_t *s)
{
   s->len = 0;
   if (s->data)
      s->data[0] = '\0';
}

char *dstr_steal(dstr_t *s)
{
   char *p = s->data;
   s->data = NULL;
   s->len = 0;
   s->cap = 0;
   return p;
}
