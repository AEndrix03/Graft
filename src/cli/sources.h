#ifndef GRAFT_CLI_SOURCES_H
#define GRAFT_CLI_SOURCES_H

#include "mpack.h"

#include <stddef.h>

/* `graft sources <diff|refresh> ...` (issue #4). argv[1] is "sources".
 * Prints JSON on stdout like every other command; returns the exit code
 * (2 usage, 1 daemon unreachable, 3 daemon reported an error). */
int mg_sources_cmd(int argc, char **argv);

/* State of a recorded file source as seen from the working directory's
 * project: "unchanged", "changed", "removed" or "unavailable". NULL when the
 * source belongs to another project (or the cwd has none). */
const char *mg_sources_file_state(const char *project, const char *locator,
                                  const char *fingerprint);

/* The `sources diff` report: re-hashes the files of a sources_list result
 * (the daemon's "result" map) under root and writes the printed envelope
 * { status: 0, result: { project, root, summary, sources } } as mpack into
 * *out (free() it). Returns 0, or 1 on allocation failure. */
int mg_sources_diff_report(mpack_node_t list_result, const char *root, const char *project,
                           int changed_only, char **out, size_t *out_len);

/* The file links of a sources_list result that no longer match the files
 * under root, written as one mpack array value for maintain_scan:
 *   [ { id_hex, locator, state: "changed"|"removed",
 *       recorded_fingerprint|nil, fingerprint|nil }, ... ]
 * Unreadable files are left out. Returns the number of links written. */
size_t mg_sources_write_stale_links(mpack_node_t list_result, const char *root,
                                    mpack_writer_t *w);

#endif
