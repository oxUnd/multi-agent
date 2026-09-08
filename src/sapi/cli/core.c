#include "sapi/cli/internal.h"
#include "sapi/cli/commands/registry.h"
#include "runtime/runtime.h"
#include <stdarg.h>

#undef fputs
#undef putchar

const char *default_db_path = "~/.morph/data.db";
const char *default_config_path = "~/.morph/config.toml";
static int g_cli_color_enabled = 1;
static int g_cli_structured_output;
static _Thread_local morph_buf_t *g_cli_command_output;

void cli_set_color_enabled(int enabled)
{
	g_cli_color_enabled = enabled ? 1 : 0;
}

int cli_color_enabled(void)
{
	return g_cli_color_enabled;
}

void cli_set_structured_output(int enabled)
{
	g_cli_structured_output = enabled ? 1 : 0;
}

int cli_structured_output_enabled(void)
{
	return g_cli_structured_output;
}

int cli_command_capture_begin(morph_buf_t *output)
{
	if (!output || g_cli_command_output)
		MORPH_RETURN(-EINVAL);
	g_cli_command_output = output;
	return 0;
}

void cli_command_capture_end(void)
{
	g_cli_command_output = NULL;
}

static int cli_capture_text(const char *text)
{
	char *safe;
	int rc;

	if (!g_cli_command_output || !text)
		MORPH_RETURN(-EINVAL);
	safe = utf8_terminal_sanitize_dup(text, strlen(text),
		UTF8_TERMINAL_TEXT_MULTILINE, NULL);
	if (!safe)
		MORPH_RETURN(-ENOMEM);
	rc = morph_buf_puts(g_cli_command_output, safe);
	free(safe);
	return rc;
}

static const unsigned char *cli_skip_control_string(
	const unsigned char *cursor)
{
	while (*cursor) {
		if (*cursor == '\a')
			return cursor + 1;
		if (*cursor == 0x1bu && cursor[1] == '\\')
			return cursor + 2;
		cursor++;
	}
	return cursor;
}

static int cli_write_visible_control(unsigned cp)
{
	if (cp == '\a')
		return fputs("\\a", stdout) == EOF ? -EIO : 0;
	if (cp == '\b')
		return fputs("\\b", stdout) == EOF ? -EIO : 0;
	if (cp <= 0xffu)
		return fprintf(stdout, "\\x%02x", cp) < 0 ? -EIO : 0;
	return fprintf(stdout, "\\u%04x", cp) < 0 ? -EIO : 0;
}

static int cli_write_terminal_safe(const char *src, int allow_sgr)
{
	const unsigned char *cursor = (const unsigned char *)src;
	const unsigned char *end;

	if (!src)
		return 0;
	end = cursor + strlen(src);
	while (cursor < end) {
		if (*cursor == 0x1bu) {
			const unsigned char *start = cursor++;

			if (cursor < end && *cursor == '[') {
				cursor++;
				while (cursor < end &&
				       (*cursor < 0x40u || *cursor > 0x7eu))
					cursor++;
				if (cursor < end) {
					cursor++;
					if (allow_sgr && cursor[-1] == 'm' &&
					    fwrite(start, 1,
						(size_t)(cursor - start), stdout) !=
						(size_t)(cursor - start))
						MORPH_RETURN(-EIO);
				}
				continue;
			}
			if (cursor < end && (*cursor == ']' || *cursor == 'P' ||
			    *cursor == 'X' || *cursor == '^' || *cursor == '_')) {
				cursor = cli_skip_control_string(cursor + 1);
				continue;
			}
			if (cursor < end)
				cursor++;
			continue;
		}
		if (*cursor < 0x20u || *cursor == 0x7fu) {
			unsigned cp = *cursor++;

			if (cp == '\n' || cp == '\t') {
				if (fputc((int)cp, stdout) == EOF)
					MORPH_RETURN(-EIO);
			} else if (cli_write_visible_control(cp) != 0) {
				MORPH_RETURN(-EIO);
			}
			continue;
		}
		if (*cursor >= 0x80u) {
			unsigned cp;
			size_t avail = (size_t)(end - cursor);
			size_t cp_len;

			if (!utf8_decode_codepoint((const char *)cursor, avail,
				&cp, &cp_len)) {
				cursor++;
				continue;
			}
			if (cp >= 0x80u && cp <= 0x9fu) {
				if (cli_write_visible_control(cp) != 0)
					MORPH_RETURN(-EIO);
			} else if (fwrite(cursor, 1, cp_len, stdout) != cp_len) {
				MORPH_RETURN(-EIO);
			}
			cursor += cp_len;
			continue;
		}
		if (fputc(*cursor++, stdout) == EOF)
			MORPH_RETURN(-EIO);
	}
	return 0;
}

int cli_printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	va_list ap_size;
	va_copy(ap_size, ap);
	n = vsnprintf(NULL, 0, fmt, ap_size);
	va_end(ap_size);
	if (n < 0) {
		va_end(ap);
		return n;
	}
	char *buf = malloc((size_t)n + 1);
	if (!buf) {
		va_end(ap);
		MORPH_RETURN(-ENOMEM);
	}
	vsnprintf(buf, (size_t)n + 1, fmt, ap);
	va_end(ap);
	if (g_cli_command_output) {
		int rc = cli_capture_text(buf);

		free(buf);
		if (rc != 0)
			MORPH_RETURN(rc);
		return n;
	}
	if (g_cli_structured_output) {
		free(buf);
		return n;
	}
	if (cli_write_terminal_safe(buf, g_cli_color_enabled) != 0) {
		free(buf);
		MORPH_RETURN(-EIO);
	}
	free(buf);
	return n;
}

int cli_fputs(const char *text, FILE *stream)
{
	int rc;

	if (!text || !stream)
		MORPH_RETURN(-EINVAL);
	if (stream != stdout)
		return fputs(text, stream);
	if (g_cli_command_output) {
		rc = cli_capture_text(text);
		return rc == 0 ? 0 : EOF;
	}
	if (g_cli_structured_output)
		return 0;
	return cli_write_terminal_safe(text, g_cli_color_enabled) == 0 ? 0 : EOF;
}

int cli_putchar(int ch)
{
	char text[2] = {(char)ch, '\0'};

	if (g_cli_command_output)
		return cli_capture_text(text) == 0 ? ch : EOF;
	if (g_cli_structured_output)
		return ch;
	return fputc(ch, stdout);
}

int cli_write_ndjson(const char *json)
{
	size_t len;

	if (!json)
		MORPH_RETURN(-EINVAL);
	len = strlen(json);
	if (fwrite(json, 1, len, stdout) != len || fputc('\n', stdout) == EOF)
		MORPH_RETURN(-EIO);
	fflush(stdout);
	return 0;
}

int cli_print_untrusted_text(const char *text,
			     enum utf8_terminal_text_mode mode)
{
	char *safe;
	int rc = 0;

	if (!text)
		return 0;
	safe = utf8_terminal_sanitize_dup(text, strlen(text), mode, NULL);
	if (!safe)
		MORPH_RETURN(-ENOMEM);
	if (cli_fputs(safe, stdout) == EOF)
		rc = -EIO;
	free(safe);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

void print_padded(const char *s, int target_width)
{
	char *safe;
	size_t width;
	int dw;
	int pad;

	if (!s)
		s = "";
	safe = utf8_terminal_sanitize_dup(s, strlen(s),
		UTF8_TERMINAL_TEXT_SINGLE_LINE, NULL);
	if (!safe)
		return;
	width = utf8_display_width(safe);
	dw = width > (size_t)INT_MAX ? INT_MAX : (int)width;
	(void)cli_fputs(safe, stdout);
	free(safe);
	pad = target_width - dw;
	for (int i = 0; i < pad; i++)
		(void)cli_putchar(' ');
}

void cli_record_media_credits(struct cli_context *ctx, const char *kind,
				     int64_t image_units,
				     int64_t video_seconds,
				     const char *provider,
				     const char *model,
				     const char *metadata_json)
{
	if (!ctx || !kind)
		return;
	(void)runtime_credit_record_media(ctx->runtime, kind, image_units,
		video_seconds, provider, model, metadata_json);
}
int cli_handle_command(struct cli_context *ctx, const char *input)
{
	int64_t command_started_at;
	int owns_turn = 0;

	if (!ctx || !input)
		return -EINVAL;
	command_started_at = (int64_t)time(NULL);
	(void)runtime_turn_prepare_tools(ctx->runtime, command_started_at);

	cli_process_due_tasks(ctx);
	{
		int handled = 0;
		int rc = cli_handle_media_path(ctx, input, &handled);

		if (handled) {
			if (rc != 0)
				MORPH_RETURN(rc);
			return 0;
		}
	}

	if (input[0] == '/') {
		int rc = cli_command_dispatch(ctx, input);
		cli_process_due_tasks(ctx);
		return rc;
	}

	morph_buf_t input_buf;
	const char *effective_input = input;
	int has_input_buf = 0;

	memset(&input_buf, 0, sizeof(input_buf));
	if (ctx->image_path[0]) {
		int rc = morph_buf_init(&input_buf, 256);

		if (rc != 0)
			MORPH_RETURN(rc);
		has_input_buf = 1;
		rc = morph_buf_printf(&input_buf,
			"[Image: %s]\n"
			"(The user attached the image above; if the task involves "
			"understanding or reading the image, use img_qa to view it.)\n%s",
			ctx->image_path, input);
		if (rc != 0) {
			morph_buf_cleanup(&input_buf);
			MORPH_RETURN(rc);
		}
		effective_input = morph_buf_cstr(&input_buf);
		ctx->image_path[0] = '\0';
	}

	if (!ctx->turn_active) {
		cli_turn_begin(ctx);
		owns_turn = 1;
	}
	if (ctx->presentation_mode == CLI_PRESENT_INTERACTIVE && !ctx->input_job)
		ctx->cancel_monitor = cli_cancel_monitor_start(STDIN_FILENO);
	struct session current;
	(void)runtime_session_current(ctx->runtime, &current);
	struct runtime_request request = {
		.session_id = current.id,
		.model_input = effective_input,
		.stored_user_input = input,
		.prompt_pending_fn = cli_command_job_prompt_pending,
		.action_drain_fn = cli_command_job_drain,
		.action_drain_user_data = ctx->input_job,
		.override_action_drain = ctx->input_job != NULL,
		.output_cb = NULL,
		.output_user_data = NULL,
		.turn_flags = AGENT_TURN_DEFAULT_FLAGS |
			AGENT_TURN_SAVE_EMPTY_USER |
			AGENT_TURN_SAVE_EMPTY_ASSISTANT,
	};
	struct runtime_result runtime_result;
	int react_rc = runtime_execute_turn(ctx->runtime, &request,
					    &runtime_result);
	/* Auto-name a lazily created session from its first user input. */
	if (!ctx->session_auto_named) {
		struct session named_session;
		char title[48];
		size_t len = strcspn(input, "\n");
		size_t max_bytes = sizeof(title) - 4;

		if (len > max_bytes) {
			size_t chop = utf8_clamp_bytes(input, max_bytes);

			memcpy(title, input, chop);
			title[chop] = '\0';
			strcat(title, "...");
		} else {
			memcpy(title, input, len);
			title[len] = '\0';
		}
		if (runtime_session_current(ctx->runtime, &named_session) == 0 &&
		    named_session.id > 0) {
			(void)runtime_session_rename_and_update(ctx->runtime,
							 named_session.id, title);
			ctx->session_auto_named = 1;
		}
	}
	if (has_input_buf)
		morph_buf_cleanup(&input_buf);
	struct cli_cancel_monitor *cancel_monitor = ctx->cancel_monitor;
	ctx->cancel_monitor = NULL;
	cli_cancel_monitor_stop(cancel_monitor);
	if (owns_turn)
		cli_turn_finish(ctx, react_rc);
	if (react_rc == -EBUSY)
		return react_rc;

	cli_process_due_tasks(ctx);
	return react_rc;
}

void cli_turn_begin(struct cli_context *ctx)
{
	if (!ctx)
		return;
	cli_cancel_state_reset();
	cli_presentation_reset(ctx);
	ctx->turn_active = 1;
}

void cli_turn_finish(struct cli_context *ctx, int turn_rc)
{
	if (!ctx)
		return;
	cli_cancel_state_reset();
	cli_presentation_finish(ctx);
	ctx->turn_active = 0;
	if (turn_rc >= 0 || ctx->final_rendered ||
	    ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
		return;
	if (ctx->presentation_mode == CLI_PRESENT_ONCE_PLAIN)
		printf("error: %s\n", morph_strerror(turn_rc));
	else
		CMD_ERROR("%s", morph_strerror(turn_rc));
}

/* ---- cli_shutdown ---- */

void cli_shutdown(struct cli_context *ctx)
{
	if (!ctx)
		return;
	runtime_close(ctx->runtime);
	ctx->runtime = NULL;
	(void)cli_ui_drain(ctx);
	cli_presentation_cleanup(ctx);
	cli_ui_cleanup(ctx);
	cli_terminal_cleanup(ctx);
	cli_set_structured_output(0);
	log_info("cli shutdown complete");
}
