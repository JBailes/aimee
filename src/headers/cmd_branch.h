#ifndef DEC_CMD_BRANCH_H
#define DEC_CMD_BRANCH_H 1

/* cmd_branch.h: types and functions exposed for testing */

#define BRANCH_MAX_BRANCHES    64
#define BRANCH_MAX_FILES       256
#define BRANCH_MAX_SHARED      32 /* max shared files tracked per edge */
#define BRANCH_MAX_EDGES       128

/* A branch entry with the files it touches relative to main. */
typedef struct
{
   char name[256];
   char *files[BRANCH_MAX_FILES]; /* allocated strings */
   int file_count;
   int conflict_count; /* how many other branches this conflicts with */
} branch_info_t;

/* A conflict edge between two branches. */
typedef struct
{
   int a; /* index into branch array */
   int b;
   int shared_file_count;
   char *shared_files[BRANCH_MAX_SHARED]; /* allocated strings */
} conflict_edge_t;

typedef struct
{
   branch_info_t branches[BRANCH_MAX_BRANCHES];
   int branch_count;
   conflict_edge_t edges[BRANCH_MAX_EDGES];
   int edge_count;
   int merge_order[BRANCH_MAX_BRANCHES];
} conflict_graph_t;

/* Build conflict graph edges and merge order from pre-populated branch file lists. */
void branch_build_conflict_graph(conflict_graph_t *g);

/* Free shared_files in edges. Does NOT free branch files. */
void branch_free_edges(conflict_graph_t *g);

/* Auto-resolution: renumber a migration entry. Returns new content (caller frees). */
char *branch_resolve_migration_renumber(const char *content, int old_num, int new_num);

/* Auto-resolution: merge two space-separated lists. Returns merged line (caller frees). */
char *branch_resolve_additive_list(const char *base_line, const char *ours, const char *theirs);

#endif /* DEC_CMD_BRANCH_H */
