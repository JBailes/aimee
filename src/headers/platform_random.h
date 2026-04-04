/*
 * platform_random.h: portable cryptographic random number generation.
 *
 * Linux/macOS: /dev/urandom
 * Windows: BCryptGenRandom (future)
 */
#ifndef DEC_PLATFORM_RANDOM_H
#define DEC_PLATFORM_RANDOM_H 1

#include "platform.h"
#include <stddef.h>

/* Fill |buf| with |len| cryptographically random bytes.
 * Returns 0 on success, -1 on error. */
int platform_random_bytes(void *buf, size_t len);

/* Generate a hex-encoded random string of |hex_len| characters (must be even).
 * |out| must have room for hex_len + 1 bytes.
 * Returns 0 on success, -1 on error. */
int platform_random_hex(char *out, size_t hex_len);

#endif /* DEC_PLATFORM_RANDOM_H */
