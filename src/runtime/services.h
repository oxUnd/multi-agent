#ifndef MORPH_RUNTIME_SERVICES_H
#define MORPH_RUNTIME_SERVICES_H

#include "agent/memory.h"
#include "agent/tool.h"
#include "credits.h"
#include "mcp/mcp.h"
#include "session.h"
#include "skill/skill.h"
#include "db/scheduled_task.h"

struct runtime;

struct runtime_trace_step {
	enum react_step_type type;
	char *content;
	char *tool_name;
	char *tool_args;
};

struct runtime_turn_status {
	enum react_state state;
	enum react_outcome outcome;
	int last_error_code;
	char *outcome_reason;
	char *error_detail;
	char *final_answer;
	struct runtime_trace_step *steps;
	int step_count;
};

int runtime_turn_status_get(struct runtime *runtime,
			    struct runtime_turn_status *out);
void runtime_turn_status_cleanup(struct runtime_turn_status *status);
char *runtime_turn_notification_body(struct runtime *runtime);
char *runtime_turn_error_message(struct runtime *runtime, int rc);
char *runtime_turn_error_json(struct runtime *runtime);

int runtime_session_list_query(struct runtime *runtime, struct session **out,
			       int *count, int limit, const char *filter);
int runtime_session_count_all(struct runtime *runtime);
int runtime_session_find_ref(struct runtime *runtime, const char *ref,
			     struct session *out);
int runtime_session_select_existing(struct runtime *runtime, int64_t id,
				    struct session *out);
int runtime_session_set_model(struct runtime *runtime, const char *model);
int runtime_turn_prepare_tools(struct runtime *runtime, int64_t now);
int runtime_session_context_stats(struct runtime *runtime, int *messages,
			      int *tokens, int *limit);
int runtime_session_model_context_stats(struct runtime *runtime,
	int *active_items, int *tokens, int *tool_tokens,
	int *compactions, int *limit);
struct runtime_history_compaction_status {
	char trigger_kind[32];
	char status[32];
	int input_tokens;
	int output_tokens;
	int error_code;
};
int runtime_session_compaction_status(struct runtime *runtime,
	struct runtime_history_compaction_status *status);
char *runtime_trace_load_latest_current(struct runtime *runtime,
					int *round_no, int *aborted);
int runtime_session_compress(struct runtime *runtime,
				     int *trace_removed,
				     int *window_removed, int *kept);

int runtime_tool_count(const struct runtime *runtime);
int runtime_tool_info(const struct runtime *runtime, int index,
		      struct tool_desc *out);
int runtime_tool_flags(const struct runtime *runtime, int index,
		       unsigned *out);
int runtime_tool_enabled(const struct runtime *runtime, int index,
			 int *out);
int runtime_tool_origin(const struct runtime *runtime, int index,
			enum tool_origin *out);
int runtime_tool_find(const struct runtime *runtime, const char *name,
		      struct tool_desc *out);

int runtime_skill_count(const struct runtime *runtime);
int runtime_skill_info(const struct runtime *runtime, int index,
		       struct skill_entry *out);
int runtime_skill_find(const struct runtime *runtime, const char *name,
		       struct skill_entry *out);
int runtime_skill_set_active(struct runtime *runtime, const char *name,
			     int active, int *changed);

char *runtime_memory_background_render(struct runtime *runtime);
char *runtime_preferences_render(struct runtime *runtime, int history);
int runtime_preference_set(struct runtime *runtime, const char *scope,
			   const char *key, const char *value);

char *runtime_memory_render_current(struct runtime *runtime, int max_episodes);
int runtime_memory_clear_current(struct runtime *runtime,
				 enum memory_clear_scope scope);

int runtime_credit_summary_today_get(struct runtime *runtime,
				     struct credit_summary *out);
int runtime_credit_summary_current_get(struct runtime *runtime,
				       struct credit_summary *out);
int runtime_credit_record_media(struct runtime *runtime, const char *kind,
				int64_t image_units, int64_t video_seconds,
				const char *provider, const char *model,
				const char *metadata_json);

int runtime_task_create(struct runtime *runtime,
			const struct scheduled_task_input *input,
			struct scheduled_task *out);
int runtime_task_update(struct runtime *runtime, int64_t id,
			const struct scheduled_task_input *input,
			struct scheduled_task *out);
int runtime_task_list(struct runtime *runtime, const char *status, int limit,
		      struct scheduled_task **out, int *count);
int runtime_task_get(struct runtime *runtime, int64_t id,
		     struct scheduled_task *out);
int runtime_task_cancel(struct runtime *runtime, int64_t id);
int runtime_notification_list(struct runtime *runtime, int limit,
			      struct notification **out, int *count);
int runtime_notification_mark_read(struct runtime *runtime, int64_t id);

int runtime_mcp_count(const struct runtime *runtime);
struct runtime_mcp_status {
	struct mcp_server_config config;
	int connected;
	char negotiated_version[32];
	char server_name[MCP_NAME_MAX];
	char server_version[64];
	int supports_tools;
	int supports_resources;
	int supports_prompts;
};
int runtime_mcp_info(const struct runtime *runtime, int index,
		     struct runtime_mcp_status *out);
int runtime_mcp_find(const struct runtime *runtime, const char *name,
		     struct runtime_mcp_status *out);
int runtime_mcp_connect(struct runtime *runtime, const char *name);
int runtime_mcp_disconnect(struct runtime *runtime, const char *name);
int runtime_mcp_discover(struct runtime *runtime, const char *name,
			 int *tools, int *resources, int *prompts);
int runtime_mcp_list_tools(struct runtime *runtime, const char *name,
			   struct mcp_tool_desc **out, int *count);
int runtime_mcp_list_resources(struct runtime *runtime, const char *name,
			       struct mcp_resource_desc **out, int *count);
int runtime_mcp_list_prompts(struct runtime *runtime, const char *name,
			     struct mcp_prompt_desc **out, int *count);
void runtime_mcp_list_free(void *items);

#endif
