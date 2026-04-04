#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *host;
    int port;
} config_t;

struct server {
    config_t cfg;
    int running;
};

static config_t default_config(void)
{
    config_t c = {.host = "localhost", .port = 8080};
    return c;
}

int server_start(struct server *s)
{
    printf("Starting on %s:%d\n", s->cfg.host, s->cfg.port);
    s->running = 1;
    return 0;
}

#define MAX_CONNECTIONS 100
