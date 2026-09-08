#include "react.h"
#include "guardrail.h"
#include "tokenizer.h"
#include "compress.h"
#include "history.h"
#include "system_prompt.h"
#include "agent/memory.h"
#include "tool_runtime.h"
#include "tool_context.h"
#include "models/llm.h"
#include "http/client.h"
#include "util/log.h"
#include "util/arena.h"
#include "util/buf.h"
#include "util/array.h"
#include "util/utf8.h"
#include "util/error.h"
#include "util/id.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <limits.h>

volatile sig_atomic_t react_sigint_flag = 0;

#define REACT_ACTIVE_MAX 16

static struct react_context *react_active_stack[REACT_ACTIVE_MAX];
static int react_active_count_val = 0;
static pthread_mutex_t react_active_mutex = PTHREAD_MUTEX_INITIALIZER;

int react_active_count(void)
{
	int c;

	pthread_mutex_lock(&react_active_mutex);
	c = react_active_count_val;
	pthread_mutex_unlock(&react_active_mutex);
	return c;
}

void react_active_push(struct react_context *ctx)
{
	pthread_mutex_lock(&react_active_mutex);
	if (react_active_count_val < REACT_ACTIVE_MAX)
		react_active_stack[react_active_count_val++] = ctx;
	pthread_mutex_unlock(&react_active_mutex);
}

void react_active_pop(struct react_context *ctx)
{
	pthread_mutex_lock(&react_active_mutex);
	for (int i = react_active_count_val - 1; i >= 0; i--) {
		if (react_active_stack[i] == ctx) {
			react_active_stack[i] =
				react_active_stack[react_active_count_val - 1];
			react_active_count_val--;
			break;
		}
	}
	pthread_mutex_unlock(&react_active_mutex);
}

void react_cancel_active(void)
{
	pthread_mutex_lock(&react_active_mutex);
	for (int i = 0; i < react_active_count_val; i++) {
		react_active_stack[i]->cancelled = 1;
		morph_cancel_token_cancel(&react_active_stack[i]->cancel_token);
	}
	pthread_mutex_unlock(&react_active_mutex);
	react_sigint_flag = 1;
}

/*
 * Check whether a tool requires human-in-the-loop approval.
 *
 * ctx - ReAct context with HITL configuration
 * tool_name - Name of the tool to check
 *
 * Returns 1 if approval is required, 0 if auto-approved or HITL disabled.
 */
int hitl_needs_approval(struct react_context *ctx, const char *tool_name)
{
	if (!ctx || !tool_name)
		return 0;
	struct hitl_config *h = &ctx->hitl;
	if (!h->enabled)
		return 0;
	for (int i = 0; i < h->auto_approved_count; i++) {
		if (strcmp(h->auto_approved[i], tool_name) == 0)
			return 0;
	}
	if (tool_has_flag(ctx->tools, tool_name, TOOL_FLAG_INTERNAL_APPROVAL))
		return 0;
	if (h->tools_count > 0) {
		for (int i = 0; i < h->tools_count; i++) {
			if (strcmp(h->tools[i], tool_name) == 0)
				return 1;
		}
		return 0;
	}
	if (h->auto_approve_readonly && tool_is_readonly(ctx->tools, tool_name))
		return 0;
	return 1;
}

/*
 * Add a tool to the HITL auto-approved list so it bypasses future approval checks.
 *
 * h - HITL configuration to update
 * tool_name - Name of the tool to auto-approve
 */
void hitl_add_auto_approved(struct hitl_config *h, const char *tool_name)
{
	if (!h || !tool_name)
		return;
	if (h->auto_approved_count >= HITL_AUTO_APPROVED_MAX)
		return;
	for (int i = 0; i < h->auto_approved_count; i++) {
		if (strcmp(h->auto_approved[i], tool_name) == 0)
			return;
	}
	strncpy(h->auto_approved[h->auto_approved_count], tool_name,
		HITL_TOOL_NAME_MAX - 1);
	h->auto_approved_count++;
}

/*
 * Register an action drain callback for external action injection.
 *
 * ctx - ReAct context to configure
 * fn - Callback function, or NULL to clear
 * user - Opaque pointer passed to fn
 *
 * Returns 0 on success, -EINVAL if ctx is NULL.
 */
int react_set_action_drain(struct react_context *ctx,
			   react_action_drain_fn fn, void *user)
{
	if (!ctx)
		return -EINVAL;
	ctx->action_drain_fn = fn;
	ctx->action_drain_user_data = user;
	return 0;
}

int react_set_event_callback(struct react_context *ctx,
			     morph_event_cb cb, void *user)
{
	if (!ctx)
		return -EINVAL;
	ctx->event_cb = cb;
	ctx->event_user_data = user;
	return 0;
}

int react_set_turn_id(struct react_context *ctx, const char *turn_id)
{
	if (!ctx)
		return -EINVAL;
	ctx->turn_id[0] = '\0';
	ctx->turn_id_user_set = 0;
	if (!turn_id || !*turn_id)
		return 0;
	snprintf(ctx->turn_id, sizeof(ctx->turn_id), "%s", turn_id);
	ctx->turn_id_user_set = 1;
	return 0;
}

const char *react_get_turn_id(const struct react_context *ctx)
{
	if (!ctx || !ctx->turn_id[0])
		return NULL;
	return ctx->turn_id;
}

int react_set_tool_runtime_context(
	struct react_context *ctx,
	const struct tool_runtime_context *runtime)
{
	if (!ctx)
		return -EINVAL;
	if (runtime)
		ctx->tool_runtime = *runtime;
	else
		memset(&ctx->tool_runtime, 0, sizeof(ctx->tool_runtime));
	return 0;
}

static int react_events_enabled(struct react_context *ctx)
{
	return ctx && ctx->event_cb;
}

static int react_emit_event(struct react_context *ctx,
			    enum morph_event_type type, const char *name,
			    const char *phase, const char *message,
			    cJSON *data)
{
	struct morph_event ev;

	if (!ctx || !ctx->event_cb)
		return 0;
	ev.type = type;
	ev.name = name;
	ev.phase = phase;
	ev.message = message;
	ev.data = data;
	ev.turn_id = ctx->turn_id[0] ? ctx->turn_id : NULL;
	return morph_event_emit(ctx->event_cb, ctx->event_user_data, &ev);
}

static int react_emit_text_event(struct react_context *ctx,
				 enum morph_event_type type,
				 const char *name, const char *phase,
				 const char *message, const char *text)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	rc = react_emit_event(ctx, type, name, phase, message, data);
	cJSON_Delete(data);
	return rc;
}

static int react_output_emit(react_output_cb cb, void *user_data,
			     enum react_step_type type,
			     enum react_output_status status,
			     const char *text,
			     const char *tool_name,
			     const char *tool_args,
			     const char *tool_call_id,
			     int error_code,
			     const struct tool_artifact_list *artifacts,
			     const cJSON *data, const cJSON *ui)
{
	struct react_output_event ev;

	if (!cb)
		return 0;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.status = status;
	ev.text = text;
	ev.tool_name = tool_name;
	ev.tool_args = tool_args;
	ev.tool_call_id = tool_call_id;
	ev.error_code = error_code;
	ev.artifacts = artifacts;
	ev.data = data;
	ev.ui = ui;
	return cb(&ev, user_data);
}

static int react_emit_observation_event(struct react_context *ctx,
					const char *text,
					const char *tool,
					int error_code,
					const struct tool_artifact_list *artifacts,
					const cJSON *result_data,
					const cJSON *result_ui)
{
	cJSON *data;
	cJSON *artifact_json = NULL;
	cJSON *data_copy = NULL;
	cJSON *ui_copy = NULL;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	if (tool)
		cJSON_AddStringToObject(data, "tool", tool);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	if (artifacts && artifacts->count > 0) {
		artifact_json = tool_artifact_list_to_json(artifacts);
		if (artifact_json)
			cJSON_AddItemToObject(data, "artifacts",
					      artifact_json);
	}
	if (result_data) {
		data_copy = cJSON_Duplicate(result_data, 1);
		if (data_copy)
			cJSON_AddItemToObject(data, "data", data_copy);
	}
	if (result_ui) {
		ui_copy = cJSON_Duplicate(result_ui, 1);
		if (ui_copy)
			cJSON_AddItemToObject(data, "ui", ui_copy);
	}
	rc = react_emit_event(ctx, MORPH_EVENT_REACT, "react.observation",
			      error_code < 0 ? "failed" : "end",
			      "tool observation", data);
	cJSON_Delete(data);
	return rc;
}

static int react_emit_auth_required(struct react_context *ctx,
				    const char *backend,
				    const char *provider,
				    const char *model,
				    const char *tool,
				    const char *reason)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "kind", "auth_required");
	cJSON_AddStringToObject(data, "backend", backend ? backend : "");
	cJSON_AddStringToObject(data, "provider", provider ? provider : "");
	cJSON_AddStringToObject(data, "model", model ? model : "");
	cJSON_AddStringToObject(data, "env_name", "");
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "reason",
				reason ? reason : "missing_api_key");
	cJSON_AddBoolToObject(data, "retryable", 1);
	rc = react_emit_event(ctx, MORPH_EVENT_HITL, "auth.required",
			      "blocked", "authentication required", data);
	cJSON_Delete(data);
	return rc;
}

static const char *react_auth_backend_for_tool(const char *tool)
{
	if (!tool)
		return NULL;
	if (strcmp(tool, "img_qa") == 0 ||
	    strcmp(tool, "plan") == 0)
		return "text";
	if (strcmp(tool, "img_gen") == 0 ||
	    strcmp(tool, "img_inpaint") == 0 ||
	    strcmp(tool, "img_compose") == 0)
		return "image";
	if (strcmp(tool, "vid_gen") == 0)
		return "video";
	return NULL;
}

static void react_set_state(struct react_context *ctx, enum react_state state)
{
	if (ctx)
		ctx->state = state;
}

static int react_emit_thinking_event(struct react_context *ctx)
{
	return react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.thinking",
				     "begin", "Thinking...", "");
}

static void react_set_result(struct react_context *ctx,
			     enum react_outcome outcome,
			     int error_code, const char *reason)
{
	if (!ctx)
		return;
	ctx->outcome = outcome;
	ctx->last_error_code = error_code;
	ctx->state = outcome == REACT_OUTCOME_SUCCESS ?
		REACT_STATE_DONE : REACT_STATE_ABORT;
	if (reason && *reason) {
		snprintf(ctx->outcome_reason, sizeof(ctx->outcome_reason),
			 "%s", reason);
	} else {
		ctx->outcome_reason[0] = '\0';
	}
}

static void react_set_error_detail(struct react_context *ctx,
				   const char *detail)
{
	if (!ctx)
		return;
	free(ctx->last_error_detail);
	ctx->last_error_detail = detail && *detail ?
		utf8_dup_clamped(detail, 8192) : NULL;
}

static int react_emit_outcome_event(struct react_context *ctx,
				    const char *name, const char *phase,
				    const char *message, const char *text)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "text", text ? text : "");
	cJSON_AddStringToObject(data, "outcome",
				react_outcome_name(ctx ? ctx->outcome :
						   REACT_OUTCOME_NONE));
	if (ctx && ctx->outcome_reason[0])
		cJSON_AddStringToObject(data, "reason", ctx->outcome_reason);
	if (ctx && ctx->last_error_detail)
		cJSON_AddStringToObject(data, "detail",
					ctx->last_error_detail);
	if (ctx && ctx->last_error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code",
					ctx->last_error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(ctx->last_error_code));
	}
	rc = react_emit_event(ctx, MORPH_EVENT_REACT, name, phase, message,
			      data);
	cJSON_Delete(data);
	return rc;
}

static int react_finish(struct react_context *ctx)
{
	const char *event_name = "react.failed";
	const char *phase = "failed";
	const char *message = "turn failed";
	int rc;

	if (!ctx)
		return -EINVAL;
	if (ctx->outcome == REACT_OUTCOME_NONE) {
		if (ctx->state == REACT_STATE_DONE) {
			react_set_result(ctx, REACT_OUTCOME_SUCCESS, 0, NULL);
		} else if (ctx->cancelled) {
			react_set_result(ctx, REACT_OUTCOME_CANCELLED,
					 -ECANCELED, "user_cancelled");
		} else {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ECANCELED, "internal_error");
		}
	}

	rc = ctx->last_error_code < 0 ? ctx->last_error_code : 0;
	if (ctx->outcome == REACT_OUTCOME_SUCCESS) {
		react_emit_outcome_event(ctx, "react.turn.end", "end",
					 "turn completed",
					 ctx->final_answer ?
					 ctx->final_answer : "");
		return 0;
	}

	if (rc == 0)
		rc = -ECANCELED;

	if (ctx->outcome == REACT_OUTCOME_CANCELLED) {
		event_name = "react.cancelled";
		phase = "cancelled";
		message = "turn cancelled";
	} else if (ctx->outcome == REACT_OUTCOME_TIMEOUT) {
		event_name = "react.timed_out";
		phase = "timeout";
		message = "turn timed out";
	} else if (ctx->outcome == REACT_OUTCOME_MAX_ITERATIONS) {
		event_name = "react.max_iterations";
		message = "maximum iterations reached";
	}

	react_emit_outcome_event(ctx, event_name, phase, message,
				 ctx->final_answer ? ctx->final_answer : "");
	react_emit_outcome_event(ctx, "react.turn.end", phase, message,
				 ctx->final_answer ? ctx->final_answer : "");
	return rc;
}

static int react_finish_run(struct react_context *ctx)
{
	int rc = react_finish(ctx);

	react_sigint_flag = 0;
	http_clear_signal_cancel();
	if (ctx)
		ctx->turn_id_user_set = 0;
	return rc;
}

static int react_emit_tool_event(struct react_context *ctx,
				 const char *name, const char *phase,
				 const char *message, const char *tool,
				 const char *args_json,
				 const char *tool_call_id,
				 const char *result, int error_code)
{
	cJSON *data;
	cJSON *args = NULL;
	int text_input = 0;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	if (ctx->tools && tool) {
		struct tool_entry *entry = tool_lookup(ctx->tools, tool);

		if (entry && entry->desc.title[0])
			cJSON_AddStringToObject(data, "toolTitle",
						entry->desc.title);
		if (entry && entry->desc.input_kind == TOOL_INPUT_TEXT)
			text_input = 1;
	}
	cJSON_AddStringToObject(data, "tool_call_id",
				tool_call_id ? tool_call_id : "");
	if (text_input) {
		args = cJSON_CreateObject();
		if (args)
			cJSON_AddStringToObject(args, "input",
				args_json ? args_json : "");
	} else {
		args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
	}
	if (!args)
		args = cJSON_CreateObject();
	if (!args) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(data, "args", args);
	if (result)
		cJSON_AddStringToObject(data, "result", result);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = react_emit_event(ctx, MORPH_EVENT_TOOL, name, phase, message,
			      data);
	cJSON_Delete(data);
	return rc;
}

static int react_tool_call_begin(struct react_context *ctx,
				 const struct tool_call *tc)
{
	if (!tc)
		return 0;
	return react_emit_tool_event(ctx, "tool.call", "begin",
				     "calling tool", tc->name,
				     tc->arguments ? tc->arguments : "{}",
				     tc->tool_call_id, NULL, 0);
}

static int react_tool_call_running(struct react_context *ctx,
				   const struct tool_call *tc)
{
	if (!tc)
		return 0;
	return react_emit_tool_event(ctx, "tool.running", "begin",
				     "tool running", tc->name,
				     tc->arguments ? tc->arguments : "{}",
				     tc->tool_call_id, NULL, 0);
}

static int react_tool_call_thread_failed(struct react_context *ctx,
					 const struct tool_call *tc, int rc)
{
	if (!tc)
		return 0;
	return react_emit_tool_event(ctx, "tool.failed", "failed",
				     "tool thread failed", tc->name,
				     tc->arguments ? tc->arguments : "{}",
				     tc->tool_call_id, NULL, rc);
}

static int react_emit_artifact_event(struct react_context *ctx,
				     const char *kind, const char *path,
				     const char *source)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "kind", kind ? kind : "");
	cJSON_AddStringToObject(data, "path", path ? path : "");
	if (source)
		cJSON_AddStringToObject(data, "source", source);
	rc = react_emit_event(ctx, MORPH_EVENT_ARTIFACT, "artifact.ready",
			      "ready", "artifact ready", data);
	cJSON_Delete(data);
	return rc;
}

static void react_emit_artifacts_from_list(struct react_context *ctx,
					   const struct tool_artifact_list *artifacts,
					   const char *source)
{
	if (!ctx || !artifacts)
		return;
	for (int i = 0; i < artifacts->count; i++) {
		const struct tool_artifact *artifact = &artifacts->items[i];
		react_emit_artifact_event(ctx,
					  tool_artifact_kind_name(artifact->kind),
					  artifact->path, source);
	}
}

static int react_emit_hitl_event(struct react_context *ctx,
				 const char *name, const char *phase,
				 const char *message, const char *tool,
				 const char *args_json,
				 const char *tool_call_id,
				 const char *verdict)
{
	cJSON *data;
	cJSON *args = NULL;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "tool", tool ? tool : "");
	cJSON_AddStringToObject(data, "tool_call_id",
				tool_call_id ? tool_call_id : "");
	args = args_json && *args_json ? cJSON_Parse(args_json) : NULL;
	if (!args)
		args = cJSON_CreateObject();
	if (!args) {
		cJSON_Delete(data);
		return -ENOMEM;
	}
	cJSON_AddItemToObject(data, "args", args);
	if (verdict)
		cJSON_AddStringToObject(data, "verdict", verdict);
	rc = react_emit_event(ctx, MORPH_EVENT_HITL, name, phase, message,
			      data);
	cJSON_Delete(data);
	return rc;
}

/*
 * Per-tool async execution state.
 *
 * Lifetime ownership:
 * - Created by async_tool_call_create() before pthread_create().
 * - Normally destroyed by async_tool_call_destroy() in react_run after join.
 * - On cancellation, ownership is transferred to the worker thread by
 *   setting `detached = 1` under the mutex and calling pthread_detach();
 *   the worker thread then frees the struct itself at exit.
 *
 * tool_name / tool_args / tool_call_id are owned copies so they survive
 * even after the originating chat_response or arena is reset.
 */
struct async_tool_call {
	struct tool_registry *tools;
	struct react_context *react;
	char *tool_name;
	char *tool_args;
	char *tool_call_id;
	char *provider_tool_call_id;
	char *result;
	cJSON *data;
	cJSON *ui;
	cJSON *meta;
	struct tool_artifact_list artifacts;
	int rc;
	react_output_cb output_cb;
	void *output_user_data;
	volatile sig_atomic_t completed;
	volatile sig_atomic_t cancelled;
	volatile sig_atomic_t detached;
	volatile sig_atomic_t start_released;
	struct morph_cancel_token cancel_token;
	void *usage_user_data;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

struct react_tool_slot {
	pthread_t thread;
	struct async_tool_call *call;
	int hitl_denied;
	int thread_started;
};

static struct async_tool_call *
async_tool_call_create(struct tool_registry *tools,
		       struct react_context *react,
		       const char *tool_name,
		       const char *tool_args,
		       const char *tool_call_id,
		       const char *provider_tool_call_id,
		       react_output_cb output_cb,
		       void *output_user_data)
{
	struct async_tool_call *call = calloc(1, sizeof(*call));
	if (!call)
		return NULL;
	pthread_mutex_init(&call->mutex, NULL);
	pthread_cond_init(&call->cond, NULL);
	call->tools = tools;
	call->react = react;
	call->tool_name = strdup(tool_name ? tool_name : "");
	call->tool_args = strdup(tool_args ? tool_args : "{}");
	call->tool_call_id = strdup(tool_call_id ? tool_call_id : "");
	call->provider_tool_call_id = strdup(provider_tool_call_id ?
					     provider_tool_call_id : "");
	call->output_cb = output_cb;
	call->output_user_data = output_user_data;
	call->usage_user_data = model_get_usage_user_data();
	morph_cancel_token_reset(&call->cancel_token);
	if (!call->tool_name || !call->tool_args || !call->tool_call_id ||
	    !call->provider_tool_call_id) {
		free(call->tool_name);
		free(call->tool_args);
		free(call->tool_call_id);
		free(call->provider_tool_call_id);
		pthread_cond_destroy(&call->cond);
		pthread_mutex_destroy(&call->mutex);
		free(call);
		return NULL;
	}
	return call;
}

static void async_tool_call_destroy(struct async_tool_call *call)
{
	if (!call)
		return;
	pthread_cond_destroy(&call->cond);
	pthread_mutex_destroy(&call->mutex);
	free(call->tool_name);
	free(call->tool_args);
	free(call->tool_call_id);
	free(call->provider_tool_call_id);
	free(call->result);
	cJSON_Delete(call->data);
	cJSON_Delete(call->ui);
	cJSON_Delete(call->meta);
	free(call);
}

static int react_tool_call_cancelled(struct react_context *ctx,
				     const struct tool_call *call)
{
	if (!call)
		return 0;
	return react_emit_tool_event(ctx, "tool.cancelled", "cancelled",
				     "tool execution cancelled",
				     call->name, call->arguments,
				     call->tool_call_id, NULL, -ECANCELED);
}

static int react_tool_call_finish(struct react_context *ctx,
				  const struct async_tool_call *call,
				  const char *result, int rc)
{
	if (!call)
		return 0;
	if (rc == MORPH_ERR_NOT_CONFIGURED) {
		const char *backend = react_auth_backend_for_tool(call->tool_name);

		if (backend)
			react_emit_auth_required(ctx, backend, "", "",
						 call->tool_name,
						 "missing_api_key");
	}
	return react_emit_tool_event(ctx,
				     rc < 0 ? "tool.failed" : "tool.result",
				     rc < 0 ? "failed" : "end",
				     rc < 0 ? "tool failed" : "tool result",
				     call->tool_name, call->tool_args,
				     call->tool_call_id, result, rc);
}

/*
 * Wait for a tool worker to finish.
 *
 * When the cancel flag is not set, joins normally and returns 0.
 *
 * On cancellation, ownership of `call` is transferred to the worker
 * thread by setting `detached = 1` under the mutex, then detaching the
 * thread. Caller must NOT touch `call` afterwards; the worker will free
 * it itself when it exits.
 *
 * Returns 0 if joined normally, -ECANCELED if cancelled, or -ETIMEDOUT if
 * the tool deadline expires.
 */
static int join_tool_thread(pthread_t thread, volatile sig_atomic_t *cancelled,
			    struct async_tool_call *call, int timeout_seconds)
{
	struct timespec ts;
	time_t deadline;

	deadline = timeout_seconds > 0 ? time(NULL) + timeout_seconds : 0;

	pthread_mutex_lock(&call->mutex);
	while (!call->completed && !(cancelled && *cancelled) &&
	       !react_sigint_flag) {
		if (deadline > 0 && time(NULL) >= deadline)
			break;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100 * 1000 * 1000;
		if (ts.tv_nsec >= 1000 * 1000 * 1000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000 * 1000 * 1000;
		}
		pthread_cond_timedwait(&call->cond, &call->mutex, &ts);
	}
	if (!call->completed && react_sigint_flag && cancelled)
		*cancelled = 1;
	if (call->completed || !cancelled || !*cancelled) {
		if (!call->completed && deadline > 0 &&
		    time(NULL) >= deadline) {
			call->cancelled = 1;
			morph_cancel_token_cancel(&call->cancel_token);
			call->detached = 1;
			pthread_mutex_unlock(&call->mutex);
			pthread_detach(thread);
			return -ETIMEDOUT;
		}
		pthread_mutex_unlock(&call->mutex);
		pthread_join(thread, NULL);
		return 0;
	}

	call->cancelled = 1;
	morph_cancel_token_cancel(&call->cancel_token);
	call->detached = 1;
	pthread_mutex_unlock(&call->mutex);
	pthread_detach(thread);
	return -ECANCELED;
}

static int react_tool_call_timeout(struct react_context *ctx,
				   const struct tool_call *tc)
{
	int timeout;
	cJSON *root;
	cJSON *arg_timeout;

	timeout = tool_timeout_seconds(ctx->tools, tc->name);
	if (timeout <= 0)
		timeout = ctx->tool_timeout_seconds;
	if (!tc->arguments)
		return timeout;

	root = cJSON_Parse(tc->arguments);
	if (!root)
		return timeout;
	arg_timeout = cJSON_GetObjectItem(root, "timeout_seconds");
	if (cJSON_IsNumber(arg_timeout) && arg_timeout->valuedouble > 0)
		timeout = (int)arg_timeout->valuedouble;
	cJSON_Delete(root);
	return timeout;
}

static void *async_tool_exec(void *arg)
{
	struct async_tool_call *call = (struct async_tool_call *)arg;
	if (!call)
		return NULL;

	pthread_mutex_lock(&call->mutex);
	while (!call->start_released && !call->cancelled)
		pthread_cond_wait(&call->cond, &call->mutex);
	pthread_mutex_unlock(&call->mutex);

	http_set_cancel_flag(&call->cancelled);
	http_set_cancel_token(&call->cancel_token);

	int notify_done = 0;

	if (tool_is_disabled(call->tools, call->tool_name)) {
		char disabled_msg[256];
		snprintf(disabled_msg, sizeof(disabled_msg),
			 "tool error: '%s' is disabled in configuration",
			 call->tool_name);

		pthread_mutex_lock(&call->mutex);
		call->result = strdup(disabled_msg);
		call->rc = -EPERM;
		call->completed = 1;
		pthread_cond_broadcast(&call->cond);
		pthread_mutex_unlock(&call->mutex);
		notify_done = 1;
	} else {
		struct tool_result res;
		struct tool_runtime_context runtime;
		void *prev_usage_user_data;
		int rc;

		tool_result_init(&res);
		prev_usage_user_data = model_get_usage_user_data();
		model_set_usage_user_data(call->usage_user_data);
		runtime = call->react->tool_runtime;
		runtime.event_cb = call->react->event_cb;
		runtime.event_user_data = call->react->event_user_data;
		runtime.turn_id = call->react->turn_id[0]
			? call->react->turn_id : NULL;
		runtime.tool_call_id = call->tool_call_id;
		tool_runtime_set_current(&runtime);
		rc = tool_exec(call->tools, call->tool_name,
			       call->tool_args, &res);
		tool_runtime_set_current(NULL);
		model_set_usage_user_data(prev_usage_user_data);

		pthread_mutex_lock(&call->mutex);
		int was_cancelled = call->cancelled;
		if (was_cancelled) {
			call->completed = 1;
			pthread_cond_broadcast(&call->cond);
		} else if (rc < 0) {
			const char *raw = res.text.data ? res.text.data : "unknown error";
			size_t need = strlen(raw) + 64;
			char *buf = malloc(need);
			if (buf)
				snprintf(buf, need, "tool error: %s (%s)",
					 raw, morph_strerror(rc));
			call->result = buf;
			call->meta = res.meta ? cJSON_Duplicate(res.meta, 1) : NULL;
			call->rc = rc;
			call->completed = 1;
			pthread_cond_broadcast(&call->cond);
		} else {
			const char *raw = res.text.data ? res.text.data :
				"(no output)";
			call->result = utf8_dup_clamped(raw, 256 * 1024);
			call->data = res.data ? cJSON_Duplicate(res.data, 1) :
				NULL;
			call->ui = res.ui ? cJSON_Duplicate(res.ui, 1) :
				NULL;
			call->meta = res.meta ? cJSON_Duplicate(res.meta, 1) :
				NULL;
			call->artifacts = res.artifacts;
			call->rc = 0;
			call->completed = 1;
			pthread_cond_broadcast(&call->cond);
		}
		pthread_mutex_unlock(&call->mutex);

		tool_result_cleanup(&res);
		notify_done = !was_cancelled;
	}

	http_set_cancel_flag(NULL);
	http_set_cancel_token(NULL);

	if (notify_done) {
		enum react_output_status status = call->rc < 0 ?
			REACT_OUTPUT_FAILED : REACT_OUTPUT_COMPLETED;
		react_output_emit(call->output_cb, call->output_user_data,
				  REACT_STEP_ACTION, status,
				  NULL, call->tool_name, call->tool_args,
				  call->tool_call_id, call->rc, NULL,
				  NULL, NULL);
	}

	pthread_mutex_lock(&call->mutex);
	int detached = call->detached;
	pthread_mutex_unlock(&call->mutex);
	if (detached)
		async_tool_call_destroy(call);
	return NULL;
}

static int summarize_cb(const char *text, void *user_data, char **out)
{
	struct react_context *ctx = user_data;
	struct model *llm = ctx->llm_model;
	if (!llm || !llm->chat || !llm->api_key[0]) {
		MORPH_RETURN(MORPH_ERR_NOT_CONFIGURED);
	}
	const char *sys = ctx->history_compaction_prompt ?
		ctx->history_compaction_prompt :
		"Write a factual continuation checkpoint for another model. Start "
		"directly with these sections: Current objective, Constraints, "
		"Completed work, Current working state, Pending work, and "
		"Verification. Preserve exact identifiers, paths, edits, tool "
		"outcomes, errors, and the next concrete action. Never respond with "
		"meta narration such as 'let me understand' or restart the task from "
		"the beginning. Distinguish facts from uncertainty and do not invent "
		"details or assume unfinished work is complete.";
	const char *msgs[] = { text };
	morph_buf_t b;
	int previous_max_tokens = llm->max_tokens;
	int rc = morph_buf_init(&b, 8192);
	if (rc != 0) {
		*out = strdup(text);
		return *out ? 0 : -ENOMEM;
	}
	if (ctx->compress.compaction_summary_max_tokens > 0 &&
	    (llm->max_tokens <= 0 ||
	     llm->max_tokens > ctx->compress.compaction_summary_max_tokens))
		llm->max_tokens = ctx->compress.compaction_summary_max_tokens;
	rc = llm->chat(llm, ctx->turn_arena, sys, msgs, 1, NULL,
		       morph_buf_append_cb, &b);
	llm->max_tokens = previous_max_tokens;
	if (rc < 0) {
		morph_buf_cleanup(&b);
		return rc;
	}
	*out = morph_buf_detach(&b);
	if (!*out) {
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

const char *react_step_type_name(enum react_step_type type)
{
	switch (type) {
	case REACT_STEP_THOUGHT:	return "Thought";
	case REACT_STEP_ACTION:		return "Action";
	case REACT_STEP_OBSERVATION:	return "Observation";
	case REACT_STEP_REFLECTION:	return "Reflection";
	case REACT_STEP_FINAL:		return "Final";
	case REACT_STEP_REASONING:	return "Reasoning";
	default:			return "Unknown";
	}
}

const char *react_state_name(enum react_state state)
{
	switch (state) {
	case REACT_STATE_INIT:		return "INIT";
	case REACT_STATE_THINKING:	return "THINKING";
	case REACT_STATE_ACTING:	return "ACTING";
	case REACT_STATE_OBSERVING:	return "OBSERVING";
	case REACT_STATE_GUARDRAIL:	return "GUARDRAIL";
	case REACT_STATE_FINAL:		return "FINAL";
	case REACT_STATE_DONE:		return "DONE";
	case REACT_STATE_ABORT:		return "ABORT";
	case REACT_STATE_TOOL_FAIL:	return "TOOL_FAIL";
	default:			return "Unknown";
	}
}

const char *react_outcome_name(enum react_outcome outcome)
{
	switch (outcome) {
	case REACT_OUTCOME_NONE:		return "none";
	case REACT_OUTCOME_SUCCESS:	return "success";
	case REACT_OUTCOME_CANCELLED:	return "cancelled";
	case REACT_OUTCOME_TIMEOUT:	return "timeout";
	case REACT_OUTCOME_MAX_ITERATIONS:
		return "max_iterations";
	case REACT_OUTCOME_LLM_ERROR:	return "llm_error";
	case REACT_OUTCOME_TOOL_ERROR:	return "tool_error";
	case REACT_OUTCOME_GUARDRAIL_DENIED:
		return "guardrail_denied";
	case REACT_OUTCOME_INTERNAL_ERROR:
		return "internal_error";
	default:			return "unknown";
	}
}

struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg,
					   struct guardrail_config *gcfg)
{
	struct react_context *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->tools = tools;
	ctx->tokenizer = tok;
	ctx->max_iterations = 10;
	ctx->tool_timeout_seconds = 300;
	ctx->tool_max_retries = 3;
	ctx->empty_round_count = 0;
	ctx->steer_count = 0;
	ctx->guardrail_retry_count = 0;
	react_set_state(ctx, REACT_STATE_INIT);
	ctx->outcome = REACT_OUTCOME_NONE;
	ctx->last_error_code = 0;
	ctx->outcome_reason[0] = '\0';
	ctx->cancelled = 0;
	morph_cancel_token_reset(&ctx->cancel_token);
	ctx->turn_arena = arena_create(0);
	ctx->session_arena = arena_create(0);
	if (!ctx->turn_arena || !ctx->session_arena) {
		arena_destroy(ctx->turn_arena);
		arena_destroy(ctx->session_arena);
		free(ctx);
		return NULL;
	}
	if (cfg)
		ctx->compress = *cfg;
	ctx->history_tool_result_tokens =
		ctx->compress.tool_result_max_tokens;
	ctx->compress.summarize = summarize_cb;
	ctx->compress.summarize_user_data = ctx;
	if (gcfg)
		ctx->guardrail = *gcfg;
	else {
		ctx->guardrail.enabled = 0;
		ctx->guardrail.max_retries = 2;
		ctx->guardrail.max_empty_rounds = 3;
	}
	if (ctx->guardrail.rule_count == 0)
		guardrail_register_builtin_rules(&ctx->guardrail);
	ctx->hitl.enabled = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.auto_approve_readonly = 1;
	ctx->hitl.approval_cb = NULL;
	ctx->hitl.approval_user_data = NULL;
	ctx->hitl.auto_approved_count = 0;
	return ctx;
}

void react_context_destroy(struct react_context *ctx)
{
	if (!ctx)
		return;
	react_reset(ctx);
	free(ctx->final_answer);
	free(ctx->last_error_detail);
	free(ctx->system_prompt);
	free(ctx->memory_context);
	free(ctx->workdir);
	free(ctx->history_compaction_prompt);
	for (int i = 0; i < ctx->history_secret_count; i++)
		free(ctx->history_secrets[i]);
	model_history_free_list(ctx->history_items);
	arena_destroy(ctx->turn_arena);
	if (ctx->session_arena)
		arena_destroy(ctx->session_arena);
	free(ctx);
}

void react_reset(struct react_context *ctx)
{
	if (!ctx)
		return;
	struct react_step *cur = ctx->steps;
	while (cur) {
		struct react_step *next = cur->next;
		react_step_destroy(cur);
		cur = next;
	}
	ctx->steps = NULL;
	ctx->step_count = 0;
	react_set_state(ctx, REACT_STATE_INIT);
	ctx->outcome = REACT_OUTCOME_NONE;
	ctx->last_error_code = 0;
	ctx->outcome_reason[0] = '\0';
	free(ctx->last_error_detail);
	ctx->last_error_detail = NULL;
	free(ctx->final_answer);
	ctx->final_answer = NULL;
	ctx->tool_fail_name = NULL;
	ctx->tool_fail_args = NULL;
	ctx->tool_fail_count = 0;
	ctx->guardrail_retry_count = 0;
	ctx->empty_round_count = 0;
	ctx->in_turn_compaction_count = 0;
	ctx->incomplete_final_retry_count = 0;
	ctx->cancelled = 0;
	morph_cancel_token_reset(&ctx->cancel_token);
}

void react_cancel(struct react_context *ctx)
{
	if (ctx) {
		ctx->cancelled = 1;
		morph_cancel_token_cancel(&ctx->cancel_token);
	}
}

int react_set_memory_context(struct react_context *ctx,
			     const char *memory_context)
{
	char *dup = NULL;

	if (!ctx)
		return -EINVAL;
	if (memory_context) {
		dup = strdup(memory_context);
		if (!dup)
			return -ENOMEM;
	}
	free(ctx->memory_context);
	ctx->memory_context = dup;
	return 0;
}

struct react_step *react_step_create(struct arena *arena,
				     enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args,
				     const char *tool_call_id)
{
	struct react_step *s = arena_alloc(arena, sizeof(*s));
	if (!s)
		return NULL;
	s->type = type;
	s->error_code = 0;
	s->content = content ? arena_strdup(arena, content) : NULL;
	s->tool_name = tool_name ? arena_strdup(arena, tool_name) : NULL;
	s->tool_args = tool_args ? arena_strdup(arena, tool_args) : NULL;
	s->tool_call_id = tool_call_id ? arena_strdup(arena, tool_call_id) : NULL;
	s->next = NULL;
	return s;
}

void react_step_destroy(struct react_step *step)
{
	if (!step)
		return;
	/* If we're using arena, we don't free individual fields;
	 * otherwise, free them
	 */
	/* Since we can't know for sure, we just free the step, but
	 * actually, the only place without arena is cli.c's free_json_react_steps,
	 * which does its own cleanup
	 */
	(void)step;
}

static void add_step(struct react_context *ctx, struct react_step *step)
{
	if (!step)
		return;
	if (!ctx->steps) {
		ctx->steps = step;
	} else {
		struct react_step *cur = ctx->steps;
		while (cur->next)
			cur = cur->next;
		cur->next = step;
	}
	ctx->step_count++;
}

static int react_has_active_tool(const struct react_context *ctx,
				 const char *name)
{
	return ctx && ctx->tools && tool_lookup(ctx->tools, name) &&
		!tool_is_disabled(ctx->tools, name);
}

static char *build_system_prompt(struct react_context *ctx, struct arena *arena)
{
	morph_buf_t buf;
	int rc = morph_buf_init_arena(&buf, arena, 8192);
	if (rc != 0)
		return NULL;

	char time_buf[128];
	{
		time_t now = time(NULL);
		struct tm tm_local;
		localtime_r(&now, &tm_local);
		strftime(time_buf, sizeof(time_buf),
			 "%Y-%m-%d %A %H:%M:%S %Z", &tm_local);
	}

	rc = morph_buf_printf(&buf, MORPH_SYSTEM_PROMPT, time_buf,
			      ctx->max_iterations);
	if (rc != 0)
		return NULL;

	rc = morph_buf_puts(&buf, MORPH_LANGUAGE_OUTPUT_PROMPT);
	if (rc != 0)
		return NULL;

	rc = morph_buf_puts(&buf, MORPH_MARKDOWN_OUTPUT_PROMPT);
	if (rc != 0)
		return NULL;

	if (ctx->workdir && *ctx->workdir) {
		rc = morph_buf_printf(&buf, "\nWorking directory: %s\n",
				      ctx->workdir);
		if (rc != 0)
			return NULL;
	}

	if (react_has_active_tool(ctx, "apply_patch")) {
		rc = morph_buf_puts(&buf,
			"\nSource editing:\n"
			"- Use apply_patch for text files inside the working directory; "
			"do not pass source content through shell commands.\n"
			"- Patch paths are relative to the Working directory shown above. "
			"Do not repeat that directory in the patch path.\n"
			"- Prefer a bare @@ for Update File hunks. If you use @@ followed "
			"by an anchor, copy one complete source line verbatim; never invent "
			"a descriptive label. Do not use unified-diff numeric ranges.\n"
			"- Every call must be a complete Codex patch: start with "
			"*** Begin Patch, use *** Add File, *** Update File, or "
			"*** Delete File with relative paths, and end with "
			"*** End Patch. The final *** End Patch line is an envelope line "
			"and must never have a leading +, space, or -. Do not emit "
			"unified-diff headers such as --- or +++.\n"
			"- Exact Update File example:\n"
			"*** Begin Patch\n"
			"*** Update File: relative/path.c\n"
			"@@\n"
			" unchanged context\n"
			"-old line\n"
			"+new line\n"
			"*** End Patch\n"
			"- Exact Add File example:\n"
			"*** Begin Patch\n"
			"*** Add File: relative/path.c\n"
			"+first content line\n"
			"+/* MORPH_CONTINUE */\n"
			"*** End Patch\n"
			"- Keep one call below 4 KiB and at most 80 changed lines. For a "
			"large new file, add a small first chunk ending in a unique "
			"continuation marker, then use later Update File calls to replace "
			"that marker with the next chunk and a fresh marker. Remove the "
			"marker in the final call.\n"
			"- If a patch is truncated or its context does not match, do not "
			"repeat the same oversized call. Read the file again and retry "
			"with a smaller complete patch.\n");
		if (rc != 0)
			return NULL;
	}

	if (react_has_active_tool(ctx, "bash_exec")) {
		rc = morph_buf_puts(&buf,
			"\nShell filesystem permissions:\n"
			"- Do not delete files, install packages, or make network calls "
			"unless the user explicitly asks.\n"
			"- Run commands with sandbox_permissions=use_default unless "
			"they need to write or delete outside workdir/output/tmp.\n"
			"- For known external paths, use "
			"sandbox_permissions=with_additional_permissions and request "
			"only the smallest absolute directories in "
			"additional_permissions.file_system.write or .delete.\n"
			"- Deletion and rename require delete permission; write "
			"permission alone is insufficient.\n"
			"- If bash_exec returns error.code=sandbox_denied, retry the "
			"same command with the narrow additional permissions indicated "
			"by the failure. Do not claim success from its exit code.\n"
			"- Use require_escalated only when narrow directory permissions "
			"cannot work; it always requires approval and is unavailable "
			"in server mode.\n");
		if (rc != 0)
			return NULL;
		if (react_has_active_tool(ctx, "request_permissions")) {
			rc = morph_buf_puts(&buf,
				"- You may call request_permissions before bash_exec "
				"when the required directories are known. Include the "
				"exact future command and use scope=turn unless repeated "
				"commands need scope=session. Grants are scoped to the "
				"command executable.\n");
			if (rc != 0)
				return NULL;
		}
	}

	if (ctx->system_prompt) {
		rc = morph_buf_printf(&buf, "%s\n", ctx->system_prompt);
		if (rc != 0)
			return NULL;
	}

	if (ctx->memory_context && ctx->memory_context[0]) {
		rc = morph_buf_printf(&buf, "\n%s\n", ctx->memory_context);
		if (rc != 0)
			return NULL;
	}

	if (ctx->skills && ctx->skills->count > 0 &&
	    react_has_active_tool(ctx, "activate_skill")) {
		rc = morph_buf_puts(&buf, "\nAvailable skills:\n");
		if (rc != 0)
			return NULL;
		for (int i = 0; i < ctx->skills->count; i++) {
			if (!ctx->skills->entries[i].enabled)
				continue;
			rc = morph_buf_printf(&buf, "- %s: %s\n",
					      ctx->skills->entries[i].fm.name,
					      ctx->skills->entries[i].fm.description);
			if (rc != 0)
				return NULL;
		}
		rc = morph_buf_puts(&buf,
			"\nWhen a skill matches the task, call activate_skill "
			"with the skill name to load its full instructions.\n");
		if (rc != 0)
			return NULL;
	}

	if (ctx->sub_agent_info && ctx->sub_agent_info_count > 0) {
		int active_sub_agents = 0;

		for (int i = 0; i < ctx->sub_agent_info_count; i++) {
			char tool_name[TOOL_NAME_MAX];

			snprintf(tool_name, sizeof(tool_name), "agent_%s",
				 ctx->sub_agent_info[i].name);
			if (react_has_active_tool(ctx, tool_name))
				active_sub_agents++;
		}
		if (active_sub_agents > 0) {
			rc = morph_buf_puts(&buf, "\nAvailable sub-agents:\n");
			if (rc != 0)
				return NULL;
			for (int i = 0; i < ctx->sub_agent_info_count; i++) {
				char tool_name[TOOL_NAME_MAX];

				snprintf(tool_name, sizeof(tool_name), "agent_%s",
					 ctx->sub_agent_info[i].name);
				if (!react_has_active_tool(ctx, tool_name))
					continue;
				rc = morph_buf_printf(&buf, "- agent_%s: %s\n",
					ctx->sub_agent_info[i].name,
					ctx->sub_agent_info[i].description);
				if (rc != 0)
					return NULL;
			}
			rc = morph_buf_puts(&buf,
				"\nTo delegate a task, call an enabled agent_<name> "
				"tool with a task description.\n");
			if (rc != 0)
				return NULL;
			if (react_has_active_tool(ctx, "fanout")) {
				rc = morph_buf_puts(&buf,
					"For parallel execution, use fanout.\n");
				if (rc != 0)
					return NULL;
			}
			if (react_has_active_tool(ctx, "delegate") &&
			    react_has_active_tool(ctx, "agent_status")) {
				rc = morph_buf_puts(&buf,
					"For async delegation, use delegate + "
					"agent_status.\n");
				if (rc != 0)
					return NULL;
			}
		}
	}

	if (ctx->ask_user_fn && react_has_active_tool(ctx, "ask_user")) {
		rc = morph_buf_puts(&buf,
			"\nYou have the ask_user tool. Use it ONLY for genuine "
			"ambiguity or irreversible decisions. Prefer acting on "
			"reasonable assumptions rather than blocking for input.\n");
		if (rc != 0)
			return NULL;
	}

	if (ctx->skills) {
		char *active = skill_build_activated_instructions(ctx->skills);
		if (active) {
			rc = morph_buf_puts(&buf, active);
			free(active);
			if (rc != 0)
				return NULL;
		}
	}

	return buf.data;
}

struct react_stream_data {
	struct react_context *ctx;
	react_output_cb user_cb;
	void *user_data;
	volatile sig_atomic_t *cancelled;
	struct arena *arena;
	char *accumulated;
	size_t acc_len;
	size_t acc_cap;
	size_t emitted_len;
	int in_final;
	int skip_legacy_final_separator;
	int provisional_typed_content;
};

static int react_final_marker_len(const char *p)
{
	if (strncmp(p, "Final:", 6) == 0)
		return 6;
	if (strncmp(p, "Final answer:", 13) == 0)
		return 13;
	if (strncmp(p, "Final Answer:", 13) == 0)
		return 13;
	return 0;
}

static int react_is_line_start(const char *base, const char *p)
{
	if (p == base)
		return 1;
	return p > base && (p[-1] == '\n' || p[-1] == '\r');
}

static const char *react_find_final_marker(const char *base, const char *from,
					   int *marker_len)
{
	const char *p = from;

	while (p && *p) {
		const char *line = p;
		while (*line == ' ' || *line == '\t')
			line++;
		if (react_is_line_start(base, p)) {
			int len = react_final_marker_len(line);
			if (len > 0) {
				*marker_len = (int)(line - p) + len;
				return p;
			}
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
	return NULL;
}

static int react_emit_thought_delta(struct react_stream_data *sd,
				    const char *text, size_t len)
{
	char *delta;

	if (len == 0)
		return 0;
	delta = arena_alloc(sd->arena, len + 1);
	if (!delta)
		return -ENOMEM;
	memcpy(delta, text, len);
	delta[len] = '\0';
	react_output_emit(sd->user_cb, sd->user_data, REACT_STEP_THOUGHT,
			  REACT_OUTPUT_DELTA, delta, NULL, NULL, NULL, 0,
			  NULL, NULL, NULL);
	react_emit_text_event(sd->ctx, MORPH_EVENT_REACT,
			      "react.thought.delta", "delta", NULL, delta);
	return 0;
}

static int react_emit_final_delta(struct react_stream_data *sd,
				  const char *text, size_t len)
{
	char *delta;

	while (sd->skip_legacy_final_separator && len > 0 &&
	       (*text == ' ' || *text == '\t')) {
		text++;
		len--;
	}
	if (len > 0)
		sd->skip_legacy_final_separator = 0;
	if (len == 0)
		return 0;
	delta = arena_alloc(sd->arena, len + 1);
	if (!delta)
		return -ENOMEM;
	memcpy(delta, text, len);
	delta[len] = '\0';
	react_emit_text_event(sd->ctx, MORPH_EVENT_REACT,
			      "react.final.delta", "delta", NULL, delta);
	return 0;
}

static size_t react_utf8_safe_len(const char *text, size_t len)
{
	while (len > 0 && utf8nvalid(text, len) != NULL)
		len--;
	return len;
}

static int react_stream_parse_emit(struct react_stream_data *sd)
{
	const size_t holdback = 16;
	const char *base = sd->accumulated;
	const char *start = base + sd->emitted_len;
	size_t available;

	if (!base || sd->acc_len <= sd->emitted_len)
		return 0;

	available = sd->acc_len - sd->emitted_len;
	if (sd->provisional_typed_content) {
		size_t safe = react_utf8_safe_len(start, available);
		int rc;

		if (safe == 0)
			return 0;
		rc = react_emit_thought_delta(sd, start, safe);
		if (rc == 0)
			sd->emitted_len += safe;
		return rc;
	}
	if (sd->in_final) {
		size_t safe = react_utf8_safe_len(start, available);
		int rc;
		if (safe == 0)
			return 0;
		rc = react_emit_final_delta(sd, start, safe);
		if (rc == 0)
			sd->emitted_len += safe;
		return rc;
	}

	{
		int marker_len = 0;
		const char *marker = react_find_final_marker(base, start,
							     &marker_len);
		if (marker) {
			int rc = react_emit_thought_delta(sd, start,
							  (size_t)(marker - start));
			if (rc != 0)
				return rc;
			sd->in_final = 1;
			sd->skip_legacy_final_separator = 1;
			sd->emitted_len = (size_t)((marker - base) + marker_len);
			return react_stream_parse_emit(sd);
		}
	}

	if (available <= holdback)
		return 0;
	available -= holdback;
	available = react_utf8_safe_len(start, available);
	if (available == 0)
		return 0;
	{
		int rc = react_emit_thought_delta(sd, start, available);
		if (rc == 0)
			sd->emitted_len += available;
		return rc;
	}
}

static int react_stream_flush(struct react_stream_data *sd)
{
	const char *base = sd->accumulated;
	const char *start;
	size_t available;

	if (!base || sd->acc_len <= sd->emitted_len)
		return 0;

	start = base + sd->emitted_len;
	available = sd->acc_len - sd->emitted_len;
	if (sd->provisional_typed_content) {
		size_t safe = react_utf8_safe_len(start, available);
		int rc;

		if (safe == 0)
			return 0;
		rc = react_emit_thought_delta(sd, start, safe);
		if (rc == 0)
			sd->emitted_len += safe;
		return rc;
	}
	if (sd->in_final) {
		size_t safe = react_utf8_safe_len(start, available);
		int rc;
		if (safe == 0)
			return 0;
		rc = react_emit_final_delta(sd, start, safe);
		if (rc == 0)
			sd->emitted_len += safe;
		return rc;
	}

	{
		int marker_len = 0;
		const char *marker = react_find_final_marker(base, start,
							     &marker_len);
		if (marker) {
			int rc = react_emit_thought_delta(sd, start,
							  (size_t)(marker - start));
			if (rc != 0)
				return rc;
			sd->in_final = 1;
			sd->skip_legacy_final_separator = 1;
			sd->emitted_len = (size_t)((marker - base) + marker_len);
			return react_stream_flush(sd);
		}
	}

	{
		size_t safe = react_utf8_safe_len(start, available);
		int rc;
		if (safe == 0)
			return 0;
		rc = react_emit_thought_delta(sd, start, safe);
		if (rc == 0)
			sd->emitted_len += safe;
		return rc;
	}
}

static int react_stream_cb(const char *token, void *user_data)
{
	struct react_stream_data *sd = user_data;
	size_t tlen = strlen(token);
	if (sd->acc_len + tlen + 1 >= sd->acc_cap) {
		size_t new_cap = (sd->acc_len + tlen + 1) * 2;
		char *new_acc = arena_alloc(sd->arena, new_cap);
		if (new_acc) {
			if (sd->accumulated) {
				memcpy(new_acc, sd->accumulated, sd->acc_len);
			}
			sd->accumulated = new_acc;
			sd->acc_cap = new_cap;
		}
	}
	if (sd->accumulated && sd->acc_len + tlen < sd->acc_cap) {
		memcpy(sd->accumulated + sd->acc_len, token, tlen);
		sd->acc_len += tlen;
		sd->accumulated[sd->acc_len] = '\0';
	}
	if (react_sigint_flag) {
		if (sd->cancelled)
			*sd->cancelled = 1;
		react_sigint_flag = 0;
	}
	if (sd->cancelled && *sd->cancelled)
		return -EINTR;
	return react_stream_parse_emit(sd);
}

static int react_typed_stream_cb(enum llm_stream_kind kind, const char *token,
				 void *user_data)
{
	struct react_stream_data *sd = user_data;

	if (kind == LLM_STREAM_REASONING) {
		if (react_sigint_flag) {
			if (sd->cancelled)
				*sd->cancelled = 1;
			react_sigint_flag = 0;
		}
		if (sd->cancelled && *sd->cancelled)
			return -EINTR;
		react_output_emit(sd->user_cb, sd->user_data,
				  REACT_STEP_REASONING, REACT_OUTPUT_DELTA,
				  token, NULL, NULL, NULL, 0, NULL,
				  NULL, NULL);
		react_emit_text_event(sd->ctx, MORPH_EVENT_REACT,
				      "react.reasoning.delta", "delta",
				      NULL, token);
		return 0;
	}
	if (kind == LLM_STREAM_CONTENT) {
		/*
		 * Structured content can still accompany native tool calls, and
		 * the response does not reveal that until streaming completes.
		 * Stream it provisionally as Thought for low latency. A response
		 * with tools keeps that classification; a response without tools
		 * is promoted by the terminal Final event.
		 */
		sd->provisional_typed_content = 1;
		return react_stream_cb(token, user_data);
	}
	return react_stream_cb(token, user_data);
}

static int count_active_tools(struct tool_registry *reg)
{
	int count = 0;
	for (int i = 0; i < reg->count; i++) {
		if (!tool_is_disabled(reg, reg->entries[i].desc.name))
			count++;
	}
	return count;
}

static void collect_active_tools(struct tool_registry *reg,
				 struct tool_desc *out, int max_count)
{
	int idx = 0;
	for (int i = 0; i < reg->count && idx < max_count; i++) {
		if (!tool_is_disabled(reg, reg->entries[i].desc.name)) {
			out[idx] = reg->entries[i].desc;
			snprintf(out[idx].description, sizeof(out[idx].description),
				 "[%s] %s",
				 tool_origin_name(reg->entries[i].origin),
				 reg->entries[i].desc.description);
			idx++;
		}
	}
}

static struct chat_message *react_push_chat_message(morph_array_t *messages)
{
	struct chat_message *m;

	m = morph_array_push(messages);
	if (!m)
		return NULL;
	memset(m, 0, sizeof(*m));
	return m;
}

static int react_append_tool_message(morph_array_t *messages,
				     const char *content,
				     const char *tool_call_id,
				     struct arena *arena)
{
	struct chat_message *m;

	m = react_push_chat_message(messages);
	if (!m)
		return -ENOMEM;
	m->role = arena_strdup(arena, "tool");
	m->content = arena_strdup(arena, content);
	m->tool_call_id = arena_strdup(arena, tool_call_id);
	m->tool_calls = NULL;
	m->tool_call_count = 0;
	return 0;
}

static int react_prepare_tool_call_ids(struct chat_response *response)
{
	if (!response)
		return -EINVAL;
	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *tc = &response->tool_calls[i];
		int rc;

		if (!tc->id[0]) {
			rc = morph_random_id("pc_", tc->id, sizeof(tc->id));
			if (rc < 0)
				return rc;
		}
		if (!tc->tool_call_id[0]) {
			rc = morph_random_id("tc_", tc->tool_call_id,
					     sizeof(tc->tool_call_id));
			if (rc < 0)
				return rc;
		}
	}
	return 0;
}

static int react_normalize_tool_inputs(struct react_context *ctx,
				       struct chat_response *response)
{
	if (!ctx || !response)
		MORPH_RETURN(-EINVAL);
	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *call = &response->tool_calls[i];
		struct tool_entry *entry = tool_lookup(ctx->tools, call->name);

		if (!entry || entry->desc.input_kind == TOOL_INPUT_JSON) {
			call->input_kind = TOOL_INPUT_JSON;
			continue;
		}
		if (call->input_kind == TOOL_INPUT_TEXT)
			continue;
		cJSON *root = cJSON_Parse(call->arguments ?
			call->arguments : "");
		cJSON *input = root ? cJSON_GetObjectItem(root, "input") : NULL;

		if (!cJSON_IsString(input) || !input->valuestring) {
			cJSON_Delete(root);
			call->input_kind = TOOL_INPUT_TEXT;
			continue;
		}
		char *normalized = response->arena
			? arena_strdup(response->arena, input->valuestring)
			: strdup(input->valuestring);
		cJSON_Delete(root);
		if (!normalized)
			MORPH_RETURN(-ENOMEM);
		if (!response->arena)
			free(call->arguments);
		call->arguments = normalized;
		call->input_kind = TOOL_INPUT_TEXT;
	}
	return 0;
}

/*
 * Check HITL approval for each tool call in a response.
 *
 * Iterates over tool calls and queries the approval callback for any
 * tool that requires human approval.  Denied tools are recorded in
 * slots and their action/observation steps are added to the
 * ReAct trace.
 *
 * ctx - ReAct context (HITL config and step list)
 * response - LLM response containing tool calls
 * slots - Output array (one slot per tool call)
 * cb - Output callback for step notifications
 * user_data - Opaque pointer passed to cb
 */
static void react_check_hitl_approvals(struct react_context *ctx,
					struct chat_response *response,
					struct react_tool_slot *slots,
					react_output_cb cb,
					void *user_data)
{
	(void)cb;
	(void)user_data;
	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *tc = &response->tool_calls[i];
		const char *tool_name = tc->name;
		const char *tool_args = tc->arguments ? tc->arguments : "{}";
		if (!hitl_needs_approval(ctx, tool_name))
			continue;
		if (!ctx->hitl.approval_cb)
			continue;
		react_emit_hitl_event(ctx, "hitl.request", "begin",
				      "approval requested", tool_name,
				      tool_args, tc->tool_call_id, NULL);
		enum hitl_verdict v = ctx->hitl.approval_cb(
			tool_name, tool_args, ctx->hitl.approval_user_data);
		if (v == HITL_ALWAYS) {
			hitl_add_auto_approved(&ctx->hitl, tool_name);
			react_emit_hitl_event(ctx, "hitl.always", "end",
					      "approval persisted",
					      tool_name, tool_args,
					      tc->tool_call_id,
					      "always");
		} else if (v == HITL_DENY) {
			slots[i].hitl_denied = 1;
			react_emit_hitl_event(ctx, "hitl.denied", "failed",
					      "approval denied", tool_name,
					      tool_args, tc->tool_call_id,
					      "denied");
			char deny_msg[512];
			snprintf(deny_msg, sizeof(deny_msg),
				 "tool error: '%s' execution denied by user",
				 tool_name);
			size_t at_len = strlen(tool_name) + strlen(tool_args) + 4;
			char *action_text = arena_alloc(ctx->turn_arena, at_len);
			if (action_text)
				snprintf(action_text, at_len,
					 "%s(%s)", tool_name, tool_args);
			struct react_step *action = react_step_create(
				ctx->turn_arena, REACT_STEP_ACTION,
				action_text ? action_text : "",
				tool_name, tool_args, tc->tool_call_id);
			add_step(ctx, action);
			struct react_step *obs = react_step_create(
				ctx->turn_arena, REACT_STEP_OBSERVATION,
				deny_msg, NULL, NULL, NULL);
			add_step(ctx, obs);
			react_emit_text_event(ctx, MORPH_EVENT_REACT,
					      "react.observation", "failed",
					      "tool execution denied",
					      deny_msg);
		} else {
			react_emit_hitl_event(ctx, "hitl.approved", "end",
					      "approval granted", tool_name,
					      tool_args, tc->tool_call_id,
					      "approved");
		}
	}
}

/*
 * Track consecutive tool failures and force a Final step if the retry
 * limit is exceeded.
 *
 * On success (rc >= 0), the failure counters are reset.
 * On failure (rc < 0), the counters are updated and checked against
 * ctx->tool_max_retries.
 *
 * Returns 1 if max retries reached (caller should abort the loop),
 * 0 otherwise.
 */
static int react_track_tool_failure(struct react_context *ctx,
				     const char *tool_name,
				     const char *tool_args,
				     int rc,
				     react_output_cb cb,
				     void *user_data)
{
	if (rc < 0) {
		react_set_state(ctx, REACT_STATE_TOOL_FAIL);
		if (ctx->tool_fail_name &&
		    strcmp(tool_name, ctx->tool_fail_name) == 0 &&
		    ctx->tool_fail_args &&
		    strcmp(tool_args, ctx->tool_fail_args) == 0) {
			ctx->tool_fail_count++;
		} else {
			ctx->tool_fail_name = arena_strdup(ctx->turn_arena, tool_name);
			ctx->tool_fail_args = arena_strdup(ctx->turn_arena, tool_args);
			ctx->tool_fail_count = 1;
		}
	} else {
		ctx->tool_fail_name = NULL;
		ctx->tool_fail_args = NULL;
		ctx->tool_fail_count = 0;
	}
	if (ctx->tool_fail_count < ctx->tool_max_retries)
		return 0;
	const char *fail_name = ctx->tool_fail_name ? ctx->tool_fail_name : "(unknown)";
	log_warn("react_run: tool '%s' failed %d times consecutively, forcing Final",
		 fail_name, ctx->tool_fail_count);
	char fail_msg[256];
	snprintf(fail_msg, sizeof(fail_msg),
		 "Tool '%s' repeatedly failed. Please try a different approach.",
		 fail_name);
	struct react_step *final_step = react_step_create(ctx->turn_arena,
		REACT_STEP_FINAL, fail_msg, NULL, NULL, NULL);
	add_step(ctx, final_step);
	free(ctx->final_answer);
	ctx->final_answer = strdup(fail_msg);
	react_set_state(ctx, REACT_STATE_DONE);
	react_output_emit(cb, user_data, REACT_STEP_FINAL,
			  REACT_OUTPUT_COMPLETED, fail_msg, NULL, NULL,
			  NULL, 0, NULL, NULL, NULL);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.final",
			      "end", "final answer", fail_msg);
	ctx->tool_fail_name = NULL;
	ctx->tool_fail_args = NULL;
	ctx->tool_fail_count = 0;
	return 1;
}

/*
 * Evaluate the guardrail for a proposed final answer.
 *
 * Returns 0 guardrail passed (caller should finalize),
 *         1 guardrail failed - revision messages appended, caller should
 *           free response and continue the iteration loop,
 *         2 repeated empty responses caused a terminal LLM error,
 *        -ENOMEM allocation failure (caller should abort).
 */
static int react_handle_guardrail_retry(struct react_context *ctx,
					const char *proposed,
					morph_array_t *messages,
					react_output_cb cb,
					void *user_data)
{
	struct chat_message *slots;
	struct chat_message *asst_msg;
	struct chat_message *user_msg;
	int answer_is_empty;

	answer_is_empty = !proposed || !*proposed ||
		strcmp(proposed, "(no response)") == 0;
	if (!ctx->guardrail.enabled && !answer_is_empty)
		return 0;
	if (answer_is_empty) {
		ctx->empty_round_count++;
	} else {
		ctx->empty_round_count = 0;
		if (ctx->guardrail_retry_count >= ctx->guardrail.max_retries)
			return 0;
	}

	react_set_state(ctx, REACT_STATE_GUARDRAIL);

	struct guardrail_eval_ctx eval = {
		.proposed_answer = proposed,
		.steps = ctx->steps,
		.empty_round_count = ctx->empty_round_count,
		.max_empty_rounds = ctx->guardrail.max_empty_rounds,
		.arena = ctx->turn_arena,
	};
	struct guardrail_result gr = guardrail_run_hook(
		&ctx->guardrail, GUARDRAIL_HOOK_OUTPUT, &eval);
	if (answer_is_empty && gr.verdict == GUARDRAIL_PASS) {
		gr.verdict = GUARDRAIL_FAIL;
		snprintf(gr.reason, sizeof(gr.reason),
			 "The model returned an empty response.");
	}

	if (gr.verdict == GUARDRAIL_PASS) {
		log_info("guardrail: PASS");
		return 0;
	}
	if (answer_is_empty &&
	    (ctx->guardrail_retry_count >= ctx->guardrail.max_retries ||
	     (ctx->guardrail.max_empty_rounds > 0 &&
	      ctx->empty_round_count >= ctx->guardrail.max_empty_rounds))) {
		const char *detail =
			"LLM repeatedly returned an empty response.";

		log_warn("guardrail: %s", detail);
		free(ctx->final_answer);
		ctx->final_answer = strdup(detail);
		if (!ctx->final_answer)
			MORPH_RETURN(-ENOMEM);
		react_set_error_detail(ctx, detail);
		react_set_result(ctx, REACT_OUTCOME_LLM_ERROR,
				 MORPH_ERR_LLM, "empty_response");
		return 2;
	}

	ctx->guardrail_retry_count++;
	log_info("guardrail: %s (attempt %d/%d)",
		 gr.reason, ctx->guardrail_retry_count,
		 ctx->guardrail.max_retries);

	struct react_step *refl_step = react_step_create(ctx->turn_arena,
		REACT_STEP_REFLECTION, gr.reason, NULL, NULL, NULL);
	add_step(ctx, refl_step);
	react_output_emit(cb, user_data, REACT_STEP_REFLECTION,
			  REACT_OUTPUT_FAILED, gr.reason, NULL, NULL,
			  NULL, 0, NULL, NULL, NULL);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.reflection",
			      "failed", "guardrail reflection", gr.reason);

	slots = morph_array_push_n(messages, 2);
	if (!slots)
		MORPH_RETURN(-ENOMEM);
	memset(slots, 0, 2 * sizeof(*slots));
	asst_msg = &slots[0];
	user_msg = &slots[1];

	asst_msg->role = arena_strdup(ctx->turn_arena, "assistant");
	asst_msg->content = arena_strdup(ctx->turn_arena, proposed);
	asst_msg->tool_call_id = NULL;
	asst_msg->tool_calls = NULL;
	asst_msg->tool_call_count = 0;

	const char *action = (gr.triggered_rule &&
			      gr.triggered_rule->action_text[0])
		? gr.triggered_rule->action_text
		: "Try again using the available tools.";

	size_t rev_cap = strlen(gr.reason) + strlen(action) + 64;
	char *rev_msg = arena_alloc(ctx->turn_arena, rev_cap);
	if (rev_msg) {
		snprintf(rev_msg, rev_cap,
			 "Quality check failed: %s\n%s",
			 gr.reason, action);
	}
	user_msg->role = arena_strdup(ctx->turn_arena, "user");
	user_msg->content = rev_msg ? rev_msg :
		arena_strdup(ctx->turn_arena,
			     "Please revise your answer using the available tools.");
	user_msg->tool_call_id = NULL;
	user_msg->tool_calls = NULL;
	user_msg->tool_call_count = 0;
	return 1;
}

static int react_check_input_guardrail(struct react_context *ctx,
				       const char *user_input)
{
	struct guardrail_eval_ctx eval;
	struct guardrail_result gr;
	const char *action;
	char msg[2048];
	struct react_step *refl;

	if (!ctx->guardrail.enabled)
		return 0;

	memset(&eval, 0, sizeof(eval));
	eval.user_input = user_input;
	eval.steps = NULL;
	eval.arena = ctx->turn_arena;
	gr = guardrail_run_hook(&ctx->guardrail, GUARDRAIL_HOOK_INPUT, &eval);
	if (gr.verdict != GUARDRAIL_FAIL)
		return 0;

	log_info("guardrail[input]: %s", gr.reason);
	action = (gr.triggered_rule && gr.triggered_rule->action_text[0])
		? gr.triggered_rule->action_text : "";
	snprintf(msg, sizeof(msg), "Input rejected: %s\n%s",
		 gr.reason, action);
	refl = react_step_create(ctx->turn_arena, REACT_STEP_REFLECTION,
				 msg, NULL, NULL, NULL);
	add_step(ctx, refl);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.reflection",
			      "failed", "input rejected", msg);
	free(ctx->final_answer);
	ctx->final_answer = strdup(msg);
	react_set_result(ctx, REACT_OUTCOME_GUARDRAIL_DENIED,
			 -EPERM, "guardrail_denied");
	return 1;
}

static int react_prepare_active_tools(struct react_context *ctx,
				      struct tool_desc **active_tools,
				      int *active_tool_count)
{
	int has_tools;

	if (!active_tools || !active_tool_count)
		return -EINVAL;
	*active_tools = NULL;
	*active_tool_count = 0;
	has_tools = ctx->tools && ctx->tools->count > 0;
	*active_tool_count = has_tools ? count_active_tools(ctx->tools) : 0;
	if (*active_tool_count <= 0)
		return 0;
	*active_tools = arena_alloc(ctx->turn_arena,
				    (size_t)*active_tool_count *
				    sizeof(**active_tools));
	if (!*active_tools)
		return -ENOMEM;
	collect_active_tools(ctx->tools, *active_tools, *active_tool_count);
	return 0;
}

static int react_count_text_tokens(struct react_context *ctx,
				   const char *text)
{
	if (!text || !text[0])
		return 0;
	return tokenizer_count(ctx->tokenizer, text);
}

static int react_estimate_active_tokens(struct react_context *ctx,
	const char *system_prompt, const morph_array_t *messages,
	const struct tool_desc *tools, int tool_count)
{
	int64_t total = react_count_text_tokens(ctx, system_prompt) + 16;

	if (messages) {
		for (size_t i = 0; i < messages->nelts; i++) {
			const struct chat_message *message =
				morph_array_get(messages, i);

			if (!message)
				continue;
			total += 12;
			total += react_count_text_tokens(ctx, message->role);
			total += react_count_text_tokens(ctx, message->content);
			total += react_count_text_tokens(ctx,
				message->reasoning_content);
			total += react_count_text_tokens(ctx,
				message->tool_call_id);
			for (int j = 0; j < message->tool_call_count; j++) {
				const struct tool_call *call = &message->tool_calls[j];

				total += 16;
				total += react_count_text_tokens(ctx, call->id);
				total += react_count_text_tokens(ctx, call->name);
				total += react_count_text_tokens(ctx,
					call->arguments);
			}
		}
	}
	for (int i = 0; tools && i < tool_count; i++) {
		total += 24;
		total += react_count_text_tokens(ctx, tools[i].name);
		total += react_count_text_tokens(ctx, tools[i].title);
		total += react_count_text_tokens(ctx, tools[i].description);
		total += react_count_text_tokens(ctx, tools[i].input_schema);
		total += react_count_text_tokens(ctx, tools[i].output_schema);
		total += react_count_text_tokens(ctx, tools[i].input_format);
	}
	if (total > INT_MAX)
		return INT_MAX;
	return (int)total;
}

static int react_compaction_trigger_tokens(struct react_context *ctx,
					   const struct model *llm)
{
	int limit = ctx->compress.max_context_tokens;
	int protocol = ctx->compress.protocol_reserve_tokens;
	int output = llm && llm->max_tokens > 0 ? llm->max_tokens : 4096;
	int ratio_limit;
	int hard_limit;

	if (limit <= 0)
		return INT_MAX;
	if (protocol < 0)
		protocol = 0;
	if (output > limit / 2)
		output = limit / 2;
	ratio_limit = (int)((double)limit *
		ctx->compress.summarize_threshold_ratio);
	hard_limit = limit - output - protocol;
	if (hard_limit < 1)
		hard_limit = 1;
	if (ratio_limit < 1 || ratio_limit > hard_limit)
		return hard_limit;
	return ratio_limit;
}

static int react_emit_compaction_event(struct react_context *ctx,
	const char *name, const char *phase, int iteration, int before_tokens,
	int after_tokens, int trigger_tokens, int error_code)
{
	cJSON *data;
	int rc;

	if (!react_events_enabled(ctx))
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		MORPH_RETURN(-ENOMEM);
	cJSON_AddNumberToObject(data, "iteration", iteration);
	cJSON_AddNumberToObject(data, "before_tokens", before_tokens);
	cJSON_AddNumberToObject(data, "after_tokens", after_tokens);
	cJSON_AddNumberToObject(data, "trigger_tokens", trigger_tokens);
	cJSON_AddNumberToObject(data, "compaction_count",
		ctx->in_turn_compaction_count);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
			morph_strerror(error_code));
	}
	rc = react_emit_event(ctx, MORPH_EVENT_REACT, name, phase,
		strcmp(phase, "end") == 0 ? "active context compacted" :
		"active context compaction", data);
	cJSON_Delete(data);
	return rc;
}

static int react_rebuild_history_messages(struct react_context *ctx,
					  morph_array_t *messages)
{
	int rc;

	morph_array_cleanup(messages);
	memset(messages, 0, sizeof(*messages));
	rc = morph_array_init(messages, 64, sizeof(struct chat_message));
	if (rc < 0)
		MORPH_RETURN(rc);
	return agent_history_build_chat_messages(ctx->history_items, messages,
		ctx->turn_arena);
}

static int react_maybe_compact_active_window(struct react_context *ctx,
	struct model *llm, const char *system_prompt, morph_array_t *messages,
	const struct tool_desc *tools, int tool_count, int iteration)
{
	char trigger_kind[64];
	int before_tokens;
	int after_tokens;
	int trigger_tokens;
	int previous_rounds;
	int compacted = 0;
	int rc;

	if (!ctx->compress.in_turn_compaction || !ctx->history_enabled ||
	    !ctx->history_db)
		return 0;
	before_tokens = react_estimate_active_tokens(ctx, system_prompt,
		messages, tools, tool_count);
	trigger_tokens = react_compaction_trigger_tokens(ctx, llm);
	if (before_tokens < trigger_tokens)
		return 0;
	ctx->in_turn_compaction_count++;
	snprintf(trigger_kind, sizeof(trigger_kind), "in_turn_%d",
		ctx->in_turn_compaction_count);
	log_info("react: in-turn compaction at iteration %d (%d >= %d tokens)",
		 iteration, before_tokens, trigger_tokens);
	(void)react_emit_compaction_event(ctx, "react.compaction.begin",
		"begin", iteration, before_tokens, before_tokens,
		trigger_tokens, 0);
	rc = agent_history_reload(ctx);
	if (rc == 0) {
		previous_rounds = ctx->compress.max_history_rounds;
		ctx->compress.max_history_rounds = 0;
		rc = agent_history_compact_with_trigger(ctx, 1, trigger_kind);
		ctx->compress.max_history_rounds = previous_rounds;
	}
	if (rc > 0) {
		compacted = 1;
		rc = react_rebuild_history_messages(ctx, messages);
	}
	if (rc < 0 || !compacted) {
		if (rc >= 0)
			rc = MORPH_ERR_PROTOCOL;
		log_err("react: in-turn compaction failed: %s",
			morph_strerror(rc));
		(void)react_emit_compaction_event(ctx, "react.compaction.failed",
			"failed", iteration, before_tokens, before_tokens,
			trigger_tokens, rc);
		MORPH_RETURN(rc);
	}
	after_tokens = react_estimate_active_tokens(ctx, system_prompt,
		messages, tools, tool_count);
	if (after_tokens >= trigger_tokens) {
		rc = MORPH_ERR_LLM;
		log_err("react: active context remains over budget after "
			"compaction (%d >= %d tokens)", after_tokens,
			trigger_tokens);
		(void)react_emit_compaction_event(ctx,
			"react.compaction.failed", "failed", iteration,
			before_tokens, after_tokens, trigger_tokens, rc);
		MORPH_RETURN(rc);
	}
	log_info("react: in-turn compaction completed (%d -> %d tokens)",
		 before_tokens, after_tokens);
	(void)react_emit_compaction_event(ctx, "react.compaction.completed",
		"end", iteration, before_tokens, after_tokens,
		trigger_tokens, 0);
	return 1;
}

static int react_prepare_messages(struct react_context *ctx,
				  morph_array_t *messages,
				  const char *current_user_input)
{
	struct message_list *hist;
	struct chat_message *message;

	if (!messages)
		return -EINVAL;
	if (morph_array_init(messages, 64, sizeof(struct chat_message)) < 0)
		return -ENOMEM;
	if (ctx->history_enabled) {
		int rc = agent_history_build_chat_messages(ctx->history_items,
			messages, ctx->turn_arena);

		if (rc != 0)
			return rc;
		message = react_push_chat_message(messages);
		if (!message)
			return -ENOMEM;
		message->role = arena_strdup(ctx->turn_arena, "user");
		message->content = arena_strdup(ctx->turn_arena,
			current_user_input ? current_user_input : "");
		if (!message->role || !message->content)
			return -ENOMEM;
		return 0;
	}

	hist = ctx->messages;
	while (hist) {
		message = react_push_chat_message(messages);
		if (!message)
			return -ENOMEM;
		message->role = arena_strdup(ctx->turn_arena, hist->role);
		message->content = hist->content ?
			arena_strdup(ctx->turn_arena, hist->content) :
			arena_strdup(ctx->turn_arena, "");
		message->tool_call_id = NULL;
		message->tool_calls = NULL;
		message->tool_call_count = 0;
		hist = hist->next;
	}
	return 0;
}

static void react_complete_max_iterations(struct react_context *ctx)
{
	struct react_step *last_obs = NULL;
	struct react_step *cur;

	if (ctx->state == REACT_STATE_DONE || ctx->state == REACT_STATE_ABORT)
		return;

	log_warn("react_run: max iterations (%d) reached, aborting",
		 ctx->max_iterations);
	if (!ctx->final_answer) {
		cur = ctx->steps;
		while (cur) {
			if (cur->type == REACT_STEP_OBSERVATION)
				last_obs = cur;
			cur = cur->next;
		}
		if (last_obs && last_obs->content && last_obs->error_code >= 0) {
			ctx->final_answer = strdup(last_obs->content);
		} else {
			ctx->final_answer = strdup(
				"Maximum iterations reached. "
				"No final answer produced.");
		}
	}
	react_set_result(ctx, REACT_OUTCOME_MAX_ITERATIONS,
			 MORPH_ERR_REACT_MAX_ITERATIONS,
			 "max_iterations");
}

static void react_append_final_message(struct react_context *ctx)
{
	const char *answer;
	struct message_list *asst;

	if (ctx->state != REACT_STATE_DONE || !ctx->steps)
		return;
	answer = ctx->final_answer ? ctx->final_answer : "(no answer)";
	asst = msg_list_create(ctx->session_arena, "assistant", answer,
			       tokenizer_count(ctx->tokenizer, answer));
	msg_list_append(&ctx->messages, asst);
}

static int react_chat_once(struct react_context *ctx, struct model *llm,
			   const char *system_prompt,
			   struct chat_message *messages, int msg_count,
			   struct tool_desc *active_tools,
			   int active_tool_count,
			   react_output_cb cb, void *user_data,
			   struct chat_response *response)
{
	struct react_stream_data sd;
	int status;

	memset(&sd, 0, sizeof(sd));
	sd.ctx = ctx;
	sd.user_cb = cb;
	sd.user_data = user_data;
	sd.cancelled = &ctx->cancelled;
	sd.arena = ctx->turn_arena;
	sd.accumulated = arena_alloc(ctx->turn_arena, 8192);
	sd.acc_len = 0;
	sd.acc_cap = 8192;
	if (sd.accumulated)
		sd.accumulated[0] = '\0';

	http_set_interrupt_check(ctx->prompt_pending_fn,
				 ctx->action_drain_user_data);
	if (llm->chat_with_tools_stream) {
		status = llm->chat_with_tools_stream(
			llm, ctx->turn_arena, system_prompt, messages,
			msg_count, active_tools, active_tool_count, response,
			react_typed_stream_cb, &sd);
	} else if (llm->chat_with_tools) {
		status = llm->chat_with_tools(llm, ctx->turn_arena,
					      system_prompt, messages, msg_count,
					      active_tools, active_tool_count,
					      response, react_stream_cb, &sd);
	} else {
		int hist_n = 0;
		struct message_list *h = ctx->messages;
		const char **hist_msgs;

		while (h) {
			hist_n++;
			h = h->next;
		}
		hist_msgs = arena_alloc(ctx->turn_arena,
					(size_t)hist_n * sizeof(*hist_msgs));
		if (!hist_msgs && hist_n > 0) {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ENOMEM, "internal_error");
			http_set_interrupt_check(NULL, NULL);
			MORPH_RETURN(-ENOMEM);
		}
		if (hist_msgs) {
			h = ctx->messages;
			for (int i = 0; i < hist_n && h; i++) {
				hist_msgs[i] = h->content ? h->content : "";
				h = h->next;
			}
		}
		status = llm->chat(llm, ctx->turn_arena, system_prompt,
				   hist_msgs, hist_n, NULL, react_stream_cb,
				   &sd);
		if (status >= 0 && sd.accumulated) {
			response->content = sd.accumulated;
			sd.accumulated = NULL;
			response->arena = ctx->turn_arena;
		}
	}
	http_set_interrupt_check(NULL, NULL);
	if (status >= 0) {
		int flush_rc = react_stream_flush(&sd);
		if (flush_rc != 0)
			return flush_rc;
	}
	return status;
}

static int react_handle_llm_cancelled(struct react_context *ctx,
				      struct chat_response *response)
{
	struct react_step *obs;

	if (!ctx->cancelled)
		return 0;
	log_info("react_run: cancelled during LLM call");
	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				"LLM call interrupted by user",
				NULL, NULL, NULL);
	add_step(ctx, obs);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.observation",
			      "cancelled", "LLM call interrupted",
			      "LLM call interrupted by user");
	if (response->content) {
		free(ctx->final_answer);
		ctx->final_answer = strdup(response->content);
	}
	react_set_result(ctx, REACT_OUTCOME_CANCELLED, -ECANCELED,
			 "user_cancelled");
	return 1;
}

static int react_handle_llm_error(struct react_context *ctx,
				  struct chat_response *response,
				  int status, react_output_cb cb,
				  void *user_data)
{
	const char *err_content = "LLM call failed";
	struct react_step *err;

	if (status >= 0)
		return 0;
	log_err("react_run: LLM call failed: %d", status);
	if (response->content && *response->content)
		err_content = response->content;
	err = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				err_content, NULL, NULL, NULL);
	add_step(ctx, err);
	react_output_emit(cb, user_data, REACT_STEP_OBSERVATION,
			  REACT_OUTPUT_FAILED, err_content, NULL, NULL,
			  NULL, status, NULL, NULL, NULL);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.observation",
			      "failed", "LLM call failed", err_content);
	react_set_result(ctx, REACT_OUTCOME_LLM_ERROR, status, "llm_error");
	return 1;
}

static int react_append_assistant_tool_call_message(
	struct react_context *ctx, morph_array_t *messages,
	struct chat_response *response)
{
	struct chat_message *asst_msg;

	asst_msg = react_push_chat_message(messages);
	if (!asst_msg)
		return -ENOMEM;
	asst_msg->role = arena_strdup(ctx->turn_arena, "assistant");
	asst_msg->content = (response->content && *response->content)
		? arena_strdup(ctx->turn_arena, response->content) : NULL;
	asst_msg->reasoning_content = response->reasoning_content
		? arena_strdup(ctx->turn_arena, response->reasoning_content) : NULL;
	asst_msg->tool_calls = arena_alloc(ctx->turn_arena,
		(size_t)response->tool_call_count *
		sizeof(*asst_msg->tool_calls));
	if (!asst_msg->tool_calls)
		return -ENOMEM;
	asst_msg->tool_call_count = response->tool_call_count;
	for (int j = 0; j < response->tool_call_count; j++) {
		char *arguments;
		int rc;

		strncpy(asst_msg->tool_calls[j].id,
			response->tool_calls[j].id,
			sizeof(asst_msg->tool_calls[j].id) - 1);
		asst_msg->tool_calls[j].id[
			sizeof(asst_msg->tool_calls[j].id) - 1] = '\0';
		strncpy(asst_msg->tool_calls[j].name,
			response->tool_calls[j].name,
			sizeof(asst_msg->tool_calls[j].name) - 1);
		asst_msg->tool_calls[j].name[
			sizeof(asst_msg->tool_calls[j].name) - 1] = '\0';
		asst_msg->tool_calls[j].input_kind =
			response->tool_calls[j].input_kind;
		if (response->tool_calls[j].input_kind == TOOL_INPUT_TEXT) {
			arguments = strdup(response->tool_calls[j].arguments ?
				response->tool_calls[j].arguments : "");
			if (!arguments)
				return -ENOMEM;
		} else {
			rc = agent_history_normalize_tool_arguments(
				response->tool_calls[j].arguments, &arguments);
			if (rc != 0)
				return rc;
		}
		asst_msg->tool_calls[j].arguments =
			arena_strdup(ctx->turn_arena, arguments);
		free(arguments);
		if (!asst_msg->tool_calls[j].arguments)
			return -ENOMEM;
	}
	return 0;
}

static void react_start_tool_calls(struct react_context *ctx,
				   struct chat_response *response,
				   struct react_tool_slot *slots,
				   react_output_cb cb, void *user_data)
{
	react_check_hitl_approvals(ctx, response, slots, cb, user_data);

	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *tc;
		const char *tool_name;
		const char *tool_args;
		size_t at_len;
		char *action_text;
		struct react_step *action;

		if (slots[i].hitl_denied)
			continue;

		tc = &response->tool_calls[i];
		tool_name = tc->name;
		tool_args = tc->arguments ? tc->arguments : "{}";
		at_len = strlen(tool_name) + strlen(tool_args) + 4;
		action_text = arena_alloc(ctx->turn_arena, at_len);
		if (action_text)
			snprintf(action_text, at_len, "%s(%s)",
				 tool_name, tool_args);
		action = react_step_create(ctx->turn_arena, REACT_STEP_ACTION,
					   action_text ? action_text : "",
					   tool_name, tool_args,
					   tc->tool_call_id);
		add_step(ctx, action);
		react_output_emit(cb, user_data, REACT_STEP_ACTION,
				  REACT_OUTPUT_STARTED,
				  action_text ? action_text : "", tool_name,
				  tool_args, tc->tool_call_id, 0, NULL,
				  NULL, NULL);
		react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.action",
				      "begin", "tool action",
				      action_text ? action_text : "");
		react_tool_call_begin(ctx, tc);

		slots[i].call = async_tool_call_create(ctx->tools, ctx,
						       tool_name, tool_args,
						       tc->tool_call_id,
						       tc->id, cb, user_data);
		if (!slots[i].call) {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ENOMEM, "internal_error");
			break;
		}

		if (pthread_create(&slots[i].thread, NULL, async_tool_exec,
				   slots[i].call) != 0) {
			react_tool_call_thread_failed(ctx, tc, -EAGAIN);
			async_tool_call_destroy(slots[i].call);
			slots[i].call = NULL;
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -EAGAIN, "tool_thread_failed");
			break;
		}
		slots[i].thread_started = 1;
	}

	for (int i = 0; i < response->tool_call_count; i++) {
		struct tool_call *tc;

		if (slots[i].hitl_denied || !slots[i].call ||
		    !slots[i].thread_started)
			continue;
		tc = &response->tool_calls[i];
		react_tool_call_running(ctx, tc);
		pthread_mutex_lock(&slots[i].call->mutex);
		slots[i].call->start_released = 1;
		pthread_cond_broadcast(&slots[i].call->cond);
		pthread_mutex_unlock(&slots[i].call->mutex);
	}
}

static void react_cancel_remaining_tool_calls(struct react_tool_slot *slots,
					      int start, int end)
{
	for (int j = start; j < end; j++) {
		if (slots[j].call && slots[j].thread_started) {
			pthread_mutex_lock(&slots[j].call->mutex);
			slots[j].call->cancelled = 1;
			morph_cancel_token_cancel(
				&slots[j].call->cancel_token);
			slots[j].call->detached = 1;
			pthread_mutex_unlock(&slots[j].call->mutex);
			pthread_detach(slots[j].thread);
			slots[j].call = NULL;
			slots[j].thread_started = 0;
		}
	}
}

static void react_cleanup_tool_calls(struct react_tool_slot *slots, int count)
{
	for (int i = 0; i < count; i++) {
		if (!slots[i].call)
			continue;
		if (!slots[i].call->completed && slots[i].thread_started) {
			pthread_mutex_lock(&slots[i].call->mutex);
			slots[i].call->cancelled = 1;
			morph_cancel_token_cancel(
				&slots[i].call->cancel_token);
			slots[i].call->detached = 1;
			pthread_mutex_unlock(&slots[i].call->mutex);
			pthread_detach(slots[i].thread);
			slots[i].call = NULL;
			slots[i].thread_started = 0;
		} else {
			async_tool_call_destroy(slots[i].call);
			slots[i].call = NULL;
			slots[i].thread_started = 0;
		}
	}
}

static int react_append_denied_tool_message(struct react_context *ctx,
					    struct tool_call *tc,
					    morph_array_t *messages)
{
	return react_append_tool_message(messages,
					 "tool error: execution denied by user",
					 tc->id, ctx->turn_arena);
}

static void react_record_tool_cancelled(struct react_context *ctx)
{
	struct react_step *obs;

	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				"Tool execution cancelled by user",
				NULL, NULL, NULL);
	add_step(ctx, obs);
	react_emit_text_event(ctx, MORPH_EVENT_REACT,
			      "react.observation", "cancelled",
			      "tool execution cancelled",
			      "Tool execution cancelled by user");
	react_set_result(ctx, REACT_OUTCOME_CANCELLED, -ECANCELED,
			 "tool_cancelled");
}

static void react_collect_tool_result(struct async_tool_call *call,
				      int *rc, char **result,
				      struct tool_artifact_list *artifacts,
				      cJSON **data, cJSON **ui, cJSON **meta)
{
	pthread_mutex_lock(&call->mutex);
	*rc = call->rc;
	*result = call->result;
	if (artifacts)
		*artifacts = call->artifacts;
	if (data)
		*data = call->data;
	if (ui)
		*ui = call->ui;
	if (meta)
		*meta = call->meta;
	call->result = NULL;
	call->data = NULL;
	call->ui = NULL;
	call->meta = NULL;
	memset(&call->artifacts, 0, sizeof(call->artifacts));
	pthread_mutex_unlock(&call->mutex);
}

static const char *react_apply_tool_output_guardrail(
	struct react_context *ctx, struct async_tool_call *call,
	const char *obs_text, int rc,
	const struct tool_artifact_list *artifacts,
	react_output_cb cb, void *user_data)
{
	struct guardrail_eval_ctx geval;
	struct guardrail_result ggr;
	const char *gaction;
	char guard_obs[2048];
	struct react_step *grefl;

	if (!ctx->guardrail.enabled)
		return obs_text;

	memset(&geval, 0, sizeof(geval));
	geval.tool_name = call->tool_name;
	geval.tool_args = call->tool_args;
	geval.tool_result = obs_text;
	geval.tool_error_code = rc;
	geval.tool_artifacts = artifacts;
	geval.steps = ctx->steps;
	geval.arena = ctx->turn_arena;
	ggr = guardrail_run_hook(&ctx->guardrail, GUARDRAIL_HOOK_TOOL_OUTPUT,
				 &geval);
	if (ggr.verdict != GUARDRAIL_FAIL)
		return obs_text;

	log_info("guardrail[tool_output]: %s", ggr.reason);
	gaction = (ggr.triggered_rule &&
		   ggr.triggered_rule->action_text[0])
		? ggr.triggered_rule->action_text
		: "Verify tool parameters and try again.";
	snprintf(guard_obs, sizeof(guard_obs), "guardrail: %s\n%s",
		 ggr.reason, gaction);
	grefl = react_step_create(ctx->turn_arena, REACT_STEP_REFLECTION,
				  ggr.reason, NULL, NULL, NULL);
	add_step(ctx, grefl);
	react_output_emit(cb, user_data, REACT_STEP_REFLECTION,
			  REACT_OUTPUT_FAILED, ggr.reason, call->tool_name,
			  call->tool_args, call->tool_call_id, 0, NULL,
			  NULL, NULL);
	react_emit_text_event(ctx, MORPH_EVENT_REACT,
			      "react.reflection", "failed",
			      "guardrail reflection", ggr.reason);
	return arena_strdup(ctx->turn_arena, guard_obs);
}

static int react_record_tool_observation(struct react_context *ctx,
					 struct async_tool_call *call,
					 const char *obs_text, int rc,
					 const struct tool_artifact_list *artifacts,
					 const cJSON *data, const cJSON *ui,
					 morph_array_t *messages,
					 react_output_cb cb, void *user_data)
{
	struct react_step *obs;

	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				obs_text, NULL, NULL, NULL);
	if (obs) {
		obs->error_code = rc;
		if (artifacts)
			obs->artifacts = *artifacts;
	}
	add_step(ctx, obs);
	react_output_emit(cb, user_data, REACT_STEP_OBSERVATION,
			  rc < 0 ? REACT_OUTPUT_FAILED : REACT_OUTPUT_COMPLETED,
			  obs_text, call->tool_name, call->tool_args,
			  call->tool_call_id, rc, artifacts, data, ui);
	react_emit_observation_event(ctx, obs_text, call->tool_name, rc,
				     artifacts, data, ui);
	return react_append_tool_message(messages, obs_text,
					 call->provider_tool_call_id,
					 ctx->turn_arena);
}

static int react_record_tool_timeout(struct react_context *ctx,
				     const struct tool_call *tc,
				     const char *timeout_msg,
				     morph_array_t *messages,
				     react_output_cb cb, void *user_data)
{
	struct react_step *obs;
	const char *args;

	args = tc->arguments ? tc->arguments : "{}";
	obs = react_step_create(ctx->turn_arena, REACT_STEP_OBSERVATION,
				timeout_msg, tc->name, args, tc->tool_call_id);
	if (obs)
		obs->error_code = -ETIMEDOUT;
	add_step(ctx, obs);
	react_output_emit(cb, user_data, REACT_STEP_ACTION,
			  REACT_OUTPUT_TIMEOUT, timeout_msg, tc->name, args,
			  tc->tool_call_id, -ETIMEDOUT, NULL, NULL, NULL);
	react_output_emit(cb, user_data, REACT_STEP_OBSERVATION,
			  REACT_OUTPUT_FAILED, timeout_msg, tc->name, args,
			  tc->tool_call_id, -ETIMEDOUT, NULL, NULL, NULL);
	react_emit_observation_event(ctx, timeout_msg, tc->name, -ETIMEDOUT,
				     NULL, NULL, NULL);
	if (agent_history_record_tool_result(ctx, tc->tool_call_id, tc->id,
		tc->name, timeout_msg, -ETIMEDOUT) != 0)
		return ctx->history_error ? ctx->history_error : MORPH_ERR_DB;

	return react_append_tool_message(messages, timeout_msg, tc->id,
					 ctx->turn_arena);
}

static int react_drain_actions(struct react_context *ctx,
			       morph_array_t *messages, int iteration,
			       int *prompt_count);

static int react_join_tool_calls(struct react_context *ctx,
				 struct chat_response *response,
				 struct react_tool_slot *slots,
				 morph_array_t *messages,
				 react_output_cb cb, void *user_data)
{
	int num_tools = response->tool_call_count;

	if (react_sigint_flag) {
		ctx->cancelled = 1;
		react_sigint_flag = 0;
	}

	for (int i = 0; i < num_tools; i++) {
		struct async_tool_call *call;
		int rc;
		char *result;
		const char *obs_text;
		struct tool_artifact_list artifacts = {0};
		cJSON *data = NULL;
		cJSON *ui = NULL;
		cJSON *meta = NULL;

		if (slots[i].hitl_denied) {
			struct tool_call *tc = &response->tool_calls[i];
			const char *denied =
				"tool error: execution denied by user";

			if (agent_history_record_tool_result(ctx,
				tc->tool_call_id, tc->id, tc->name, denied,
				-EPERM) != 0) {
				react_set_result(ctx,
						 REACT_OUTCOME_INTERNAL_ERROR,
						 ctx->history_error,
						 "history_persistence_error");
				return 1;
			}
			if (react_append_denied_tool_message(ctx, tc,
							     messages) < 0) {
				react_set_result(ctx,
						 REACT_OUTCOME_INTERNAL_ERROR,
						 -ENOMEM, "internal_error");
				return 1;
			}
			continue;
		}

		if (!slots[i].call)
			continue;

		if (ctx->cancelled) {
			pthread_mutex_lock(&slots[i].call->mutex);
			slots[i].call->cancelled = 1;
			morph_cancel_token_cancel(&slots[i].call->cancel_token);
			pthread_mutex_unlock(&slots[i].call->mutex);
		}

		rc = join_tool_thread(slots[i].thread, &ctx->cancelled,
				      slots[i].call,
				      react_tool_call_timeout(
					      ctx, &response->tool_calls[i]));
		if (rc == -ETIMEDOUT) {
			char timeout_msg[256];
			struct tool_call *tc = &response->tool_calls[i];

			snprintf(timeout_msg, sizeof(timeout_msg),
				 "tool error: '%s' timed out after %ds",
				 tc->name, react_tool_call_timeout(ctx, tc));
			react_emit_tool_event(ctx, "tool.failed", "timeout",
					      "tool timed out", tc->name,
					      tc->arguments ? tc->arguments : "{}",
					      tc->tool_call_id, timeout_msg,
					      -ETIMEDOUT);
			if (react_record_tool_timeout(ctx, tc, timeout_msg,
						      messages, cb,
						      user_data) < 0) {
				react_set_result(ctx,
						 REACT_OUTCOME_INTERNAL_ERROR,
						 ctx->history_error ?
						 ctx->history_error : MORPH_ERR_DB,
						 "history_persistence_error");
				return 1;
			}
			slots[i].call = NULL;
			slots[i].thread_started = 0;
			continue;
		}
		if (rc != 0) {
			struct tool_call *tc = &response->tool_calls[i];
			const char *cancelled =
				"tool error: execution cancelled by user";

			if (agent_history_record_tool_result(ctx,
				tc->tool_call_id, tc->id, tc->name, cancelled,
				-ECANCELED) != 0)
				react_set_result(ctx,
					REACT_OUTCOME_INTERNAL_ERROR,
					ctx->history_error,
					"history_persistence_error");
			react_tool_call_cancelled(ctx, tc);
			slots[i].call = NULL;
			slots[i].thread_started = 0;
			react_record_tool_cancelled(ctx);
			break;
		}
		slots[i].thread_started = 0;

		react_set_state(ctx, REACT_STATE_OBSERVING);
		call = slots[i].call;
		react_collect_tool_result(call, &rc, &result, &artifacts,
					  &data, &ui, &meta);
		obs_text = result ? result : "";
		if (rc < 0)
			react_set_error_detail(ctx, obs_text);
		{
			cJSON *history_content = meta ?
				cJSON_GetObjectItem(meta, "history_content") : NULL;

			if (cJSON_IsString(history_content) &&
			    history_content->valuestring)
				obs_text = history_content->valuestring;
		}

		if (react_track_tool_failure(ctx, call->tool_name,
					     call->tool_args, rc, cb,
					     user_data)) {
			if (agent_history_record_tool_result_ex(ctx,
				call->tool_call_id, call->provider_tool_call_id,
				call->tool_name, obs_text, rc, &artifacts, meta) != 0)
				react_set_result(ctx,
					REACT_OUTCOME_INTERNAL_ERROR,
					ctx->history_error,
					"history_persistence_error");
			react_cancel_remaining_tool_calls(slots, i + 1,
							  num_tools);
			for (int j = 0; j <= i; j++) {
				if (slots[j].call) {
					async_tool_call_destroy(slots[j].call);
					slots[j].call = NULL;
					slots[j].thread_started = 0;
				}
			}
			free(result);
			cJSON_Delete(data);
			cJSON_Delete(ui);
			cJSON_Delete(meta);
			return 1;
		}

		react_tool_call_finish(ctx, call, obs_text, rc);
		if (rc >= 0)
			react_emit_artifacts_from_list(ctx, &artifacts,
						       call->tool_name);

		obs_text = react_apply_tool_output_guardrail(ctx, call,
						     obs_text, rc,
							     &artifacts, cb,
							     user_data);
		if (!obs_text)
			obs_text = "";
		int persist_rc = agent_history_record_tool_result_ex(ctx,
			call->tool_call_id, call->provider_tool_call_id,
			call->tool_name, obs_text, rc, &artifacts, meta);

		if (persist_rc != 0) {
			free(result);
			cJSON_Delete(data);
			cJSON_Delete(ui);
			cJSON_Delete(meta);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
				persist_rc, "history_persistence_error");
			return 1;
		}
		char *prepared_obs = NULL;
		int prepare_rc = agent_history_prepare_tool_content(ctx,
			obs_text, &prepared_obs, NULL);

		if (prepare_rc != 0) {
			free(result);
			cJSON_Delete(data);
			cJSON_Delete(ui);
			cJSON_Delete(meta);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
				prepare_rc, "history_persistence_error");
			return 1;
		}
		if (react_record_tool_observation(ctx, call, prepared_obs, rc,
						  &artifacts, data, ui,
						  messages,
						  cb, user_data) < 0) {
			int history_rc = ctx->history_error ?
				ctx->history_error : MORPH_ERR_DB;

			free(result);
			cJSON_Delete(data);
			cJSON_Delete(ui);
			cJSON_Delete(meta);
			free(prepared_obs);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 history_rc,
					 "history_persistence_error");
			return 1;
		}
		free(prepared_obs);
		free(result);
		cJSON_Delete(data);
		cJSON_Delete(ui);
		cJSON_Delete(meta);
	}

	react_cleanup_tool_calls(slots, num_tools);

	if (react_drain_actions(ctx, messages, -1, NULL))
		return 1;
	return ctx->state == REACT_STATE_DONE ||
		ctx->state == REACT_STATE_ABORT;
}

static int react_drain_actions(struct react_context *ctx,
			       morph_array_t *messages, int iteration,
			       int *prompt_count)
{
	struct react_action act;
	int got;

	if (prompt_count)
		*prompt_count = 0;
	if (react_sigint_flag) {
		ctx->cancelled = 1;
		morph_cancel_token_cancel(&ctx->cancel_token);
		react_sigint_flag = 0;
	}

	while (ctx->action_drain_fn) {
		cJSON *payload;
		cJSON *text;
		struct chat_message *message;
		struct message_list *history_message;
		const char *content;

		memset(&act, 0, sizeof(act));
		got = ctx->action_drain_fn(ctx->action_drain_user_data,
					   &act, 0);
		if (got <= 0)
			break;
		if (act.type && strcmp(act.type, "cancel") == 0) {
			ctx->cancelled = 1;
			morph_cancel_token_cancel(&ctx->cancel_token);
			continue;
		}
		if (!messages || !act.type || strcmp(act.type, "prompt") != 0 ||
		    !act.payload_json)
			continue;
		payload = cJSON_Parse(act.payload_json);
		text = cJSON_IsObject(payload) ?
			cJSON_GetObjectItemCaseSensitive(payload, "text") : NULL;
		content = cJSON_IsString(text) ? text->valuestring : NULL;
		if (!content || !content[0]) {
			cJSON_Delete(payload);
			continue;
		}
		message = react_push_chat_message(messages);
		if (!message) {
			cJSON_Delete(payload);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, -ENOMEM,
					 "internal_error");
			return 1;
		}
		message->role = arena_strdup(ctx->turn_arena, "user");
		message->content = arena_strdup(ctx->turn_arena, content);
		if (!message->role || !message->content) {
			cJSON_Delete(payload);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, -ENOMEM,
					 "internal_error");
			return 1;
		}
		ctx->steer_count++;
		if (agent_history_record_user_steer(ctx, content,
						    ctx->steer_count) != 0) {
			cJSON_Delete(payload);
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 ctx->history_error,
					 "history_persistence_error");
			return 1;
		}
		if (ctx->memory_options && ctx->history_db) {
			morph_buf_t token;
			int rc = morph_buf_init(&token, 64);

			if (rc == 0)
				rc = morph_buf_printf(&token, "turn:%s:user:steer:%d",
					react_get_turn_id(ctx), ctx->steer_count);
			if (rc == 0)
				rc = memory_accept_input(ctx->history_db,
					ctx->history_session_id, content,
					morph_buf_cstr(&token), ctx->memory_options);
			morph_buf_cleanup(&token);
			if (rc != 0) {
				cJSON_Delete(payload);
				react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					rc, "preference_persistence_error");
				return 1;
			}
		}
		history_message = msg_list_create(ctx->session_arena, "user",
			content, tokenizer_count(ctx->tokenizer, content));
		if (history_message)
			msg_list_append(&ctx->messages, history_message);
		react_emit_text_event(ctx, MORPH_EVENT_REACT,
			"react.user.steer", "end", "requirement added", content);
		if (prompt_count)
			(*prompt_count)++;
		cJSON_Delete(payload);
	}

	if (!ctx->cancelled)
		return 0;

	if (iteration >= 0) {
		log_info("react_run: cancelled by user at iteration %d",
			 iteration);
	}
	react_set_result(ctx, REACT_OUTCOME_CANCELLED, -ECANCELED,
			 "user_cancelled");
	return 1;
}

static void react_maybe_compress_context(struct react_context *ctx)
{
	struct compress_result cr = {0};
	int rc;

	if (!context_needs_compress(ctx->messages, ctx->tokenizer,
				    &ctx->compress))
		return;

	compress_detect_react_cycles(ctx->messages);
	compress_react_trace(&ctx->messages, &cr);
	rc = compress_summarize(&ctx->messages,
				ctx->compress.max_history_rounds,
				ctx->compress.summarize,
				ctx->compress.summarize_user_data,
				ctx->session_arena,
				&cr);
	log_info("auto-compress: detected+removed %d, summarized %d "
		 "messages (%d -> %d tokens)",
		 cr.messages_removed, cr.messages_summarized,
		 cr.original_tokens, cr.compressed_tokens);
	free(cr.summary);
	key_info_free(cr.preserved);
	(void)rc;
}

static char *react_trimmed_legacy_text(struct arena *arena,
				       const char *start, size_t len,
				       int strip_thought_prefix)
{
	const char *end;
	char *out;

	if (!arena || !start)
		return NULL;
	while (len > 0 && isspace((unsigned char)*start)) {
		start++;
		len--;
	}
	if (strip_thought_prefix && len >= 8 &&
	    strncasecmp(start, "Thought:", 8) == 0) {
		start += 8;
		len -= 8;
		while (len > 0 && (*start == ' ' || *start == '\t')) {
			start++;
			len--;
		}
	}
	end = start + len;
	while (end > start && isspace((unsigned char)end[-1]))
		end--;
	len = (size_t)(end - start);
	out = arena_alloc(arena, len + 1);
	if (!out)
		return NULL;
	memcpy(out, start, len);
	out[len] = '\0';
	return out;
}

static void react_split_legacy_final(struct react_context *ctx,
				     const char *proposed,
				     const char **thought_out,
				     const char **final_out)
{
	int marker_len = 0;
	const char *marker;
	const char *final_start;

	if (thought_out)
		*thought_out = NULL;
	if (final_out)
		*final_out = proposed;
	if (!ctx || !proposed)
		return;
	marker = react_find_final_marker(proposed, proposed, &marker_len);
	if (!marker)
		return;
	if (thought_out && marker > proposed) {
		*thought_out = react_trimmed_legacy_text(ctx->turn_arena,
							 proposed,
							 (size_t)(marker -
								  proposed),
							 1);
		if (*thought_out && !**thought_out)
			*thought_out = NULL;
	}
	final_start = marker + marker_len;
	while (*final_start == ' ' || *final_start == '\t')
		final_start++;
	if (final_out)
		*final_out = final_start;
}

static int react_final_is_continuation_note(struct react_context *ctx,
					     const char *proposed)
{
	static const char *prefixes[] = {
		"let me understand",
		"let me inspect",
		"let me investigate",
		"let me check",
		"now let me",
		"i need to inspect",
		"i need to investigate",
		"i need to check",
	};
	const char *text = proposed;

	if (!ctx || ctx->in_turn_compaction_count <= 0 ||
	    ctx->incomplete_final_retry_count > 0 || !text)
		return 0;
	while (*text && isspace((unsigned char)*text))
		text++;
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t len = strlen(prefixes[i]);

		if (strncasecmp(text, prefixes[i], len) == 0)
			return 1;
	}
	return 0;
}

static int react_retry_incomplete_final(struct react_context *ctx,
					 const char *proposed,
					 morph_array_t *messages)
{
	struct chat_message *slots;
	const char *retry_prompt =
		"Your previous response was a continuation note, not a completed "
		"answer. Continue the current task from the checkpoint and preserved "
		"recent tool results. Use tools if needed. Return a final answer only "
		"after the requested work is complete or a concrete blocker exists.";

	if (!react_final_is_continuation_note(ctx, proposed))
		return 0;
	slots = morph_array_push_n(messages, 2);
	if (!slots)
		MORPH_RETURN(-ENOMEM);
	memset(slots, 0, 2 * sizeof(*slots));
	slots[0].role = arena_strdup(ctx->turn_arena, "assistant");
	slots[0].content = arena_strdup(ctx->turn_arena, proposed);
	slots[1].role = arena_strdup(ctx->turn_arena, "user");
	slots[1].content = arena_strdup(ctx->turn_arena, retry_prompt);
	if (!slots[0].role || !slots[0].content || !slots[1].role ||
	    !slots[1].content)
		MORPH_RETURN(-ENOMEM);
	ctx->incomplete_final_retry_count++;
	log_warn("react: rejected continuation note as Final after compaction");
	(void)react_emit_text_event(ctx, MORPH_EVENT_REACT,
		"react.final.retry", "retry", "incomplete final response",
		proposed);
	return 1;
}

static int react_handle_final_response(struct react_context *ctx,
				       const char *proposed,
				       morph_array_t *messages,
				       react_output_cb cb, void *user_data)
{
	struct react_step *final_step;
	const char *thought = NULL;
	const char *final_text = proposed;
	int gr;
	int retry;

	react_split_legacy_final(ctx, proposed, &thought, &final_text);
	if (thought) {
		struct react_step *thought_step =
			react_step_create(ctx->turn_arena, REACT_STEP_THOUGHT,
					  thought, NULL, NULL, NULL);
		add_step(ctx, thought_step);
		react_emit_text_event(ctx, MORPH_EVENT_REACT,
				      "react.thought.end", "end",
				      NULL, thought);
	}
	retry = react_retry_incomplete_final(ctx, final_text, messages);
	if (retry < 0) {
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, retry,
				 "internal_error");
		return 1;
	}
	if (retry > 0)
		return 0;

	gr = react_handle_guardrail_retry(ctx, final_text, messages, cb,
					  user_data);
	if (gr < 0) {
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, gr,
				 "internal_error");
		return 1;
	}
	if (gr == 1)
		return 0;
	if (gr == 2)
		return 1;
	final_step = react_step_create(ctx->turn_arena, REACT_STEP_FINAL,
				       final_text, NULL, NULL, NULL);
	add_step(ctx, final_step);
	free(ctx->final_answer);
	ctx->final_answer = strdup(final_text);
	react_set_result(ctx, REACT_OUTCOME_SUCCESS, 0, NULL);
	react_output_emit(cb, user_data, REACT_STEP_FINAL,
			  REACT_OUTPUT_COMPLETED, final_text, NULL, NULL,
			  NULL, 0, NULL, NULL, NULL);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.final",
			      "end", "final answer", final_text);
	return 1;
}

/*
 * Execute the ReAct (Reasoning + Acting) loop.
 *
 * Sends the user input through the LLM, processes tool calls or
 * final answers, and iterates until a final answer is produced,
 * the loop is cancelled, or the maximum iteration count is reached.
 *
 * ctx - ReAct context (must be initialised)
 * user_input - User prompt text
 * cb - Optional output callback for step notifications
 * user_data - Opaque pointer forwarded to cb
 *
 * Returns 0 on success, or a stable negative errno/domain error for the
 * terminal outcome.  Inspect ctx->outcome for the user-facing reason.
 */
int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data)
{
	if (!ctx || !user_input)
		return -EINVAL;
	if (ctx->tools) {
		struct tool_entry *permission_tool =
			tool_lookup(ctx->tools, "request_permissions");

		if (permission_tool && permission_tool->user_data)
			tool_context_clear_turn_grants(
				permission_tool->user_data);
	}
	int use_user_turn_id = ctx->turn_id_user_set && ctx->turn_id[0];
	react_reset(ctx);
	arena_reset(ctx->turn_arena);
	if (!use_user_turn_id) {
		int id_rc = morph_random_id("turn_", ctx->turn_id,
					    sizeof(ctx->turn_id));
		if (id_rc != 0)
			return id_rc;
	}
	ctx->history_error = 0;
	morph_cancel_token_reset(&ctx->cancel_token);
	http_clear_signal_cancel();
	react_set_state(ctx, REACT_STATE_THINKING);
	react_emit_text_event(ctx, MORPH_EVENT_REACT, "react.turn.begin",
			      "begin", "turn started", user_input);

	if (react_check_input_guardrail(ctx, user_input) > 0)
		MORPH_RETURN(react_finish_run(ctx));
	struct model *llm = (struct model *)ctx->llm_model;

	if (!llm || !llm->api_key[0]) {
		int history_rc = agent_history_record_user(ctx, user_input);

		if (history_rc != 0) {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
				history_rc, "history_persistence_error");
			MORPH_RETURN(react_finish_run(ctx));
		}
		(void)cb;
		(void)user_data;
		react_emit_auth_required(ctx, "text",
					 llm ? llm->provider : "",
					 llm ? llm->model_id : "",
					 "", "missing_api_key");
		react_set_result(ctx, REACT_OUTCOME_LLM_ERROR,
				 MORPH_ERR_NOT_CONFIGURED, "missing_api_key");
		MORPH_RETURN(react_finish_run(ctx));
	}
	if (agent_history_record_user(ctx, user_input) != 0) {
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
				 ctx->history_error, "history_persistence_error");
		MORPH_RETURN(react_finish_run(ctx));
	}

	struct message_list *msg = msg_list_create(ctx->session_arena, "user", user_input,
						  tokenizer_count(ctx->tokenizer, user_input));
	msg_list_append(&ctx->messages, msg);

	char *system_prompt = build_system_prompt(ctx, ctx->turn_arena);
	if (!system_prompt) {
		log_err("react_run: failed to build system prompt");
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, -ENOMEM,
				  "internal_error");
		MORPH_RETURN(react_finish_run(ctx));
	}

	struct tool_desc *active_tools = NULL;
	int active_tool_count = 0;
	int has_tools = ctx->tools && ctx->tools->count > 0;

	if (!ctx->history_enabled)
		react_maybe_compress_context(ctx);

	morph_array_t messages;
	int messages_ready = 0;

	memset(&messages, 0, sizeof(messages));
	if (react_prepare_messages(ctx, &messages, user_input) < 0) {
		morph_array_cleanup(&messages);
		react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR, -ENOMEM,
				  "internal_error");
		MORPH_RETURN(react_finish_run(ctx));
	}
	messages_ready = 1;

	react_active_push(ctx);
	http_set_cancel_flag(&ctx->cancelled);
	http_set_cancel_token(&ctx->cancel_token);

	for (int iteration = 0; iteration < ctx->max_iterations; iteration++) {
		if (react_drain_actions(ctx, &messages, iteration, NULL))
			break;

		if (ctx->memory_options && ctx->history_db) {
			char *memory = NULL;
			int rc = memory_build_context_checked(ctx->history_db,
				ctx->history_session_id, user_input, ctx->memory_options, &memory);

			if (rc == 0)
				rc = react_set_memory_context(ctx, memory);

			free(memory);
			if (rc == 0)
				system_prompt = build_system_prompt(ctx, ctx->turn_arena);
			if (rc != 0 || !system_prompt) {
				react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					rc != 0 ? rc : -ENOMEM, "memory_context_error");
				break;
			}
		}

		react_set_state(ctx, REACT_STATE_THINKING);
		react_emit_thinking_event(ctx);

		react_output_emit(cb, user_data, REACT_STEP_THOUGHT,
				  REACT_OUTPUT_STARTED, "", NULL, NULL,
				  NULL, 0, NULL, NULL, NULL);

		struct chat_response response = {0};
		int status;

		if (react_prepare_active_tools(ctx, &active_tools,
					       &active_tool_count) < 0) {
			react_set_result(ctx, REACT_OUTCOME_INTERNAL_ERROR,
					 -ENOMEM, "internal_error");
			break;
		}
		has_tools = active_tool_count > 0;
		status = react_maybe_compact_active_window(ctx, llm,
			system_prompt, &messages, active_tools,
			active_tool_count, iteration);
		if (status < 0) {
			react_set_result(ctx, REACT_OUTCOME_LLM_ERROR, status,
				"context_compaction_failed");
			break;
		}

		status = react_chat_once(ctx, llm, system_prompt,
					 (struct chat_message *)messages.elts,
					 (int)messages.nelts, active_tools,
					 active_tool_count, cb, user_data,
					 &response);
		if (ctx->outcome != REACT_OUTCOME_NONE) {
			chat_response_free(&response);
			break;
		}

		if (react_sigint_flag) {
			ctx->cancelled = 1;
			morph_cancel_token_cancel(&ctx->cancel_token);
			react_sigint_flag = 0;
		}
		if (react_handle_llm_cancelled(ctx, &response)) {
			chat_response_free(&response);
			break;
		}
		{
			int prompt_count = 0;

			if (react_drain_actions(ctx, &messages, iteration,
						&prompt_count)) {
				chat_response_free(&response);
				break;
			}
			if (prompt_count > 0) {
				chat_response_free(&response);
				continue;
			}
		}

		if (react_handle_llm_error(ctx, &response, status, cb, user_data)) {
			chat_response_free(&response);
			break;
		}

		if (response.tool_call_count > 0 &&
		    response.content && *response.content) {
			struct react_step *thought = react_step_create(ctx->turn_arena,
				REACT_STEP_THOUGHT, response.content, NULL, NULL, NULL);
			add_step(ctx, thought);
			react_emit_text_event(ctx, MORPH_EVENT_REACT,
					      "react.thought.end", "end",
					      NULL, response.content);
		}

		if (response.tool_call_count > 0 && has_tools) {
			int num_tools = response.tool_call_count;
			morph_array_t tool_slots;
			struct react_tool_slot *slots;
			int id_rc;

			id_rc = react_prepare_tool_call_ids(&response);
			if (id_rc < 0) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  id_rc, "internal_error");
				break;
			}
			id_rc = react_normalize_tool_inputs(ctx, &response);
			if (id_rc < 0) {
				chat_response_free(&response);
				react_set_result(ctx,
					REACT_OUTCOME_INTERNAL_ERROR,
					id_rc, "invalid_tool_input");
				break;
			}
			id_rc = agent_history_record_tool_calls(
				ctx, response.content, response.reasoning_content,
				response.tool_calls, response.tool_call_count);
			if (id_rc != 0) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  id_rc,
						  "history_persistence_error");
				break;
			}
			if (react_append_assistant_tool_call_message(
				ctx, &messages, &response) < 0) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  -ENOMEM, "internal_error");
				break;
			}
			react_set_state(ctx, REACT_STATE_ACTING);
			if (morph_array_init_arena(&tool_slots, ctx->turn_arena,
						   (size_t)num_tools,
						   sizeof(*slots)) < 0) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  -ENOMEM, "internal_error");
				break;
			}
			slots = morph_array_push_n(&tool_slots,
						   (size_t)num_tools);
			if (!slots) {
				chat_response_free(&response);
				react_set_result(ctx,
						  REACT_OUTCOME_INTERNAL_ERROR,
						  -ENOMEM, "internal_error");
				break;
			}
			memset(slots, 0, (size_t)num_tools * sizeof(*slots));

			react_start_tool_calls(ctx, &response, slots, cb,
					       user_data);
			if (react_join_tool_calls(ctx, &response, slots,
						  &messages, cb, user_data)) {
				chat_response_free(&response);
				break;
			}
		} else {
			const char *proposed = response.content
					       ? response.content : "";

			if (!react_handle_final_response(ctx, proposed, &messages,
							 cb, user_data)) {
				chat_response_free(&response);
				continue;
			}
			chat_response_free(&response);
			break;
		}

		chat_response_free(&response);
	}

	http_set_cancel_flag(NULL);
	http_set_cancel_token(NULL);
	http_clear_signal_cancel();
	react_active_pop(ctx);

	react_complete_max_iterations(ctx);
	react_append_final_message(ctx);
	if (messages_ready)
		morph_array_cleanup(&messages);

	MORPH_RETURN(react_finish_run(ctx));
}
