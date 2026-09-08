#include "runtime/runtime_internal.h"
#include "runtime/usage.h"
#include "runtime/output.h"
#include "util/log.h"

#include <errno.h>
#include <string.h>

struct runtime_output_forwarder {
	const struct runtime_request *request;
	struct runtime_engine *engine;
};

static int runtime_output_forward(const struct react_output_event *event,
				  void *user_data)
{
	struct runtime_output_forwarder *forwarder = user_data;
	struct runtime_output_context output;
	int rc;

	memset(&output, 0, sizeof(output));
	output.db = forwarder->engine->db;
	output.config = forwarder->engine->config;
	output.session_id = forwarder->request->session_id;
	output.request_prompt = forwarder->request->stored_user_input;
	output.turn_id = forwarder->engine->react->turn_id;
	output.event_cb = forwarder->engine->react->event_cb;
	output.event_user_data = forwarder->engine->react->event_user_data;
	rc = runtime_output_record_event(&output, event);
	if (rc != 0)
		log_warn("failed to record generation output: %d", rc);
	if (forwarder->request->output_cb)
		return forwarder->request->output_cb(
			event, forwarder->request->output_user_data);
	return 0;
}

int runtime_execute(struct runtime_engine *engine,
		    const struct runtime_request *request,
		    struct runtime_result *result)
{
	struct agent_session_runtime session_runtime;
	struct agent_turn turn;
	morph_event_cb old_event_cb;
	void *old_event_user_data;
	hitl_approval_cb old_hitl_cb;
	void *old_hitl_user_data;
	ask_user_callback_fn old_ask_user_fn;
	void *old_ask_user_data;
	int (*old_prompt_pending_fn)(void *);
	react_action_drain_fn old_action_drain_fn;
	void *old_action_drain_user_data;
	void *old_usage_user_data = NULL;
	int event_bound = 0;
	int usage_bound = 0;
	int hitl_bound = 0;
	int ask_user_bound = 0;
	int action_drain_bound = 0;
	int rc;
	int finish_rc;
	struct runtime_output_forwarder output_forwarder;

	if (!engine || !engine->db || !engine->react || !request ||
	    request->session_id <= 0 || !request->model_input || !result)
		return -EINVAL;
	memset(result, 0, sizeof(*result));
	result->outcome = RUNTIME_OUTCOME_FAILED;
	rc = runtime_lock_turn(engine, request, result);
	if (rc != 0)
		return rc;
	old_event_cb = engine->react->event_cb;
	old_event_user_data = engine->react->event_user_data;
	old_hitl_cb = engine->react->hitl.approval_cb;
	old_hitl_user_data = engine->react->hitl.approval_user_data;
	old_ask_user_fn = engine->react->ask_user_fn;
	old_ask_user_data = engine->react->ask_user_data;
	old_prompt_pending_fn = engine->react->prompt_pending_fn;
	old_action_drain_fn = engine->react->action_drain_fn;
	old_action_drain_user_data = engine->react->action_drain_user_data;
	if (request->bind_usage_user_data) {
		old_usage_user_data =
			runtime_usage_bind(request->usage_user_data);
		usage_bound = 1;
	}
	if (engine->prepare_turn) {
		rc = engine->prepare_turn(engine->user_data, request);
		if (rc != 0)
			goto out;
	}

	if (request->event_cb) {
		react_set_event_callback(engine->react, request->event_cb,
				 request->event_user_data);
		event_bound = 1;
	}
	if (request->override_hitl) {
		engine->react->hitl.approval_cb = request->hitl_cb;
		engine->react->hitl.approval_user_data =
			request->hitl_user_data;
		hitl_bound = 1;
	}
	if (request->override_ask_user) {
		engine->react->ask_user_fn = request->ask_user_fn;
		engine->react->ask_user_data = request->ask_user_user_data;
		ask_user_bound = 1;
	}
	if (request->override_action_drain) {
		engine->react->prompt_pending_fn = request->prompt_pending_fn;
		engine->react->action_drain_fn = request->action_drain_fn;
		engine->react->action_drain_user_data =
			request->action_drain_user_data;
		action_drain_bound = 1;
	}
	memset(&session_runtime, 0, sizeof(session_runtime));
	session_runtime.db = engine->db;
	session_runtime.session_id = request->session_id;
	session_runtime.react = engine->react;
	session_runtime.memory_options = request->memory_options ?
		request->memory_options : engine->memory_options;
	session_runtime.render_assistant = request->render_assistant;
	session_runtime.render_user_data = request->render_user_data;
	session_runtime.background_cb = engine->background_cb;
	session_runtime.background_user_data = engine->background_user_data;
	session_runtime.flags = request->turn_flags ? request->turn_flags :
		AGENT_TURN_DEFAULT_FLAGS;
	memset(&turn, 0, sizeof(turn));
	rc = agent_turn_begin(&turn, &session_runtime,
		&(struct agent_turn_input){
			.model_input = request->model_input,
			.stored_user_input = request->stored_user_input,
			.turn_id = request->turn_id,
		});
	if (rc == 0) {
		output_forwarder.request = request;
		output_forwarder.engine = engine;
		rc = react_run(engine->react, request->model_input,
			       runtime_output_forward, &output_forwarder);
	}
	if (turn.begun) {
		finish_rc = agent_turn_finish(&turn, &result->turn);
		result->persistence_rc = finish_rc;
		if (rc == 0 && finish_rc != 0)
			rc = finish_rc;
	}
	result->final_text = engine->react->final_answer;
	if (engine->react->outcome == REACT_OUTCOME_CANCELLED)
		result->outcome = RUNTIME_OUTCOME_CANCELLED;
	else if (rc == 0)
		result->outcome = RUNTIME_OUTCOME_COMPLETED;
	result->execution_rc = rc;

out:
	if (result->execution_rc == 0 && result->outcome != RUNTIME_OUTCOME_COMPLETED)
		result->execution_rc = rc;
	if (event_bound)
		react_set_event_callback(engine->react, old_event_cb,
					 old_event_user_data);
	if (hitl_bound) {
		engine->react->hitl.approval_cb = old_hitl_cb;
		engine->react->hitl.approval_user_data =
			old_hitl_user_data;
	}
	if (ask_user_bound) {
		engine->react->ask_user_fn = old_ask_user_fn;
		engine->react->ask_user_data = old_ask_user_data;
	}
	if (action_drain_bound) {
		engine->react->prompt_pending_fn = old_prompt_pending_fn;
		engine->react->action_drain_fn = old_action_drain_fn;
		engine->react->action_drain_user_data =
			old_action_drain_user_data;
	}
	if (engine->finish_turn)
		engine->finish_turn(engine->user_data, request, result);
	if (usage_bound)
		runtime_usage_restore(old_usage_user_data);
	runtime_unlock_turn(engine);
	return rc;
}
