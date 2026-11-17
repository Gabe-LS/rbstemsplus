/* core_ids_hash (D7): the shells compare it with their generated ids' value at start. */
#include "internal.h"
#include "ids_hash.h"

uint64_t core_ids_hash(void) { return ids_hash_compute(); }
