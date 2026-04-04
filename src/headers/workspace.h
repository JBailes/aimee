#ifndef DEC_WORKSPACE_H
#define DEC_WORKSPACE_H 1

#include <sqlite3.h>

#define MAX_DISCOVERED_PROJECTS 256
#define MAX_WORKSPACE_DEPTH     10

/* Recursively discover git repos under root, up to max_depth levels.
 * When a .git directory is found, the path is added and recursion stops
 * for that subtree. Returns count of discovered projects, -1 on error. */
int workspace_discover_projects(const char *root, int max_depth,
                                char projects[][MAX_PATH_LEN], int max);

/* Build session context from config workspaces + DB projects.
 * Reads describe files and style files for indexed projects.
 * Caller owns returned string. */
char *workspace_build_context_from_config(sqlite3 *db, const config_t *cfg);

/* Resolve a proposal path: try path as is, then search docs/proposals/ subdirectories.
 * Returns newly allocated absolute path string, or NULL if not found. Caller frees. */
char *resolve_proposal_path(const char *proposal);

/* Read a project's style guide from ~/.config/aimee/projects/<name>.style.md.
 * Returns malloc'd string or NULL. Caller frees. */
char *style_read(const char *project_name);

#endif /* DEC_WORKSPACE_H */
