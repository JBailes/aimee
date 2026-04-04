/* platform_ipc.c: portable IPC (Unix domain sockets on POSIX) */
#define _GNU_SOURCE
#include "platform_ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef AIMEE_POSIX
/* ---- POSIX: Unix domain sockets ---- */

int platform_ipc_listen(const char *path, int backlog)
{
   /* Ensure parent directory exists */
   char dir[4096];
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      mkdir(dir, 0700);
   }

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;
   fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

   mode_t old_umask = umask(0077);
   int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
   umask(old_umask);

   if (rc < 0)
   {
      close(fd);
      return -1;
   }

   if (listen(fd, backlog) < 0)
   {
      close(fd);
      unlink(path);
      return -1;
   }

   chmod(path, 0600);
   return fd;
}

int platform_ipc_accept(int listen_fd)
{
   int fd = accept(listen_fd, NULL, NULL);
   if (fd < 0)
      return -1;

   /* Set non-blocking */
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags >= 0)
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);

   return fd;
}

int platform_ipc_connect(const char *path, int timeout_ms)
{
   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return -1;

   /* Set non-blocking for timeout support */
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags >= 0)
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);

   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

   int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
   if (rc == 0)
      goto connected;

   if (errno != EINPROGRESS)
   {
      close(fd);
      return -1;
   }

   /* Wait for connection */
   struct pollfd pfd = {.fd = fd, .events = POLLOUT};
   rc = poll(&pfd, 1, timeout_ms);
   if (rc <= 0)
   {
      close(fd);
      return -1;
   }

   int err = 0;
   socklen_t errlen = sizeof(err);
   getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
   if (err != 0)
   {
      close(fd);
      return -1;
   }

connected:
   /* Restore blocking mode */
   if (flags >= 0)
      fcntl(fd, F_SETFL, flags);

   return fd;
}

int platform_ipc_probe(const char *path)
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
   close(fd);
   return rc;
}

void platform_ipc_close(int fd)
{
   if (fd >= 0)
      close(fd);
}

int platform_ipc_peer_cred(int fd, platform_peer_cred_t *out)
{
#ifdef AIMEE_LINUX
   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
      return -1;
   out->uid = cred.uid;
   out->gid = cred.gid;
   out->pid = cred.pid;
   return 0;
#elif defined(AIMEE_MACOS)
   /* macOS: getpeereid() gives uid+gid, no pid */
   uid_t euid;
   gid_t egid;
   if (getpeereid(fd, &euid, &egid) < 0)
      return -1;
   out->uid = euid;
   out->gid = egid;
   out->pid = 0; /* not available via getpeereid */
   return 0;
#else
   (void)fd;
   (void)out;
   return -1;
#endif
}

void platform_ipc_cleanup_stale(const char *path)
{
   struct stat st;
   if (lstat(path, &st) != 0)
      return;

   /* Reject symlinks */
   if (S_ISLNK(st.st_mode))
      return;

   if (!S_ISSOCK(st.st_mode))
      return;

   /* Try connecting to see if a server is running */
   if (platform_ipc_probe(path) == 0)
      return; /* Server is alive, do not remove */

   /* Stale socket */
   unlink(path);
}

#elif defined(AIMEE_WINDOWS)
/* ---- Windows: named pipe stubs (Phase 2) ---- */

int platform_ipc_listen(const char *path, int backlog)
{
   (void)path;
   (void)backlog;
   return -1;
}

int platform_ipc_accept(int listen_fd)
{
   (void)listen_fd;
   return -1;
}

int platform_ipc_connect(const char *path, int timeout_ms)
{
   (void)path;
   (void)timeout_ms;
   return -1;
}

int platform_ipc_probe(const char *path)
{
   (void)path;
   return -1;
}

void platform_ipc_close(int fd)
{
   (void)fd;
}

int platform_ipc_peer_cred(int fd, platform_peer_cred_t *out)
{
   (void)fd;
   (void)out;
   return -1;
}

void platform_ipc_cleanup_stale(const char *path)
{
   (void)path;
}

#endif
