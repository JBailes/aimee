/* agent_http.c: HTTP POST via libcurl (platform abstraction for agent API calls) */
#include "aimee.h"
#include "agent_exec.h"
#include <string.h>

#ifdef _WIN32

/* ================================================================
 * Windows: WinHTTP implementation
 * ================================================================ */

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

void agent_http_init(void)
{
   /* WinHTTP does not need global init */
}

void agent_http_cleanup(void)
{
}

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   (void)extra_headers; /* Phase 2: add WinHTTP extra headers support */
   *response_buf = NULL;

   /* Parse URL into components */
   wchar_t wurl[2048];
   MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 2048);

   URL_COMPONENTS uc;
   memset(&uc, 0, sizeof(uc));
   uc.dwStructSize = sizeof(uc);
   wchar_t host[256], path[1024];
   uc.lpszHostName = host;
   uc.dwHostNameLength = 256;
   uc.lpszUrlPath = path;
   uc.dwUrlPathLength = 1024;

   if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
      return -1;

   HINTERNET session = WinHttpOpen(L"aimee/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
   if (!session)
      return -1;

   /* Set timeouts */
   WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

   BOOL use_ssl = (uc.nScheme == INTERNET_SCHEME_HTTPS);
   HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
   if (!conn)
   {
      WinHttpCloseHandle(session);
      return -1;
   }

   DWORD flags = use_ssl ? WINHTTP_FLAG_SECURE : 0;
   HINTERNET req = WinHttpOpenRequest(conn, L"POST", path, NULL, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
   if (!req)
   {
      WinHttpCloseHandle(conn);
      WinHttpCloseHandle(session);
      return -1;
   }

   /* Add headers */
   WinHttpAddRequestHeaders(req, L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);

   if (auth_header && auth_header[0])
   {
      wchar_t wauth[1024];
      MultiByteToWideChar(CP_UTF8, 0, auth_header, -1, wauth, 1024);
      WinHttpAddRequestHeaders(req, wauth, -1, WINHTTP_ADDREQ_FLAG_ADD);
   }

   /* Send */
   DWORD body_len = (DWORD)strlen(body);
   if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (void *)body, body_len, body_len,
                           0) ||
       !WinHttpReceiveResponse(req, NULL))
   {
      WinHttpCloseHandle(req);
      WinHttpCloseHandle(conn);
      WinHttpCloseHandle(session);
      return -1;
   }

   /* Read status */
   DWORD status_code = 0;
   DWORD sz = sizeof(status_code);
   WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sz, WINHTTP_NO_HEADER_INDEX);

   /* Read body */
   char *result = NULL;
   size_t result_len = 0;
   DWORD bytes_available, bytes_read;

   while (WinHttpQueryDataAvailable(req, &bytes_available) && bytes_available > 0)
   {
      char *tmp = realloc(result, result_len + bytes_available + 1);
      if (!tmp)
      {
         free(result);
         result = NULL;
         break;
      }
      result = tmp;
      WinHttpReadData(req, result + result_len, bytes_available, &bytes_read);
      result_len += bytes_read;
      result[result_len] = '\0';
   }

   WinHttpCloseHandle(req);
   WinHttpCloseHandle(conn);
   WinHttpCloseHandle(session);

   *response_buf = result;
   return (int)status_code;
}

int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms)
{
   /* Phase 2: implement WinHTTP form POST for Windows support */
   (void)url;
   (void)body;
   (void)response_buf;
   (void)timeout_ms;
   return -1;
}

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers)
{
   (void)url;
   (void)auth_header;
   (void)body;
   (void)callback;
   (void)userdata;
   (void)timeout_ms;
   (void)extra_headers;
   return -1;
}

#else

/* ================================================================
 * Linux/macOS: libcurl implementation
 * ================================================================ */

#include <curl/curl.h>

void agent_http_init(void)
{
   curl_global_init(CURL_GLOBAL_DEFAULT);
}

void agent_http_cleanup(void)
{
   curl_global_cleanup();
}

typedef struct
{
   char *data;
   size_t size;
} response_t;

#define HTTP_MAX_RESPONSE_SIZE (10 * 1024 * 1024) /* 10MB */

static size_t write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
   response_t *resp = (response_t *)userdata;
   size_t total = size * nmemb;
   if (resp->size + total > HTTP_MAX_RESPONSE_SIZE)
      return 0; /* reject oversized responses */
   char *tmp = realloc(resp->data, resp->size + total + 1);
   if (!tmp)
      return 0;
   resp->data = tmp;
   memcpy(resp->data + resp->size, ptr, total);
   resp->size += total;
   resp->data[resp->size] = '\0';
   return total;
}

/* Common curl setup. Caller must curl_easy_cleanup(curl) and
 * curl_slist_free_all(*headers_out) after use. */
static CURL *setup_curl(const char *url, struct curl_slist **headers_out, const char *content_type,
                        const char *auth_header, const char *body, int timeout_ms,
                        const char *user_agent, const char *extra_headers, char *errbuf,
                        size_t errbuf_len)
{
   CURL *curl = curl_easy_init();
   if (!curl)
      return NULL;

   struct curl_slist *headers = NULL;
   if (content_type)
      headers = curl_slist_append(headers, content_type);
   if (auth_header && auth_header[0])
      headers = curl_slist_append(headers, auth_header);

   /* Append newline-separated extra headers */
   if (extra_headers && extra_headers[0])
   {
      char tmp[512];
      snprintf(tmp, sizeof(tmp), "%s", extra_headers);
      char *saveptr;
      char *line = strtok_r(tmp, "\n", &saveptr);
      while (line)
      {
         if (line[0])
            headers = curl_slist_append(headers, line);
         line = strtok_r(NULL, "\n", &saveptr);
      }
   }

   if (errbuf)
      errbuf[0] = '\0';

   curl_easy_setopt(curl, CURLOPT_URL, url);
   curl_easy_setopt(curl, CURLOPT_POST, 1L);
   curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
   curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
   curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
   curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
   curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent ? user_agent : "aimee/1.0");
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
   curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
   if (errbuf)
      curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

   *headers_out = headers;
   return curl;
}

/* Execute curl, extract HTTP status, clean up. Returns HTTP status or -1. */
static int curl_perform_and_cleanup(CURL *curl, struct curl_slist *headers, const char *label,
                                    char *errbuf, int allow_write_error)
{
   CURLcode res = curl_easy_perform(curl);

   long http_code = 0;
   curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

   curl_slist_free_all(headers);
   curl_easy_cleanup(curl);

   if (res != CURLE_OK && !(allow_write_error && res == CURLE_WRITE_ERROR))
   {
      if (label)
         fprintf(stderr, "%s: curl error %d: %s\n", label, res,
                 (errbuf && errbuf[0]) ? errbuf : curl_easy_strerror(res));
      return -1;
   }

   return (int)http_code;
}

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   *response_buf = NULL;
   char errbuf[CURL_ERROR_SIZE];
   struct curl_slist *headers = NULL;

   CURL *curl = setup_curl(url, &headers, "Content-Type: application/json", auth_header, body,
                           timeout_ms, "aimee/1.0", extra_headers, errbuf, sizeof(errbuf));
   if (!curl)
      return -1;

   response_t resp = {NULL, 0};
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

   int rc = curl_perform_and_cleanup(curl, headers, "agent_http_post", errbuf, 0);
   if (rc < 0)
   {
      free(resp.data);
      return -1;
   }

   *response_buf = resp.data;
   return rc;
}

int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms)
{
   *response_buf = NULL;
   struct curl_slist *headers = NULL;

   CURL *curl = setup_curl(url, &headers, "Content-Type: application/x-www-form-urlencoded", NULL,
                           body, timeout_ms, "codex_cli/1.0", "originator: codex_cli_rs", NULL, 0);
   if (!curl)
      return -1;

   response_t resp = {NULL, 0};
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

   int rc = curl_perform_and_cleanup(curl, headers, NULL, NULL, 0);
   if (rc < 0)
   {
      free(resp.data);
      return -1;
   }

   *response_buf = resp.data;
   return rc;
}

/* --- Streaming POST --- */

typedef struct
{
   agent_http_stream_cb cb;
   void *userdata;
} stream_ctx_t;

static size_t stream_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata)
{
   stream_ctx_t *ctx = (stream_ctx_t *)userdata;
   size_t total = size * nmemb;
   if (ctx->cb((const char *)ptr, total, ctx->userdata) != 0)
      return 0; /* abort */
   return total;
}

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers)
{
   char errbuf[CURL_ERROR_SIZE];
   struct curl_slist *headers = NULL;

   CURL *curl = setup_curl(url, &headers, "Content-Type: application/json", auth_header, body,
                           timeout_ms, "aimee/1.0", extra_headers, errbuf, sizeof(errbuf));
   if (!curl)
      return -1;

   stream_ctx_t sctx = {callback, userdata};
   curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_write_callback);
   curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sctx);

   return curl_perform_and_cleanup(curl, headers, "agent_http_post_stream", errbuf, 1);
}

#endif /* _WIN32 */

/* ================================================================
 * Provider health tracking (shared across platforms)
 * ================================================================ */

#include <time.h>

#define MAX_TRACKED_PROVIDERS 8

static struct
{
   char name[64];
   provider_health_t health;
} s_provider_health[MAX_TRACKED_PROVIDERS];
static int s_provider_health_count;

provider_err_class_t provider_classify_error(int http_status)
{
   if (http_status < 0)
      return PROVIDER_ERR_NETWORK;
   if (http_status == 401 || http_status == 403)
      return PROVIDER_ERR_AUTH;
   if (http_status == 429)
      return PROVIDER_ERR_RATE_LIMIT;
   if (http_status >= 500 && http_status < 600)
      return PROVIDER_ERR_SERVER;
   if (http_status >= 400 && http_status < 500)
      return PROVIDER_ERR_CLIENT;
   if (http_status >= 200 && http_status < 300)
      return PROVIDER_ERR_NONE;
   return PROVIDER_ERR_UNKNOWN;
}

const char *provider_error_message(provider_err_class_t cls)
{
   switch (cls)
   {
   case PROVIDER_ERR_NONE:
      return "ok";
   case PROVIDER_ERR_NETWORK:
      return "unreachable (connection failed). Check network connection.";
   case PROVIDER_ERR_AUTH:
      return "authentication failed. Check API key with: aimee agent test <name>";
   case PROVIDER_ERR_RATE_LIMIT:
      return "rate limited. Retry later.";
   case PROVIDER_ERR_SERVER:
      return "server error. Retry later.";
   case PROVIDER_ERR_CLIENT:
      return "client error. Check request parameters.";
   case PROVIDER_ERR_UNKNOWN:
      return "unknown error.";
   }
   return "unknown error.";
}

static provider_health_t *find_or_create_health(const char *provider_name)
{
   for (int i = 0; i < s_provider_health_count; i++)
   {
      if (strcmp(s_provider_health[i].name, provider_name) == 0)
         return &s_provider_health[i].health;
   }
   if (s_provider_health_count >= MAX_TRACKED_PROVIDERS)
      return &s_provider_health[0].health; /* overwrite first if full */
   int idx = s_provider_health_count++;
   snprintf(s_provider_health[idx].name, sizeof(s_provider_health[idx].name), "%s", provider_name);
   s_provider_health[idx].health.available = -1;
   s_provider_health[idx].health.last_http_status = -1;
   return &s_provider_health[idx].health;
}

void provider_health_update(const char *provider_name, int http_status)
{
   if (!provider_name || !provider_name[0])
      return;
   provider_health_t *h = find_or_create_health(provider_name);
   h->last_http_status = http_status;
   h->last_check_ms = (int64_t)time(NULL) * 1000;

   provider_err_class_t cls = provider_classify_error(http_status);
   if (cls == PROVIDER_ERR_NONE)
   {
      h->available = 1;
      h->error[0] = '\0';
   }
   else
   {
      h->available = 0;
      snprintf(h->error, sizeof(h->error), "%s", provider_error_message(cls));
   }
}

const provider_health_t *provider_health_get(const char *provider_name)
{
   for (int i = 0; i < s_provider_health_count; i++)
   {
      if (strcmp(s_provider_health[i].name, provider_name) == 0)
         return &s_provider_health[i].health;
   }
   return NULL;
}
