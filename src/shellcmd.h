/* Filesystem commands for the MVP console. See shellcmd.c. */
#ifndef CARDOS_SHELLCMD_H
#define CARDOS_SHELLCMD_H

const char *shell_cwd(void);

void cmd_pwd(void);
void cmd_cd(const char *arg);
void cmd_ls(const char *arg);
void cmd_cat(const char *arg);
void cmd_df(void);
void cmd_mkdir(const char *arg);
void cmd_rm(const char *arg);
void cmd_apps(void);
void cmd_boot(const char *arg, int confirmed);
void cmd_bootinfo(void);
void cmd_taskcost(void);
void cmd_mouse(const char *arg);

/* wifi              -- status
 * wifi scan         -- list networks
 * wifi SSID PASS    -- join and remember
 * wifi saved        -- rejoin what was remembered
 * wifi forget / off */
void cmd_wifi(const char *arg);

/* get URL -- fetch and print, for checking that the network actually works. */
void cmd_get(const char *arg);

/* run          -- what there is to run
 * run NAME     -- start it fullscreen; escape returns to the launcher */
void cmd_run(const char *arg);

/* env            -- list the variables
 * set NAME=VALUE -- set one, or unset it with an empty value */
void cmd_env(void);
void cmd_set(const char *arg);

/* Resolve a bare word or a path into something runnable and start it. This is
 * what an unrecognised command falls through to, so "./grep x" and "grep x"
 * both work -- the first as a path, the second through PATH. Returns 0 if it
 * started something. */
int  shell_exec(const char *word, const char *args);

#endif /* CARDOS_SHELLCMD_H */
