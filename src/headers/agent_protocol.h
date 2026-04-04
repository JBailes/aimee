/* agent_protocol.h: LLM request building and response parsing */
#ifndef DEC_AGENT_PROTOCOL_H
#define DEC_AGENT_PROTOCOL_H 1

#include "agent_types.h"

struct cJSON;

/* --- Parsed response types --- */

typedef struct
{
   char id[64];
   char name[32];
   char *arguments; /* malloc'd */
} parsed_tool_call_t;

typedef struct
{
   int is_tool_call;
   char *content; /* malloc'd, NULL if tool call */
   parsed_tool_call_t calls[AGENT_MAX_TOOL_CALLS];
   int call_count;
   struct cJSON *assistant_message; /* cloned, caller frees */
   int prompt_tokens;
   int completion_tokens;
} parsed_response_t;

/* --- Request builders --- */

struct cJSON *agent_build_request_openai(const agent_t *agent, struct cJSON *messages,
                                         struct cJSON *tools, int max_tokens, double temperature);

struct cJSON *agent_build_request_responses(const agent_t *agent, struct cJSON *input,
                                            struct cJSON *tools, const char *system_prompt);

struct cJSON *agent_build_request_anthropic(const agent_t *agent, struct cJSON *messages,
                                            struct cJSON *tools, const char *system_prompt,
                                            int max_tokens, double temperature);

/* --- Response parsers --- */

void agent_parse_response_openai(struct cJSON *root, parsed_response_t *out);
void agent_parse_response_responses(const char *body, parsed_response_t *out);
void agent_parse_response_anthropic(struct cJSON *root, parsed_response_t *out);
void agent_free_parsed_response(parsed_response_t *p);

/* --- Message utilities --- */

int messages_compact_consecutive(struct cJSON *messages);

#endif /* DEC_AGENT_PROTOCOL_H */
