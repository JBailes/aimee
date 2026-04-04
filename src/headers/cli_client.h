#ifndef DEC_CLI_CLIENT_H
#define DEC_CLI_CLIENT_H 1

#include <stddef.h>

#define CLIENT_DEFAULT_TIMEOUT_MS 5000
#define CLIENT_CONNECT_TIMEOUT_MS 1000
#define CLIENT_READ_BUF_SIZE      65536

/* Forward declaration -- cJSON.h included by .c files */
typedef struct cJSON cJSON;

/* Connection handle */
typedef struct
{
   int fd;
   char read_buf[CLIENT_READ_BUF_SIZE];
   size_t read_len;
} cli_conn_t;

/* Connect to server. Returns 0 on success, -1 on failure.
 * socket_path may be NULL for default (~/.config/aimee/aimee.sock). */
int cli_connect(cli_conn_t *conn, const char *socket_path);

/* Send a JSON request and receive a JSON response.
 * Caller owns the returned cJSON object (must cJSON_Delete).
 * Returns NULL on error (timeout, disconnect, parse failure). */
cJSON *cli_request(cli_conn_t *conn, cJSON *request, int timeout_ms);

/* Check if server is available (connect + server.info + disconnect).
 * Returns 1 if available, 0 if not. */
int cli_server_available(const char *socket_path);

/* Authenticate with the server using the capability token.
 * Returns 0 on success, -1 on failure. */
int cli_authenticate(cli_conn_t *conn);

/* Close connection. */
void cli_close(cli_conn_t *conn);

/* Connect with a custom timeout (ms). socket_path may be NULL for default. */
int cli_connect_timeout(cli_conn_t *conn, const char *socket_path, int timeout_ms);

/* Default socket path: ~/.config/aimee/aimee.sock */
const char *cli_default_socket_path(void);

/* Ensure a server is running. Tries AIMEE_SOCK, then well-known socket,
 * then auto-starts a non-persistent server. Sets active_socket_path.
 * Returns the socket path to use, or NULL on failure. */
const char *cli_ensure_server(void);

/* RPC thin-client routing.
 * Looks up an RPC route for a CLI command+subcommand combination. */
typedef struct
{
   const char *method;  /* server RPC method name */
   const char *extract; /* response field to extract (NULL = return object minus "status") */
   int skip_subcmd;     /* 1 if first arg is a subcmd to skip when marshaling */
   int timeout_ms;      /* RPC timeout (0 = CLIENT_DEFAULT_TIMEOUT_MS) */
} cli_rpc_route_t;

/* Returns 1 if a matching RPC route was found, 0 otherwise. */
int cli_rpc_lookup(const char *cmd, const char *subcmd, cli_rpc_route_t *route);

/* Forward a CLI command through the server RPC.
 * Returns 0 on success, >0 on application error, -1 on transport/protocol
 * error (caller should fall back to in-process execution).
 * argc/argv are the args AFTER the command name (e.g., for "aimee memory search foo",
 * argv = ["search", "foo"]). The route's skip_subcmd controls whether the first
 * arg is stripped before marshaling. */
int cli_rpc_forward(const char *socket_path, const cli_rpc_route_t *route, int json_output,
                    const char *json_fields, const char *response_profile, int argc, char **argv);

/* Launch metadata parsed from server output */
typedef struct
{
   char provider[64];
   int builtin;
   int autonomous;
   char worktree_cwd[4096];
   size_t context_len; /* bytes of session context before __LAUNCH__ marker */
} launch_meta_t;

/* Parse __LAUNCH__ metadata from server output.
 * Returns 1 if launch metadata was found and parsed, 0 otherwise. */
int parse_launch_meta(const char *output, launch_meta_t *meta);

#endif /* DEC_CLI_CLIENT_H */
