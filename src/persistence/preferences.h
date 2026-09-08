#ifndef MORPH_PREFERENCES_H
#define MORPH_PREFERENCES_H

#include "db/database.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind once: a session cannot be reassigned to another user's memory. */
int preference_bind(struct db *db, int64_t session_id,
		    const char *owner, const char *project);
const char *preference_key(const char *key);
int preference_set(struct db *db, int64_t session_id, const char *scope,
		   const char *key, const char *value, const char *source,
		   const char *event_token);
/* Resolved values, or all revisions (including deletions) for inspection. */
char *preference_render(struct db *db, int64_t session_id, int history);
int preference_clear_session(struct db *db, int64_t session_id);
int preference_candidate(struct db *db, int64_t session_id,
			 const char *kind, const char *key, const char *value,
			 const char *source);

#ifdef __cplusplus
}
#endif
#endif
