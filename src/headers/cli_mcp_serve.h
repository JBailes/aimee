#ifndef DEC_CLI_MCP_SERVE_H
#define DEC_CLI_MCP_SERVE_H 1

/* Entry point for `aimee mcp-serve`: thin MCP stdio proxy that forwards
 * tool calls to aimee-server over the Unix socket. */
int cli_mcp_serve(void);

#endif /* DEC_CLI_MCP_SERVE_H */
