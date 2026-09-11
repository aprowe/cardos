/* Environment variables. Device-only.
 *
 * A handful of strings the shell keeps, PATH being the one that earns the
 * machinery. Stored in NVS, so a PATH set once survives a reboot -- a variable
 * you have to set again every time is a variable you stop using.
 *
 * Deliberately small: no export, no scopes, no substitution. The shell is one
 * process and there is nothing to inherit.
 */
#ifndef CARDOS_ENV_H
#define CARDOS_ENV_H

#define ENV_MAX       8
#define ENV_NAME_MAX  16
#define ENV_VALUE_MAX 96

/* Loads what was saved and fills in the defaults for anything unset. */
void env_init(void);

/* NULL if unset. The pointer is valid until the next env_set. */
const char *env_get(const char *name);

/* A NULL or empty value unsets. Saved immediately. */
int env_set(const char *name, const char *value);

int         env_count(void);
const char *env_name_at(int i);
const char *env_value_at(int i);

/* Walk PATH one directory at a time. `iter` starts at 0 and is advanced;
 * returns 0 when there are no more. */
int env_path_next(int *iter, char *out, int out_size);

#endif /* CARDOS_ENV_H */
