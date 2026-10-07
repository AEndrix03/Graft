#ifndef MG_CLI_UPGRADE_H
#define MG_CLI_UPGRADE_H

int mg_upgrade_cmd(int argc, char **argv);

/* At most once per 24h, spawns a detached `graft upgrade --yes`. Silent and
 * non-blocking; skipped for dev builds, CI, non-standard or package-manager
 * installs, and when GRAFT_AUTO_UPDATE is 0/off/false/no. */
void mg_upgrade_auto(void);

#endif
