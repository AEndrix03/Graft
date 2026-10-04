#ifndef GRAFT_CLI_MAINTAIN_H
#define GRAFT_CLI_MAINTAIN_H

/* `graft maintain <status|scan|apply-safe|resolve|log> ...` (issue #5).
 * argv[1] is "maintain". Prints JSON on stdout like every other command;
 * returns the exit code (2 usage, 1 daemon unreachable, 3 daemon error). */
int mg_maintain_cmd(int argc, char **argv);

#endif
