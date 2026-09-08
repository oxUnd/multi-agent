#include "sapi/cli/internal.h"
#include "sapi/cli/commands/registry.h"
#include "http/client.h"
#include "sapi/cli/composer.h"
#include <sys/ioctl.h>

#define CLI_BANNER_INNER_WIDTH 60
#define CLI_BANNER_CONTENT_WIDTH (CLI_BANNER_INNER_WIDTH - 2)
#define CLI_BANNER_LABEL_WIDTH 12
#define CLI_BANNER_HINT_WIDTH 18
#define CLI_STRUCTURED_POLL_TIMEOUT_MS 100

/* ---- sigint ---- */

volatile sig_atomic_t cli_sigint_received = 0;
static volatile sig_atomic_t cli_sigwinch_received = 0;
static volatile sig_atomic_t cli_structured_signal_mode = 0;

void cli_sigint_handler(int sig)
{
	(void)sig;
	react_sigint_flag = 1;
	http_cancel_from_signal();
	cli_sigint_received = 1;
	if (!cli_structured_signal_mode &&
	    write(STDOUT_FILENO, "\n", 1) < 0) {
		/* ignore */
	}
}

static void cli_sigwinch_handler(int sig)
{
	(void)sig;
	cli_sigwinch_received = 1;
}

#ifdef HAVE_READLINE

static struct cli_context *g_comp_ctx;
static char *g_readline_ready_input;
static struct cli_composer g_composer;
static void cli_readline_configure(void);

/* Readline still owns editing and wrapping. Apply color to image labels after
 * redisplay, restoring the cursor and attributes before accepting more input. */
static void cli_readline_position(int offset, int columns, int *row, int *column)
{
	const char *cursor = rl_line_buffer;
	const char *end = cursor + offset;

	*row = 0;
	*column = 2;
	while (cursor < end) {
		utf8_int32_t cp;
		int width;

		cursor = utf8codepoint(cursor, &cp);
		if (cp == '\n') {
			(*row)++;
			*column = 0;
			continue;
		}
		width = cp == '\t' ? 8 - *column % 8 :
			utf8_codepoint_width((unsigned)cp);
		if (width < 0)
			width = 2;
		if (*column + width > columns) {
			(*row)++;
			*column = 0;
		}
		*column += width;
		if (*column >= columns) {
			(*row)++;
			*column = 0;
		}
	}
}

static void cli_readline_color_images(void)
{
	struct winsize size;
	int cursor_row;
	int cursor_column;
	int last_row;
	int last_column;
	const char *found = rl_line_buffer;
	FILE *output = rl_outstream ? rl_outstream : stdout;

	if (!RL_ISSTATE(RL_STATE_CALLBACK) || !cli_color_enabled() ||
	    !rl_line_buffer || ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
	    size.ws_col < 2 || size.ws_row < 2)
		return;
	cli_readline_position(rl_point, size.ws_col, &cursor_row, &cursor_column);
	cli_readline_position(rl_end, size.ws_col, &last_row, &last_column);
	if (last_row >= size.ws_row - 1)
		return;
	while ((found = strchr(found, '['))) {
		int start;
		int end;
		int row;
		int column;

		if (!cli_composer_image_span(&g_composer, rl_line_buffer,
			(int)(found - rl_line_buffer), 0, &start, &end)) {
			found++;
			continue;
		}
		cli_readline_position(start, size.ws_col, &row, &column);
		fprintf(output, "\0337\r");
		if (row < cursor_row)
			fprintf(output, "\033[%dA", cursor_row - row);
		else if (row > cursor_row)
			fprintf(output, "\033[%dB", row - cursor_row);
		fprintf(output, "\033[%dG\033[48;5;24m\033[38;5;153m%.*s\033[0m\0338",
			column + 1, end - start, rl_line_buffer + start);
		found = rl_line_buffer + end;
	}
	fflush(output);
}

static void cli_readline_redisplay(void)
{
	rl_redisplay();
	cli_readline_color_images();
}

static void cli_readline_line_ready(char *input)
{
	struct cli_context *ctx = g_comp_ctx;

	if (!ctx) {
		free(input);
		return;
	}
	if (!input) {
		ctx->running = 0;
		if (ctx->input_job && ctx->input_job->active) {
			react_cancel_active();
			http_cancel_from_signal();
		}
		return;
	}
	free(g_readline_ready_input);
	g_readline_ready_input = input;
}

/* Readline owns the whole editable area, including wrapped and pasted lines.
 * Erase it through Readline before moving up to the live status row. */
static char *cli_readline_suspend(struct cli_context *ctx, int *point)
{
	char *draft = strdup(rl_line_buffer ? rl_line_buffer : "");

	*point = rl_point;
	if (!draft)
		return NULL;
	rl_replace_line("", 0);
	rl_point = 0;
	cli_readline_redisplay();
	cli_terminal_composer_suspend(ctx);
	return draft;
}

static void cli_readline_resume(struct cli_context *ctx, char *draft, int point)
{
	cli_terminal_composer_resume(ctx);
	/* Resize and callback installation may already have painted a prompt.
	 * rl_on_new_line assumes column zero on an empty row. */
	if (isatty(STDOUT_FILENO))
		fprintf(stdout, "\r\033[2K");
	rl_replace_line(draft, 0);
	rl_point = point;
	rl_on_new_line();
	rl_forced_update_display();
	free(draft);
}

static void cli_readline_drain_ui(struct cli_context *ctx)
{
	int point;
	char *draft = cli_readline_suspend(ctx, &point);

	if (!draft)
		return;
	/* Owner calls may use blocking Readline for approval or ask_user. */
	rl_callback_handler_remove();
	(void)cli_ui_drain(ctx);
	cli_terminal_composer_resume(ctx);
	/* Installing a callback paints its prompt immediately. Restore with an
	 * empty prompt so the draft redraw is the sole prompt renderer. */
	rl_callback_handler_install("", cli_readline_line_ready);
	cli_readline_configure();
	rl_set_prompt(cli_input_prompt());
	cli_readline_resume(ctx, draft, point);
}

static void cli_readline_render_frame(struct cli_context *ctx, int resized)
{
	int point;
	char *draft = cli_readline_suspend(ctx, &point);

	if (!draft)
		return;
	if (resized) {
		rl_resize_terminal();
		cli_terminal_resize(ctx);
	}
	cli_terminal_render_frame(ctx, resized);
	cli_readline_resume(ctx, draft, point);
}

static int cli_readline_getc(FILE *stream)
{
	int ch = rl_getc(stream);

	if (ch == 0x1b && g_comp_ctx && g_comp_ctx->input_job &&
	    g_comp_ctx->input_job->active) {
		struct pollfd fd = {.fd = fileno(stream), .events = POLLIN};

		/* Keep arrows, Alt+Enter and bracketed paste intact. A lone Esc
		 * cancels without a second thread stealing composer keystrokes. */
		if (poll(&fd, 1, 30) == 0) {
			react_cancel_active();
			http_cancel_from_signal();
			return 0;
		}
	}
	return ch;
}

static int cli_readline_convert_images(void)
{
	char *converted = NULL;
	const char *input = rl_line_buffer;
	int rc;

	if (!RL_ISSTATE(RL_STATE_CALLBACK))
		return 0;
	if (strncmp(input, "/image", 6) == 0 &&
	    isspace((unsigned char)input[6]))
		input += 7;
	else if (input[0] == '/') {
		char *name = strndup(input, strcspn(input, " \t\r\n"));
		int command;

		if (!name)
			MORPH_RETURN(-ENOMEM);
		command = cli_command_find(name) != NULL;
		free(name);
		if (command)
			return 0;
	}

	rc = cli_composer_convert_paths(&g_composer, input, &converted);
	if (rc > 0) {
		rl_begin_undo_group();
		rl_delete_text(0, rl_end);
		rl_point = 0;
		(void)rl_insert_text(converted);
		rl_end_undo_group();
		cli_readline_redisplay();
	}
	free(converted);
	return rc;
}

static int cli_readline_compose_image(struct cli_context *ctx, const char *path)
{
	const char *label;
	int rc;

	(void)ctx;
	rc = cli_composer_add_image(&g_composer, path, &label);
	if (rc != 0)
		MORPH_RETURN(rc);
	rl_begin_undo_group();
	(void)rl_insert_text(label);
	rl_end_undo_group();
	cli_readline_redisplay();
	return 0;
}

static int cli_readline_image_edit(int count, int key, int backward, int erase)
{
	for (int i = 0; i < count; i++) {
		int start;
		int end;

		if (RL_ISSTATE(RL_STATE_CALLBACK) &&
		    cli_composer_image_span(&g_composer, rl_line_buffer, rl_point,
					    backward, &start, &end)) {
			if (erase)
				rl_delete_text(start, end);
			rl_point = erase || backward ? start : end;
		} else if (erase) {
			if (backward)
				rl_rubout(1, key);
			else
				rl_delete(1, key);
		} else if (backward) {
			rl_backward_char(1, key);
		} else {
			rl_forward_char(1, key);
		}
	}
	return 0;
}

static int cli_readline_backspace(int count, int key)
{
	return cli_readline_image_edit(count, key, 1, 1);
}

static int cli_readline_delete_word(int count, int key)
{
	int start;
	int end;

	if (cli_composer_image_span(&g_composer, rl_line_buffer, rl_point,
				    1, &start, &end))
		return cli_readline_backspace(count, key);
	return rl_unix_word_rubout(count, key);
}

static int cli_readline_delete(int count, int key)
{
	return cli_readline_image_edit(count, key, 0, 1);
}

static int cli_readline_backward(int count, int key)
{
	return cli_readline_image_edit(count, key, 1, 0);
}

static int cli_readline_forward(int count, int key)
{
	return cli_readline_image_edit(count, key, 0, 0);
}

#ifdef HAVE_RL_BRACKETED_PASTE_BEGIN
static int cli_readline_paste(int count, int key)
{
	int rc = rl_bracketed_paste_begin(count, key);

	if (rc == 0)
		(void)cli_readline_convert_images();
	return rc;
}
#endif

static int cli_readline_accept(int count, int key)
{
	int point;
	char *draft;

	/* Empty Enter should not commit an orphan prompt into the history.
	 * Blocking approval/question prompts still accept an empty answer. */
	if (RL_ISSTATE(RL_STATE_CALLBACK) &&
	    strspn(rl_line_buffer, " \t\r\n") == (size_t)rl_end)
		return 0;
	if (cli_readline_convert_images() != 0)
		return 0;
	draft = cli_readline_suspend(g_comp_ctx, &point);

	if (!draft)
		return 1;
	/* Commit the submitted input where the live row was, so accepting a
	 * line never leaves an old spinner behind in terminal history. */
	rl_replace_line(draft, 0);
	rl_point = point;
	rl_on_new_line();
	rl_forced_update_display();
	free(draft);
	return rl_newline(count, key);
}

static int cli_readline_insert_newline(int count, int key)
{
	(void)key;
	for (int i = 0; i < count; i++) {
		if (rl_insert_text("\n") != 0)
			return 1;
	}
	cli_readline_redisplay();
	return 0;
}

static int cli_readline_paste_image(int count, int key)
{
	char *path = NULL;
	int rc;

	(void)count;
	(void)key;
	if (!g_comp_ctx || !RL_ISSTATE(RL_STATE_CALLBACK))
		return 1;
	rc = cli_clipboard_save_image(g_comp_ctx, &path);
	if (rc == 0)
		rc = cli_readline_compose_image(g_comp_ctx, path);
	if (rc != 0) {
		int point;
		char *draft = cli_readline_suspend(g_comp_ctx, &point);

		if (path)
			(void)unlink(path);
		if (draft) {
			CMD_ERROR("clipboard does not contain a supported image");
			cli_readline_resume(g_comp_ctx, draft, point);
		}
		putchar('\a');
	}
	free(path);
	return rc == 0 ? 0 : 1;
}

static void cli_readline_configure(void)
{
	(void)rl_variable_bind("enable-bracketed-paste", "on");
	(void)rl_variable_bind("horizontal-scroll-mode", "off");
	(void)rl_bind_key('\r', cli_readline_accept);
	(void)rl_bind_key('\n', cli_readline_insert_newline);
	(void)rl_bind_key(0x16, cli_readline_paste_image);
	(void)rl_bind_key(0x7f, cli_readline_backspace);
	(void)rl_bind_key(0x08, cli_readline_backspace);
	(void)rl_bind_key(0x04, cli_readline_delete);
	(void)rl_bind_key(0x17, cli_readline_delete_word);
	(void)rl_bind_key(0x02, cli_readline_backward);
	(void)rl_bind_key(0x06, cli_readline_forward);
#ifdef HAVE_RL_BIND_KEYSEQ
	(void)rl_bind_keyseq("\033[D", cli_readline_backward);
	(void)rl_bind_keyseq("\033[C", cli_readline_forward);
	(void)rl_bind_keyseq("\033[3~", cli_readline_delete);
#ifdef HAVE_RL_BRACKETED_PASTE_BEGIN
	(void)rl_bind_keyseq("\033[200~", cli_readline_paste);
#endif
	(void)rl_bind_keyseq("\\e\\C-M", cli_readline_insert_newline);
	(void)rl_bind_keyseq("\033[13;2u", cli_readline_insert_newline);
	(void)rl_bind_keyseq("\033[27;2;13~", cli_readline_insert_newline);
	/* Ctrl+Command+V on macOS (kitty keyboard protocol):
	 * modifier = 1 + (ctrl=4 | super=8) = 13, keycode for V base key = 118,
	 * with 86 as an upper-case fallback some terminals emit. */
	(void)rl_bind_keyseq("\033[118;13u", cli_readline_paste_image);
	(void)rl_bind_keyseq("\033[86;13u", cli_readline_paste_image);
#endif
}

static char *session_completion_generator(const char *text, int state)
{
	static struct session *slist;
	static int scount;
	static int idx;
	static int len;

	if (state == 0) {
		if (slist) {
			free(slist);
			slist = NULL;
		}
		scount = 0;
		idx = 0;
		len = (int)strlen(text);
		if (!g_comp_ctx)
			return NULL;
		(void)runtime_session_list_query(g_comp_ctx->runtime, &slist,
						 &scount, 0, NULL);
	}

	while (idx < scount) {
		struct session *s = &slist[idx];
		idx++;
		if (s->display_id[0] &&
		    strncmp(s->display_id, text, (size_t)len) == 0)
			return strdup(s->display_id);
		if (s->name[0] &&
		    strncmp(s->name, text, (size_t)len) == 0)
			return strdup(s->name);
	}

	if (slist) {
		free(slist);
		slist = NULL;
	}
	scount = 0;
	return NULL;
}

static int is_session_arg_command(const char *cmd)
{
	return (strcmp(cmd, "/switch") == 0 ||
		strcmp(cmd, "/s") == 0 ||
		strcmp(cmd, "/delete") == 0 ||
		strcmp(cmd, "/del") == 0);
}

static char **cmd_completion(const char *text, int start, int end)
{
	(void)end;
	rl_attempted_completion_over = 1;
	if (start == 0)
		return rl_completion_matches(text, cli_command_completion_generator);

	char *cmd = strndup(rl_line_buffer, (size_t)(start - 1));
	int match = is_session_arg_command(cmd);
	free(cmd);
	if (match)
		return rl_completion_matches(text, session_completion_generator);

	return NULL;
}

#endif

const char *cli_input_prompt(void)
{
	if (!cli_color_enabled())
		return "> ";
	return CLI_RL_IGNORE_START ANSI_BOLD ANSI_CYAN CLI_RL_IGNORE_END
		"› "
		CLI_RL_IGNORE_START ANSI_RESET CLI_RL_IGNORE_END;
}

static void cli_print_banner_title(void)
{
	size_t used;
	int pad;

	used = utf8_display_width(">_ ") + utf8_display_width("morph") +
		2 + utf8_display_width("(v)") +
		utf8_display_width(MORPH_VERSION);
	pad = CLI_BANNER_CONTENT_WIDTH -
		(used > (size_t)INT_MAX ? INT_MAX : (int)used);
	printf(ANSI_CYAN "│" ANSI_RESET ANSI_DIM " >_ "
	       ANSI_RESET ANSI_BOLD "morph" ANSI_RESET
	       "  " ANSI_DIM "(v%s)" ANSI_RESET, MORPH_VERSION);
	for (int i = 0; i < pad; i++)
		putchar(' ');
	printf(ANSI_CYAN " │" ANSI_RESET "\n");
}

static void cli_print_banner_blank(void)
{
	printf(ANSI_CYAN "│" ANSI_RESET);
	for (int i = 0; i < CLI_BANNER_INNER_WIDTH; i++)
		putchar(' ');
	printf(ANSI_CYAN "│" ANSI_RESET "\n");
}

static void cli_print_banner_field(const char *label, const char *value,
				   const char *hint, int keep_tail)
{
	char clipped[BUFSIZ];
	int hint_width = hint ? CLI_BANNER_HINT_WIDTH : 0;
	int value_width = CLI_BANNER_CONTENT_WIDTH -
		CLI_BANNER_LABEL_WIDTH - hint_width;

	(void)utf8_copy_ellipsized_display_width(
		clipped, sizeof(clipped), value, (size_t)value_width,
		keep_tail);
	printf(ANSI_CYAN "│" ANSI_RESET " " ANSI_DIM);
	print_padded(label, CLI_BANNER_LABEL_WIDTH);
	printf(ANSI_RESET ANSI_BOLD);
	print_padded(clipped, value_width);
	printf(ANSI_RESET);
	if (hint) {
		printf(ANSI_DIM);
		print_padded(hint, CLI_BANNER_HINT_WIDTH);
		printf(ANSI_RESET);
	}
	printf(ANSI_CYAN " │" ANSI_RESET "\n");
}

/* ---- cli_run ---- */

static void cli_run_structured(struct cli_context *ctx)
{
	char line[BUFSIZ];
	morph_buf_t input;
	struct cli_command_job job;
	int rc;

	rc = cli_command_job_init(&job);
	if (rc != 0)
		return;
	rc = morph_buf_init(&input, BUFSIZ);
	if (rc != 0) {
		cli_command_job_cleanup(&job);
		return;
	}
	while (ctx->running || job.active) {
		struct pollfd fds[2];
		int wake_fd = cli_ui_wake_fd(ctx);
		int nfds = wake_fd >= 0 ? 2 : 1;
		int poll_timeout = job.active ?
			CLI_STRUCTURED_POLL_TIMEOUT_MS : -1;

		fds[0].fd = job.active ? -1 : STDIN_FILENO;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		if (wake_fd >= 0) {
			fds[1].fd = wake_fd;
			fds[1].events = POLLIN;
			fds[1].revents = 0;
		}
		rc = poll(fds, (nfds_t)nfds, poll_timeout);
		if (rc < 0) {
			if (errno == EINTR) {
				cli_sigint_received = 0;
				continue;
			}
			break;
		}
		if ((wake_fd >= 0 &&
		     (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) ||
		    (job.active && rc == 0))
			(void)cli_ui_drain(ctx);
		if (cli_command_job_done(&job)) {
			int command_rc = cli_command_job_finish(&job);

			(void)cli_ui_drain(ctx);
			cli_turn_finish(ctx, command_rc);
		}
		if (!job.active &&
		    (fds[0].revents & (POLLIN | POLLHUP))) {
			size_t len;
			int complete = 0;

			morph_buf_reset(&input);
			while (!complete && ctx->running) {
				int continuation;

				cli_cancel_state_reset();
				if (!fgets(line, sizeof(line), stdin)) {
					if (feof(stdin))
						ctx->running = 0;
					else
						clearerr(stdin);
					break;
				}
				len = strlen(line);
				complete = len > 0 && line[len - 1] == '\n';
				if (complete)
					line[--len] = '\0';
				if (len > 0 && line[len - 1] == '\r')
					line[--len] = '\0';
				continuation = complete && len > 0 &&
					line[len - 1] == '\\';
				if (continuation) {
					len--;
					complete = 0;
				}
				rc = morph_buf_append(&input, line, len);
				if (rc == 0 && continuation)
					rc = morph_buf_putc(&input, '\n');
				if (rc != 0) {
					morph_buf_reset(&input);
					break;
				}
			}
			if (input.len > 0) {
				cli_turn_begin(ctx);
				rc = cli_command_job_start(&job, ctx,
					morph_buf_cstr(&input));
				if (rc != 0)
					cli_turn_finish(ctx, rc);
			}
		}
	}
	if (job.active) {
		int command_rc = cli_command_job_finish(&job);

		(void)cli_ui_drain(ctx);
		cli_turn_finish(ctx, command_rc);
	}
	morph_buf_cleanup(&input);
	cli_command_job_cleanup(&job);
}

void cli_run(struct cli_context *ctx)
{
	const struct config *config;
	const char *workdir;
	const char *home;
	struct session current;
	morph_buf_t directory;

	if (!ctx)
		return;
	if (cli_scheduler_start(ctx) != 0)
		log_warn("failed to start task scheduler");
	if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON) {
		struct sigaction sa;

		cli_structured_signal_mode = 1;
		(void)setvbuf(stdin, NULL, _IONBF, 0);
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = cli_sigint_handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;
		sigaction(SIGINT, &sa, NULL);
		cli_run_structured(ctx);
		signal(SIGINT, SIG_DFL);
		cli_structured_signal_mode = 0;
		return;
	}
	config = runtime_config_get(ctx->runtime);
	workdir = runtime_workdir_get(ctx->runtime);
	memset(&current, 0, sizeof(current));
	(void)runtime_session_current(ctx->runtime, &current);
	memset(&directory, 0, sizeof(directory));
	if (morph_buf_init(&directory, BUFSIZ) != 0)
		return;
	home = getenv("HOME");
	if (workdir && workdir[0] && home && home[0] &&
	    strncmp(workdir, home, strlen(home)) == 0 &&
	    (workdir[strlen(home)] == '\0' ||
	     workdir[strlen(home)] == '/')) {
		(void)morph_buf_putc(&directory, '~');
		(void)morph_buf_puts(&directory, workdir + strlen(home));
	} else {
		(void)morph_buf_puts(&directory,
				    workdir && workdir[0] ? workdir : ".");
	}
	printf("\n" ANSI_CYAN
	       "╭───────────────"
	       "───────────────"
	       "───────────────"
	       "───────────────╮\n"
	       ANSI_RESET);
	cli_print_banner_title();
	cli_print_banner_blank();
	cli_print_banner_field("model:",
		config ? config->models.text.model : "model", NULL, 0);
	cli_print_banner_field("session:",
		current.display_id[0] ? current.display_id : "session",
		"/switch to change", 0);
	cli_print_banner_field("directory:", morph_buf_cstr(&directory), NULL, 1);
	printf(ANSI_CYAN
	       "╰───────────────"
	       "───────────────"
	       "───────────────"
	       "───────────────╯"
	       ANSI_RESET "\n\n");
	morph_buf_cleanup(&directory);
	printf(ANSI_DIM "  /help · ./image.png attach · Ctrl+J/Alt+Enter newline\n"
	       "  Enter send/adjust while running · Ctrl+Command+V image\n"
	       "  Esc/Ctrl-C cancel" ANSI_RESET "\n\n");
#ifndef HAVE_READLINE
	char line[BUFSIZ];
#endif

	struct sigaction sa;
	struct sigaction winch_sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = cli_sigint_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	memset(&winch_sa, 0, sizeof(winch_sa));
	winch_sa.sa_handler = cli_sigwinch_handler;
	sigemptyset(&winch_sa.sa_mask);
	winch_sa.sa_flags = 0;
	sigaction(SIGWINCH, &winch_sa, NULL);

#ifdef HAVE_READLINE
	using_history();
	int callback_installed = 1;
	struct cli_command_job job;
	int job_mutex_rc;

	job_mutex_rc = cli_command_job_init(&job);
	if (job_mutex_rc != 0) {
		CMD_ERROR("failed to initialize command worker: %s",
			  morph_strerror(job_mutex_rc));
		signal(SIGINT, SIG_DFL);
		signal(SIGWINCH, SIG_DFL);
		return;
	}
	if (cli_composer_init(&g_composer) != 0) {
		cli_command_job_cleanup(&job);
		signal(SIGINT, SIG_DFL);
		signal(SIGWINCH, SIG_DFL);
		return;
	}
	cli_structured_signal_mode = 1;
	g_comp_ctx = ctx;
	g_readline_ready_input = NULL;
	ctx->input_job = &job;
	rl_initialize();
	rl_getc_function = cli_readline_getc;
	rl_redisplay_function = cli_readline_redisplay;
	rl_attempted_completion_function = cmd_completion;
	cli_readline_configure();
	rl_callback_handler_install(cli_input_prompt(),
				    cli_readline_line_ready);
	cli_readline_configure();
	while (ctx->running || job.active) {
		struct pollfd fds[2];
		int nfds = 1;
		int wake_fd = cli_ui_wake_fd(ctx);
		int timeout_ms = cli_terminal_next_frame_ms(ctx);
		int rc;

		fds[0].fd = ctx->running ? STDIN_FILENO : -1;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		if (wake_fd >= 0) {
			fds[1].fd = wake_fd;
			fds[1].events = POLLIN;
			fds[1].revents = 0;
			nfds = 2;
		}
		rc = poll(fds, (nfds_t)nfds, timeout_ms);
		if (rc < 0) {
			if (errno == EINTR) {
				if (cli_sigwinch_received) {
					cli_sigwinch_received = 0;
					if (callback_installed)
						cli_readline_render_frame(ctx, 1);
					else {
						cli_terminal_resize(ctx);
						cli_terminal_render_frame(ctx, 1);
					}
				}
				if (cli_sigint_received) {
					cli_sigint_received = 0;
					if (callback_installed) {
						rl_replace_line("", 0);
						rl_point = 0;
						cli_readline_redisplay();
					}
				}
				continue;
			}
			CMD_ERROR("input polling failed: %s", strerror(errno));
			break;
		}
		if (rc == 0) {
			if (callback_installed)
				cli_readline_render_frame(ctx, 0);
			else
				cli_terminal_render_frame(ctx, 0);
			continue;
		}
		if (nfds == 2 && (fds[1].revents & POLLIN)) {
			if (callback_installed)
				cli_readline_drain_ui(ctx);
			else
				(void)cli_ui_drain(ctx);
		}
		if (cli_command_job_done(&job)) {
			int point;
			char *draft = cli_readline_suspend(ctx, &point);
			int turn_rc = cli_command_job_finish(&job);
			char *pending;

			(void)cli_ui_drain(ctx);
			cli_turn_finish(ctx, turn_rc);
			pending = cli_command_job_take_prompt(&job);
			if (pending) {
				cli_turn_begin(ctx);
				turn_rc = cli_command_job_start(&job, ctx, pending);
				free(pending);
				if (turn_rc != 0)
					cli_turn_finish(ctx, turn_rc);
			}
			if (draft)
				cli_readline_resume(ctx, draft, point);
		}
		if (fds[0].revents & (POLLIN | POLLHUP)) {
			int previous_point = rl_point;
			int image_start;
			int image_end;

			if (!job.active)
				cli_cancel_state_reset();
			rl_callback_read_char();
			if (!g_readline_ready_input &&
			    cli_composer_image_span(&g_composer, rl_line_buffer, rl_point,
						    0, &image_start, &image_end) &&
			    rl_point > image_start) {
				rl_point = rl_point < previous_point ? image_start : image_end;
				cli_readline_redisplay();
			}
			if (g_readline_ready_input) {
				char *input = g_readline_ready_input;

				g_readline_ready_input = NULL;
				rl_callback_handler_remove();
				callback_installed = 0;
				cli_terminal_composer_suspend(ctx);
				if (input[0] != '\0') {
					int handled = 0;
					int command_rc;

					add_history(input);
					if (input[0] != '/') {
						char *expanded = cli_composer_expand(&g_composer, input);

						if (expanded) {
							cli_composer_record_images(&g_composer, ctx, input);
							free(input);
							input = expanded;
						} else {
							handled = 1;
							CMD_ERROR("%s", morph_strerror(-ENOMEM));
						}
					}
					if (!handled && job.active) {
						command_rc = cli_command_job_prompt(&job, input);
						if (command_rc != 0)
							CMD_ERROR("%s", morph_strerror(command_rc));
						else
							printf(ANSI_DIM "  Requirement queued" ANSI_RESET "\n");
						handled = 1;
					} else if (!handled) {
						command_rc = cli_handle_media_path(
							ctx, input, &handled);
					}
					if (handled || input[0] == '/') {
						if (!handled)
							command_rc = cli_handle_command(
								ctx, input);
						(void)cli_ui_drain(ctx);
					} else {
						cli_turn_begin(ctx);
						command_rc = cli_command_job_start(
							&job, ctx, input);
						if (command_rc != 0)
							cli_turn_finish(ctx, command_rc);
					}
				}
				free(input);
				if (ctx->running) {
					cli_terminal_composer_resume(ctx);
					rl_callback_handler_install(
						cli_input_prompt(),
						cli_readline_line_ready);
					cli_readline_configure();
					callback_installed = 1;
				}
			}
		}
	}
	if (g_readline_ready_input) {
		free(g_readline_ready_input);
		g_readline_ready_input = NULL;
	}
	if (callback_installed)
		rl_callback_handler_remove();
	if (job.active) {
		int turn_rc = cli_command_job_finish(&job);

		(void)cli_ui_drain(ctx);
		cli_turn_finish(ctx, turn_rc);
	}
	cli_composer_cleanup(&g_composer);
	ctx->input_job = NULL;
	rl_getc_function = rl_getc;
	rl_redisplay_function = rl_redisplay;
	cli_command_job_cleanup(&job);
	g_comp_ctx = NULL;
	cli_structured_signal_mode = 0;
#else
	morph_buf_t input;
	if (morph_buf_init(&input, BUFSIZ) != 0)
		return;
	while (ctx->running) {
		int complete = 0;

		(void)cli_ui_drain(ctx);
		morph_buf_reset(&input);
		while (!complete) {
			size_t len;
			int has_newline;
			int continuation;

			printf(input.len == 0 ?
				ANSI_BOLD ANSI_CYAN "› " ANSI_RESET :
				ANSI_DIM "… " ANSI_RESET);
			fflush(stdout);
			cli_cancel_state_reset();
			if (!fgets(line, sizeof(line), stdin)) {
				if (cli_sigint_received) {
					cli_sigint_received = 0;
					clearerr(stdin);
					morph_buf_reset(&input);
					break;
				}
				if (feof(stdin)) {
					ctx->running = 0;
					break;
				}
				clearerr(stdin);
				continue;
			}
			len = strlen(line);
			has_newline = len > 0 && line[len - 1] == '\n';
			if (has_newline)
				line[--len] = '\0';
			if (len > 0 && line[len - 1] == '\r')
				line[--len] = '\0';
			continuation = has_newline && len > 0 &&
				line[len - 1] == '\\';
			if (continuation)
				len--;
			if (morph_buf_append(&input, line, len) != 0) {
				CMD_ERROR("input is too large");
				morph_buf_reset(&input);
				break;
			}
			if (continuation) {
				if (morph_buf_putc(&input, '\n') != 0) {
					CMD_ERROR("input is too large");
					morph_buf_reset(&input);
					break;
				}
			} else if (has_newline) {
				complete = 1;
			}
		}
		if (input.len > 0) {
			int turn_rc;

			cli_turn_begin(ctx);
			turn_rc = cli_handle_command(ctx, morph_buf_cstr(&input));
			(void)cli_ui_drain(ctx);
			cli_turn_finish(ctx, turn_rc);
		}
		(void)cli_ui_drain(ctx);
	}
	morph_buf_cleanup(&input);
#endif
	signal(SIGINT, SIG_DFL);
	signal(SIGWINCH, SIG_DFL);
}
