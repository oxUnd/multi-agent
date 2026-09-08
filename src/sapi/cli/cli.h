#ifndef CLI_H
#define CLI_H

#include "config/config.h"
#include "runtime/runtime.h"
#include "util/buf.h"
#include "util/strmap.h"
#include "util/utf8.h"
#include <stdint.h>
#include <stdio.h>

enum cli_presentation_mode {
	CLI_PRESENT_INTERACTIVE,
	CLI_PRESENT_ONCE_PLAIN,
	CLI_PRESENT_EVENTS_JSON,
};

struct morph_md_kitty;
struct cli_cancel_monitor;
struct cli_ui;
struct cli_terminal;
struct cli_command_job;

struct cli_context {
	struct runtime *runtime;
	struct cli_ui *ui;
	struct cli_terminal *terminal;
	struct cli_command_job *input_job;
	enum cli_presentation_mode presentation_mode;
	int running;
	int session_auto_named;
	int presentation_ready;
	int turn_active;
	struct cli_cancel_monitor *cancel_monitor;
	int event_stream_kind;
	int event_stream_has_delta;
	int event_stream_complete;
	int event_stream_visible;
	int final_rendered;
	int markdown_stream_kind;
	int markdown_stream_visible;
	int mcp_tree_active;
	char image_path[PATH_MAX];
	char mcp_tree_server[MCP_NAME_MAX];
	morph_buf_t event_stream;
	morph_buf_t markdown_stream_text;
	struct utf8_terminal_sanitizer event_stream_sanitizer;
	struct utf8_terminal_sanitizer markdown_stream_sanitizer;
	morph_strmap_t rendered_artifacts;
	morph_strmap_t announced_artifacts;
	struct morph_md_kitty *markdown_stream;
	int trace_json;
	int pending_db_restore;
	uint64_t interaction_sequence;
	struct morph_sync_restore_plan db_restore_plan;
	morph_event_cb event_cb;
	void *event_user_data;
};

int cli_init(struct cli_context *ctx, const char *config_path,
	     const char *workdir, const char *session_name,
	     enum cli_presentation_mode mode);
void cli_run(struct cli_context *ctx);
void cli_run_once(struct cli_context *ctx, const char *prompt);
void cli_shutdown(struct cli_context *ctx);
int cli_handle_command(struct cli_context *ctx, const char *input);
void cli_print_help(void);
void cli_set_color_enabled(int enabled);
int cli_color_enabled(void);
int cli_printf(const char *fmt, ...);
int cli_fputs(const char *text, FILE *stream);
int cli_putchar(int ch);
int cli_write_ndjson(const char *json);
void cli_set_structured_output(int enabled);
int cli_structured_output_enabled(void);

#endif
