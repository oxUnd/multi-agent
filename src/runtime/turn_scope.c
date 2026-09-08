#include "runtime/turn_scope.h"

#include "agent/tools/dynamic_tools.h"
#include "runtime/usage.h"
#include "util/error.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct memory_options runtime_memory_options_from_config(
	const struct config *config)
{
	struct memory_options opts;

	memset(&opts, 0, sizeof(opts));
	if (!config)
		return opts;
	opts.enabled = config->memory.enabled;
	opts.hot_path_enabled = config->memory.hot_path_enabled;
	opts.cold_path_enabled = config->memory.cold_path_enabled;
	opts.llm_extract_enabled = config->memory.llm_extract_enabled;
	opts.max_facts = config->memory.max_facts;
	opts.max_episodes = config->memory.max_episodes;
	opts.max_procedures = config->memory.max_procedures;
	opts.max_context_chars = config->memory.max_context_chars;
	return opts;
}

void runtime_credit_session_key(const struct runtime_turn_scope *scope,
				const struct session *current_session,
				char *buf, size_t buf_size)
{
	if (!buf || buf_size == 0)
		return;
	if (!current_session) {
		buf[0] = '\0';
		return;
	}
	if (scope && scope->bound && scope->credit_session_id[0]) {
		snprintf(buf, buf_size, "%s", scope->credit_session_id);
		return;
	}
	if (current_session->display_id[0]) {
		snprintf(buf, buf_size, "%s", current_session->display_id);
		return;
	}
	snprintf(buf, buf_size, "%lld", (long long)current_session->id);
}

static void runtime_turn_scope_credit_session_id(
	struct db *db, const struct session *current_session,
	int64_t session_id, char *buf, size_t buf_size)
{
	struct session s;
	const char *credit_session_id = NULL;

	if (!buf || buf_size == 0)
		return;
	buf[0] = '\0';
	if (current_session && session_id == current_session->id) {
		credit_session_id = current_session->display_id[0]
			? current_session->display_id
			: current_session->name;
	} else if (db && session_id > 0 &&
		   session_get_by_id(db, session_id, &s) == 0) {
		session_ensure_display_id(db, &s);
		credit_session_id = s.display_id[0] ? s.display_id : s.name;
	}
	if (credit_session_id)
		snprintf(buf, buf_size, "%s", credit_session_id);
}

static void runtime_turn_scope_bind_tool_context(
	struct runtime_turn_scope_context *ctx,
	const struct runtime_request *request)
{
	struct tool_runtime_context rt;
	int64_t session_id;

	if (!ctx || !ctx->react || !request)
		return;
	session_id = request->session_id;
	if (runtime_tool_context_for_session(ctx->db, ctx->config,
					     ctx->current_session, session_id,
					     &rt) != 0)
		return;
	if (request->user_id && request->user_id[0])
		rt.user_id = request->user_id;
	rt.restrict_memory_to_user = request->restrict_memory_to_user;
	rt.memory_visible_fn = request->memory_visible_fn;
	rt.memory_visible_user_data = request->memory_visible_user_data;
	react_set_tool_runtime_context(ctx->react, &rt);
	if (ctx->tools)
		(void)dynamic_tools_set_session_id(ctx->tools,
						   rt.credit_session_id);
}

int runtime_turn_scope_begin(struct runtime_turn_scope_context *ctx,
			     const struct runtime_request *request)
{
	if (!ctx || !ctx->scope || !ctx->db || !ctx->config || !request ||
	    request->session_id <= 0)
		MORPH_RETURN(-EINVAL);
	{
		int rc = preference_bind(ctx->db, request->session_id,
			request->user_id && request->user_id[0] ? request->user_id : "local",
			ctx->react ? ctx->react->workdir : NULL);

		if (rc != 0)
			return rc;
	}
	ctx->scope->previous_usage_user_data =
		runtime_usage_bind(ctx->usage_user_data);
	ctx->scope->bound = 1;
	ctx->scope->memory_session_id = request->session_id;
	if (request->user_id && request->user_id[0])
		snprintf(ctx->scope->user_id, sizeof(ctx->scope->user_id), "%s",
			 request->user_id);
	else
		snprintf(ctx->scope->user_id, sizeof(ctx->scope->user_id),
			 "local");
	runtime_turn_scope_credit_session_id(
		ctx->db, ctx->current_session, request->session_id,
		ctx->scope->credit_session_id,
		sizeof(ctx->scope->credit_session_id));
	if (ctx->tools)
		(void)scheduled_tasks_tool_set_time_anchor(
			ctx->tools, (int64_t)time(NULL));
	if (ctx->tools)
		(void)scheduled_tasks_tool_set_source_session(
			ctx->tools, request->session_id);
	if (ctx->plan_sessions && ctx->active_plan_session_id &&
	    ctx->active_plans) {
		runtime_plan_session_select(ctx->plan_sessions,
					    ctx->plan_session_count,
					    ctx->active_plan_session_id,
					    ctx->active_plans,
					    request->session_id);
	}
	runtime_turn_scope_bind_tool_context(ctx, request);
	return 0;
}

void runtime_turn_scope_finish(struct runtime_turn_scope_context *ctx)
{
	struct runtime_turn_scope *scope;

	if (!ctx || !ctx->scope || !ctx->scope->bound)
		return;
	scope = ctx->scope;
	runtime_usage_restore(scope->previous_usage_user_data);
	scope->previous_usage_user_data = NULL;
	scope->bound = 0;
	scope->memory_session_id = 0;
	scope->credit_session_id[0] = '\0';
	scope->user_id[0] = '\0';
	if (ctx->current_session && ctx->plan_sessions &&
	    ctx->active_plan_session_id && ctx->active_plans) {
		runtime_plan_session_select(ctx->plan_sessions,
					    ctx->plan_session_count,
					    ctx->active_plan_session_id,
					    ctx->active_plans,
					    ctx->current_session->id);
	}
	if (ctx->current_session) {
		struct runtime_request request;

		memset(&request, 0, sizeof(request));
		request.session_id = ctx->current_session
			? ctx->current_session->id : 0;
		if (request.session_id > 0)
			runtime_turn_scope_bind_tool_context(ctx, &request);
	}
}
