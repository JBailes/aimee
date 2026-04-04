#ifndef DEC_FEEDBACK_H
#define DEC_FEEDBACK_H 1

/* Parse polarity string (+, -, posi, negi, principle). Returns canonical form
 * or NULL on error. Result is a static string, do not free. */
const char *feedback_parse_polarity(const char *input);

/* Record feedback. Creates or reinforces a rule.
 * weight_override: use -1 for default behavior.
 * Sets *reinforced to 1 if an existing rule was reinforced.
 * Returns the rule ID on success, -1 on error. */
int feedback_record(sqlite3 *db, const char *polarity, const char *title,
                    const char *description, int weight_override, int *reinforced);

#endif /* DEC_FEEDBACK_H */
