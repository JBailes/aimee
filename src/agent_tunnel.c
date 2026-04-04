/* agent_tunnel.c: reverse SSH tunnel lifecycle for NAT piercing */
#define _GNU_SOURCE
#include "aimee.h"
#include "agent_tunnel.h"
#include "cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* --- State string conversion --- */

const char *agent_tunnel_state_str(agent_tunnel_state_t state)
{
   switch (state)
   {
   case TUNNEL_STATE_IDLE:
      return "idle";
   case TUNNEL_STATE_CONNECTING:
      return "connecting";
   case TUNNEL_STATE_ACTIVE:
      return "active";
   case TUNNEL_STATE_RECONNECTING:
      return "reconnecting";
   case TUNNEL_STATE_FAILED:
      return "failed";
   case TUNNEL_STATE_STOPPED:
      return "stopped";
   }
   return "unknown";
}

/* --- Internal: parse allocated port from ssh stderr --- */

static int parse_allocated_port(int fd, int *port_out, int timeout_ms)
{
   char buf[4096];
   size_t pos = 0;
   struct pollfd pfd = {.fd = fd, .events = POLLIN};

   while (pos < sizeof(buf) - 1)
   {
      int ret = poll(&pfd, 1, timeout_ms);
      if (ret <= 0)
         return -1; /* timeout or error */

      ssize_t n = read(fd, buf + pos, sizeof(buf) - 1 - pos);
      if (n <= 0)
         return -1;
      pos += (size_t)n;
      buf[pos] = '\0';

      /* Look for "Allocated port NNNNN for remote forward" */
      const char *marker = strstr(buf, "Allocated port ");
      if (marker)
      {
         int port = atoi(marker + 15);
         if (port > 0 && port < 65536)
         {
            *port_out = port;
            return 0;
         }
      }
   }
   return -1;
}

/* --- Internal: extract user@host from relay_ssh string --- */

static void extract_relay_target(const char *relay_ssh, char *user_host, size_t len)
{
   /* relay_ssh is like "ssh [-p PORT] [-i KEY] user@host" -- we want the last arg */
   const char *last_space = strrchr(relay_ssh, ' ');
   if (last_space)
      snprintf(user_host, len, "%s", last_space + 1);
   else
      snprintf(user_host, len, "%s", relay_ssh);
}

/* --- Internal: fork/exec ssh -R --- */

static int tunnel_fork_ssh(agent_tunnel_t *t)
{
   int stderr_pipe[2];
   if (pipe(stderr_pipe) < 0)
   {
      snprintf(t->error, sizeof(t->error), "pipe: %s", strerror(errno));
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      snprintf(t->error, sizeof(t->error), "fork: %s", strerror(errno));
      close(stderr_pipe[0]);
      close(stderr_pipe[1]);
      return -1;
   }

   if (pid == 0)
   {
      /* Child: exec ssh */
      close(stderr_pipe[0]);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stderr_pipe[1]);

      /* Redirect stdout to /dev/null */
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDIN_FILENO);
         if (devnull > 2)
            close(devnull);
      }

      /* Build argv */
      char remote_spec[256];
      snprintf(remote_spec, sizeof(remote_spec), "0:%s:%d", t->target_host, t->target_port);

      /* Parse relay_ssh to extract args. Simple approach: tokenize by space.
       * relay_ssh is like "ssh -p 22 relay@host" or "ssh relay@host" */
      char relay_copy[512];
      snprintf(relay_copy, sizeof(relay_copy), "%s", t->relay_ssh);

      const char *argv[32];
      int argc = 0;

      char *tok = strtok(relay_copy, " ");
      while (tok && argc < 20)
      {
         argv[argc++] = tok;
         tok = strtok(NULL, " ");
      }

      /* If first arg is "ssh", skip it (we'll use execvp("ssh", ...)) */
      int start = 0;
      if (argc > 0 && strcmp(argv[0], "ssh") == 0)
         start = 1;

      const char *exec_argv[32];
      int ei = 0;
      exec_argv[ei++] = "ssh";

      /* Copy relay args (skipping "ssh" if present) */
      for (int i = start; i < argc; i++)
         exec_argv[ei++] = argv[i];

      /* Add key if configured */
      if (t->relay_key[0])
      {
         exec_argv[ei++] = "-i";
         exec_argv[ei++] = t->relay_key;
      }

      /* Add tunnel options */
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "ServerAliveInterval=15";
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "ServerAliveCountMax=3";
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "ExitOnForwardFailure=yes";
      exec_argv[ei++] = "-o";
      exec_argv[ei++] = "StrictHostKeyChecking=accept-new";
      exec_argv[ei++] = "-v"; /* verbose -- needed for "Allocated port" message */
      exec_argv[ei++] = "-N"; /* no remote command */
      exec_argv[ei++] = "-R";
      exec_argv[ei++] = remote_spec;
      exec_argv[ei] = NULL;

      execvp("ssh", (char *const *)exec_argv);
      _exit(127);
   }

   /* Parent */
   close(stderr_pipe[1]);
   t->ssh_pid = pid;

   /* Wait for port allocation (15 second timeout) */
   int port = 0;
   if (parse_allocated_port(stderr_pipe[0], &port, 15000) == 0)
   {
      t->allocated_port = port;

      /* Build effective entry: "ssh -p PORT user@relay" */
      char relay_target[256];
      extract_relay_target(t->relay_ssh, relay_target, sizeof(relay_target));
      snprintf(t->effective_entry, sizeof(t->effective_entry), "ssh -p %d %s", port, relay_target);

      t->state = TUNNEL_STATE_ACTIVE;
      t->error[0] = '\0';
      close(stderr_pipe[0]);
      return 0;
   }

   /* Port discovery failed */
   close(stderr_pipe[0]);
   snprintf(t->error, sizeof(t->error), "port allocation timeout");
   kill(pid, SIGTERM);
   waitpid(pid, NULL, 0);
   t->ssh_pid = 0;
   return -1;
}

/* --- Monitor thread --- */

typedef struct
{
   agent_tunnel_mgr_t *mgr;
   int index;
} monitor_arg_t;

static void *tunnel_monitor_thread(void *arg)
{
   monitor_arg_t *ma = (monitor_arg_t *)arg;
   agent_tunnel_mgr_t *mgr = ma->mgr;
   int idx = ma->index;
   free(ma);

   while (!mgr->shutdown)
   {
      agent_tunnel_t *t = &mgr->tunnels[idx];

      if (t->ssh_pid > 0)
      {
         int status = 0;
         pid_t w = waitpid(t->ssh_pid, &status, WNOHANG);
         if (w > 0)
         {
            /* SSH process exited */
            pthread_mutex_lock(&mgr->lock);
            t->ssh_pid = 0;
            t->allocated_port = 0;
            t->effective_entry[0] = '\0';

            if (mgr->shutdown)
            {
               t->state = TUNNEL_STATE_STOPPED;
               pthread_mutex_unlock(&mgr->lock);
               break;
            }

            /* Attempt reconnect */
            t->reconnect_count++;
            if (t->max_reconnects > 0 && t->reconnect_count > t->max_reconnects)
            {
               t->state = TUNNEL_STATE_FAILED;
               snprintf(t->error, sizeof(t->error), "max reconnects exceeded (%d)",
                        t->max_reconnects);
               fprintf(stderr, "aimee: tunnel '%s': %s\n", t->name, t->error);
               pthread_mutex_unlock(&mgr->lock);
               break;
            }

            t->state = TUNNEL_STATE_RECONNECTING;
            fprintf(stderr, "aimee: tunnel '%s': reconnecting (attempt %d)...\n", t->name,
                    t->reconnect_count);
            pthread_mutex_unlock(&mgr->lock);

            int delay = t->reconnect_delay_s > 0 ? t->reconnect_delay_s : 5;
            for (int i = 0; i < delay && !mgr->shutdown; i++)
               sleep(1);

            if (mgr->shutdown)
               break;

            pthread_mutex_lock(&mgr->lock);
            t->state = TUNNEL_STATE_CONNECTING;
            pthread_mutex_unlock(&mgr->lock);

            if (tunnel_fork_ssh(t) != 0)
            {
               pthread_mutex_lock(&mgr->lock);
               t->state = TUNNEL_STATE_FAILED;
               fprintf(stderr, "aimee: tunnel '%s': reconnect failed: %s\n", t->name, t->error);
               pthread_mutex_unlock(&mgr->lock);
            }
            else
            {
               pthread_mutex_lock(&mgr->lock);
               t->reconnect_count = 0; /* reset on success */
               fprintf(stderr, "aimee: tunnel '%s': reconnected on port %d\n", t->name,
                       t->allocated_port);
               pthread_mutex_unlock(&mgr->lock);
            }
         }
      }

      /* Poll every 2 seconds */
      for (int i = 0; i < 4 && !mgr->shutdown; i++)
         usleep(500000);
   }

   return NULL;
}

/* --- Public API --- */

void agent_tunnel_mgr_init(agent_tunnel_mgr_t *mgr)
{
   pthread_mutex_init(&mgr->lock, NULL);
   mgr->shutdown = 0;
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      mgr->tunnels[i].state = TUNNEL_STATE_IDLE;
      mgr->tunnels[i].ssh_pid = 0;
      mgr->tunnels[i].allocated_port = 0;
      mgr->tunnels[i].reconnect_count = 0;
      mgr->tunnels[i].error[0] = '\0';
      mgr->tunnels[i].effective_entry[0] = '\0';
   }
}

int agent_tunnel_start_all(agent_tunnel_mgr_t *mgr)
{
   if (mgr->tunnel_count == 0)
      return -1;

   int started = 0;
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      agent_tunnel_t *t = &mgr->tunnels[i];
      t->state = TUNNEL_STATE_CONNECTING;
      fprintf(stderr, "aimee: tunnel '%s': connecting to %s (target %s:%d)...\n", t->name,
              t->relay_ssh, t->target_host, t->target_port);

      if (tunnel_fork_ssh(t) == 0)
      {
         fprintf(stderr, "aimee: tunnel '%s': active on port %d\n", t->name, t->allocated_port);

         /* Spawn monitor thread */
         monitor_arg_t *ma = malloc(sizeof(monitor_arg_t));
         if (ma)
         {
            ma->mgr = mgr;
            ma->index = i;
            pthread_create(&t->monitor_thread, NULL, tunnel_monitor_thread, ma);
         }
         started++;
      }
      else
      {
         t->state = TUNNEL_STATE_FAILED;
         fprintf(stderr, "aimee: tunnel '%s': failed: %s\n", t->name, t->error);
      }
   }

   return started > 0 ? 0 : -1;
}

void agent_tunnel_stop_all(agent_tunnel_mgr_t *mgr)
{
   mgr->shutdown = 1;

   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      agent_tunnel_t *t = &mgr->tunnels[i];
      if (t->ssh_pid > 0)
      {
         kill(t->ssh_pid, SIGTERM);
         waitpid(t->ssh_pid, NULL, 0);
         t->ssh_pid = 0;
      }
   }

   /* Join monitor threads */
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      agent_tunnel_t *t = &mgr->tunnels[i];
      if (t->state != TUNNEL_STATE_IDLE && t->state != TUNNEL_STATE_FAILED)
         pthread_join(t->monitor_thread, NULL);
      t->state = TUNNEL_STATE_STOPPED;
      t->allocated_port = 0;
      t->effective_entry[0] = '\0';
   }
}

void agent_tunnel_mgr_destroy(agent_tunnel_mgr_t *mgr)
{
   agent_tunnel_stop_all(mgr);
   pthread_mutex_destroy(&mgr->lock);
}

agent_tunnel_t *agent_tunnel_find(agent_tunnel_mgr_t *mgr, const char *name)
{
   for (int i = 0; i < mgr->tunnel_count; i++)
   {
      if (strcmp(mgr->tunnels[i].name, name) == 0)
         return &mgr->tunnels[i];
   }
   return NULL;
}

int agent_tunnel_resolve_entry(const agent_tunnel_mgr_t *mgr, const agent_network_t *network,
                               const agent_net_host_t *host, char *buf, size_t buf_len)
{
   if (mgr && host->tunnel[0])
   {
      for (int i = 0; i < mgr->tunnel_count; i++)
      {
         const agent_tunnel_t *t = &mgr->tunnels[i];
         if (strcmp(t->name, host->tunnel) == 0 && t->state == TUNNEL_STATE_ACTIVE &&
             t->effective_entry[0])
         {
            snprintf(buf, buf_len, "%s", t->effective_entry);
            return 1;
         }
      }
   }

   /* Fallback to network ssh_entry */
   if (network && network->ssh_entry[0])
      snprintf(buf, buf_len, "%s", network->ssh_entry);
   else
      buf[0] = '\0';
   return 0;
}

void agent_tunnel_print_status(const agent_tunnel_mgr_t *mgr, int json_output)
{
   if (json_output)
   {
      cJSON *arr = cJSON_CreateArray();
      for (int i = 0; i < mgr->tunnel_count; i++)
      {
         const agent_tunnel_t *t = &mgr->tunnels[i];
         cJSON *obj = cJSON_CreateObject();
         cJSON_AddStringToObject(obj, "name", t->name);
         cJSON_AddStringToObject(obj, "state", agent_tunnel_state_str(t->state));
         cJSON_AddStringToObject(obj, "relay", t->relay_ssh);
         cJSON_AddStringToObject(obj, "target_host", t->target_host);
         cJSON_AddNumberToObject(obj, "target_port", t->target_port);
         cJSON_AddNumberToObject(obj, "allocated_port", t->allocated_port);
         if (t->error[0])
            cJSON_AddStringToObject(obj, "error", t->error);
         cJSON_AddItemToArray(arr, obj);
      }
      char *json = cJSON_Print(arr);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
      cJSON_Delete(arr);
   }
   else
   {
      if (mgr->tunnel_count == 0)
      {
         printf("No tunnels configured.\n");
         return;
      }
      printf("%-12s %-12s %-30s %-20s %s\n", "TUNNEL", "STATE", "RELAY", "TARGET", "PORT");
      for (int i = 0; i < mgr->tunnel_count; i++)
      {
         const agent_tunnel_t *t = &mgr->tunnels[i];
         char target[80];
         snprintf(target, sizeof(target), "%s:%d", t->target_host, t->target_port);
         printf("%-12s %-12s %-30s %-20s %d\n", t->name, agent_tunnel_state_str(t->state),
                t->relay_ssh, target, t->allocated_port);
      }
   }
}
