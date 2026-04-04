#ifndef DEC_UTIL_H
#define DEC_UTIL_H 1

/* Normalize a key: lowercase, strip filler words, collapse whitespace.
 * Result written to buf (max buf_len). Returns buf. */
char *normalize_key(const char *key, char *buf, size_t buf_len);

/* Trigram Jaccard similarity between two strings. Returns 0.0-1.0. */
double trigram_similarity(const char *a, const char *b);

/* Basic Porter-like stemming. Result written to buf. Returns buf. */
char *stem_word(const char *word, char *buf, size_t buf_len);

/* Canonical fingerprint: normalize + stem + sort words. */
char *canonical_fingerprint(const char *text, char *buf, size_t buf_len);

/* Word-set Jaccard similarity. */
double word_similarity(const char *a, const char *b);

/* Check if two strings are likely contradictions. */
int is_contradiction(const char *a, const char *b);

/* POSIX-like shell token splitting. Returns token count.
 * Tokens written to out array (max max_tokens). Caller frees each token. */
int shlex_split(const char *command, char **out, int max_tokens);

/* Extract file paths from a shell command. Returns path count.
 * Paths written to out array. Caller frees each path. */
int extract_paths_shlex(const char *command, char **out, int max_paths);

/* Check if a token looks like a file path. */
int is_likely_path(const char *tok);

/* Tokenize text for search: lowercase, split, filter stops. Returns token count.
 * Tokens written to out array. Caller frees each token. */
int tokenize_for_search(const char *text, char **out, int max_tokens);

/* Expand terms for FTS5 indexing (includes camelCase/snake_case splits). */
char *expand_terms_for_fts(char **terms, int count, char *buf, size_t buf_len);

/* Split camelCase into parts. Returns part count. */
int split_camel_case(const char *s, char **out, int max_parts);

/* --- Option parsing --- */

#define OPT_MAX_POSITIONAL 16
#define OPT_MAX_FLAGS      32

typedef struct
{
   const char *name;   /* e.g., "weight" for --weight=N */
   const char *value;  /* points into argv, NULL if not found */
   int is_bool;        /* 1 if flag-only (no value), 0 if key=value */
} opt_flag_t;

typedef struct
{
   opt_flag_t flags[OPT_MAX_FLAGS];
   int flag_count;
   const char *positional[OPT_MAX_POSITIONAL];
   int pos_count;
} opt_parsed_t;

/* Parse argc/argv into structured flags and positional args.
 * Handles --flag=value, --flag value, --bool-flag, and positionals.
 * bool_flags is a NULL-terminated list of flags that take no value. */
void opt_parse(int argc, char **argv, const char **bool_flags, opt_parsed_t *out);

/* Get a flag value by name. Returns NULL if not found. */
const char *opt_get(const opt_parsed_t *opts, const char *name);

/* Check if a boolean flag is set. */
int opt_has(const opt_parsed_t *opts, const char *name);

/* Get positional arg by index. Returns NULL if out of range. */
const char *opt_pos(const opt_parsed_t *opts, int index);

/* Get flag value as int. Returns default_val if not found. */
int opt_get_int(const opt_parsed_t *opts, const char *name, int default_val);

/* --- Command execution --- */

/* Run a shell command, capture stdout+stderr. Returns allocated buffer (caller frees).
 * Sets *exit_code to process exit status. Returns NULL on popen failure. */
char *run_cmd(const char *cmd, int *exit_code);

/* --- Regex helpers --- */

/* One-shot regex match: compile pattern, test against text, free.
 * Returns 1 on match, 0 on no match or compile error.
 * flags are passed to regcomp (e.g. REG_EXTENDED | REG_ICASE). */
int regex_match(const char *pattern, const char *text, int flags);

/* --- Security helpers --- */

/* Fork/exec with argv array (no shell). Capture stdout into *out_buf (caller frees).
 * Returns child exit code, or -1 on fork/pipe failure. */
int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out);

/* Returns 1 if string contains shell metacharacters (;|&$`(){}><\n\r'"\\). */
int has_shell_metachar(const char *s);

/* Shell-escape a string for use inside single quotes (' -> '\''). Caller frees. */
char *shell_escape(const char *raw);

/* Returns 1 if s is a valid dpkg package name (alphanumeric, dots, hyphens, plus, colon). */
int is_valid_pkg_name(const char *s);

/* Returns 1 if url starts with a recognized git URL scheme. */
int is_valid_git_url(const char *url);

/* Safe strdup: aborts on allocation failure. */
char *safe_strdup(const char *s);

/* Sanitize a string to only contain safe characters (alphanumeric, dash, underscore, dot).
 * Replaces all other characters with underscores in-place. */
void sanitize_shell_token(char *s);

/* Returns 1 if string contains only [A-Za-z0-9_-] and length 1-128. */
int is_safe_id(const char *s);

#endif /* DEC_UTIL_H */
