/* cli_client.c: shared client library for aimee-server Unix socket communication */
#define _GNU_SOURCE
#include "cli_client.h"
#include "aimee.h"
#include "cJSON.h"
#define RPC_PROTOCOL_VERSION 1
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/file.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

static const char *cli_config_dir(void)
{
   static char dir[4096];
   if (dir[0])
      return dir;
   const char *home = getenv("HOME");
   if (!home)
      home = "/tmp";
   snprintf(dir, sizeof(dir), "%s/.config/aimee", home);
   return dir;
}

const char *cli_default_socket_path(void)
{
   static char path[4096];
   if (path[0])
      return path;

   snprintf(path, sizeof(path), "%s/%s", cli_config_dir(), "aimee.sock");
   return path;
}

int cli_connect_timeout(cli_conn_t *conn, const char *socket_path, int timeout_ms)
{
   if (!socket_path)
      socket_path = cli_default_socket_path();

   memset(conn, 0, sizeof(*conn));
   conn->fd = -1;

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;

   /* Set non-blocking for connect timeout */
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
   {
      close(fd);
      return -1;
   }

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

   int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
   if (rc < 0 && errno != EINPROGRESS)
   {
      close(fd);
      return -1;
   }

   if (rc < 0)
   {
      /* Wait for connect with timeout */
      struct pollfd pfd = {.fd = fd, .events = POLLOUT};
      rc = poll(&pfd, 1, timeout_ms);
      if (rc <= 0)
      {
         close(fd);
         return -1;
      }

      /* Check for connect error */
      int err = 0;
      socklen_t elen = sizeof(err);
      getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
      if (err != 0)
      {
         close(fd);
         return -1;
      }
   }

   /* Set back to blocking */
   fcntl(fd, F_SETFL, flags);

   conn->fd = fd;
   conn->read_len = 0;
   return 0;
}

int cli_connect(cli_conn_t *conn, const char *socket_path)
{
   return cli_connect_timeout(conn, socket_path, CLIENT_CONNECT_TIMEOUT_MS);
}

cJSON *cli_request(cli_conn_t *conn, cJSON *request, int timeout_ms)
{
   if (!conn || conn->fd < 0 || !request)
      return NULL;

   /* Serialize request */
   char *json_str = cJSON_PrintUnformatted(request);
   if (!json_str)
      return NULL;

   size_t json_len = strlen(json_str);

   /* Write request + newline */
   size_t total = 0;
   while (total < json_len)
   {
      ssize_t n = write(conn->fd, json_str + total, json_len - total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(json_str);
         return NULL;
      }
      total += (size_t)n;
   }
   free(json_str);

   /* Write newline delimiter */
   char nl = '\n';
   if (write(conn->fd, &nl, 1) != 1)
      return NULL;

   /* Read response until newline */
   conn->read_len = 0;
   for (;;)
   {
      /* Check for newline in buffer */
      for (size_t i = 0; i < conn->read_len; i++)
      {
         if (conn->read_buf[i] == '\n')
         {
            conn->read_buf[i] = '\0';
            cJSON *resp = cJSON_Parse(conn->read_buf);
            /* Shift remaining data (if any) */
            size_t remain = conn->read_len - i - 1;
            if (remain > 0)
               memmove(conn->read_buf, conn->read_buf + i + 1, remain);
            conn->read_len = remain;
            return resp;
         }
      }

      /* Buffer full without newline */
      if (conn->read_len >= CLIENT_READ_BUF_SIZE - 1)
         return NULL;

      /* Wait for data */
      struct pollfd pfd = {.fd = conn->fd, .events = POLLIN};
      int rc = poll(&pfd, 1, timeout_ms);
      if (rc <= 0)
         return NULL;

      ssize_t n = read(conn->fd, conn->read_buf + conn->read_len,
                       CLIENT_READ_BUF_SIZE - 1 - conn->read_len);
      if (n <= 0)
         return NULL;
      conn->read_len += (size_t)n;
   }
}

int cli_server_available(const char *socket_path)
{
   cli_conn_t conn;
   if (cli_connect(&conn, socket_path) != 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "server.info");

   cJSON *resp = cli_request(&conn, req, CLIENT_CONNECT_TIMEOUT_MS);
   cJSON_Delete(req);

   int available = 0;
   if (resp)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
         available = 1;
      cJSON_Delete(resp);
   }

   cli_close(&conn);
   return available;
}

int cli_authenticate(cli_conn_t *conn)
{
   /* Read token from ~/.config/aimee/server.token */
   char path[4096];
   snprintf(path, sizeof(path), "%s/server.token", cli_config_dir());

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   char token[256];
   size_t n = fread(token, 1, sizeof(token) - 1, f);
   fclose(f);
   token[n] = '\0';
   /* Trim whitespace */
   while (n > 0 && (token[n - 1] == '\n' || token[n - 1] == '\r'))
      token[--n] = '\0';

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "auth");
   cJSON_AddStringToObject(req, "token", token);

   cJSON *resp = cli_request(conn, req, CLIENT_DEFAULT_TIMEOUT_MS);
   cJSON_Delete(req);

   if (!resp)
      return -1;

   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   int ok = (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) ? 0 : -1;
   cJSON_Delete(resp);
   return ok;
}

void cli_close(cli_conn_t *conn)
{
   if (conn && conn->fd >= 0)
   {
      close(conn->fd);
      conn->fd = -1;
   }
}

/* --- Auto-start server --- */

/* Check if a Unix socket has a live server behind it.
 * Returns 1 if live, 0 if stale (ECONNREFUSED), -1 on error. */
static int socket_is_live(const char *path)
{
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

   int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
   int err = errno;
   close(fd);

   if (rc == 0 || err == EINPROGRESS)
      return 1; /* live */
   if (err == ECONNREFUSED)
      return 0; /* stale */
   return -1;   /* error */
}

/* Remove a stale socket file. Returns 1 if removed, 0 if not stale or not a socket. */
static int cleanup_stale_socket(const char *path)
{
   struct stat st;
   if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode))
      return 0;

   if (socket_is_live(path) == 0)
   {
      unlink(path);
      return 1;
   }
   return 0;
}

/* Acquire a spawn lock to prevent concurrent CLI invocations from double-spawning.
 * Returns the lock fd (>= 0) on success, -1 if another process holds the lock. */
static int acquire_spawn_lock(void)
{
   char lock_path[4096];
   snprintf(lock_path, sizeof(lock_path), "%s/server.lock", cli_config_dir());
   int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
   if (fd < 0)
      return -1;
   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      /* Another process is spawning — wait for it */
      flock(fd, LOCK_EX);
      /* By the time we get the lock, the other process has spawned.
       * Release immediately and signal caller to retry connect. */
      close(fd);
      return -1;
   }
   return fd;
}

static void release_spawn_lock(int fd)
{
   if (fd >= 0)
      close(fd); /* flock is released on close */
}

/* Try to reach a server at socket_path with a short timeout.
 * Returns 1 if available, 0 if not. */
/* Get the PID of the peer process on a Unix socket connection (Linux only). */
static pid_t get_peer_pid(int fd)
{
#ifdef __linux__
   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
      return cred.pid;
#else
   (void)fd;
#endif
   return 0;
}

static int try_server(const char *socket_path, int timeout_ms)
{
   cli_conn_t conn;
   if (cli_connect_timeout(&conn, socket_path, timeout_ms) != 0)
      return 0;

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "server.info");
   cJSON *resp = cli_request(&conn, req, timeout_ms);
   cJSON_Delete(req);

   int ok = 0;
   int killed_stale = 0;
   if (resp)
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
         ok = 1;

      /* Stale server detection: if the server's build timestamp differs from
       * ours, the binary was updated but the server wasn't restarted. Kill it
       * so cli_ensure_server will spawn a fresh one. The MCP proxy (thin client)
       * auto-reconnects, so this is safe. */
      if (ok)
      {
         cJSON *jbid = cJSON_GetObjectItemCaseSensitive(resp, "build_id");
         const char *server_bid = cJSON_IsString(jbid) ? jbid->valuestring : "";
         if (strcmp(server_bid, AIMEE_BUILD_ID) != 0)
         {
            pid_t server_pid = get_peer_pid(conn.fd);
            fprintf(stderr,
                    "aimee: server build mismatch (server=%s, client=%s) — restarting\n",
                    server_bid[0] ? server_bid : "(unknown)", AIMEE_BUILD_ID);
            if (server_pid > 1)
            {
               kill(server_pid, SIGTERM);
               killed_stale = 1;
            }
            ok = 0;
         }
      }

      cJSON_Delete(resp);
   }
   cli_close(&conn);

   /* Wait for the stale server to shut down and release the socket */
   if (killed_stale)
      usleep(300000);

   return ok;
}

/* Wait for a server to become ready at sock_path with exponential backoff.
 * Returns 1 if ready, 0 on timeout. */
static int wait_for_ready(const char *sock_path, int timeout_ms)
{
   struct stat st;
   int elapsed = 0;
   int backoff = 10; /* start at 10ms */

   while (elapsed < timeout_ms)
   {
      if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode))
      {
         if (try_server(sock_path, 500))
            return 1;
      }
      usleep((unsigned)(backoff * 1000));
      elapsed += backoff;
      if (backoff < 200)
         backoff *= 2;
   }
   return 0;
}

/* Spawn an aimee-server on the well-known socket.
 * The server outlives this process but shuts down after idle timeout. */
static const char *spawn_server(void)
{
   static char sock_path[4096];
   snprintf(sock_path, sizeof(sock_path), "%s", cli_default_socket_path());

   /* Acquire spawn lock to prevent concurrent double-spawn */
   int lock_fd = acquire_spawn_lock();
   if (lock_fd < 0)
   {
      /* Another process is spawning; by the time we get the lock, the server
       * may already be up. Try the well-known socket before giving up. */
      const char *well_known = cli_default_socket_path();
      if (try_server(well_known, 500))
         return well_known;
      return NULL;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      release_spawn_lock(lock_fd);
      return NULL;
   }

   if (pid == 0)
   {
      /* Child: become a daemon */
      setsid();

      /* Redirect stdio to /dev/null */
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
         dup2(devnull, STDIN_FILENO);
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDERR_FILENO);
         if (devnull > 2)
            close(devnull);
      }

      char sock_arg[4112];
      snprintf(sock_arg, sizeof(sock_arg), "--socket=%s", sock_path);
      execlp("aimee-server", "aimee-server", sock_arg, NULL);

      /* Fallback: try same directory as this binary */
      char self_path[4096];
      ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
      if (len > 0)
      {
         self_path[len] = '\0';
         char *slash = strrchr(self_path, '/');
         if (slash)
         {
            snprintf(slash + 1, sizeof(self_path) - (size_t)(slash - self_path) - 1,
                     "aimee-server");
            execl(self_path, "aimee-server", sock_arg, NULL);
         }
      }
      _exit(127);
   }

   /* Parent: wait for server readiness with exponential backoff (max 3s) */
#ifdef __linux__
   {
      /* Use inotify to wake immediately when socket file appears */
      char watch_dir[4096];
      snprintf(watch_dir, sizeof(watch_dir), "%s", sock_path);
      char *slash = strrchr(watch_dir, '/');
      if (slash)
         *slash = '\0';
      else
         snprintf(watch_dir, sizeof(watch_dir), ".");

      int ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
      if (ifd >= 0)
      {
         int wd = inotify_add_watch(ifd, watch_dir, IN_CREATE);
         if (wd >= 0)
         {
            struct stat st;
            if (stat(sock_path, &st) == 0 && S_ISSOCK(st.st_mode))
            {
               close(ifd);
               if (try_server(sock_path, 1000))
               {
                  release_spawn_lock(lock_fd);
                  return sock_path;
               }
            }

            struct pollfd pfd = {.fd = ifd, .events = POLLIN};
            int ret = poll(&pfd, 1, 3000);
            if (ret > 0)
            {
               char evbuf[256];
               (void)read(ifd, evbuf, sizeof(evbuf));
            }
            close(ifd);

            if (wait_for_ready(sock_path, 1000))
            {
               release_spawn_lock(lock_fd);
               return sock_path;
            }

            fprintf(stderr, "aimee: server did not become ready (3s timeout)\n");
            release_spawn_lock(lock_fd);
            return NULL;
         }
         close(ifd);
      }
   }
   /* Fallback: inotify not available */
#endif

   if (wait_for_ready(sock_path, 3000))
   {
      release_spawn_lock(lock_fd);
      return sock_path;
   }

   fprintf(stderr, "aimee: server did not become ready (3s timeout)\n");
   release_spawn_lock(lock_fd);
   return NULL;
}

const char *cli_ensure_server(void)
{
   /* 1. Try AIMEE_SOCK (session-scoped server) with short timeout */
   const char *env_sock = getenv("AIMEE_SOCK");
   if (env_sock && env_sock[0])
   {
      if (try_server(env_sock, 100))
         return env_sock;
      /* Session server is dead — clean up stale socket if needed */
      cleanup_stale_socket(env_sock);
   }

   /* 2. Try well-known socket (persistent server) */
   const char *well_known = cli_default_socket_path();
   if (try_server(well_known, 200))
      return well_known;
   /* Clean stale well-known socket before attempting spawn */
   cleanup_stale_socket(well_known);

   /* 3. Auto-start server on well-known socket */
   const char *no_auto = getenv("AIMEE_NO_AUTOSTART");
   if (no_auto && no_auto[0])
      return NULL;

   return spawn_server();

   return NULL;
}

/* ===================================================================
 * RPC thin-client routing: route CLI subcommands through native server
 * RPCs instead of cli.forward (which forks and dispatches in-process).
 * =================================================================== */

/* --- Minimal arg parser (no util.h dependency) --- */

#define RPC_MAX_POS   16
#define RPC_MAX_FLAGS 32

typedef struct
{
   const char *positional[RPC_MAX_POS];
   int pos_count;
   struct
   {
      const char *raw; /* flag name from argv (after --), may include =val */
      const char *value;
   } flags[RPC_MAX_FLAGS];
   int flag_count;
} rpc_opts_t;

static int rpc_is_bool(const char *name, const char **bool_flags)
{
   if (!bool_flags)
      return 0;
   for (int i = 0; bool_flags[i]; i++)
      if (strcmp(name, bool_flags[i]) == 0)
         return 1;
   return 0;
}

static void rpc_parse(int argc, char **argv, const char **bool_flags, rpc_opts_t *out)
{
   memset(out, 0, sizeof(*out));
   for (int i = 0; i < argc; i++)
   {
      if (argv[i][0] == '-' && argv[i][1] == '-')
      {
         const char *flag = argv[i] + 2;
         const char *eq = strchr(flag, '=');
         if (eq)
         {
            if (out->flag_count < RPC_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = eq + 1;
               out->flag_count++;
            }
         }
         else if (rpc_is_bool(flag, bool_flags))
         {
            if (out->flag_count < RPC_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = "1";
               out->flag_count++;
            }
         }
         else if (i + 1 < argc)
         {
            if (out->flag_count < RPC_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = argv[i + 1];
               out->flag_count++;
            }
            i++;
         }
      }
      else
      {
         if (out->pos_count < RPC_MAX_POS)
            out->positional[out->pos_count++] = argv[i];
      }
   }
}

static const char *rpc_get(const rpc_opts_t *opts, const char *name)
{
   size_t nlen = strlen(name);
   for (int i = 0; i < opts->flag_count; i++)
   {
      const char *raw = opts->flags[i].raw;
      const char *eq = strchr(raw, '=');
      size_t rlen = eq ? (size_t)(eq - raw) : strlen(raw);
      if (rlen == nlen && memcmp(raw, name, nlen) == 0)
         return opts->flags[i].value;
   }
   return NULL;
}

static int rpc_get_int(const rpc_opts_t *opts, const char *name, int def)
{
   const char *v = rpc_get(opts, name);
   return v ? atoi(v) : def;
}

/* --- RPC route table --- */

static const struct
{
   const char *cmd;
   const char *subcmd; /* NULL = match any (first arg is NOT a subcmd) */
   const char *method;
   const char *extract; /* response array field to extract, or NULL */
   int timeout_ms;      /* 0 = CLIENT_DEFAULT_TIMEOUT_MS */
} rpc_routes[] = {
    {"memory", "search", "memory.search", NULL, 0},
    {"memory", "store", "memory.store", NULL, 0},
    {"memory", "list", "memory.list", "memories", 0},
    {"memory", "get", "memory.get", NULL, 0},
    {"rules", "list", "rules.list", "rules", 0},
    {"rules", "generate", "rules.generate", NULL, 0},
    {"wm", "set", "wm.set", NULL, 0},
    {"wm", "get", "wm.get", NULL, 0},
    {"wm", "list", "wm.list", "entries", 0},
    {"index", "find", "index.find", NULL, 0},
    {"index", "list", "index.list", "projects", 0},
    {"delegate", NULL, "delegate", NULL, 300000},
    {NULL, NULL, NULL, NULL, 0},
};

int cli_rpc_lookup(const char *cmd, const char *subcmd, cli_rpc_route_t *route)
{
   if (!cmd)
      return 0;
   for (int i = 0; rpc_routes[i].cmd; i++)
   {
      if (strcmp(cmd, rpc_routes[i].cmd) != 0)
         continue;
      if (rpc_routes[i].subcmd == NULL)
      {
         route->method = rpc_routes[i].method;
         route->extract = rpc_routes[i].extract;
         route->skip_subcmd = 0;
         route->timeout_ms = rpc_routes[i].timeout_ms;
         return 1;
      }
      if (subcmd && strcmp(subcmd, rpc_routes[i].subcmd) == 0)
      {
         route->method = rpc_routes[i].method;
         route->extract = rpc_routes[i].extract;
         route->skip_subcmd = 1;
         route->timeout_ms = rpc_routes[i].timeout_ms;
         return 1;
      }
   }
   return 0;
}

/* Validate socket ownership: must be owned by current uid */
static int validate_socket(const char *path)
{
   struct stat st;
   if (stat(path, &st) != 0)
      return -1;
   if (!S_ISSOCK(st.st_mode))
      return -1;
   if (st.st_uid != getuid())
      return -1;
   return 0;
}

/* --- Per-method argv marshaling --- */

static cJSON *marshal_memory_search(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "memory.search");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   cJSON *kw = cJSON_CreateArray();
   for (int i = 0; i < opts.pos_count; i++)
      cJSON_AddItemToArray(kw, cJSON_CreateString(opts.positional[i]));
   cJSON_AddItemToObject(req, "keywords", kw);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 10));
   return req;
}

static cJSON *marshal_memory_store(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "memory.store");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "content", opts.positional[1]);

   const char *v;
   if ((v = rpc_get(&opts, "tier")))
      cJSON_AddStringToObject(req, "tier", v);
   if ((v = rpc_get(&opts, "kind")))
      cJSON_AddStringToObject(req, "kind", v);
   if ((v = rpc_get(&opts, "session")))
      cJSON_AddStringToObject(req, "session_id", v);
   if ((v = rpc_get(&opts, "confidence")))
      cJSON_AddNumberToObject(req, "confidence", atof(v));
   return req;
}

static cJSON *marshal_memory_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "memory.list");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   const char *v;
   if ((v = rpc_get(&opts, "tier")))
      cJSON_AddStringToObject(req, "tier", v);
   if ((v = rpc_get(&opts, "kind")))
      cJSON_AddStringToObject(req, "kind", v);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_memory_get(int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "memory.get");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   if (argc > 0)
      cJSON_AddNumberToObject(req, "id", atoll(argv[0]));
   return req;
}

static cJSON *marshal_no_args(const char *method)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   return req;
}

static const char *resolve_session_env(const rpc_opts_t *opts)
{
   const char *s = rpc_get(opts, "session");
   if (s)
      return s;
   s = getenv("AIMEE_SESSION_ID");
   if (s && s[0])
      return s;
   return "default";
}

static cJSON *marshal_wm_set(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "wm.set");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "value", opts.positional[1]);

   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));

   const char *v;
   if ((v = rpc_get(&opts, "category")))
      cJSON_AddStringToObject(req, "category", v);
   int ttl = rpc_get_int(&opts, "ttl", 0);
   if (ttl > 0)
      cJSON_AddNumberToObject(req, "ttl", ttl);
   return req;
}

static cJSON *marshal_wm_get(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "wm.get");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));
   return req;
}

static cJSON *marshal_wm_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "wm.list");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));
   const char *v;
   if ((v = rpc_get(&opts, "category")))
      cJSON_AddStringToObject(req, "category", v);
   return req;
}

static cJSON *marshal_index_find(int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "index.find");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   if (argc > 0)
      cJSON_AddStringToObject(req, "identifier", argv[0]);
   return req;
}

static cJSON *marshal_delegate(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", "background", "durable", "coordination",
                                      "plan", "dry-run",    "tools",   NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "delegate");
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "role", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "prompt", opts.positional[1]);

   const char *v;
   if ((v = rpc_get(&opts, "system")))
      cJSON_AddStringToObject(req, "system_prompt", v);
   if ((v = getenv("CLAUDE_SESSION_ID")) && v[0])
      cJSON_AddStringToObject(req, "session_id", v);
   if (rpc_get(&opts, "tools"))
      cJSON_AddTrueToObject(req, "tools");
   int mt = rpc_get_int(&opts, "max-tokens", 0);
   if (mt > 0)
      cJSON_AddNumberToObject(req, "max_tokens", mt);
   return req;
}

static cJSON *marshal_request(const char *method, int argc, char **argv)
{
   if (strcmp(method, "memory.search") == 0)
      return marshal_memory_search(argc, argv);
   if (strcmp(method, "memory.store") == 0)
      return marshal_memory_store(argc, argv);
   if (strcmp(method, "memory.list") == 0)
      return marshal_memory_list(argc, argv);
   if (strcmp(method, "memory.get") == 0)
      return marshal_memory_get(argc, argv);
   if (strcmp(method, "rules.list") == 0 || strcmp(method, "rules.generate") == 0 ||
       strcmp(method, "index.list") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "wm.set") == 0)
      return marshal_wm_set(argc, argv);
   if (strcmp(method, "wm.get") == 0)
      return marshal_wm_get(argc, argv);
   if (strcmp(method, "wm.list") == 0)
      return marshal_wm_list(argc, argv);
   if (strcmp(method, "index.find") == 0)
      return marshal_index_find(argc, argv);
   if (strcmp(method, "delegate") == 0)
      return marshal_delegate(argc, argv);
   return NULL;
}

/* --- Non-JSON output formatting --- */

static void print_text_output(const char *method, cJSON *resp)
{
   if (!resp)
      return;
   if (strcmp(method, "rules.generate") == 0)
   {
      cJSON *c = cJSON_GetObjectItemCaseSensitive(resp, "content");
      if (cJSON_IsString(c) && c->valuestring[0])
         printf("%s", c->valuestring);
   }
   else if (strcmp(method, "wm.get") == 0)
   {
      cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
      if (cJSON_IsString(v))
         printf("%s\n", v->valuestring);
   }
   else if (strcmp(method, "wm.set") == 0)
   {
      cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "key");
      cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
      if (cJSON_IsString(k) && cJSON_IsString(v))
         printf("set %s = %s\n", k->valuestring, v->valuestring);
   }
   else if (strcmp(method, "wm.list") == 0)
   {
      cJSON *entries = cJSON_GetObjectItemCaseSensitive(resp, "entries");
      if (cJSON_IsArray(entries))
      {
         cJSON *el;
         int n = 0;
         cJSON_ArrayForEach(el, entries)
         {
            cJSON *cat = cJSON_GetObjectItemCaseSensitive(el, "category");
            cJSON *key = cJSON_GetObjectItemCaseSensitive(el, "key");
            cJSON *val = cJSON_GetObjectItemCaseSensitive(el, "value");
            if (cJSON_IsString(key) && cJSON_IsString(val))
               printf("[%s] %s: %s\n", cJSON_IsString(cat) ? cat->valuestring : "general",
                      key->valuestring, val->valuestring);
            n++;
         }
         if (n == 0)
            printf("(no entries)\n");
      }
   }
   else if (strcmp(method, "delegate") == 0)
   {
      cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
      if (cJSON_IsString(r) && r->valuestring[0])
         printf("%s\n", r->valuestring);
   }
   else if (strcmp(method, "index.find") == 0)
   {
      cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
      if (!cJSON_IsArray(hits) || cJSON_GetArraySize(hits) == 0)
      {
         printf("No matches found.\n");
         return;
      }
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         cJSON *fp = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *ln = cJSON_GetObjectItemCaseSensitive(h, "line");
         cJSON *kd = cJSON_GetObjectItemCaseSensitive(h, "kind");
         cJSON *pj = cJSON_GetObjectItemCaseSensitive(h, "project");
         printf("  %s:%d  %-12s [%s]\n", cJSON_IsString(fp) ? fp->valuestring : "?",
                cJSON_IsNumber(ln) ? (int)ln->valuedouble : 0,
                cJSON_IsString(kd) ? kd->valuestring : "",
                cJSON_IsString(pj) ? pj->valuestring : "");
      }
   }
   else if (strcmp(method, "index.list") == 0)
   {
      cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
      if (!cJSON_IsArray(projects) || cJSON_GetArraySize(projects) == 0)
      {
         printf("No indexed projects.\n");
         return;
      }
      printf("%-30s %6s %6s  %s\n", "PROJECT", "FILES", "DEFS", "ROOT");
      printf("%-30s %6s %6s  %s\n", "-------", "-----", "----", "----");
      cJSON *p;
      cJSON_ArrayForEach(p, projects)
      {
         cJSON *nm = cJSON_GetObjectItemCaseSensitive(p, "name");
         cJSON *rt = cJSON_GetObjectItemCaseSensitive(p, "root");
         cJSON *fc = cJSON_GetObjectItemCaseSensitive(p, "files");
         cJSON *dc = cJSON_GetObjectItemCaseSensitive(p, "definitions");
         printf("%-30s %6d %6d  %s\n", cJSON_IsString(nm) ? nm->valuestring : "?",
                cJSON_IsNumber(fc) ? (int)fc->valuedouble : 0,
                cJSON_IsNumber(dc) ? (int)dc->valuedouble : 0,
                cJSON_IsString(rt) ? rt->valuestring : "");
      }
   }
}

/* --- Main RPC forward function --- */

int cli_rpc_forward(const char *socket_path, const cli_rpc_route_t *route, int json_output,
                    const char *json_fields, const char *response_profile, int argc, char **argv)
{
   (void)json_fields;
   (void)response_profile;

   if (!socket_path || !route || !route->method)
      return -1;

   /* Validate socket ownership */
   if (validate_socket(socket_path) != 0)
      return -1;

   /* Skip subcmd arg if route matched by subcmd */
   int fwd_argc = argc;
   char **fwd_argv = argv;
   if (route->skip_subcmd && argc > 0)
   {
      fwd_argc--;
      fwd_argv++;
   }

   cJSON *req = marshal_request(route->method, fwd_argc, fwd_argv);
   if (!req)
      return -1;

   /* Connect and authenticate */
   cli_conn_t conn;
   if (cli_connect(&conn, socket_path) != 0)
   {
      cJSON_Delete(req);
      return -1;
   }
   if (cli_authenticate(&conn) != 0)
   {
      cli_close(&conn);
      cJSON_Delete(req);
      return -1;
   }

   int timeout = route->timeout_ms > 0 ? route->timeout_ms : CLIENT_DEFAULT_TIMEOUT_MS;
   cJSON *resp = cli_request(&conn, req, timeout);
   cli_close(&conn);
   cJSON_Delete(req);

   if (!resp)
      return -1;

   /* Protocol version mismatch → fall back */
   cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (cJSON_IsString(err) && strstr(err->valuestring, "protocol version"))
   {
      cJSON_Delete(resp);
      return -1;
   }

   /* Server-side error */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);
      cJSON_Delete(resp);
      return 1;
   }

   if (json_output)
   {
      if (route->extract)
      {
         /* Extract named array from response */
         cJSON *arr = cJSON_DetachItemFromObjectCaseSensitive(resp, route->extract);
         if (arr)
         {
            char *str = cJSON_PrintUnformatted(arr);
            if (str)
            {
               puts(str);
               free(str);
            }
            cJSON_Delete(arr);
         }
         cJSON_Delete(resp);
      }
      else
      {
         /* Strip "status", print remaining object */
         cJSON_DeleteItemFromObjectCaseSensitive(resp, "status");
         char *str = cJSON_PrintUnformatted(resp);
         if (str)
         {
            puts(str);
            free(str);
         }
         cJSON_Delete(resp);
      }
   }
   else
   {
      print_text_output(route->method, resp);
      cJSON_Delete(resp);
   }

   return 0;
}
