#ifndef MEMORY_H
#define MEMORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "db/database.h"
#include "agent/react.h"
#include "persistence/memory_store.h"
#include "persistence/preferences.h"

struct model;

struct memory_options {
	int enabled;
	int hot_path_enabled;
	int cold_path_enabled;
	int llm_extract_enabled;
	int max_facts;
	int max_episodes;
	int max_procedures;
	int max_context_chars;
	/* Background extraction is fenced by the original session generation. */
	int generation_bound;
	int64_t generation;
	int64_t background_job_id;
};

enum memory_clear_scope {
	MEMORY_CLEAR_ALL = 0,
	MEMORY_CLEAR_FACTS,
	MEMORY_CLEAR_EPISODES,
	MEMORY_CLEAR_PROCEDURES,
};

/* Inject the LLM model used for structured extraction / consolidation.
 * Pass NULL to disable LLM-based memory and fall back to anchor heuristics
 * only. Safe to call multiple times. */
void memory_set_llm(struct model *llm);

/* Accept explicit preferences before model execution, independently of outcome.
 * event_token identifies the original input, including steering messages. */
const char *memory_preference_request_scope(const char *input);

int memory_accept_input(struct db *db, int64_t session_id,
			const char *input, const char *event_token,
			const struct memory_options *opts);

int memory_build_context_checked(struct db *db, int64_t session_id,
				 const char *query, const struct memory_options *opts,
				 char **out);
char *memory_build_context(struct db *db, int64_t session_id,
			   const char *query,
			   const struct memory_options *opts);

char *memory_render_session(struct db *db, int64_t session_id,
			    int max_episodes);

char *memory_query_render(struct db *db, int64_t current_session_id,
			  const struct memory_query_options *opts);

int memory_consolidate_turn(struct db *db, int64_t session_id,
			    const char *user_input,
			    const char *assistant_output,
			    const struct react_step *steps,
			    int success,
			    const struct memory_options *opts);

/* Asynchronous variant: deep-copies all inputs and posts a job to a
 * background worker thread that owns its own SQLite connection. The
 * caller may free user_input/assistant_output/steps and continue to
 * interact with the foreground db immediately after this returns.
 *
 * Returns 0 on enqueue, negative errno on alloc / thread creation
 * failure (caller may then fall back to memory_consolidate_turn). */
int memory_consolidate_turn_async(struct db *db, int64_t session_id,
				  const char *user_input,
				  const char *assistant_output,
				  const struct react_step *steps,
				  int success,
				  const struct memory_options *opts);

/* Drain the async queue and join the worker thread. CLI must call this
 * before db_close() so any in-flight job finishes against a still-open
 * database file. Safe to call when the worker was never started. */
void memory_async_shutdown(void);
/* Recover queued/crashed advisory jobs; preferences are already committed. */
int memory_async_resume(struct db *db);
char *memory_background_render(struct db *db, int64_t session_id);

/* Return non-zero when async memory consolidation has queued or in-flight
 * work that shutdown may need to wait for. */
int memory_async_pending(void);

int memory_clear(struct db *db, int64_t session_id,
		 enum memory_clear_scope scope);

#ifdef __cplusplus
}
#endif

#endif
