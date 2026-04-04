/*
 * platform_ipc.h: portable IPC abstraction.
 *
 * Linux/macOS use Unix domain sockets, Windows uses named pipes.
 * Provides listen/accept/connect/close operations.
 */
#ifndef DEC_PLATFORM_IPC_H
#define DEC_PLATFORM_IPC_H 1

#include "platform.h"
#include <stddef.h>
#include <sys/types.h>

/* Peer credential information extracted from the connection. */
typedef struct
{
   uid_t uid;
   gid_t gid;
   pid_t pid;
} platform_peer_cred_t;

/* Create a listening IPC endpoint at |path|.
 * On POSIX: creates a Unix domain socket.
 * On Windows: creates a named pipe (future).
 * The socket is set non-blocking.
 * Returns the fd on success, -1 on error. */
int platform_ipc_listen(const char *path, int backlog);

/* Accept a connection from a listening IPC endpoint.
 * Returns the new fd on success, -1 on error/EAGAIN. */
int platform_ipc_accept(int listen_fd);

/* Connect to an IPC endpoint at |path| with a timeout (ms).
 * Returns the fd on success, -1 on error. */
int platform_ipc_connect(const char *path, int timeout_ms);

/* Try to connect without blocking. Returns 0 on success, -1 on error.
 * Used for stale socket detection. */
int platform_ipc_probe(const char *path);

/* Close an IPC connection. */
void platform_ipc_close(int fd);

/* Extract peer credentials from a connected IPC socket.
 * Returns 0 on success, -1 on error or unsupported. */
int platform_ipc_peer_cred(int fd, platform_peer_cred_t *out);

/* Check if a socket file exists and remove it if stale. */
void platform_ipc_cleanup_stale(const char *path);

#endif /* DEC_PLATFORM_IPC_H */
