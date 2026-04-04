/* platform_process.c: portable process spawning (fork+exec on POSIX) */
#include "platform_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AIMEE_POSIX

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

pid_t platform_spawn_daemon(const char *const argv[])
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;

   if (pid > 0)
      return pid; /* Parent: return child PID */

   /* Child: become daemon */
   setsid();

   /* Redirect stdio to /dev/null */
   int devnull = open("/dev/null", O_RDWR);
   if (devnull >= 0)
   {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO)
         close(devnull);
   }

   execvp(argv[0], (char *const *)argv);
   _exit(127); /* exec failed */
}

int platform_exec_capture(const char *cmd, char **out, size_t *out_len, int timeout_ms)
{
   int pipefd[2];
   if (pipe(pipefd) < 0)
      return -1;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(pipefd[0]);
      close(pipefd[1]);
      return -1;
   }

   if (pid == 0)
   {
      /* Child */
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   /* Parent: read output */
   close(pipefd[1]);

   /* Set pipe to non-blocking if we have a timeout */
   if (timeout_ms > 0)
   {
      int flags = fcntl(pipefd[0], F_GETFL, 0);
      if (flags >= 0)
         fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
   }

   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      close(pipefd[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
   }

   int timed_out = 0;
   int elapsed_ms = 0;
   const int poll_interval_ms = 10;
   const struct timespec poll_ts = {0, poll_interval_ms * 1000000L};

   for (;;)
   {
      if (len + 1024 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
            break;
         buf = tmp;
      }
      ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
      if (n > 0)
      {
         len += (size_t)n;
         continue;
      }
      if (n == 0)
         break; /* EOF */
      /* n < 0 */
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
         /* Check if child has exited */
         int status = 0;
         pid_t w = waitpid(pid, &status, WNOHANG);
         if (w > 0)
         {
            /* Child exited; drain remaining output */
            for (;;)
            {
               if (len + 1024 > cap)
               {
                  cap *= 2;
                  char *tmp = realloc(buf, cap);
                  if (!tmp)
                     break;
                  buf = tmp;
               }
               ssize_t r = read(pipefd[0], buf + len, cap - len - 1);
               if (r <= 0)
                  break;
               len += (size_t)r;
            }
            close(pipefd[0]);
            buf[len] = '\0';
            *out = buf;
            if (out_len)
               *out_len = len;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
         }
         /* Check timeout */
         if (timeout_ms > 0 && elapsed_ms >= timeout_ms)
         {
            timed_out = 1;
            break;
         }
         nanosleep(&poll_ts, NULL);
         elapsed_ms += poll_interval_ms;
         continue;
      }
      /* Real read error */
      break;
   }
   close(pipefd[0]);

   if (timed_out)
   {
      kill(pid, SIGTERM);
      /* Give child a brief grace period to exit */
      const struct timespec grace = {0, 100 * 1000000L}; /* 100ms */
      nanosleep(&grace, NULL);
      if (waitpid(pid, NULL, WNOHANG) == 0)
      {
         kill(pid, SIGKILL);
         waitpid(pid, NULL, 0);
      }
      fprintf(stderr, "aimee: child process timed out after %d ms\n", timeout_ms);
      buf[len] = '\0';
      *out = buf;
      if (out_len)
         *out_len = len;
      return -1;
   }

   int status = 0;
   waitpid(pid, &status, 0);

   buf[len] = '\0';
   *out = buf;
   if (out_len)
      *out_len = len;

   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#elif defined(AIMEE_WINDOWS)
/* ---- Windows stubs (Phase 2) ---- */

pid_t platform_spawn_daemon(const char *const argv[])
{
   (void)argv;
   return -1;
}

int platform_exec_capture(const char *cmd, char **out, size_t *out_len, int timeout_ms)
{
   (void)cmd;
   (void)out;
   (void)out_len;
   (void)timeout_ms;
   return -1;
}

#endif
