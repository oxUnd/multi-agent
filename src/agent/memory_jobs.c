#include "agent/memory.h"
#include "agent/memory_internal.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/log.h"
#include "cJSON.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ----------------------------------------------------------------------
 * Async consolidate worker.
 *
 * Background:
 *   memory_capture_llm_path() makes a blocking HTTP SSE call that can
 *   take 1-3 seconds. When invoked from the CLI on the main thread it
 *   stalls the readline prompt right after the assistant finishes.
 *
 * Design:
 *   A single long-lived worker thread owns its own SQLite connection
 *   (opened against the same db->path) and drains a FIFO queue of jobs
 *   posted by memory_consolidate_turn_async(). The caller deep-copies
 *   user_input / assistant_output / react_step list into the job, so
 *   the foreground may free or reuse those buffers immediately.
 *
 *   We pin to one worker (not a pool) on purpose: SQLite + LLM API
 *   serialisation is fine, and one writer keeps the WAL contention low.
 *
 * Lifecycle:
 *   The worker is started lazily on the first async submission and is
 *   torn down by memory_async_shutdown() from the owning runtime before
 *   db_close, so all in-flight jobs flush first.
 * ---------------------------------------------------------------------- */

struct memory_job {
	char *db_path;
	int64_t session_id;
	char *user_input;
	char *assistant_output;
	struct react_step *steps;
	int success;
	struct memory_options opts;
	struct memory_job *next;
};

static pthread_mutex_t g_async_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_async_cv = PTHREAD_COND_INITIALIZER;
static struct memory_job *g_async_head;
static struct memory_job *g_async_tail;
static pthread_t g_async_thread;
static int g_async_running;
static int g_async_stop;
static int g_async_active;

static void memory_steps_free(struct react_step *s)
{
	while (s) {
		struct react_step *n = s->next;
		free(s->content);
		free(s->tool_name);
		free(s->tool_args);
		free(s->tool_call_id);
		free(s);
		s = n;
	}
}

static struct react_step *memory_steps_dup(const struct react_step *src)
{
	struct react_step *head = NULL;
	struct react_step *tail = NULL;

	for (const struct react_step *cur = src; cur; cur = cur->next) {
		struct react_step *node = calloc(1, sizeof(*node));
		if (!node) {
			memory_steps_free(head);
			return NULL;
		}
		node->type = cur->type;
		node->error_code = cur->error_code;
		node->artifacts = cur->artifacts;
		if (cur->content) {
			node->content = strdup(cur->content);
			if (!node->content)
				goto oom;
		}
		if (cur->tool_name) {
			node->tool_name = strdup(cur->tool_name);
			if (!node->tool_name)
				goto oom;
		}
		if (cur->tool_args) {
			node->tool_args = strdup(cur->tool_args);
			if (!node->tool_args)
				goto oom;
		}
		if (cur->tool_call_id) {
			node->tool_call_id = strdup(cur->tool_call_id);
			if (!node->tool_call_id)
				goto oom;
		}
		if (!head)
			head = node;
		else
			tail->next = node;
		tail = node;
		continue;
oom:
		free(node->content);
		free(node->tool_name);
		free(node->tool_args);
		free(node->tool_call_id);
		free(node);
		memory_steps_free(head);
		return NULL;
	}
	return head;
}

static void memory_job_free(struct memory_job *j)
{
	if (!j)
		return;
	free(j->db_path);
	free(j->user_input);
	free(j->assistant_output);
	memory_steps_free(j->steps);
	free(j);
}

static int memory_job_record(struct db *db, struct memory_job *job)
{
	cJSON *payload = cJSON_CreateObject();
	cJSON *tools = payload ? cJSON_AddArrayToObject(payload, "tools") : NULL;
	char *json;
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!tools || !cJSON_AddStringToObject(payload, "input", job->user_input) ||
	    !cJSON_AddStringToObject(payload, "answer",
		job->assistant_output ? job->assistant_output : "") ||
	    !cJSON_AddBoolToObject(payload, "success", job->success) ||
	    !cJSON_AddBoolToObject(payload, "llm", job->opts.llm_extract_enabled) ||
	    !cJSON_AddBoolToObject(payload, "episodes", job->opts.cold_path_enabled)) {
		cJSON_Delete(payload);
		MORPH_RETURN(-ENOMEM);
	}
	for (struct react_step *step = job->steps; step; step = step->next) {
		cJSON *name;

		if (step->type != REACT_STEP_ACTION || !step->tool_name)
			continue;
		name = cJSON_CreateString(step->tool_name);
		if (!name || !cJSON_AddItemToArray(tools, name)) {
			cJSON_Delete(name);
			cJSON_Delete(payload);
			MORPH_RETURN(-ENOMEM);
		}
	}
	json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	rc = sqlite3_prepare_v2(db->handle,
		"INSERT INTO memory_jobs(session_id,generation,payload) VALUES(?,?,?)",
		-1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, job->session_id);
		sqlite3_bind_int64(stmt, 2, job->opts.generation);
		sqlite3_bind_text(stmt, 3, json, -1, SQLITE_TRANSIENT);
		rc = sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	free(json);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	job->opts.background_job_id = sqlite3_last_insert_rowid(db->handle);
	return 0;
}

static int memory_job_claim(struct db *db, int64_t id)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle,
		"UPDATE memory_jobs SET state='running',attempts=attempts+1,"
		"worker_pid=?,updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE id=? "
		"AND state='queued'",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, (int64_t)getpid());
	sqlite3_bind_int64(stmt, 2, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return sqlite3_changes(db->handle) ? 1 : 0;
}

static int memory_job_complete(struct db *db, int64_t id, int error)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->handle,
		"UPDATE memory_jobs SET state=?,error_code=?,updated_at=CAST(strftime('%s',"
		"'now') AS INTEGER) WHERE id=? AND state='running'",
		-1, &stmt, NULL);

	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_text(stmt, 1, error == -ESTALE ? "cancelled" :
		error ? "failed" : "completed", -1, SQLITE_STATIC);
	sqlite3_bind_int(stmt, 2, error);
	sqlite3_bind_int64(stmt, 3, id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

static void *memory_async_worker(void *arg)
{
	(void)arg;
	for (;;) {
		struct memory_job *job = NULL;

		pthread_mutex_lock(&g_async_lock);
		while (!g_async_stop && !g_async_head)
			pthread_cond_wait(&g_async_cv, &g_async_lock);
		if (g_async_stop && !g_async_head) {
			pthread_mutex_unlock(&g_async_lock);
			break;
		}
		job = g_async_head;
		g_async_head = job->next;
		if (!g_async_head)
			g_async_tail = NULL;
		g_async_active = 1;
		pthread_mutex_unlock(&g_async_lock);

		if (!job)
			continue;

		/* Worker owns its own SQLite handle so it never collides
		 * with the foreground db on the main thread. WAL keeps
		 * concurrent reads/writes consistent. */
		struct db worker_db = {0};
		int rc = db_open(&worker_db, job->db_path);
		if (rc == 0) {
			int claimed = memory_job_claim(&worker_db, job->opts.background_job_id);

			if (claimed > 0) {
				rc = memory_consolidate_turn(&worker_db, job->session_id,
						job->user_input,
						job->assistant_output,
						job->steps,
						job->success,
						&job->opts);
				(void)memory_job_complete(&worker_db,
					job->opts.background_job_id, rc);
			} else if (claimed < 0) {
				rc = claimed;
			}
			if (rc != 0 && rc != -ESTALE)
				log_err("memory: background extraction failed: %s",
					morph_strerror(rc));
			db_close(&worker_db);
		} else {
			log_dbg("memory: async worker failed to open db: %d",
				rc);
		}
		memory_job_free(job);

		pthread_mutex_lock(&g_async_lock);
		g_async_active = 0;
		pthread_mutex_unlock(&g_async_lock);
	}
	return NULL;
}

static int memory_async_ensure_worker(void)
{
	int rc = 0;

	pthread_mutex_lock(&g_async_lock);
	if (!g_async_running) {
		g_async_stop = 0;
		int err = pthread_create(&g_async_thread, NULL,
					 memory_async_worker, NULL);
		if (err != 0)
			rc = -err;
		else
			g_async_running = 1;
	}
	pthread_mutex_unlock(&g_async_lock);
	return rc;
}

int memory_consolidate_turn_async(struct db *db, int64_t session_id,
				  const char *user_input,
				  const char *assistant_output,
				  const struct react_step *steps,
				  int success,
				  const struct memory_options *opts)
{
	struct memory_job *job;

	if (!db || !db->path[0] || !opts || !opts->enabled || !user_input)
		return 0;

	if (!strcmp(db->path, ":memory:"))
		MORPH_RETURN(-ENOTSUP);

	job = calloc(1, sizeof(*job));
	if (!job)
		return -ENOMEM;
	job->db_path = strdup(db->path);
	job->user_input = strdup(user_input);
	if (assistant_output)
		job->assistant_output = strdup(assistant_output);
	job->steps = memory_steps_dup(steps);
	job->session_id = session_id;
	job->success = success;
	job->opts = *opts;
	job->opts.hot_path_enabled = 0;
	if (memory_prepare_session(db, session_id) != 0 ||
	    memory_generation(db, session_id, &job->opts.generation) != 0) {
		memory_job_free(job);
		MORPH_RETURN(MORPH_ERR_DB);
	}
	job->opts.generation_bound = 1;
	if (!job->db_path || !job->user_input ||
	    (assistant_output && !job->assistant_output) ||
	    (steps && !job->steps)) {
		memory_job_free(job);
		return -ENOMEM;
	}

	{
		int rc = memory_job_record(db, job);

		if (rc != 0) {
			memory_job_free(job);
			return rc;
		}
	}
	if (memory_async_ensure_worker() != 0) {
		/* Durable queued work will be recovered on the next input. */
		memory_job_free(job);
		return 0;
	}

	pthread_mutex_lock(&g_async_lock);
	if (g_async_tail)
		g_async_tail->next = job;
	else
		g_async_head = job;
	g_async_tail = job;
	pthread_cond_signal(&g_async_cv);
	pthread_mutex_unlock(&g_async_lock);
	return 0;
}

void memory_async_shutdown(void)
{
	pthread_t th;
	int running;

	pthread_mutex_lock(&g_async_lock);
	running = g_async_running;
	if (running) {
		g_async_stop = 1;
		th = g_async_thread;
		pthread_cond_broadcast(&g_async_cv);
	}
	pthread_mutex_unlock(&g_async_lock);

	if (running)
		pthread_join(th, NULL);

	pthread_mutex_lock(&g_async_lock);
	g_async_running = 0;
	g_async_active = 0;
	pthread_mutex_unlock(&g_async_lock);
}

int memory_async_pending(void)
{
	int pending;

	pthread_mutex_lock(&g_async_lock);
	pending = g_async_running && (g_async_active || g_async_head != NULL);
	pthread_mutex_unlock(&g_async_lock);
	return pending;
}

static struct memory_job *memory_job_restore(struct db *db, sqlite3_stmt *row)
{
	const char *json = (const char *)sqlite3_column_text(row, 3);
	cJSON *payload = json ? cJSON_Parse(json) : NULL;
	cJSON *input = cJSON_GetObjectItemCaseSensitive(payload, "input");
	cJSON *answer = cJSON_GetObjectItemCaseSensitive(payload, "answer");
	cJSON *name;
	struct memory_job *job;

	if (!cJSON_IsString(input) || !cJSON_IsString(answer)) {
		cJSON_Delete(payload);
		return NULL;
	}
	job = calloc(1, sizeof(*job));
	if (!job) {
		cJSON_Delete(payload);
		return NULL;
	}
	job->db_path = strdup(db->path);
	job->user_input = strdup(input->valuestring);
	job->assistant_output = strdup(answer->valuestring);
	job->session_id = sqlite3_column_int64(row, 1);
	job->success = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "success"));
	job->opts.enabled = 1;
	job->opts.cold_path_enabled = cJSON_IsTrue(
		cJSON_GetObjectItemCaseSensitive(payload, "episodes"));
	job->opts.llm_extract_enabled = cJSON_IsTrue(
		cJSON_GetObjectItemCaseSensitive(payload, "llm"));
	job->opts.generation_bound = 1;
	job->opts.generation = sqlite3_column_int64(row, 2);
	job->opts.background_job_id = sqlite3_column_int64(row, 0);
	if (!job->db_path || !job->user_input || !job->assistant_output) {
		cJSON_Delete(payload);
		memory_job_free(job);
		return NULL;
	}
	cJSON_ArrayForEach(name, cJSON_GetObjectItemCaseSensitive(payload, "tools")) {
		struct react_step *step;

		if (!cJSON_IsString(name))
			continue;
		step = calloc(1, sizeof(*step));
		if (!step) {
			cJSON_Delete(payload);
			memory_job_free(job);
			return NULL;
		}
		step->type = REACT_STEP_ACTION;
		step->tool_name = strdup(name->valuestring);
		step->next = job->steps;
		job->steps = step;
		if (!step->tool_name) {
			cJSON_Delete(payload);
			memory_job_free(job);
			return NULL;
		}
	}
	cJSON_Delete(payload);
	return job;
}

int memory_async_resume(struct db *db)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	/* A private in-memory connection cannot be reopened by the worker. */
	if (!db || !db->handle || !strcmp(db->path, ":memory:"))
		return 0;
	rc = sqlite3_prepare_v2(db->handle,
		"SELECT id,session_id,generation,payload,state,worker_pid FROM memory_jobs "
		"WHERE state IN ('queued','running') OR "
		"(state='failed' AND attempts<3 AND updated_at<CAST(strftime('%s','now') "
		"AS INTEGER)-30) ORDER BY id",
		-1, &stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		const char *state = (const char *)sqlite3_column_text(stmt, 4);
		int64_t id = sqlite3_column_int64(stmt, 0);
		struct memory_job *job;
		int result;

		if (!strcmp(state, "running")) {
			pid_t pid = (pid_t)sqlite3_column_int64(stmt, 5);

			if (pid > 0 && (kill(pid, 0) == 0 || errno != ESRCH))
				continue;
		}
		if (strcmp(state, "queued")) {
			sqlite3_stmt *reset = NULL;

			result = sqlite3_prepare_v2(db->handle,
				"UPDATE memory_jobs SET state='queued' WHERE id=? AND state=?",
				-1, &reset, NULL);
			if (result == SQLITE_OK) {
				sqlite3_bind_int64(reset, 1, id);
				sqlite3_bind_text(reset, 2, state, -1, SQLITE_TRANSIENT);
				result = sqlite3_step(reset);
			}
			sqlite3_finalize(reset);
			if (result != SQLITE_DONE) {
				sqlite3_finalize(stmt);
				MORPH_RETURN(MORPH_ERR_DB);
			}
		}
		job = memory_job_restore(db, stmt);
		if (!job) {
			sqlite3_finalize(stmt);
			MORPH_RETURN(-ENOMEM);
		}
		result = memory_async_ensure_worker();
		if (result != 0) {
			memory_job_free(job);
			sqlite3_finalize(stmt);
			return result;
		}
		pthread_mutex_lock(&g_async_lock);
		{
			int exists = 0;

			for (struct memory_job *queued = g_async_head; queued;
			     queued = queued->next) {
				if (queued->opts.background_job_id == id &&
				    !strcmp(queued->db_path, db->path)) {
					exists = 1;
					break;
				}
			}
			if (exists) {
				memory_job_free(job);
			} else {
				if (g_async_tail)
					g_async_tail->next = job;
				else
					g_async_head = job;
				g_async_tail = job;
				pthread_cond_signal(&g_async_cv);
			}
		}
		pthread_mutex_unlock(&g_async_lock);
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE)
		MORPH_RETURN(MORPH_ERR_DB);
	return 0;
}

char *memory_background_render(struct db *db, int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	morph_buf_t buf;
	int rc;

	if (!db || !db->handle || morph_buf_init(&buf, 512) != 0)
		return NULL;
	rc = sqlite3_prepare_v2(db->handle,
		"SELECT id,state,attempts,error_code FROM memory_jobs WHERE session_id=? "
		"ORDER BY id DESC LIMIT 20", -1, &stmt, NULL);
	if (rc == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, session_id);
		while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
			int error = sqlite3_column_int(stmt, 3);

			if (morph_buf_printf(&buf, "job #%lld: %s (attempts=%d; %s)\n",
				(long long)sqlite3_column_int64(stmt, 0),
				(const char *)sqlite3_column_text(stmt, 1),
				sqlite3_column_int(stmt, 2),
				error ? morph_strerror(error) : "ok") != 0) {
				rc = SQLITE_NOMEM;
				break;
			}
		}
	}
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}
