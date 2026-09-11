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

#endif /* CARDOS_SHELLCMD_H */
