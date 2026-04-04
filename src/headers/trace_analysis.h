#ifndef DEC_TRACE_ANALYSIS_H
#define DEC_TRACE_ANALYSIS_H 1

#include <sqlite3.h>

/* Run trace mining on recent execution traces.
 * Detects retry loops, recovery sequences, and common tool sequences.
 * Records findings as anti-patterns or procedure memories.
 * Returns the number of patterns discovered, or -1 on error. */
int trace_mine(sqlite3 *db);

#endif /* DEC_TRACE_ANALYSIS_H */
