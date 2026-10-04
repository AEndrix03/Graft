#ifndef MG_STORAGE_INTERNAL_H
#define MG_STORAGE_INTERNAL_H

#include "graft/storage.h"

#include <sqlite3.h>

/* Raw connection for the other storage translation units. Callers hold the
 * storage mutex (mg_storage_lock) for as long as they use it. */
sqlite3 *mg_storage_sqlite(mg_storage_t *s);

#endif
