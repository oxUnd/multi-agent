#ifndef MORPH_MEMORY_INTERNAL_H
#define MORPH_MEMORY_INTERNAL_H

#include "db/database.h"

int memory_prepare_session(struct db *db, int64_t session_id);
int memory_generation(struct db *db, int64_t session_id, int64_t *generation);

#endif
