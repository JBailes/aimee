#ifndef DEC_SECRET_STORE_H
#define DEC_SECRET_STORE_H 1

#include <stddef.h>

#define SECRET_SERVICE "aimee"

/* Backend abstraction for secret storage */
typedef struct
{
   int (*store)(const char *service, const char *key, const char *value);
   int (*load)(const char *service, const char *key, char *buf, size_t len);
   int (*remove)(const char *service, const char *key);
   const char *name;
} secret_backend_t;

/* Get the active secret backend (keyring if available, file fallback).
 * The returned pointer is static and valid for the process lifetime. */
const secret_backend_t *secret_backend(void);

/* Convenience: store a secret via the active backend. Returns 0 on success. */
int secret_store(const char *key, const char *value);

/* Convenience: load a secret via the active backend. Returns 0 on success. */
int secret_load(const char *key, char *buf, size_t len);

/* Convenience: remove a secret via the active backend. Returns 0 on success. */
int secret_remove(const char *key);

#endif /* DEC_SECRET_STORE_H */
