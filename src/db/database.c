#include "database.h"
#include "util/log.h"
#include "util/error.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *schema_sql =
	"CREATE TABLE IF NOT EXISTS schema_migrations ("
	"version INTEGER PRIMARY KEY,"
	"name TEXT NOT NULL,"
	"applied_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS sessions ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"display_id TEXT UNIQUE,"
	"name TEXT UNIQUE NOT NULL,"
	"model TEXT,"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL,"
	"token_used INTEGER DEFAULT 0);"

	"CREATE TABLE IF NOT EXISTS messages ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"role TEXT NOT NULL,"
	"content TEXT NOT NULL,"
	"turn_id TEXT,"
	"token_count INTEGER NOT NULL,"
	"compressed INTEGER DEFAULT 0,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS message_attachments ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,"
	"kind TEXT NOT NULL,"
	"path TEXT NOT NULL,"
	"sha256 TEXT);"

	"CREATE TABLE IF NOT EXISTS react_traces ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"round_no INTEGER NOT NULL,"
	"steps_json TEXT NOT NULL,"
	"aborted INTEGER DEFAULT 0,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS exts ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"name TEXT UNIQUE NOT NULL,"
	"version TEXT,"
	"path TEXT NOT NULL,"
	"type TEXT NOT NULL,"
	"permissions INTEGER,"
	"enabled INTEGER DEFAULT 1,"
	"installed_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS outputs ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"kind TEXT NOT NULL,"
	"path TEXT NOT NULL,"
	"prompt TEXT,"
	"model TEXT,"
	"request_prompt TEXT,"
	"provider TEXT,"
	"tool_name TEXT,"
	"idempotency_key TEXT,"
	"tool_call_id TEXT,"
	"turn_id TEXT,"
	"recipe_json TEXT,"
	"mime TEXT,"
	"width INTEGER NOT NULL DEFAULT 0,"
	"height INTEGER NOT NULL DEFAULT 0,"
	"duration_seconds INTEGER NOT NULL DEFAULT 0,"
	"size_bytes INTEGER NOT NULL DEFAULT 0,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_profiles ("
	"session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
	"profile_text TEXT NOT NULL DEFAULT '',"
	"updated_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_facts ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"key_name TEXT NOT NULL,"
	"value_text TEXT NOT NULL,"
	"source_text TEXT,"
	"confidence REAL DEFAULT 1.0,"
	"category TEXT DEFAULT 'general',"
	"importance REAL DEFAULT 0.5,"
	"is_current INTEGER NOT NULL DEFAULT 1,"
	"valid_from INTEGER NOT NULL,"
	"valid_to INTEGER,"
	"superseded_by INTEGER,"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_episodes ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"task_type TEXT,"
	"summary_text TEXT NOT NULL,"
	"outcome_text TEXT,"
	"success INTEGER NOT NULL DEFAULT 1,"
	"entities TEXT,"
	"key_decisions TEXT,"
	"artifacts TEXT,"
	"tools_used TEXT,"
	"importance REAL DEFAULT 0.5,"
	"created_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS memory_procedures ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"rule_text TEXT NOT NULL,"
	"trigger_text TEXT,"
	"evidence_count INTEGER NOT NULL DEFAULT 1,"
	"updated_at INTEGER NOT NULL,"
	"UNIQUE(session_id, rule_text));"

	"CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_messages_session_order "
	"ON messages(session_id, created_at, id);"
	"CREATE INDEX IF NOT EXISTS idx_traces_session ON react_traces(session_id, round_no);"
	"CREATE INDEX IF NOT EXISTS idx_outputs_session ON outputs(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_facts_session_key "
	"ON memory_facts(session_id, key_name, is_current, updated_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_episodes_session "
	"ON memory_episodes(session_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_memory_procedures_session "
	"ON memory_procedures(session_id, updated_at);";

static const char *preference_schema_sql =
	"CREATE TABLE IF NOT EXISTS memory_scopes ("
	"session_id INTEGER PRIMARY KEY REFERENCES sessions(id) ON DELETE CASCADE,"
	"owner TEXT NOT NULL,project TEXT NOT NULL DEFAULT '',"
	"migrated INTEGER NOT NULL DEFAULT 0,generation INTEGER NOT NULL DEFAULT 0);"
	"CREATE TABLE IF NOT EXISTS memory_preferences ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,owner TEXT NOT NULL,"
	"scope TEXT NOT NULL CHECK(scope IN ('personal','project','session')),"
	"target TEXT NOT NULL,key TEXT NOT NULL,value TEXT,source TEXT NOT NULL,"
	"origin TEXT NOT NULL DEFAULT 'explicit',source_order INTEGER NOT NULL DEFAULT 0,"
	"session_id INTEGER NOT NULL,event_token TEXT NOT NULL,"
	"created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
	"UNIQUE(owner,scope,target,key,session_id,event_token));"
	"CREATE INDEX IF NOT EXISTS idx_preferences_resolve "
	"ON memory_preferences(owner,scope,target,key,id DESC);"
	"CREATE TABLE IF NOT EXISTS memory_candidates ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"kind TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,"
	"source TEXT NOT NULL,created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)),"
	"UNIQUE(session_id,kind,key,value,source));"
	"CREATE TABLE IF NOT EXISTS memory_jobs ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"generation INTEGER NOT NULL,payload TEXT NOT NULL,"
	"state TEXT NOT NULL DEFAULT 'queued',attempts INTEGER NOT NULL DEFAULT 0,"
	"worker_pid INTEGER NOT NULL DEFAULT 0,error_code INTEGER NOT NULL DEFAULT 0,"
	"updated_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER)));";

static const char *sub_agent_schema_sql =
	"CREATE TABLE IF NOT EXISTS sub_agent_tasks ("
	"task_id TEXT PRIMARY KEY,"
	"parent_session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,"
	"child_session_id INTEGER REFERENCES sessions(id) ON DELETE CASCADE,"
	"agent_name TEXT NOT NULL,"
	"description TEXT NOT NULL,"
	"mode TEXT NOT NULL,"
	"status INTEGER NOT NULL,"
	"result TEXT,"
	"error_code INTEGER NOT NULL DEFAULT 0,"
	"iterations INTEGER NOT NULL DEFAULT 0,"
	"started_at INTEGER NOT NULL,"
	"ended_at INTEGER NOT NULL DEFAULT 0);"
	"CREATE TABLE IF NOT EXISTS sub_agent_events ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"task_id TEXT NOT NULL REFERENCES sub_agent_tasks(task_id) "
	"ON DELETE CASCADE,"
	"event_json TEXT NOT NULL,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_sub_agent_tasks_parent "
	"ON sub_agent_tasks(parent_session_id,started_at);"
	"CREATE INDEX IF NOT EXISTS idx_sub_agent_events_task "
	"ON sub_agent_events(task_id,id);";

static const char *history_schema_sql =
	"CREATE TABLE IF NOT EXISTS model_history_items ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"sequence_no INTEGER NOT NULL,"
	"turn_id TEXT,"
	"kind TEXT NOT NULL,"
	"role TEXT,"
	"content TEXT,"
	"payload_json TEXT,"
	"tool_call_id TEXT,"
	"provider_call_id TEXT,"
	"tool_name TEXT,"
	"idempotency_key TEXT,"
	"token_count INTEGER NOT NULL DEFAULT 0,"
	"truncated INTEGER NOT NULL DEFAULT 0,"
	"active INTEGER NOT NULL DEFAULT 1,"
	"created_at INTEGER NOT NULL,"
	"UNIQUE(session_id,sequence_no));"
	"CREATE TABLE IF NOT EXISTS history_compactions ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"turn_id TEXT,"
	"cutoff_sequence_no INTEGER NOT NULL,"
	"summary_item_id INTEGER NOT NULL REFERENCES model_history_items(id),"
	"input_tokens INTEGER NOT NULL,"
	"output_tokens INTEGER NOT NULL,"
	"trigger_kind TEXT NOT NULL,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_model_history_active "
	"ON model_history_items(session_id,active,sequence_no);"
	"CREATE INDEX IF NOT EXISTS idx_model_history_turn "
	"ON model_history_items(session_id,turn_id,sequence_no);"
	"CREATE INDEX IF NOT EXISTS idx_model_history_call "
	"ON model_history_items(session_id,tool_call_id);"
	"CREATE TABLE IF NOT EXISTS history_compaction_attempts ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
	"turn_id TEXT,"
	"trigger_kind TEXT NOT NULL,"
	"status TEXT NOT NULL,"
	"input_tokens INTEGER NOT NULL DEFAULT 0,"
	"output_tokens INTEGER NOT NULL DEFAULT 0,"
	"error_code INTEGER NOT NULL DEFAULT 0,"
	"error_text TEXT,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_history_compaction_attempts_session "
	"ON history_compaction_attempts(session_id,created_at,id);";

static const char *permission_schema_sql =
	"CREATE TABLE IF NOT EXISTS permission_grants ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"subject TEXT NOT NULL,"
	"resource_kind TEXT NOT NULL,"
	"resource TEXT NOT NULL,"
	"project_root TEXT NOT NULL,"
	"created_at INTEGER NOT NULL,"
	"UNIQUE(subject,resource_kind,resource,project_root));"
	"CREATE INDEX IF NOT EXISTS idx_permission_grants_project "
	"ON permission_grants(project_root,subject,resource_kind);";

static const char *schema_baseline_sql =
	"INSERT OR IGNORE INTO schema_migrations(version,name,applied_at) "
	"VALUES(1,'baseline',strftime('%s','now'));";

static const char *scheduled_schema_sql =
	"CREATE TABLE IF NOT EXISTS scheduled_tasks ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"source_session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"latest_session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"title TEXT NOT NULL,"
	"kind TEXT NOT NULL,"
	"status TEXT NOT NULL,"
	"trigger_type TEXT NOT NULL,"
	"next_run_at INTEGER NOT NULL,"
	"interval_seconds INTEGER NOT NULL DEFAULT 0,"
	"timeout_at INTEGER NOT NULL DEFAULT 0,"
	"attempts INTEGER NOT NULL DEFAULT 0,"
	"max_attempts INTEGER NOT NULL DEFAULT 0,"
	"action_type TEXT NOT NULL,"
	"payload_json TEXT,"
	"policy_json TEXT,"
	"notify_json TEXT,"
	"last_error TEXT,"
	"created_at INTEGER NOT NULL,"
	"updated_at INTEGER NOT NULL);"

	"CREATE TABLE IF NOT EXISTS notifications ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"task_id INTEGER REFERENCES scheduled_tasks(id) ON DELETE SET NULL,"
	"session_id INTEGER REFERENCES sessions(id) ON DELETE SET NULL,"
	"level TEXT NOT NULL,"
	"title TEXT NOT NULL,"
	"body TEXT NOT NULL,"
	"created_at INTEGER NOT NULL,"
	"read_at INTEGER NOT NULL DEFAULT 0,"
	"delivery_status TEXT NOT NULL);"

	"CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_due "
	"ON scheduled_tasks(status, next_run_at);"
	"CREATE INDEX IF NOT EXISTS idx_notifications_unread "
	"ON notifications(read_at, created_at);";

static const char *credit_schema_sql =
	"CREATE TABLE IF NOT EXISTS credit_events ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"user_id TEXT,"
	"session_id TEXT,"
	"kind TEXT NOT NULL,"
	"provider TEXT,"
	"model TEXT,"
	"input_tokens INTEGER NOT NULL DEFAULT 0,"
	"output_tokens INTEGER NOT NULL DEFAULT 0,"
	"image_units INTEGER NOT NULL DEFAULT 0,"
	"video_seconds INTEGER NOT NULL DEFAULT 0,"
	"estimated_cost REAL NOT NULL DEFAULT 0,"
	"currency TEXT NOT NULL DEFAULT 'USD',"
	"credits INTEGER NOT NULL DEFAULT 0,"
	"metadata_json TEXT,"
	"created_at INTEGER NOT NULL);"
	"CREATE INDEX IF NOT EXISTS idx_credit_events_user_day "
	"ON credit_events(user_id, created_at);"
	"CREATE INDEX IF NOT EXISTS idx_credit_events_session "
	"ON credit_events(session_id, created_at);";

int db_open(struct db *db, const char *path)
{
	if (!db || !path)
		return -EINVAL;
	memset(db, 0, sizeof(*db));
	strncpy(db->path, path, sizeof(db->path) - 1);
	int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
		SQLITE_OPEN_FULLMUTEX;
	int rc = sqlite3_open_v2(path, &db->handle, flags, NULL);
	if (rc != SQLITE_OK) {
		log_err("failed to open db %s: %s", path, sqlite3_errmsg(db->handle));
		sqlite3_close(db->handle);
		db->handle = NULL;
		MORPH_RETURN(MORPH_ERR_DB);
	}
	sqlite3_exec(db->handle, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	sqlite3_exec(db->handle, "PRAGMA foreign_keys=ON;", NULL, NULL, NULL);
	/* Multiple frontends and background workers may write this WAL database. */
	sqlite3_busy_timeout(db->handle, 5000);
	log_info("database opened: %s", path);
	return 0;
}

void db_close(struct db *db)
{
	if (db && db->handle) {
		sqlite3_close(db->handle);
		db->handle = NULL;
		log_info("database closed: %s", db->path);
	}
}

int db_exec(struct db *db, const char *sql)
{
	if (!db || !db->handle || !sql)
		return -EINVAL;
	char *err = NULL;
	int rc = sqlite3_exec(db->handle, sql, NULL, NULL, &err);
	if (rc != SQLITE_OK) {
		log_err("db exec error: %s", err);
		sqlite3_free(err);
		MORPH_RETURN(MORPH_ERR_DB);
	}
	return 0;
}

static int db_migrate_display_id(struct db *db)
{
	sqlite3_stmt *stmt;
	const char *sql = "PRAGMA table_info(sessions)";
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	int has = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *cname = (const char *)sqlite3_column_text(stmt, 1);
		if (cname && strcmp(cname, "display_id") == 0) {
			has = 1;
			break;
		}
	}
	sqlite3_finalize(stmt);
	if (!has)
		db_exec(db, "ALTER TABLE sessions ADD COLUMN display_id TEXT UNIQUE");
	return 0;
}

static int db_table_has_column(struct db *db, const char *table,
			       const char *column)
{
	sqlite3_stmt *stmt = NULL;
	char sql[128];
	int has = 0;

	snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK)
		return 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *cname = (const char *)sqlite3_column_text(stmt, 1);
		if (cname && strcmp(cname, column) == 0) {
			has = 1;
			break;
		}
	}
	sqlite3_finalize(stmt);
	return has;
}

static void db_add_column_if_missing(struct db *db, const char *table,
				     const char *column, const char *type_def)
{
	char sql[256];

	if (db_table_has_column(db, table, column))
		return;
	snprintf(sql, sizeof(sql),
		 "ALTER TABLE %s ADD COLUMN %s %s",
		 table, column, type_def);
	db_exec(db, sql);
}

static int db_migrate_memory_columns(struct db *db)
{
	db_add_column_if_missing(db, "memory_preferences", "origin",
		"TEXT NOT NULL DEFAULT 'explicit'");
	db_add_column_if_missing(db, "memory_preferences", "source_order",
		"INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "memory_scopes", "generation",
		"INTEGER NOT NULL DEFAULT 0");
	if (!db_table_has_column(db, "memory_preferences", "origin") ||
	    !db_table_has_column(db, "memory_preferences", "source_order") ||
	    !db_table_has_column(db, "memory_scopes", "generation"))
		MORPH_RETURN(MORPH_ERR_DB);
	db_add_column_if_missing(db, "memory_episodes", "job_id", "INTEGER");
	if (!db_table_has_column(db, "memory_episodes", "job_id"))
		MORPH_RETURN(MORPH_ERR_DB);
	if (db_exec(db, "CREATE UNIQUE INDEX IF NOT EXISTS idx_memory_episode_job "
		"ON memory_episodes(job_id) WHERE job_id IS NOT NULL") != 0)
		MORPH_RETURN(MORPH_ERR_DB);
	db_add_column_if_missing(db, "memory_facts", "category",
				 "TEXT DEFAULT 'general'");
	db_add_column_if_missing(db, "memory_facts", "importance",
				 "REAL DEFAULT 0.5");
	db_add_column_if_missing(db, "memory_episodes", "key_decisions",
				 "TEXT");
	db_add_column_if_missing(db, "memory_episodes", "artifacts", "TEXT");
	db_add_column_if_missing(db, "memory_episodes", "tools_used", "TEXT");
	db_add_column_if_missing(db, "memory_episodes", "importance",
				 "REAL DEFAULT 0.5");
	return 0;
}

static int db_migrate_message_columns(struct db *db)
{
	db_add_column_if_missing(db, "messages", "turn_id", "TEXT");
	return 0;
}

static int db_migrate_output_columns(struct db *db)
{
	db_add_column_if_missing(db, "outputs", "request_prompt", "TEXT");
	db_add_column_if_missing(db, "outputs", "provider", "TEXT");
	db_add_column_if_missing(db, "outputs", "tool_name", "TEXT");
	db_add_column_if_missing(db, "outputs", "tool_call_id", "TEXT");
	db_add_column_if_missing(db, "outputs", "turn_id", "TEXT");
	db_add_column_if_missing(db, "outputs", "recipe_json", "TEXT");
	db_add_column_if_missing(db, "outputs", "mime", "TEXT");
	db_add_column_if_missing(db, "outputs", "width",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "outputs", "height",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "outputs", "duration_seconds",
				 "INTEGER NOT NULL DEFAULT 0");
	db_add_column_if_missing(db, "outputs", "size_bytes",
				 "INTEGER NOT NULL DEFAULT 0");
	return db_exec(db, "CREATE INDEX IF NOT EXISTS idx_outputs_path "
			      "ON outputs(path, id DESC)");
}

static int db_migrate_scheduled_task_columns(struct db *db)
{
	db_add_column_if_missing(db, "scheduled_tasks", "source_session_id",
				 "INTEGER REFERENCES sessions(id) ON DELETE SET NULL");
	db_add_column_if_missing(db, "scheduled_tasks", "latest_session_id",
				 "INTEGER REFERENCES sessions(id) ON DELETE SET NULL");
	db_add_column_if_missing(db, "notifications", "session_id",
				 "INTEGER REFERENCES sessions(id) ON DELETE SET NULL");
	return 0;
}

int db_init_schema(struct db *db)
{
	if (!db || !db->handle)
		return -EINVAL;
	int rc = db_exec(db, schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, preference_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, sub_agent_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, history_schema_sql);
	if (rc != 0)
		return rc;
	db_add_column_if_missing(db, "model_history_items",
		"idempotency_key", "TEXT");
	if (!db_table_has_column(db, "model_history_items", "idempotency_key"))
		MORPH_RETURN(MORPH_ERR_DB);
	rc = db_exec(db,
		"CREATE UNIQUE INDEX IF NOT EXISTS idx_model_history_idempotency "
		"ON model_history_items(session_id,idempotency_key) "
		"WHERE idempotency_key IS NOT NULL;");
	if (rc != 0)
		return rc;
	rc = db_exec(db, scheduled_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_exec(db, permission_schema_sql);
	if (rc != 0)
		return rc;
	rc = db_migrate_display_id(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_scheduled_task_columns(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_memory_columns(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_message_columns(db);
	if (rc != 0)
		return rc;
	rc = db_migrate_output_columns(db);
	if (rc != 0)
		return rc;
	rc = db_exec(db, credit_schema_sql);
	if (rc != 0)
		return rc;
	return db_exec(db, schema_baseline_sql);
}
