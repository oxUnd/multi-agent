#ifndef REACT_H
#define REACT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <signal.h>
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/context.h"
#include "agent/guardrail.h"
#include "agent/tools/ask_user.h"
#include "event/event.h"
#include "skill/skill.h"
#include "util/cancel.h"

enum react_step_type {
	REACT_STEP_THOUGHT,
	REACT_STEP_ACTION,
	REACT_STEP_OBSERVATION,
	REACT_STEP_REFLECTION,
	REACT_STEP_FINAL,
	REACT_STEP_REASONING,
};

enum react_state {
	REACT_STATE_INIT,
	REACT_STATE_THINKING,
	REACT_STATE_ACTING,
	REACT_STATE_OBSERVING,
	REACT_STATE_GUARDRAIL,
	REACT_STATE_FINAL,
	REACT_STATE_DONE,
	REACT_STATE_ABORT,
	REACT_STATE_TOOL_FAIL,
};

enum react_outcome {
	REACT_OUTCOME_NONE,
	REACT_OUTCOME_SUCCESS,
	REACT_OUTCOME_CANCELLED,
	REACT_OUTCOME_TIMEOUT,
	REACT_OUTCOME_MAX_ITERATIONS,
	REACT_OUTCOME_LLM_ERROR,
	REACT_OUTCOME_TOOL_ERROR,
	REACT_OUTCOME_GUARDRAIL_DENIED,
	REACT_OUTCOME_INTERNAL_ERROR,
};

struct react_step {
	enum react_step_type type;
	int error_code;
	struct tool_artifact_list artifacts;
	char *content;
	char *tool_name;
	char *tool_args;
	char *tool_call_id;
	struct react_step *next;
};

enum react_output_status {
	REACT_OUTPUT_NONE,
	REACT_OUTPUT_DELTA,
	REACT_OUTPUT_STARTED,
	REACT_OUTPUT_COMPLETED,
	REACT_OUTPUT_FAILED,
	REACT_OUTPUT_CANCELLED,
	REACT_OUTPUT_TIMEOUT,
};

struct db;
struct model_history_item;

struct react_output_event {
	enum react_step_type type;
	enum react_output_status status;
	const char *text;
	const char *tool_name;
	const char *tool_args;
	const char *tool_call_id;
	int error_code;
	const struct tool_artifact_list *artifacts;
	const cJSON *data;
	const cJSON *ui;
};

#define HITL_TOOLS_MAX 32
#define HITL_TOOL_NAME_MAX 64
#define HITL_AUTO_APPROVED_MAX 32
#define HISTORY_SECRET_MAX 32

enum hitl_verdict {
	HITL_APPROVE,
	HITL_DENY,
	HITL_ALWAYS,
};

typedef enum hitl_verdict (*hitl_approval_cb)(const char *tool_name,
					      const char *tool_args,
					      void *user_data);

struct hitl_config {
	int enabled;
	char tools[HITL_TOOLS_MAX][HITL_TOOL_NAME_MAX];
	int tools_count;
	int auto_approve_readonly;
	hitl_approval_cb approval_cb;
	void *approval_user_data;
	char auto_approved[HITL_AUTO_APPROVED_MAX][HITL_TOOL_NAME_MAX];
	int auto_approved_count;
};

struct react_action {
	const char *type;          /* "approve" | "reject" | "cancel" | "prompt" */
	const char *payload_json;
};

typedef int (*react_action_drain_fn)(void *user, struct react_action *out,
				     int timeout_sec);

struct react_context {
	struct react_step *steps;
	int step_count;
	int max_iterations;
	int tool_timeout_seconds;
	int tool_max_retries;
	struct guardrail_config guardrail;
	int guardrail_retry_count;
	struct hitl_config hitl;
	ask_user_callback_fn ask_user_fn;
	void *ask_user_data;
	struct tool_registry *tools;
	struct message_list *messages;
	struct model_history_item *history_items;
	struct db *history_db;
	int64_t history_session_id;
	int history_enabled;
	int history_error;
	int history_tool_result_tokens;
	char *history_compaction_prompt;
	char *history_secrets[HISTORY_SECRET_MAX];
	int history_secret_count;
	struct tokenizer *tokenizer;
	struct compress_config compress;
	void *llm_model;
	char *final_answer;
	enum react_state state;
	enum react_outcome outcome;
	int last_error_code;
	char outcome_reason[64];
	char *last_error_detail;
	char *tool_fail_name;
	char *tool_fail_args;
	int tool_fail_count;
	int empty_round_count;
	int steer_count;
	int in_turn_compaction_count;
	int incomplete_final_retry_count;
	volatile sig_atomic_t cancelled;
	struct morph_cancel_token cancel_token;
	struct arena *turn_arena;
	struct arena *session_arena;
	char *system_prompt;
	char *memory_context;
	struct skill_registry *skills;
	char *workdir;
	/* Non-consuming prompt check; interrupts only the current model call. */
	int (*prompt_pending_fn)(void *user_data);
	react_action_drain_fn action_drain_fn;
	void *action_drain_user_data;
	morph_event_cb event_cb;
	void *event_user_data;
	int sub_agent_depth;
	struct {
		char name[64];
		char description[256];
	} *sub_agent_info;
	int sub_agent_info_count;
	char turn_id[64];
	int turn_id_user_set;
	struct tool_runtime_context tool_runtime;
};

typedef int (*react_output_cb)(const struct react_output_event *event,
			       void *user_data);

struct session_store;

struct react_context *react_context_create(struct tool_registry *tools,
					   struct tokenizer *tok,
					   struct compress_config *cfg,
					   struct guardrail_config *gcfg);
void react_context_destroy(struct react_context *ctx);
void react_reset(struct react_context *ctx);

struct react_context *
react_context_create_for_session(struct session_store *store,
				 const char *session_id,
				 const char *user_id);

int react_run(struct react_context *ctx, const char *user_input,
	      react_output_cb cb, void *user_data);
void react_cancel(struct react_context *ctx);
void react_cancel_active(void);
int react_set_memory_context(struct react_context *ctx, const char *memory_context);
extern volatile sig_atomic_t react_sigint_flag;

int react_active_count(void);
void react_active_push(struct react_context *ctx);
void react_active_pop(struct react_context *ctx);

int hitl_needs_approval(struct react_context *ctx, const char *tool_name);
void hitl_add_auto_approved(struct hitl_config *h, const char *tool_name);

int react_set_action_drain(struct react_context *ctx,
			   react_action_drain_fn fn, void *user);
int react_set_event_callback(struct react_context *ctx,
			     morph_event_cb cb, void *user);
int react_set_turn_id(struct react_context *ctx, const char *turn_id);
const char *react_get_turn_id(const struct react_context *ctx);
int react_set_tool_runtime_context(
	struct react_context *ctx,
	const struct tool_runtime_context *runtime);

struct react_step *react_step_create(struct arena *arena,
				     enum react_step_type type,
				     const char *content,
				     const char *tool_name,
				     const char *tool_args,
				     const char *tool_call_id);

void react_step_destroy(struct react_step *step);

const char *react_step_type_name(enum react_step_type type);
const char *react_state_name(enum react_state state);
const char *react_outcome_name(enum react_outcome outcome);

#ifdef __cplusplus
}
#endif

#endif
