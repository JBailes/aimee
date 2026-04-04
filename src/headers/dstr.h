/* dstr.h: lightweight dynamic string library */
#ifndef DEC_DSTR_H
#define DEC_DSTR_H 1

#include <stddef.h>
#include <stdarg.h>

typedef struct
{
   char *data;
   size_t len;
   size_t cap;
} dstr_t;

/* Initialize a dynamic string (empty, no allocation). */
void dstr_init(dstr_t *s);

/* Free the backing buffer and reset to empty. */
void dstr_free(dstr_t *s);

/* Append raw bytes. */
void dstr_append(dstr_t *s, const char *data, size_t len);

/* Append a NUL-terminated string. */
void dstr_append_str(dstr_t *s, const char *str);

/* Append a single character. */
void dstr_append_char(dstr_t *s, char c);

/* Append formatted output (printf-style). */
__attribute__((format(printf, 2, 3))) void dstr_appendf(dstr_t *s, const char *fmt, ...);

/* Append formatted output (va_list variant). */
void dstr_vappendf(dstr_t *s, const char *fmt, va_list ap);

/* Reset length to 0 without freeing (reuse buffer). */
void dstr_reset(dstr_t *s);

/* Ensure at least `additional` bytes of free capacity. */
void dstr_reserve(dstr_t *s, size_t additional);

/* Steal the backing buffer. Caller owns the returned pointer (must free()).
 * The dstr is reset to empty after this call. Returns NULL if empty. */
char *dstr_steal(dstr_t *s);

/* Return the string data (always NUL-terminated, never NULL). */
static inline const char *dstr_cstr(const dstr_t *s)
{
   return s->data ? s->data : "";
}

/* Return the current length. */
static inline size_t dstr_len(const dstr_t *s)
{
   return s->len;
}

#endif /* DEC_DSTR_H */
