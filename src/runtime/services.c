#include "runtime/services.h"

#include "runtime/runtime_internal.h"
#include "agent/compress.h"
#include "agent/history.h"
#include "agent/turn.h"
#include "util/arena.h"
#include "util/id.h"
#include "runtime/scheduler.h"
#include "cJSON.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *runtime_strdup_nullable(const char *value)
{
	return value ? strdup(value) : NULL;
}

void runtime_turn_status_cleanup(struct runtime_turn_status *status)
{
	if (!status)
		return;
	for (int i = 0; i < status->step_count; i++) {
		free(status->steps[i].content);
		free(status->steps[i].tool_name);
		free(status->steps[i].tool_args);
	}
	free(status->steps);
	free(status->outcome_reason);
	free(status->error_detail);
	free(status->final_answer);
	memset(status, 0, sizeof(*status));
}

int runtime_turn_status_get(struct runtime *runtime,
			    struct runtime_turn_status *out)
{
	struct react_context *react;
	struct react_step *step;
	int index = 0;

	if (!runtime || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	pthread_mutex_lock(&runtime->context.execution_lock);
	react = runtime->context.react;
	if (!react) {
		pthread_mutex_unlock(&runtime->context.execution_lock);
		return -ENOENT;
	}
	out->state = react->state;
	out->outcome = react->outcome;
	out->last_error_code = react->last_error_code;
	out->outcome_reason = runtime_strdup_nullable(react->outcome_reason[0]
		? react->outcome_reason : NULL);
	out->error_detail = runtime_strdup_nullable(react->last_error_detail);
	out->final_answer = runtime_strdup_nullable(react->final_answer);
	out->step_count = react->step_count;
	if (out->step_count > 0) {
		out->steps = calloc((size_t)out->step_count, sizeof(*out->steps));
		if (!out->steps)
			goto nomem;
	}
	for (step = react->steps; step && index < out->step_count;
	     step = step->next, index++) {
		out->steps[index].type = step->type;
		out->steps[index].content = runtime_strdup_nullable(step->content);
		out->steps[index].tool_name = runtime_strdup_nullable(step->tool_name);
		out->steps[index].tool_args = runtime_strdup_nullable(step->tool_args);
	}
	out->step_count = index;
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return 0;
nomem:
	pthread_mutex_unlock(&runtime->context.execution_lock);
	runtime_turn_status_cleanup(out);
	return -ENOMEM;
}

char *runtime_turn_notification_body(struct runtime *runtime)
{
	char *out;
	if (!runtime)
		return NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	out = runtime_react_notification_body(runtime->context.react);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return out;
}

char *runtime_turn_error_message(struct runtime *runtime, int rc)
{
	char *out;
	if (!runtime)
		return NULL;
	pthread_mutex_lock(&runtime->context.execution_lock);
	out = runtime_react_error_message(runtime->context.react, rc);
	pthread_mutex_unlock(&runtime->context.execution_lock);
	return out;
}

char *runtime_turn_error_json(struct runtime *runtime)
{
	struct runtime_turn_status status;
	cJSON *root;
	char *out;
	int rc;

	if (runtime_turn_status_get(runtime, &status) != 0)
		return NULL;
	root = cJSON_CreateObject();
	if (!root) {
		runtime_turn_status_cleanup(&status);
		return NULL;
	}
	rc = status.last_error_code < 0 ? status.last_error_code : 0;
	cJSON_AddNumberToObject(root, "rc", rc);
	cJSON_AddStringToObject(root, "outcome",
		react_outcome_name(status.outcome));
	if (status.outcome_reason)
		cJSON_AddStringToObject(root, "reason", status.outcome_reason);
	if (status.error_detail)
		cJSON_AddStringToObject(root, "detail", status.error_detail);
	if (rc < 0) {
		cJSON_AddNumberToObject(root, "error_code", rc);
		cJSON_AddStringToObject(root, "error", morph_strerror(rc));
	}
	if (status.final_answer)
		cJSON_AddStringToObject(root, "text", status.final_answer);
	out = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	runtime_turn_status_cleanup(&status);
	return out;
}

int runtime_session_list_query(struct runtime *runtime, struct session **out,
			       int *count, int limit, const char *filter)
{
	if (!runtime || !out || !count)
		return -EINVAL;
	return session_list(&runtime->context.database, out, count, limit, filter);
}

int runtime_session_count_all(struct runtime *runtime)
{
	return runtime ? session_count(&runtime->context.database) : 0;
}

int runtime_sub_agent_list(struct runtime *runtime,
			   int64_t parent_session_id,
			   struct sub_agent_task_info **out, int *count)
{
	if (!runtime || !runtime->context.sub_agents)
		return -ENOENT;
	return sub_agent_runtime_list_tasks(runtime->context.sub_agents,
					    parent_session_id, out, count);
}

void runtime_sub_agent_list_free(struct sub_agent_task_info *tasks,
				 int count)
{
	sub_agent_runtime_free_task_list(tasks, count);
}

int runtime_sub_agent_select(struct runtime *runtime, const char *task_id)
{
	if (!runtime || !runtime->context.sub_agents)
		return -ENOENT;
	return sub_agent_runtime_select_task(runtime->context.sub_agents,
					     task_id);
}

int runtime_sub_agent_events(struct runtime *runtime, const char *task_id,
			     char ***events, int *count)
{
	if (!runtime || !runtime->context.sub_agents)
		return -ENOENT;
	return sub_agent_runtime_task_events(runtime->context.sub_agents,
					     task_id, events, count);
}

void runtime_sub_agent_events_free(char **events, int count)
{
	sub_agent_runtime_free_events(events, count);
}

int runtime_session_find_ref(struct runtime *runtime, const char *ref,
			     struct session *out)
{
	char *end;
	long long id;
	int rc;
	if (!runtime || !ref || !out)
		return -EINVAL;
	rc = session_get_by_name(&runtime->context.database, ref, out);
	if (rc != 0)
		rc = session_get_by_display_id(&runtime->context.database, ref, out);
	if (rc == 0)
		return 0;
	errno = 0;
	id = strtoll(ref, &end, 10);
	if (*end != '\0' || errno != 0 || id <= 0)
		return -ENOENT;
	return session_get_by_id(&runtime->context.database, (int64_t)id, out);
}

int runtime_session_select_existing(struct runtime *runtime, int64_t id,
				    struct session *out)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct session session;
	int rc;
	if (!ctx || id <= 0)
		return -EINVAL;
	pthread_mutex_lock(&ctx->execution_lock);
	rc = session_get_by_id(&ctx->database, id, &session);
	if (rc == 0) {
		ctx->current_session = session;
		runtime_context_select_plan_session(ctx, session.id);
		(void)runtime_context_update_tool_runtime_context(ctx, session.id);
		runtime_session_load_history(&ctx->engine, session.id);
		if (out)
			*out = session;
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	return rc;
}

int runtime_session_set_model(struct runtime *runtime, const char *model)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	int rc;

	if (!ctx || !model || !model[0])
		return -EINVAL;
	if (ctx->current_session.id > 0) {
		rc = session_update_model(&ctx->database,
					  ctx->current_session.id, model);
		if (rc != 0)
			return rc;
	}
	strncpy(ctx->current_session.model, model,
		sizeof(ctx->current_session.model) - 1);
	ctx->current_session.model[sizeof(ctx->current_session.model) - 1] = '\0';
	strncpy(ctx->config.models.text.model, model,
		sizeof(ctx->config.models.text.model) - 1);
	ctx->config.models.text.model[
		sizeof(ctx->config.models.text.model) - 1] = '\0';
	if (ctx->llm) {
		strncpy(ctx->llm->model_id, model, sizeof(ctx->llm->model_id) - 1);
		ctx->llm->model_id[sizeof(ctx->llm->model_id) - 1] = '\0';
	}
	return 0;
}

int runtime_turn_prepare_tools(struct runtime *runtime, int64_t now)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	if (!ctx)
		return -EINVAL;
	(void)runtime_context_select_plan_session(ctx, ctx->current_session.id);
	(void)scheduled_tasks_tool_set_time_anchor(&ctx->tools, now);
	return scheduled_tasks_tool_set_source_session(&ctx->tools,
						       ctx->current_session.id);
}

int runtime_session_context_stats(struct runtime *runtime, int *messages,
			      int *tokens, int *limit)
{
	struct message *list;
	int count = 0;
	int total = 0;
	if (!runtime)
		return -EINVAL;
	list = message_list(&runtime->context.database,
			    runtime->context.current_session.id, &count);
	for (struct message *item = list; item; item = item->next)
		total += item->token_count;
	message_free_list(list);
	if (messages)
		*messages = count;
	if (tokens)
		*tokens = total;
	if (limit)
		*limit = runtime->context.tokenizer
			? runtime->context.tokenizer->context_limit : 0;
	return 0;
}

int runtime_session_model_context_stats(struct runtime *runtime,
	int *active_items, int *tokens, int *tool_tokens,
	int *compactions, int *limit)
{
	struct model_history_item *items;
	int count = 0;
	int total = 0;
	int tools = 0;
	int compact_count = 0;

	if (!runtime)
		return -EINVAL;
	items = model_history_list(&runtime->context.database,
		runtime->context.current_session.id, 1, &count);
	for (struct model_history_item *item = items; item; item = item->next) {
		total += item->token_count;
		if (strcmp(item->kind, "tool_result") == 0)
			tools += item->token_count;
	}
	model_history_free_list(items);
	compact_count = model_history_compaction_count(
		&runtime->context.database, runtime->context.current_session.id);
	if (compact_count < 0)
		return compact_count;
	if (active_items)
		*active_items = count;
	if (tokens)
		*tokens = total;
	if (tool_tokens)
		*tool_tokens = tools;
	if (compactions)
		*compactions = compact_count;
	if (limit)
		*limit = runtime->context.tokenizer ?
			runtime->context.tokenizer->context_limit : 0;
	return 0;
}

int runtime_session_compaction_status(struct runtime *runtime,
	struct runtime_history_compaction_status *status)
{
	const char *sql =
		"SELECT trigger_kind,status,input_tokens,output_tokens,error_code "
		"FROM history_compaction_attempts WHERE session_id=? "
		"ORDER BY created_at DESC,id DESC LIMIT 1";
	sqlite3_stmt *stmt;
	int rc;

	if (!runtime || !status)
		MORPH_RETURN(-EINVAL);
	memset(status, 0, sizeof(*status));
	rc = sqlite3_prepare_v2(runtime->context.database.handle, sql, -1,
		&stmt, NULL);
	if (rc != SQLITE_OK)
		MORPH_RETURN(MORPH_ERR_DB);
	sqlite3_bind_int64(stmt, 1, runtime->context.current_session.id);
	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		const char *trigger =
			(const char *)sqlite3_column_text(stmt, 0);
		const char *state =
			(const char *)sqlite3_column_text(stmt, 1);

		snprintf(status->trigger_kind, sizeof(status->trigger_kind),
			 "%s", trigger ? trigger : "");
		snprintf(status->status, sizeof(status->status), "%s",
			 state ? state : "");
		status->input_tokens = sqlite3_column_int(stmt, 2);
		status->output_tokens = sqlite3_column_int(stmt, 3);
		status->error_code = sqlite3_column_int(stmt, 4);
		rc = 0;
	} else if (rc == SQLITE_DONE) {
		rc = -ENOENT;
	} else {
		rc = MORPH_ERR_DB;
	}
	sqlite3_finalize(stmt);
	return rc;
}

char *runtime_trace_load_latest_current(struct runtime *runtime,
					int *round_no, int *aborted)
{
	if (!runtime)
		return NULL;
	return trace_load_latest(&runtime->context.database,
		runtime->context.current_session.id, round_no, aborted);
}

int runtime_session_compress(struct runtime *runtime,
				     int *trace_removed,
				     int *window_removed, int *kept)
{
	struct runtime_context *ctx = runtime ? &runtime->context : NULL;
	struct react_context *react;
	char previous_turn_id[sizeof(react->turn_id)];
	int previous_turn_id_user_set;
	int before;
	int after;
	int rc;

	if (!ctx)
		return -EINVAL;
	react = ctx->react;
	if (!react)
		return -EINVAL;
	rc = agent_session_load_history(&(struct agent_session_runtime) {
		.db = &ctx->database,
		.session_id = ctx->current_session.id,
		.react = react,
	});
	if (rc != 0)
		return rc;
	before = model_history_count(&ctx->database,
		ctx->current_session.id, 1);
	if (before < 0)
		return before;
	react->history_db = &ctx->database;
	react->history_session_id = ctx->current_session.id;
	react->history_enabled = 1;
	memcpy(previous_turn_id, react->turn_id, sizeof(previous_turn_id));
	previous_turn_id_user_set = react->turn_id_user_set;
	rc = morph_random_id("compact_", react->turn_id,
			    sizeof(react->turn_id));
	if (rc != 0) {
		memcpy(react->turn_id, previous_turn_id,
		       sizeof(react->turn_id));
		react->turn_id_user_set = previous_turn_id_user_set;
		react->history_db = NULL;
		react->history_session_id = 0;
		react->history_enabled = 0;
		return rc;
	}
	rc = agent_history_compact(react, 1);
	memcpy(react->turn_id, previous_turn_id, sizeof(react->turn_id));
	react->turn_id_user_set = previous_turn_id_user_set;
	react->history_db = NULL;
	react->history_session_id = 0;
	react->history_enabled = 0;
	if (rc < 0)
		return rc;
	after = model_history_count(&ctx->database,
		ctx->current_session.id, 1);
	if (after < 0)
		return after;
	if (trace_removed)
		*trace_removed = 0;
	if (window_removed)
		*window_removed = before - after;
	if (kept)
		*kept = after;
	return 0;
}

int runtime_tool_count(const struct runtime *runtime)
{
	return runtime ? runtime->context.tools.count : 0;
}

int runtime_tool_info(const struct runtime *runtime, int index,
		      struct tool_desc *out)
{
	if (!runtime || !out || index < 0 || index >= runtime->context.tools.count)
		return -EINVAL;
	*out = runtime->context.tools.entries[index].desc;
	return 0;
}

int runtime_tool_flags(const struct runtime *runtime, int index,
		       unsigned *out)
{
	if (!runtime || !out || index < 0 ||
	    index >= runtime->context.tools.count)
		return -EINVAL;
	*out = runtime->context.tools.entries[index].flags;
	return 0;
}

int runtime_tool_enabled(const struct runtime *runtime, int index, int *out)
{
	const struct tool_registry *tools;

	if (!runtime || !out || index < 0 ||
	    index >= runtime->context.tools.count)
		return -EINVAL;
	tools = &runtime->context.tools;
	*out = !tool_is_disabled(tools, tools->entries[index].desc.name);
	return 0;
}

int runtime_tool_origin(const struct runtime *runtime, int index,
			enum tool_origin *out)
{
	if (!runtime || !out || index < 0 ||
	    index >= runtime->context.tools.count)
		return -EINVAL;
	*out = runtime->context.tools.entries[index].origin;
	return 0;
}

int runtime_tool_find(const struct runtime *runtime, const char *name,
		      struct tool_desc *out)
{
	struct tool_entry *entry;
	if (!runtime || !name || !out)
		return -EINVAL;
	entry = tool_lookup((struct tool_registry *)&runtime->context.tools, name);
	if (!entry)
		return -ENOENT;
	*out = entry->desc;
	return 0;
}

int runtime_skill_count(const struct runtime *runtime)
{
	return runtime && runtime->context.skills ? runtime->context.skills->count : 0;
}

int runtime_skill_info(const struct runtime *runtime, int index,
		       struct skill_entry *out)
{
	if (!runtime || !runtime->context.skills || !out || index < 0 ||
	    index >= runtime->context.skills->count)
		return -EINVAL;
	*out = runtime->context.skills->entries[index];
	return 0;
}

int runtime_skill_find(const struct runtime *runtime, const char *name,
		       struct skill_entry *out)
{
	struct skill_entry *entry;
	if (!runtime || !runtime->context.skills || !name || !out)
		return -EINVAL;
	entry = skill_lookup(runtime->context.skills, name);
	if (!entry)
		return -ENOENT;
	*out = *entry;
	return 0;
}

int runtime_skill_set_active(struct runtime *runtime, const char *name,
			     int active, int *changed)
{
	struct skill_entry *entry;
	int before;
	int rc = 0;
	if (!runtime || !runtime->context.skills || !name)
		return -EINVAL;
	entry = skill_lookup(runtime->context.skills, name);
	if (!entry)
		return -ENOENT;
	before = entry->activated;
	if (active)
		rc = skill_activate(entry);
	else
		skill_deactivate(entry);
	if (changed)
		*changed = before != entry->activated;
	return rc;
}

static int runtime_preferences_bind(struct runtime *runtime)
{
	int rc;

	if (!runtime)
		MORPH_RETURN(-EINVAL);
	rc = runtime_ensure_current_session(runtime);
	if (rc != 0)
		return rc;
	return preference_bind(&runtime->context.database,
		runtime->context.current_session.id,
		runtime->context.turn_scope.bound ? runtime->context.turn_scope.user_id : "local",
		runtime->context.workdir);
}

char *runtime_memory_background_render(struct runtime *runtime)
{
	if (runtime_preferences_bind(runtime) != 0)
		return NULL;
	return memory_background_render(&runtime->context.database,
		runtime->context.current_session.id);
}

char *runtime_preferences_render(struct runtime *runtime, int history)
{
	if (runtime_preferences_bind(runtime) != 0)
		return NULL;
	return preference_render(&runtime->context.database,
		runtime->context.current_session.id, history);
}

int runtime_preference_set(struct runtime *runtime, const char *scope,
			   const char *key, const char *value)
{
	int rc = runtime_preferences_bind(runtime);

	if (rc != 0)
		return rc;
	return preference_set(&runtime->context.database,
		runtime->context.current_session.id, scope, key, value,
		"explicit /memory command", NULL);
}

char *runtime_memory_render_current(struct runtime *runtime, int max_episodes)
{
	if (!runtime)
		return NULL;
	return memory_render_session(&runtime->context.database,
				     runtime->context.current_session.id,
				     max_episodes);
}

int runtime_memory_clear_current(struct runtime *runtime,
				 enum memory_clear_scope scope)
{
	int rc;
	if (!runtime)
		return -EINVAL;
	rc = memory_clear(&runtime->context.database,
			  runtime->context.current_session.id, scope);
	if (rc == 0 && runtime->context.react)
		react_set_memory_context(runtime->context.react, NULL);
	return rc;
}

int runtime_credit_summary_today_get(struct runtime *runtime,
				     struct credit_summary *out)
{
	return runtime && out ? credit_summary_today(&runtime->context.database,
						     "local", out) : -EINVAL;
}

int runtime_credit_summary_current_get(struct runtime *runtime,
				       struct credit_summary *out)
{
	char key[64];
	if (!runtime || !out)
		return -EINVAL;
	runtime_context_credit_session_key(&runtime->context, key, sizeof(key));
	return credit_summary_session(&runtime->context.database, key, out);
}

int runtime_credit_record_media(struct runtime *runtime, const char *kind,
				int64_t image_units, int64_t video_seconds,
				const char *provider, const char *model,
				const char *metadata_json)
{
	struct credit_event event;
	char key[64];
	if (!runtime || !kind)
		return -EINVAL;
	runtime_context_credit_session_key(&runtime->context, key, sizeof(key));
	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = key;
	event.kind = kind;
	event.provider = provider;
	event.model = model;
	event.image_units = image_units;
	event.video_seconds = video_seconds;
	event.metadata_json = metadata_json;
	return credit_record_event(&runtime->context.database,
				   &runtime->context.config.credits, &event, NULL);
}
