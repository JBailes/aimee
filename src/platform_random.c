/* platform_random.c: portable cryptographic random generation */
#include "platform_random.h"
#include <stdio.h>
#include <string.h>

#ifdef AIMEE_WINDOWS

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

int platform_random_bytes(void *buf, size_t len)
{
   NTSTATUS status =
       BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
   return NT_SUCCESS(status) ? 0 : -1;
}

#else /* POSIX (Linux + macOS) */

int platform_random_bytes(void *buf, size_t len)
{
   FILE *f = fopen("/dev/urandom", "r");
   if (!f)
      return -1;
   size_t n = fread(buf, 1, len, f);
   fclose(f);
   return (n == len) ? 0 : -1;
}

#endif

int platform_random_hex(char *out, size_t hex_len)
{
   if (hex_len == 0 || (hex_len % 2) != 0)
      return -1;

   size_t raw_len = hex_len / 2;
   unsigned char raw[256];
   if (raw_len > sizeof(raw))
      return -1;

   if (platform_random_bytes(raw, raw_len) != 0)
   {
      memset(out, '0', hex_len);
      out[hex_len] = '\0';
      return -1;
   }

   for (size_t i = 0; i < raw_len; i++)
      snprintf(out + i * 2, 3, "%02x", raw[i]);
   out[hex_len] = '\0';
   return 0;
}
