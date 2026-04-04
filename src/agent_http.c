/* agent_http.c: HTTP client (platform abstraction for agent API calls) */
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
 * Linux/macOS: POSIX sockets + OpenSSL (no libcurl dependency)
 * ================================================================ */

#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static SSL_CTX *s_ssl_ctx;

void agent_http_init(void)
{
   s_ssl_ctx = SSL_CTX_new(TLS_client_method());
   if (s_ssl_ctx)
   {
      SSL_CTX_set_default_verify_paths(s_ssl_ctx);
      SSL_CTX_set_verify(s_ssl_ctx, SSL_VERIFY_PEER, NULL);
      SSL_CTX_set_min_proto_version(s_ssl_ctx, TLS1_2_VERSION);
   }
}

void agent_http_cleanup(void)
{
   if (s_ssl_ctx)
   {
      SSL_CTX_free(s_ssl_ctx);
      s_ssl_ctx = NULL;
   }
}

#define HTTP_MAX_RESPONSE_SIZE (10 * 1024 * 1024) /* 10MB */

/* ---- URL parsing ---- */

typedef struct
{
   char host[256];
   char path[2048];
   int port;
   int use_ssl;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
   memset(out, 0, sizeof(*out));

   if (strncmp(url, "https://", 8) == 0)
   {
      out->use_ssl = 1;
      out->port = 443;
      url += 8;
   }
   else if (strncmp(url, "http://", 7) == 0)
   {
      out->use_ssl = 0;
      out->port = 80;
      url += 7;
   }
   else
      return -1;

   /* Extract host[:port] and path */
   const char *slash = strchr(url, '/');
   const char *colon = strchr(url, ':');
   size_t hostlen;

   if (colon && (!slash || colon < slash))
   {
      hostlen = (size_t)(colon - url);
      out->port = atoi(colon + 1);
   }
   else
      hostlen = slash ? (size_t)(slash - url) : strlen(url);

   if (hostlen == 0 || hostlen >= sizeof(out->host))
      return -1;

   memcpy(out->host, url, hostlen);
   out->host[hostlen] = '\0';

   if (slash)
      snprintf(out->path, sizeof(out->path), "%s", slash);
   else
      snprintf(out->path, sizeof(out->path), "/");

   return 0;
}

/* ---- Socket I/O with timeout ---- */

typedef struct
{
   int fd;
   SSL *ssl;
} http_conn_t;

static int connect_with_timeout(const char *host, int port, int timeout_ms)
{
   char port_str[16];
   snprintf(port_str, sizeof(port_str), "%d", port);

   struct addrinfo hints = {0}, *res = NULL;
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;

   if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
      return -1;

   int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
   if (fd < 0)
   {
      freeaddrinfo(res);
      return -1;
   }

   /* Set non-blocking for connect timeout */
   int flags = fcntl(fd, F_GETFL, 0);
   fcntl(fd, F_SETFL, flags | O_NONBLOCK);

   int rc = connect(fd, res->ai_addr, res->ai_addrlen);
   freeaddrinfo(res);

   if (rc < 0 && errno != EINPROGRESS)
   {
      close(fd);
      return -1;
   }

   if (rc < 0)
   {
      /* Wait for connect */
      struct pollfd pfd = {fd, POLLOUT, 0};
      int pr = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 30000);
      if (pr <= 0)
      {
         close(fd);
         return -1;
      }
      int err = 0;
      socklen_t errlen = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
      if (err)
      {
         close(fd);
         return -1;
      }
   }

   /* Restore blocking */
   fcntl(fd, F_SETFL, flags);
   return fd;
}

static int conn_open(http_conn_t *conn, const parsed_url_t *url, int timeout_ms)
{
   conn->ssl = NULL;
   conn->fd = connect_with_timeout(url->host, url->port, timeout_ms);
   if (conn->fd < 0)
      return -1;

   /* Set socket-level send/recv timeout */
   if (timeout_ms > 0)
   {
      struct timeval tv;
      tv.tv_sec = timeout_ms / 1000;
      tv.tv_usec = (timeout_ms % 1000) * 1000;
      setsockopt(conn->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      setsockopt(conn->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   }

   if (url->use_ssl)
   {
      if (!s_ssl_ctx)
      {
         close(conn->fd);
         conn->fd = -1;
         return -1;
      }
      conn->ssl = SSL_new(s_ssl_ctx);
      if (!conn->ssl)
      {
         close(conn->fd);
         conn->fd = -1;
         return -1;
      }
      SSL_set_fd(conn->ssl, conn->fd);
      SSL_set_tlsext_host_name(conn->ssl, url->host);

      /* Enable hostname verification */
      SSL_set1_host(conn->ssl, url->host);

      if (SSL_connect(conn->ssl) <= 0)
      {
         SSL_free(conn->ssl);
         close(conn->fd);
         conn->fd = -1;
         conn->ssl = NULL;
         return -1;
      }
   }
   return 0;
}

static void conn_close(http_conn_t *conn)
{
   if (conn->ssl)
   {
      SSL_shutdown(conn->ssl);
      SSL_free(conn->ssl);
      conn->ssl = NULL;
   }
   if (conn->fd >= 0)
   {
      close(conn->fd);
      conn->fd = -1;
   }
}

static ssize_t conn_write(http_conn_t *conn, const void *buf, size_t len)
{
   if (conn->ssl)
      return SSL_write(conn->ssl, buf, (int)len);
   return send(conn->fd, buf, len, 0);
}

static ssize_t conn_read(http_conn_t *conn, void *buf, size_t len)
{
   if (conn->ssl)
      return SSL_read(conn->ssl, buf, (int)len);
   return recv(conn->fd, buf, len, 0);
}

/* Write all bytes, retrying short writes */
static int conn_write_all(http_conn_t *conn, const char *buf, size_t len)
{
   while (len > 0)
   {
      ssize_t n = conn_write(conn, buf, len);
      if (n <= 0)
         return -1;
      buf += n;
      len -= (size_t)n;
   }
   return 0;
}

/* ---- HTTP request/response ---- */

/* Build and send an HTTP request. Returns 0 on success, -1 on error. */
static int send_request(http_conn_t *conn, const char *method, const parsed_url_t *url,
                        const char *content_type, const char *auth_header,
                        const char *extra_headers, const char *user_agent, const char *body,
                        size_t body_len)
{
   /* Build request into a dynamic buffer */
   size_t cap = 4096 + body_len;
   char *req = malloc(cap);
   if (!req)
      return -1;

   int off = snprintf(req, cap, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method,
                      url->path, url->host);

   if (user_agent)
      off += snprintf(req + off, cap - (size_t)off, "User-Agent: %s\r\n", user_agent);

   if (content_type)
      off += snprintf(req + off, cap - (size_t)off, "%s\r\n", content_type);

   if (auth_header && auth_header[0])
      off += snprintf(req + off, cap - (size_t)off, "%s\r\n", auth_header);

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
            off += snprintf(req + off, cap - (size_t)off, "%s\r\n", line);
         line = strtok_r(NULL, "\n", &saveptr);
      }
   }

   if (body && body_len > 0)
      off += snprintf(req + off, cap - (size_t)off, "Content-Length: %zu\r\n\r\n", body_len);
   else
      off += snprintf(req + off, cap - (size_t)off, "\r\n");

   /* Append body */
   if (body && body_len > 0)
   {
      memcpy(req + off, body, body_len);
      off += (int)body_len;
   }

   int rc = conn_write_all(conn, req, (size_t)off);
   free(req);
   return rc;
}

/* Parse status line, return HTTP status code. Advances *buf past headers.
 * Sets *chunked=1 if Transfer-Encoding: chunked, else sets *content_length. */
static int parse_response_headers(char *buf, size_t len, size_t *header_end, int *chunked,
                                  size_t *content_length)
{
   *chunked = 0;
   *content_length = 0;
   *header_end = 0;

   /* Find end of headers */
   char *hdr_end = strstr(buf, "\r\n\r\n");
   if (!hdr_end)
      return -1;

   *header_end = (size_t)(hdr_end - buf) + 4;

   /* Parse status code from "HTTP/1.x NNN ..." */
   int status = 0;
   if (len < 12 || (strncmp(buf, "HTTP/1.0", 8) != 0 && strncmp(buf, "HTTP/1.1", 8) != 0))
      return -1;

   status = atoi(buf + 9);
   if (status < 100 || status > 999)
      return -1;

   /* Null-terminate headers for easy searching */
   *hdr_end = '\0';

   /* Check for Transfer-Encoding: chunked (case-insensitive scan) */
   for (char *p = buf; *p; p++)
   {
      if ((*p == 'T' || *p == 't') && strncasecmp(p, "Transfer-Encoding:", 18) == 0)
      {
         char *val = p + 18;
         while (*val == ' ')
            val++;
         if (strncasecmp(val, "chunked", 7) == 0)
            *chunked = 1;
         break;
      }
   }

   /* Check for Content-Length */
   for (char *p = buf; *p; p++)
   {
      if ((*p == 'C' || *p == 'c') && strncasecmp(p, "Content-Length:", 15) == 0)
      {
         *content_length = (size_t)atoll(p + 15);
         break;
      }
   }

   /* Restore the header terminator */
   *hdr_end = '\r';
   return status;
}

/* Read a full HTTP response (headers + body) into *out_body, returning status code.
 * For buffered (non-streaming) reads. */
static int http_read_response(http_conn_t *conn, char **out_body, size_t *out_len)
{
   *out_body = NULL;
   *out_len = 0;

   /* Read into a growing buffer until we have all headers + body */
   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return -1;

   /* Phase 1: read until we have complete headers */
   int headers_done = 0;
   size_t header_end = 0;
   int chunked = 0;
   size_t content_length = 0;
   int status = -1;

   while (!headers_done)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
         {
            free(buf);
            return -1;
         }
         buf = tmp;
      }

      ssize_t n = conn_read(conn, buf + len, cap - len - 1);
      if (n <= 0)
      {
         if (len > 0)
            break; /* might have enough */
         free(buf);
         return -1;
      }
      len += (size_t)n;
      buf[len] = '\0';

      if (strstr(buf, "\r\n\r\n"))
         headers_done = 1;
   }

   status = parse_response_headers(buf, len, &header_end, &chunked, &content_length);
   if (status < 0)
   {
      free(buf);
      return -1;
   }

   /* Phase 2: read body */
   if (chunked)
   {
      /* Decode chunked transfer encoding */
      char *body = NULL;
      size_t body_len = 0;

      /* Start from data after headers */
      size_t pos = header_end;

      for (;;)
      {
         /* Ensure we have data to parse chunk size from */
         while (!strstr(buf + pos, "\r\n") || pos >= len)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            buf[len] = '\0';
         }

         /* Parse chunk size */
         char *crlf = strstr(buf + pos, "\r\n");
         if (!crlf)
            break;

         size_t chunk_size = (size_t)strtoul(buf + pos, NULL, 16);
         pos = (size_t)(crlf - buf) + 2;

         if (chunk_size == 0)
            break; /* final chunk */

         /* Read until we have the full chunk */
         while (len - pos < chunk_size + 2)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(body);
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            buf[len] = '\0';
         }

         if (body_len + chunk_size > HTTP_MAX_RESPONSE_SIZE)
         {
            free(body);
            free(buf);
            return -1;
         }

         char *tmp = realloc(body, body_len + chunk_size + 1);
         if (!tmp)
         {
            free(body);
            free(buf);
            return -1;
         }
         body = tmp;
         memcpy(body + body_len, buf + pos, chunk_size);
         body_len += chunk_size;
         body[body_len] = '\0';

         pos += chunk_size + 2; /* skip chunk data + trailing CRLF */
      }

      free(buf);
      *out_body = body;
      *out_len = body_len;
   }
   else
   {
      /* Content-Length or read-until-close */
      size_t body_so_far = len - header_end;
      size_t target = content_length > 0 ? content_length : 0;

      if (content_length > 0)
      {
         /* Read remaining body bytes */
         while (body_so_far < target)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            body_so_far += (size_t)n;
            buf[len] = '\0';
         }
      }
      else
      {
         /* Read until connection close */
         for (;;)
         {
            if (len + 4096 > cap)
            {
               if (cap > HTTP_MAX_RESPONSE_SIZE)
               {
                  free(buf);
                  return -1;
               }
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  return -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               break;
            len += (size_t)n;
            buf[len] = '\0';
         }
      }

      /* Extract body */
      size_t blen = len - header_end;
      char *body = malloc(blen + 1);
      if (!body)
      {
         free(buf);
         return -1;
      }
      memcpy(body, buf + header_end, blen);
      body[blen] = '\0';
      free(buf);

      *out_body = body;
      *out_len = blen;
   }

   return status;
}

/* Streaming response reader: reads headers, then delivers body chunks via callback.
 * Handles both chunked and content-length/close modes. */
static int http_read_response_stream(http_conn_t *conn, agent_http_stream_cb callback,
                                     void *userdata)
{
   size_t cap = 8192, len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return -1;

   /* Read headers */
   int headers_done = 0;
   while (!headers_done)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
         {
            free(buf);
            return -1;
         }
         buf = tmp;
      }
      ssize_t n = conn_read(conn, buf + len, cap - len - 1);
      if (n <= 0)
      {
         free(buf);
         return -1;
      }
      len += (size_t)n;
      buf[len] = '\0';
      if (strstr(buf, "\r\n\r\n"))
         headers_done = 1;
   }

   size_t header_end = 0;
   int chunked = 0;
   size_t content_length = 0;
   int status = parse_response_headers(buf, len, &header_end, &chunked, &content_length);
   if (status < 0)
   {
      free(buf);
      return -1;
   }

   /* Deliver any body data already read past the headers */
   size_t leftover = len - header_end;
   int aborted = 0;

   if (chunked)
   {
      /* For chunked streaming, we need to parse chunk boundaries and deliver decoded data */
      /* Move leftover to start of buffer */
      memmove(buf, buf + header_end, leftover);
      len = leftover;

      for (;;)
      {
         /* Ensure we have a chunk header */
         buf[len] = '\0';
         while (!strstr(buf, "\r\n"))
         {
            if (len + 4096 > cap)
            {
               cap *= 2;
               char *tmp = realloc(buf, cap);
               if (!tmp)
               {
                  free(buf);
                  return aborted ? status : -1;
               }
               buf = tmp;
            }
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               goto stream_done;
            len += (size_t)n;
            buf[len] = '\0';
         }

         char *crlf = strstr(buf, "\r\n");
         size_t chunk_size = (size_t)strtoul(buf, NULL, 16);
         size_t hdr_len = (size_t)(crlf - buf) + 2;

         /* Remove chunk header from buffer */
         len -= hdr_len;
         memmove(buf, buf + hdr_len, len);

         if (chunk_size == 0)
            break;

         /* Read and deliver this chunk */
         size_t delivered = 0;
         while (delivered < chunk_size)
         {
            size_t avail = len < (chunk_size - delivered) ? len : (chunk_size - delivered);
            if (avail > 0)
            {
               if (callback(buf, avail, userdata) != 0)
               {
                  aborted = 1;
                  goto stream_done;
               }
               delivered += avail;
               len -= avail;
               memmove(buf, buf + avail, len);
            }
            if (delivered < chunk_size)
            {
               ssize_t n = conn_read(conn, buf + len, cap - len - 1);
               if (n <= 0)
                  goto stream_done;
               len += (size_t)n;
            }
         }

         /* Skip trailing CRLF after chunk data */
         while (len < 2)
         {
            ssize_t n = conn_read(conn, buf + len, cap - len - 1);
            if (n <= 0)
               goto stream_done;
            len += (size_t)n;
         }
         len -= 2;
         memmove(buf, buf + 2, len);
      }
   }
   else
   {
      /* Deliver leftover from header read */
      if (leftover > 0)
      {
         if (callback(buf + header_end, leftover, userdata) != 0)
            aborted = 1;
      }

      if (!aborted)
      {
         /* Read remaining body and deliver */
         char chunk[8192];
         for (;;)
         {
            ssize_t n = conn_read(conn, chunk, sizeof(chunk));
            if (n <= 0)
               break;
            if (callback(chunk, (size_t)n, userdata) != 0)
            {
               aborted = 1;
               break;
            }
         }
      }
   }

stream_done:
   free(buf);
   return status;
}

/* ---- Public API ---- */

int agent_http_post(const char *url, const char *auth_header, const char *body, char **response_buf,
                    int timeout_ms, const char *extra_headers)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "POST", &pu, "Content-Type: application/json", auth_header,
                    extra_headers, "aimee/1.0", body, body ? strlen(body) : 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   size_t resp_len = 0;
   int status = http_read_response(&conn, response_buf, &resp_len);
   conn_close(&conn);

   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
      fprintf(stderr, "agent_http_post: request failed\n");
   }
   return status;
}

int agent_http_post_form(const char *url, const char *body, char **response_buf, int timeout_ms)
{
   *response_buf = NULL;

   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "POST", &pu, "Content-Type: application/x-www-form-urlencoded", NULL,
                    "originator: codex_cli_rs", "codex_cli/1.0", body, body ? strlen(body) : 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   size_t resp_len = 0;
   int status = http_read_response(&conn, response_buf, &resp_len);
   conn_close(&conn);

   if (status < 0)
   {
      free(*response_buf);
      *response_buf = NULL;
   }
   return status;
}

int agent_http_post_stream(const char *url, const char *auth_header, const char *body,
                           agent_http_stream_cb callback, void *userdata, int timeout_ms,
                           const char *extra_headers)
{
   parsed_url_t pu;
   if (parse_url(url, &pu) < 0)
      return -1;

   http_conn_t conn;
   if (conn_open(&conn, &pu, timeout_ms) < 0)
      return -1;

   if (send_request(&conn, "POST", &pu, "Content-Type: application/json", auth_header,
                    extra_headers, "aimee/1.0", body, body ? strlen(body) : 0) < 0)
   {
      conn_close(&conn);
      return -1;
   }

   int status = http_read_response_stream(&conn, callback, userdata);
   conn_close(&conn);
   return status;
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
