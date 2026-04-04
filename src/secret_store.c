/* secret_store.c: OS keyring abstraction with file fallback */
#include "aimee.h"
#include "secret_store.h"
#include <sys/stat.h>
#include <unistd.h>

/* --- File backend (always available) --- */

static int file_secret_path(const char *key, char *buf, size_t len)
{
   snprintf(buf, len, "%s/%s", config_default_dir(), key);
   return 0;
}

static int file_store(const char *service, const char *key, const char *value)
{
   (void)service;
   char path[MAX_PATH_LEN];
   file_secret_path(key, path, sizeof(path));

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fprintf(f, "%s\n", value);
   fclose(f);
   chmod(path, 0600);
   return 0;
}

static int file_load(const char *service, const char *key, char *buf, size_t len)
{
   (void)service;
   char path[MAX_PATH_LEN];
   file_secret_path(key, path, sizeof(path));

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   size_t n = fread(buf, 1, len - 1, f);
   fclose(f);
   buf[n] = '\0';
   /* Trim trailing whitespace */
   while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
      buf[--n] = '\0';
   return (n > 0) ? 0 : -1;
}

static int file_remove(const char *service, const char *key)
{
   (void)service;
   char path[MAX_PATH_LEN];
   file_secret_path(key, path, sizeof(path));
   return unlink(path);
}

static const secret_backend_t file_backend = {
    .store = file_store,
    .load = file_load,
    .remove = file_remove,
    .name = "file",
};

/* --- macOS Keychain backend (compile-time, Darwin only) --- */

#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <Security/Security.h>

static int keychain_store(const char *service, const char *key, const char *value)
{
   OSStatus status;
   size_t val_len = strlen(value);

   /* Try to update first */
   SecKeychainItemRef item = NULL;
   status = SecKeychainFindGenericPassword(NULL, (UInt32)strlen(service), service,
                                           (UInt32)strlen(key), key, NULL, NULL, &item);
   if (status == errSecSuccess && item)
   {
      status = SecKeychainItemModifyAttributesAndData(item, NULL, (UInt32)val_len, value);
      CFRelease(item);
      return (status == errSecSuccess) ? 0 : -1;
   }

   /* Add new */
   status = SecKeychainAddGenericPassword(NULL, (UInt32)strlen(service), service,
                                          (UInt32)strlen(key), key, (UInt32)val_len, value, NULL);
   return (status == errSecSuccess) ? 0 : -1;
}

static int keychain_load(const char *service, const char *key, char *buf, size_t len)
{
   UInt32 pw_len = 0;
   void *pw_data = NULL;
   OSStatus status = SecKeychainFindGenericPassword(
       NULL, (UInt32)strlen(service), service, (UInt32)strlen(key), key, &pw_len, &pw_data, NULL);
   if (status != errSecSuccess || !pw_data)
      return -1;

   size_t copy = pw_len < len - 1 ? pw_len : len - 1;
   memcpy(buf, pw_data, copy);
   buf[copy] = '\0';
   SecKeychainItemFreeContent(NULL, pw_data);
   return 0;
}

static int keychain_remove(const char *service, const char *key)
{
   SecKeychainItemRef item = NULL;
   OSStatus status = SecKeychainFindGenericPassword(NULL, (UInt32)strlen(service), service,
                                                    (UInt32)strlen(key), key, NULL, NULL, &item);
   if (status != errSecSuccess || !item)
      return -1;
   status = SecKeychainItemDelete(item);
   CFRelease(item);
   return (status == errSecSuccess) ? 0 : -1;
}

static const secret_backend_t keychain_backend = {
    .store = keychain_store,
    .load = keychain_load,
    .remove = keychain_remove,
    .name = "keychain",
};

#pragma clang diagnostic pop
#endif /* __APPLE__ */

/* --- libsecret backend (Linux, compile-time optional) --- */

#if defined(WITH_LIBSECRET)
#include <libsecret/secret.h>

static const SecretSchema aimee_schema = {
    "com.aimee.secret",
    SECRET_SCHEMA_NONE,
    {{"service", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"key", SECRET_SCHEMA_ATTRIBUTE_STRING}},
};

static int libsecret_store(const char *service, const char *key, const char *value)
{
   GError *err = NULL;
   char label[256];
   snprintf(label, sizeof(label), "aimee %s/%s", service, key);
   gboolean ok = secret_password_store_sync(&aimee_schema, SECRET_COLLECTION_DEFAULT, label, value,
                                            NULL, &err, "service", service, "key", key, NULL);
   if (err)
      g_error_free(err);
   return ok ? 0 : -1;
}

static int libsecret_load(const char *service, const char *key, char *buf, size_t len)
{
   GError *err = NULL;
   gchar *pw =
       secret_password_lookup_sync(&aimee_schema, NULL, &err, "service", service, "key", key, NULL);
   if (err)
      g_error_free(err);
   if (!pw)
      return -1;
   snprintf(buf, len, "%s", pw);
   secret_password_free(pw);
   return 0;
}

static int libsecret_remove(const char *service, const char *key)
{
   GError *err = NULL;
   gboolean ok =
       secret_password_clear_sync(&aimee_schema, NULL, &err, "service", service, "key", key, NULL);
   if (err)
      g_error_free(err);
   return ok ? 0 : -1;
}

static const secret_backend_t libsecret_backend = {
    .store = libsecret_store,
    .load = libsecret_load,
    .remove = libsecret_remove,
    .name = "libsecret",
};

#endif /* WITH_LIBSECRET */

/* --- Backend selection --- */

static const secret_backend_t *detect_backend(void)
{
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
   /* Test keychain access with a dummy lookup (returns errSecItemNotFound if accessible) */
   SecKeychainItemRef item = NULL;
   OSStatus status =
       SecKeychainFindGenericPassword(NULL, 5, "aimee", 5, "_test", NULL, NULL, &item);
   if (status == errSecSuccess || status == errSecItemNotFound)
   {
      fprintf(stderr, "aimee: secret backend: keychain\n");
      if (item)
         CFRelease(item);
      return &keychain_backend;
   }
#pragma clang diagnostic pop
#endif

#if defined(WITH_LIBSECRET)
   /* Test libsecret availability with a dummy lookup */
   GError *err = NULL;
   gchar *pw = secret_password_lookup_sync(&aimee_schema, NULL, &err, "service", "aimee", "key",
                                           "_test", NULL);
   if (!err)
   {
      fprintf(stderr, "aimee: secret backend: libsecret\n");
      if (pw)
         secret_password_free(pw);
      return &libsecret_backend;
   }
   g_error_free(err);
#endif

   fprintf(stderr, "aimee: secret backend: file\n");
   return &file_backend;
}

const secret_backend_t *secret_backend(void)
{
   static const secret_backend_t *cached = NULL;
   if (!cached)
      cached = detect_backend();
   return cached;
}

/* --- Convenience wrappers --- */

int secret_store(const char *key, const char *value)
{
   return secret_backend()->store(SECRET_SERVICE, key, value);
}

int secret_load(const char *key, char *buf, size_t len)
{
   return secret_backend()->load(SECRET_SERVICE, key, buf, len);
}

int secret_remove(const char *key)
{
   return secret_backend()->remove(SECRET_SERVICE, key);
}
