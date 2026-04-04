/* server_main.c: aimee-server entry point -- socket lifecycle, signal handling */
#include "aimee.h"
#include "cli_client.h"
#include "config.h"
#include "server.h"
#include "agent_exec.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static server_ctx_t g_ctx;

static void signal_handler(int sig)
{
   (void)sig;
   g_ctx.running = 0;
}

int main(int argc, char **argv)
{
   const char *socket_path = NULL;
   int persistent = 0;
   log_level_t log_level = LOG_INFO;

   /* Parse args (before stderr redirect so help/errors print to terminal) */
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-f") == 0)
      {
         /* Default behavior is foreground; flag accepted for compatibility */
      }
      else if (strcmp(argv[i], "--persistent") == 0 || strcmp(argv[i], "-p") == 0)
      {
         persistent = 1;
      }
      else if (strncmp(argv[i], "--socket=", 9) == 0)
      {
         socket_path = argv[i] + 9;
      }
      else if (strncmp(argv[i], "--log-level=", 12) == 0)
      {
         if (log_parse_level(argv[i] + 12, &log_level) != 0)
         {
            fprintf(stderr, "aimee-server: invalid log level: %s\n", argv[i] + 12);
            return 1;
         }
      }
      else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
      {
         fprintf(stdout, "aimee-server %s (protocol %d)\n", AIMEE_VERSION, SERVER_PROTOCOL_VERSION);
         return 0;
      }
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         fprintf(stdout,
                 "Usage: aimee-server [options]\n"
                 "  --socket=PATH        Unix socket path (default: ~/.config/aimee/aimee.sock)\n"
                 "  --persistent         Ignore shutdown requests and idle timeout (for systemd)\n"
                 "  --log-level=LEVEL    Log level: error, warn, info, debug (default: info)\n"
                 "  --foreground         Run in foreground (default)\n"
                 "  --version            Print version\n"
                 "  --help               Show this help\n");
         return 0;
      }
      else
      {
         fprintf(stderr, "aimee-server: unknown option: %s\n", argv[i]);
         return 1;
      }
   }

   /* Redirect stderr to log file so server messages never leak to the user's
    * terminal -- regardless of how the server was started. */
   {
      char log_path[4096];
      snprintf(log_path, sizeof(log_path), "%s/server.log", config_default_dir());
      FILE *log_fp = fopen(log_path, "a");
      if (log_fp)
      {
         setvbuf(log_fp, NULL, _IOLBF, 0);
         dup2(fileno(log_fp), STDERR_FILENO);
         fclose(log_fp);
      }
   }

   /* Initialize logging */
   log_init(log_level);
   audit_log_open();

   if (!socket_path)
      socket_path = cli_default_socket_path();

   /* Initialize server first — creates the Unix socket so clients can connect
    * (and queue in the listen backlog) while HTTP/SSL initializes. */
   if (server_init(&g_ctx, socket_path) != 0)
      return 1;

   /* Initialize HTTP client after socket is listening */
   agent_http_init();

   /* Install signal handlers */
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_handler = signal_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;

   sigaction(SIGTERM, &sa, NULL);
   sigaction(SIGINT, &sa, NULL);

   /* Ignore SIGPIPE (write to closed socket) */
   signal(SIGPIPE, SIG_IGN);

   g_ctx.persistent = persistent;
   g_ctx.running = 1;
   if (persistent)
      LOG_INFO("server", "persistent mode enabled");
   int rc = server_run(&g_ctx);

   server_shutdown(&g_ctx);
   agent_http_cleanup();
   audit_log_close();

   return rc < 0 ? 1 : 0;
}
