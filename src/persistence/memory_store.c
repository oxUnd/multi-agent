#include "memory_store.h"
#include "preferences.h"
#include "session.h"
#include "util/buf.h"
#include "util/error.h"
#include <errno.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_STORE_APPEND_INIT_CAP 1024

static int memory_store_append_session_header(morph_buf_t *buf,
					      const struct session *s)
{
	return morph_buf_printf(buf, "session %lld",
				(long long)(s ? s->id : 0));
}

static int memory_store_append_profile(struct db *db, int64_t session_id,
				       morph_buf_t *buf)
{
	const char *sql =
		"SELECT profile_text FROM memory_profiles WHERE session_id=?";
	sqlite3_stmt *stmt = NULL;
	int rc = 0;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *text = (const char *)sqlite3_column_text(stmt, 0);
		if (text && *text)
			rc = morph_buf_printf(buf, "profile\n%s\n", text);
	}
	sqlite3_finalize(stmt);
	return rc;
}

static int memory_store_append_facts(struct db *db, int64_t session_id,
				     morph_buf_t *buf)
{
	const char *sql =
		"SELECT key_name,value_text,category,importance "
		"FROM memory_facts WHERE session_id=? AND is_current=1 "
		"ORDER BY importance DESC, updated_at DESC";
	sqlite3_stmt *stmt = NULL;
	int count = 0;
	int rc = 0;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *key = (const char *)sqlite3_column_text(stmt, 0);
		const char *val = (const char *)sqlite3_column_text(stmt, 1);
		const char *cat = (const char *)sqlite3_column_text(stmt, 2);
		double imp = sqlite3_column_double(stmt, 3);
		if (!key || !val)
			continue;
		if (count++ == 0) {
			rc = morph_buf_puts(buf, "facts\n");
			if (rc != 0)
				break;
		}
		rc = morph_buf_printf(buf, "- [%s|imp=%.2f] %s: %s\n",
				      cat && *cat ? cat : "general",
				      imp, key, val);
		if (rc != 0)
			break;
	}
	sqlite3_finalize(stmt);
	return rc;
}

static int memory_store_append_procedures(struct db *db, int64_t session_id,
					  morph_buf_t *buf)
{
	const char *sql =
		"SELECT rule_text,evidence_count FROM memory_procedures "
		"WHERE session_id=? ORDER BY evidence_count DESC, updated_at DESC";
	sqlite3_stmt *stmt = NULL;
	int count = 0;
	int rc = 0;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *rule = (const char *)sqlite3_column_text(stmt, 0);
		int evidence = sqlite3_column_int(stmt, 1);
		if (!rule)
			continue;
		if (count++ == 0) {
			rc = morph_buf_puts(buf, "procedures\n");
			if (rc != 0)
				break;
		}
		rc = morph_buf_printf(buf, "- %s (evidence=%d)\n",
				      rule, evidence);
		if (rc != 0)
			break;
	}
	sqlite3_finalize(stmt);
	return rc;
}

static int memory_store_append_episodes(struct db *db, int64_t session_id,
					int max_episodes, morph_buf_t *buf)
{
	const char *sql =
		"SELECT summary_text,tools_used,importance,created_at "
		"FROM memory_episodes WHERE session_id=? "
		"ORDER BY created_at DESC";
	sqlite3_stmt *stmt = NULL;
	int count = 0;
	int rc = 0;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *summary = (const char *)sqlite3_column_text(stmt, 0);
		const char *tools = (const char *)sqlite3_column_text(stmt, 1);
		double imp = sqlite3_column_double(stmt, 2);
		long long created = (long long)sqlite3_column_int64(stmt, 3);
		if (!summary)
			continue;
		if (max_episodes > 0 && count >= max_episodes)
			break;
		if (count++ == 0) {
			rc = morph_buf_puts(buf, "episodes\n");
			if (rc != 0)
				break;
		}
		rc = morph_buf_printf(buf, "- [imp=%.2f t=%lld] %s\n",
				      imp, created, summary);
		if (rc == 0 && tools && *tools)
			rc = morph_buf_printf(buf, "  tools: %s\n", tools);
		if (rc != 0)
			break;
	}
	sqlite3_finalize(stmt);
	return rc;
}

static int memory_store_append_changes(struct db *db, int64_t session_id,
				       morph_buf_t *buf)
{
	const char *sql =
		"SELECT old.key_name,old.value_text,cur.value_text,old.updated_at "
		"FROM memory_facts old "
		"LEFT JOIN memory_facts cur ON cur.id=old.superseded_by "
		"WHERE old.session_id=? AND old.is_current=0 "
		"ORDER BY old.updated_at DESC";
	sqlite3_stmt *stmt = NULL;
	int count = 0;
	int rc = 0;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *key = (const char *)sqlite3_column_text(stmt, 0);
		const char *oldv = (const char *)sqlite3_column_text(stmt, 1);
		const char *newv = (const char *)sqlite3_column_text(stmt, 2);
		long long at = (long long)sqlite3_column_int64(stmt, 3);
		if (!key || !oldv)
			continue;
		if (count++ == 0) {
			rc = morph_buf_puts(buf, "changes\n");
			if (rc != 0)
				break;
		}
		rc = morph_buf_printf(buf, "- %s: %s -> %s (t=%lld)\n",
				      key, oldv, newv ? newv : "(unset)", at);
		if (rc != 0)
			break;
	}
	sqlite3_finalize(stmt);
	return rc;
}

static int memory_store_append_type(struct db *db, int64_t session_id,
				    const struct memory_query_options *opts,
				    morph_buf_t *buf)
{
	switch (opts->type) {
	case MEMORY_QUERY_PROFILE:
		return memory_store_append_profile(db, session_id, buf);
	case MEMORY_QUERY_FACTS:
		return memory_store_append_facts(db, session_id, buf);
	case MEMORY_QUERY_PROCEDURES:
		return memory_store_append_procedures(db, session_id, buf);
	case MEMORY_QUERY_EPISODES:
		return memory_store_append_episodes(
			db, session_id, opts->max_episodes, buf);
	case MEMORY_QUERY_CHANGES:
		return memory_store_append_changes(db, session_id, buf);
	case MEMORY_QUERY_ALL:
		break;
	}
	return 0;
}

static int memory_store_session_has_rows(struct db *db, int64_t session_id)
{
	const char *sql =
		"SELECT 1 FROM memory_profiles WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_facts WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_procedures WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_episodes WHERE session_id=? "
		"UNION ALL SELECT 1 FROM memory_scopes WHERE session_id=? "
		"LIMIT 1";
	sqlite3_stmt *stmt = NULL;
	int has = 0;

	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	for (int i = 1; i <= 5; i++)
		sqlite3_bind_int64(stmt, i, session_id);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		has = 1;
	sqlite3_finalize(stmt);
	return has;
}

static int memory_store_session_is_visible(
	const struct memory_query_options *opts, const struct session *s)
{
	if (!opts || !opts->visible_fn)
		return 1;
	if (!s || !s->display_id[0])
		return 0;
	return opts->visible_fn(s->display_id, opts->user_id,
				opts->visible_user_data);
}

static int memory_store_append_one_session(
	struct db *db, const struct session *s,
	const struct memory_query_options *opts,
	char *(*render_session)(struct db *db, int64_t session_id,
				int max_episodes),
	morph_buf_t *buf, int *written)
{
	char *rendered;
	size_t start_len;
	size_t before;
	int rc;

	if (!memory_store_session_is_visible(opts, s) ||
	    !memory_store_session_has_rows(db, s->id))
		return 0;

	start_len = buf->len;
	if (*written > 0) {
		rc = morph_buf_putc(buf, '\n');
		if (rc != 0)
			return rc;
	}
	rc = memory_store_append_session_header(buf, s);
	if (rc == 0 && s->name[0])
		rc = morph_buf_printf(buf, " %s", s->name);
	if (rc == 0)
		rc = morph_buf_putc(buf, '\n');
	if (rc != 0)
		return rc;

	before = buf->len;
	if (opts->type == MEMORY_QUERY_PROFILE || opts->type == MEMORY_QUERY_FACTS ||
	    opts->type == MEMORY_QUERY_CHANGES) {
		char *preferences = preference_render(db, s->id, opts->type == MEMORY_QUERY_CHANGES);

		if (!preferences)
			MORPH_RETURN(MORPH_ERR_DB);
		if (*preferences)
			rc = morph_buf_printf(buf, "Effective preferences\n%s", preferences);
		free(preferences);
		if (rc != 0)
			return rc;
	}
	if (opts->type == MEMORY_QUERY_ALL) {
		rendered = render_session(db, s->id, opts->max_episodes);
		if (!rendered)
			return -ENOMEM;
		rc = morph_buf_puts(buf, rendered);
		rc = rc == 0 ? morph_buf_putc(buf, '\n') : rc;
		free(rendered);
	} else {
		rc = memory_store_append_type(db, s->id, opts, buf);
	}
	if (rc != 0)
		return rc;
	if (buf->len == before) {
		morph_buf_truncate(buf, start_len);
	} else {
		(*written)++;
	}
	return 0;
}

static int memory_store_append_all_sessions(
	struct db *db, const struct memory_query_options *opts,
	char *(*render_session)(struct db *db, int64_t session_id,
				int max_episodes),
	morph_buf_t *buf)
{
	struct session *sessions = NULL;
	int count = 0;
	int written = 0;
	int rc;

	rc = session_list(db, &sessions, &count, 0, NULL);
	if (rc != 0)
		return rc;
	for (int i = 0; i < count; i++) {
		rc = memory_store_append_one_session(
			db, &sessions[i], opts, render_session, buf, &written);
		if (rc != 0)
			break;
	}
	free(sessions);
	if (rc != 0)
		return rc;
	if (written == 0)
		return morph_buf_puts(buf, "No long-term memory stored.");
	return 0;
}

static int memory_store_append_current_session(
	struct db *db, int64_t current_session_id,
	const struct memory_query_options *opts,
	char *(*render_session)(struct db *db, int64_t session_id,
				int max_episodes),
	morph_buf_t *buf)
{
	struct session s;
	int written = 0;
	int rc;

	if (current_session_id <= 0)
		return morph_buf_puts(buf,
				      "No current memory session is available.");
	rc = session_get_by_id(db, current_session_id, &s);
	if (rc != 0)
		return rc;
	rc = memory_store_append_one_session(db, &s, opts, render_session,
					     buf, &written);
	if (rc != 0)
		return rc;
	if (written == 0)
		return morph_buf_puts(buf,
				      "No long-term memory stored for this session.");
	return 0;
}

char *memory_store_query_render(struct db *db, int64_t current_session_id,
				const struct memory_query_options *opts,
				char *(*render_session)(struct db *db,
							int64_t session_id,
							int max_episodes))
{
	struct memory_query_options local_opts;
	morph_buf_t buf;
	int rc;

	if (!db || !db->handle || !render_session)
		return NULL;
	memset(&local_opts, 0, sizeof(local_opts));
	if (opts)
		local_opts = *opts;
	if (morph_buf_init(&buf, MEMORY_STORE_APPEND_INIT_CAP) != 0)
		return NULL;
	if (local_opts.scope_all)
		rc = memory_store_append_all_sessions(
			db, &local_opts, render_session, &buf);
	else
		rc = memory_store_append_current_session(
			db, current_session_id, &local_opts, render_session,
			&buf);
	if (rc != 0) {
		morph_buf_cleanup(&buf);
		return NULL;
	}
	return morph_buf_detach(&buf);
}

static int memory_store_exec_delete(struct db *db, const char *sql,
				    int64_t session_id)
{
	sqlite3_stmt *stmt = NULL;
	int rc;

	if (!db || !db->handle || !sql)
		MORPH_RETURN(-EINVAL);
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, session_id);
	rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	return rc == SQLITE_DONE ? 0 : MORPH_ERR_DB;
}

int memory_store_clear_all(struct db *db, int64_t session_id)
{
	int rc;

	rc = memory_store_clear_facts(db, session_id);
	if (rc != 0)
		return rc;
	rc = memory_store_clear_episodes(db, session_id);
	if (rc != 0)
		return rc;
	return memory_store_clear_procedures(db, session_id);
}

int memory_store_clear_facts(struct db *db, int64_t session_id)
{
	int rc;

	rc = memory_store_exec_delete(
		db, "DELETE FROM memory_profiles WHERE session_id=?",
		session_id);
	if (rc != 0)
		return rc;
	return memory_store_exec_delete(
		db, "DELETE FROM memory_facts WHERE session_id=?",
		session_id);
}

int memory_store_clear_episodes(struct db *db, int64_t session_id)
{
	return memory_store_exec_delete(
		db, "DELETE FROM memory_episodes WHERE session_id=?",
		session_id);
}

int memory_store_clear_procedures(struct db *db, int64_t session_id)
{
	return memory_store_exec_delete(
		db, "DELETE FROM memory_procedures WHERE session_id=?",
		session_id);
}
