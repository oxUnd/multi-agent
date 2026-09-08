#include "runtime/runtime_internal.h"

#include "runtime/bootstrap.h"
#include "runtime/mcp.h"
#include "runtime/session.h"
#include "runtime/turn_scope.h"
#include "runtime/usage.h"
#include "runtime/extensions.h"
#include "runtime/output.h"
#include "agent/history.h"
#include "event/event.h"
#include "util/error.h"
#include "util/file.h"
#include "util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RUNTIME_NEW_SESSION_RETRIES 100

static int runtime_create_startup_session(struct runtime_context *ctx,
					  struct session *out)
{
	char name[256];
	time_t now;
	int rc = -EEXIST;

	if (!ctx || !out)
		MORPH_RETURN(-EINVAL);
	now = time(NULL);
	for (int i = 0; i < RUNTIME_NEW_SESSION_RETRIES && rc == -EEXIST; i++) {
		if (i == 0) {
			snprintf(name, sizeof(name), "new_%lld",
				 (long long)now);
		} else {
			snprintf(name, sizeof(name), "new_%lld_%d",
				 (long long)now, i);
		}
		rc = runtime_session_create(&ctx->engine, name,
			ctx->config.models.text.model, out);
	}
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

int runtime_ensure_current_session(struct runtime *runtime)
{
	struct runtime_context *ctx;
	struct session session;
	int rc = 0;

	if (!runtime)
		MORPH_RETURN(-EINVAL);
	ctx = &runtime->context;
	if (ctx->current_session.id > 0)
		return 0;
	pthread_mutex_lock(&ctx->execution_lock);
	if (ctx->current_session.id <= 0) {
		rc = runtime_create_startup_session(ctx, &session);
		if (rc == 0) {
			ctx->current_session = session;
			runtime_context_select_plan_session(ctx, session.id);
			(void)runtime_context_update_tool_runtime_context(ctx,
							 session.id);
			runtime_session_clear_history(ctx->react);
		}
	}
	pthread_mutex_unlock(&ctx->execution_lock);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

static void runtime_emit_startup(struct runtime *runtime, const char *name,
				 const char *phase, const char *message)
{
	(void)morph_event_emit_simple(runtime->options.event_cb,
				     runtime->options.event_user_data,
				     MORPH_EVENT_STARTUP, name, phase,
				     message, NULL);
}

static int runtime_emit_background(void *user_data, const char *name,
				   const char *phase, const char *message,
				   const char *task, int count,
				   int error_code)
{
	struct runtime *runtime = user_data;
	morph_event_cb cb;
	void *event_user_data;
	cJSON *data;
	int rc;

	if (!runtime)
		return -EINVAL;
	if (runtime->context.react)
		(void)agent_history_record_receipt(runtime->context.react,
			name, phase, message, task, count, error_code);
	cb = runtime->context.react ? runtime->context.react->event_cb :
		runtime->options.event_cb;
	event_user_data = runtime->context.react ?
		runtime->context.react->event_user_data :
		runtime->options.event_user_data;
	if (!cb)
		return 0;
	data = cJSON_CreateObject();
	if (!data)
		return -ENOMEM;
	cJSON_AddStringToObject(data, "task", task ? task : "");
	if (count >= 0)
		cJSON_AddNumberToObject(data, "count", count);
	if (error_code < 0) {
		cJSON_AddNumberToObject(data, "error_code", error_code);
		cJSON_AddStringToObject(data, "error",
					morph_strerror(error_code));
	}
	rc = morph_event_emit_simple(cb, event_user_data,
				     MORPH_EVENT_BACKGROUND, name, phase,
				     message, data);
	cJSON_Delete(data);
	return rc;
}

static int runtime_sync_source_directory(const struct runtime *runtime,
					 const char *expanded_database,
					 char source[PATH_MAX])
{
	char *expanded = NULL;
	char *slash;
	const char *base = runtime->context.config_path;

	if (!base[0]) {
		if (expanded_database) {
			base = expanded_database;
		} else {
			expanded = file_expand_path(runtime->options.db_path);
			if (!expanded)
				MORPH_RETURN(-ENOMEM);
			base = expanded;
		}
	}
	if (strlen(base) >= PATH_MAX) {
		free(expanded);
		MORPH_RETURN(-ENAMETOOLONG);
	}
	strncpy(source, base, PATH_MAX - 1);
	source[PATH_MAX - 1] = '\0';
	free(expanded);
	slash = strrchr(source, '/');
	if (slash)
		*slash = '\0';
	else
		strncpy(source, ".", PATH_MAX - 1);
	return 0;
}

static int runtime_load_config(struct runtime *runtime)
{
	struct runtime_context *ctx = &runtime->context;
	char *expanded = NULL;

	config_set_defaults(&ctx->config);
	if (!runtime->options.config_path || !runtime->options.config_path[0])
		return 0;
	expanded = file_expand_path(runtime->options.config_path);
	if (!expanded)
		return -ENOMEM;
	strncpy(ctx->config_path, expanded, sizeof(ctx->config_path) - 1);
	if (file_exists(expanded)) {
		int rc = config_load(&ctx->config, expanded);
		if (rc != 0) {
			free(expanded);
			return rc;
		}
	}
	if (runtime->options.output_dir_override &&
	    runtime->options.output_dir_override[0]) {
		strncpy(ctx->config.general.output_dir,
			runtime->options.output_dir_override,
			sizeof(ctx->config.general.output_dir) - 1);
	}
	if (!ctx->config.dynamic_tools.mode_explicit &&
	    runtime->options.default_dynamic_tools_mode &&
	    runtime->options.default_dynamic_tools_mode[0]) {
		strncpy(ctx->config.dynamic_tools.mode,
			runtime->options.default_dynamic_tools_mode,
			sizeof(ctx->config.dynamic_tools.mode) - 1);
	}
	free(expanded);
	return 0;
}

static int runtime_open_database(struct runtime *runtime)
{
	struct runtime_context *ctx = &runtime->context;
	char source[PATH_MAX];
	char *expanded;
	char *dir;
	int rc;

	if (!runtime->options.db_path || !runtime->options.db_path[0])
		return -EINVAL;
	expanded = file_expand_path(runtime->options.db_path);
	if (!expanded)
		return -ENOMEM;
	rc = runtime_sync_source_directory(runtime, expanded, source);
	if (rc == 0)
		rc = morph_sync_recover_db_replacements(source);
	if (rc != 0) {
		free(expanded);
		return rc;
	}
	dir = file_expand_path("~/.morph");
	if (dir) {
		(void)file_ensure_dir(dir);
		free(dir);
	}
	rc = db_open(&ctx->database, expanded);
	free(expanded);
	if (rc != 0)
		return rc;
	return db_init_schema(&ctx->database);
}

static int runtime_set_workdir(struct runtime *runtime)
{
	struct runtime_context *ctx = &runtime->context;
	char *resolved;
	int rc;

	if (runtime->options.workdir && runtime->options.workdir[0]) {
		resolved = file_resolve_path(runtime->options.workdir);
		if (!resolved)
			resolved = file_expand_path(runtime->options.workdir);
		strncpy(ctx->workdir, resolved ? resolved : runtime->options.workdir,
			sizeof(ctx->workdir) - 1);
		rc = file_path_join(ctx->config.general.output_dir,
				    sizeof(ctx->config.general.output_dir),
				    ctx->workdir, "output");
		free(resolved);
		if (rc != 0)
			MORPH_RETURN(rc);
		rc = file_ensure_dir(ctx->config.general.output_dir);
		if (rc != 0)
			MORPH_RETURN(rc);
		return 0;
	}
	if (!getcwd(ctx->workdir, sizeof(ctx->workdir)))
		strncpy(ctx->workdir, ".", sizeof(ctx->workdir) - 1);
	return 0;
}

static int runtime_start_components(struct runtime *runtime)
{
	struct runtime_context *ctx = &runtime->context;
	struct runtime_models models;
	struct runtime_bootstrap_profile profile;
	const char *session_name;
	int rc;

	memset(&models, 0, sizeof(models));
	memset(&profile, 0, sizeof(profile));
	runtime_emit_startup(runtime, "startup.models", "begin",
			     "Preparing models...");
	profile.config = &ctx->config;
	profile.tools = &ctx->tools;
	profile.models = &models;
	profile.workdir = ctx->workdir;
	profile.event_cb = runtime->options.event_cb;
	profile.event_user_data = runtime->options.event_user_data;
	profile.usage_cb = runtime_record_usage;
	profile.usage_user_data = runtime;
	profile.process_replica = runtime->options.process_replica;
	profile.hitl_cb = runtime->options.hitl_cb;
	profile.hitl_user_data = runtime->options.hitl_user_data;
	rc = runtime_bootstrap_models(&profile);
	if (rc != 0)
		return rc;
	runtime_context_update_models(ctx, &models);
	if (runtime->options.after_models_cb) {
		rc = runtime->options.after_models_cb(ctx->react,
					      runtime->options.after_models_user_data);
		if (rc != 0)
			return rc;
	}

	ctx->skills = runtime->options.allocate_skill_registry
		? NULL : &ctx->skill_storage;
	memset(&profile, 0, sizeof(profile));
	profile.config = &ctx->config;
	profile.db = &ctx->database;
	profile.tools = &ctx->tools;
	profile.tool_context = &ctx->tctx;
	profile.skills = &ctx->skills;
	profile.plans = &ctx->plans;
	profile.models = &models;
	profile.workdir = ctx->workdir;
	profile.config_path = ctx->config_path;
	profile.ask_user_cb = runtime->options.ask_user_cb;
	profile.ask_user_user_data = runtime->options.ask_user_user_data;
	profile.operation_approval_cb = runtime->options.operation_approval_cb;
	profile.operation_approval_user_data =
		runtime->options.operation_approval_user_data;
	profile.platform_tools_cb = runtime->options.platform_tools_cb;
	profile.platform_tools_user_data = runtime->options.platform_tools_user_data;
	profile.task_events = runtime->options.task_events;
	profile.img_annotate_pause_cb = runtime->options.img_annotate_pause_cb;
	profile.img_annotate_resume_cb = runtime->options.img_annotate_resume_cb;
	profile.img_annotate_user_data = runtime->options.img_annotate_user_data;
	profile.enable_bash = runtime->options.enable_bash;
	profile.enable_apply_patch = runtime->options.enable_apply_patch;
	profile.enable_config_write = runtime->options.enable_config_write;
	profile.enable_img_annotate = runtime->options.enable_img_annotate;
	profile.enable_shell_exts = runtime->options.enable_shell_exts;
	profile.allocate_skill_registry = runtime->options.allocate_skill_registry;
	runtime_emit_startup(runtime, "startup.tools", "begin",
			     "Loading tools and skills...");
	rc = runtime_bootstrap_tools(&profile);
	if (rc != 0)
		return rc;
	if (runtime->options.enable_shell_exts) {
		rc = runtime_extensions_load(ctx, runtime->options.front_name);
		if (rc != 0)
			return rc;
	}

	if (runtime->options.enable_sub_agents) {
		memset(&profile, 0, sizeof(profile));
		profile.config = &ctx->config;
		profile.db = &ctx->database;
		profile.tools = &ctx->tools;
		profile.models = &models;
		profile.event_cb = runtime->options.event_cb;
		profile.event_user_data = runtime->options.event_user_data;
		profile.enable_sub_agents = 1;
		rc = runtime_bootstrap_sub_agents(&profile, &ctx->sub_agents);
		if (rc != 0)
			return rc;
	}

	runtime_emit_startup(runtime, "startup.mcp", "begin",
			     "Connecting services...");
	(void)runtime_mcp_init_from_config(&ctx->mcp, &ctx->config.mcp,
					   &ctx->tools,
					   runtime->options.auto_connect_mcp,
					   runtime->options.event_cb,
					   runtime->options.event_user_data);
	runtime_emit_startup(runtime, "startup.mcp", "end",
			     "Services connected");
	runtime_context_configure_engine(ctx);
	ctx->memory_options = runtime_memory_options_from_config(&ctx->config);
	ctx->engine.memory_options = &ctx->memory_options;
	ctx->engine.prepare_turn = runtime_prepare_turn;
	ctx->engine.finish_turn = runtime_finish_turn;
	ctx->engine.user_data = runtime;
	ctx->engine.background_cb = runtime_emit_background;
	ctx->engine.background_user_data = runtime;

	session_name = runtime->options.default_session;
	if (!session_name || !session_name[0])
		session_name = ctx->config.general.default_session;
	runtime_emit_startup(runtime, "startup.session", "begin",
		runtime->options.create_new_session ?
		"Waiting for your first message..." :
		"Restoring your session...");
	if (runtime->options.create_new_session) {
		memset(&ctx->current_session, 0,
		       sizeof(ctx->current_session));
	} else if (runtime->options.restore_recent_session) {
		struct session *sessions = NULL;
		int count = 0;
		rc = session_list(&ctx->database, &sessions, &count, 1, NULL);
		if (rc == 0 && count > 0) {
			ctx->current_session = sessions[0];
			free(sessions);
			(void)session_update_model(&ctx->database,
				ctx->current_session.id, ctx->config.models.text.model);
		} else {
			free(sessions);
			rc = runtime_session_get_or_create(&ctx->engine, session_name,
				ctx->config.models.text.model, &ctx->current_session, NULL);
			if (rc != 0)
				return rc;
		}
	} else {
		rc = runtime_session_get_or_create(&ctx->engine, session_name,
			ctx->config.models.text.model, &ctx->current_session, NULL);
		if (rc != 0)
			return rc;
	}
	if (ctx->current_session.id > 0) {
		(void)session_ensure_display_id(&ctx->database,
						&ctx->current_session);
		runtime_context_select_plan_session(ctx, ctx->current_session.id);
		(void)runtime_context_update_tool_runtime_context(ctx,
							ctx->current_session.id);
	}
	{
		char session_id[32] = "";
		struct runtime_models dynamic_models = runtime_context_models(ctx);

		if (ctx->current_session.id > 0) {
			snprintf(session_id, sizeof(session_id), "%lld",
				 (long long)ctx->current_session.id);
		}
		memset(&profile, 0, sizeof(profile));
		profile.config = &ctx->config;
		profile.tools = &ctx->tools;
		profile.tool_context = &ctx->tctx;
		profile.models = &dynamic_models;
		(void)runtime_bootstrap_dynamic_tools(&profile, session_id);
	}
	if (ctx->current_session.id > 0)
		runtime_session_load_history(&ctx->engine,
					     ctx->current_session.id);
	return 0;
}

int runtime_open(const struct runtime_options *options, struct runtime **out)
{
	struct runtime *runtime;
	int rc;

	if (!options || !out || !options->db_path)
		return -EINVAL;
	*out = NULL;
	runtime = calloc(1, sizeof(*runtime));
	if (!runtime)
		return -ENOMEM;
	runtime->options = *options;
	if (options->task_events) {
		runtime->task_events = *options->task_events;
		runtime->options.task_events = &runtime->task_events;
	} else if (options->event_cb) {
		runtime->task_events.cb = options->event_cb;
		runtime->task_events.user_data = options->event_user_data;
		runtime->options.task_events = &runtime->task_events;
	}
	runtime_context_init_empty(&runtime->context);
	runtime_emit_startup(runtime, "startup.begin", "begin",
			     "Morph startup started");
	runtime_emit_startup(runtime, "startup.config", "begin",
			     "Reading configuration...");
	rc = runtime_context_init_lock(&runtime->context);
	if (rc == 0)
		rc = runtime_load_config(runtime);
	if (rc == 0)
		rc = runtime_set_workdir(runtime);
	if (rc == 0) {
		runtime_emit_startup(runtime, "startup.database", "begin",
				     "Opening workspace...");
		rc = runtime_open_database(runtime);
	}
	if (rc == 0)
		rc = runtime_start_components(runtime);
	if (rc == 0) {
		char source[PATH_MAX];
		int cleanup_rc;

		rc = runtime_sync_source_directory(runtime, NULL, source);
		if (rc == 0) {
			cleanup_rc = morph_sync_finalize_db_replacements(source);
			if (cleanup_rc != 0) {
				log_warn("database restore cleanup deferred: %s",
					 morph_strerror(cleanup_rc));
			}
		}
	}
	if (rc != 0) {
		runtime_emit_startup(runtime, "startup.failed", "failed",
				     "Morph could not start");
		runtime_close(runtime);
		return rc;
	}
	runtime_emit_startup(runtime, "startup.ready", "ready",
			     "Morph is ready");
	*out = runtime;
	return 0;
}

void runtime_close(struct runtime *runtime)
{
	struct runtime_shutdown_resources cleanup;

	if (!runtime)
		return;
	runtime_task_worker_stop(&runtime->task_worker);
	runtime_sync_stop_instance(runtime);
	if (runtime->context.execution_lock_ready)
		pthread_mutex_lock(&runtime->context.execution_lock);
	cleanup = runtime_context_shutdown_resources(
		&runtime->context,
		runtime->options.process_replica ? 0 : 1,
		runtime->options.process_replica ? 0 : 1,
		runtime->options.allocate_skill_registry);
	runtime_bootstrap_cleanup(&cleanup);
	if (runtime->context.execution_lock_ready)
		pthread_mutex_unlock(&runtime->context.execution_lock);
	runtime_context_cleanup_lock(&runtime->context);
	free(runtime);
}

int runtime_execute_turn(struct runtime *runtime,
			 const struct runtime_request *request,
			 struct runtime_result *result)

{
	struct runtime_request effective;

	if (!runtime)
		return -EINVAL;
	if (!request)
		return -EINVAL;
	effective = *request;
	if (effective.session_id <= 0 &&
	    runtime->context.current_session.id <= 0) {
		int rc = runtime_ensure_current_session(runtime);

		if (rc != 0)
			return rc;
	}
	if (effective.session_id <= 0)
		effective.session_id = runtime->context.current_session.id;
	if (!effective.memory_options)
		effective.memory_options = &runtime->context.memory_options;
	return runtime_execute(&runtime->context.engine, &effective, result);
}

void runtime_cancel_turn(struct runtime *runtime)
{
	if (runtime)
		runtime_cancel(&runtime->context.engine);
}

void runtime_record_usage(const struct model_usage *usage, void *user_data)
{
	struct runtime *runtime = user_data;
	char session_id[64];

	if (!runtime || !usage || !runtime_model_usage_is_billable(usage))
		return;
	runtime_context_credit_session_key(&runtime->context, session_id,
					   sizeof(session_id));
	(void)runtime_record_model_usage_for_user(
		&runtime->context.database, &runtime->context.config,
		runtime->context.turn_scope.user_id[0]
			? runtime->context.turn_scope.user_id : "local",
		session_id, usage);
	if (runtime->options.usage_observer)
		runtime->options.usage_observer(usage,
						runtime->options.usage_observer_user_data);
}

int runtime_prepare_turn(void *user_data, const struct runtime_request *request)
{
	return runtime_context_prepare_turn(&((struct runtime *)user_data)->context,
				    request, user_data);
}

void runtime_finish_turn(void *user_data, const struct runtime_request *request,
			 const struct runtime_result *result)
{
	(void)request;
	(void)result;
	runtime_context_finish_turn(&((struct runtime *)user_data)->context);
}

const struct config *runtime_config_get(const struct runtime *runtime)
{
	return runtime ? &runtime->context.config : NULL;
}

const char *runtime_workdir_get(const struct runtime *runtime)
{
	return runtime ? runtime->context.workdir : NULL;
}

const char *runtime_config_path_get(const struct runtime *runtime)
{
	return runtime ? runtime->context.config_path : NULL;
}

char *runtime_output_get_json(struct runtime *runtime, const char *path)
{
	return runtime ? runtime_output_get_json_by_path(
		&runtime->context.database, path) : NULL;
}
