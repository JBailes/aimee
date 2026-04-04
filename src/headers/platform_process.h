/*
 * platform_process.h: portable process spawning.
 *
 * Wraps fork+exec (POSIX) / CreateProcess (Windows) for the two
 * main patterns used in aimee:
 *   1. Daemon spawn (detach from parent, redirect stdio)
 *   2. Capture spawn (run command, capture stdout/stderr)
 */
#ifndef DEC_PLATFORM_PROCESS_H
#define DEC_PLATFORM_PROCESS_H 1

#include "platform.h"
#include <sys/types.h>

/* Spawn a daemon process. The child:
 *   - calls setsid() (POSIX) or detaches (Windows)
 *   - redirects stdin/stdout/stderr to /dev/null (or NUL)
 *   - execs |argv[0]| with the given argument list (NULL-terminated)
 *
 * Returns the child PID on success (to the parent), -1 on error.
 * The parent does not wait for the child. */
pid_t platform_spawn_daemon(const char *const argv[]);

/* Spawn a subprocess and capture its stdout.
 * |cmd| is executed via /bin/sh -c (POSIX) or cmd.exe /c (Windows).
 * |out| receives the captured stdout (caller must free).
 * |out_len| receives the length.
 * |timeout_ms| is the max execution time (0 = no limit).
 *
 * Returns the exit code, or -1 on spawn failure. */
int platform_exec_capture(const char *cmd, char **out, size_t *out_len,
                          int timeout_ms);

#endif /* DEC_PLATFORM_PROCESS_H */
