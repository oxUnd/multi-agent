#include "sapi/cli/commands/registry.h"

static const char *memory_scope_display(enum memory_clear_scope scope)
{
	switch (scope) {
	case MEMORY_CLEAR_ALL:
		return "all";
	case MEMORY_CLEAR_FACTS:
		return "facts";
	case MEMORY_CLEAR_EPISODES:
		return "episodes";
	case MEMORY_CLEAR_PROCEDURES:
		return "procedures";
	default:
		return "unknown";
	}
}

static int memory_parse_scope(const char *name, enum memory_clear_scope *scope)
{
	if (!scope)
		return -EINVAL;
	if (!name || strcmp(name, "all") == 0) {
		*scope = MEMORY_CLEAR_ALL;
		return 0;
	}
	if (strcmp(name, "facts") == 0) {
		*scope = MEMORY_CLEAR_FACTS;
		return 0;
	}
	if (strcmp(name, "episodes") == 0) {
		*scope = MEMORY_CLEAR_EPISODES;
		return 0;
	}
	if (strcmp(name, "procedures") == 0 ||
	    strcmp(name, "rules") == 0) {
		*scope = MEMORY_CLEAR_PROCEDURES;
		return 0;
	}
	return -EINVAL;
}

static int cmd_memory(struct cli_context *ctx, int argc, char **argv)
{
	const char *sub = cli_cmd_arg(argc, argv, 1);

	if (sub && !strcmp(sub, "jobs")) {
		char *jobs = runtime_memory_background_render(ctx->runtime);

		if (!jobs)
			MORPH_RETURN(MORPH_ERR_DB);
		CMD_HEADER("Background jobs: queued = pending; completed = finished");
		(void)cli_print_untrusted_text(jobs, UTF8_TERMINAL_TEXT_MULTILINE);
		free(jobs);
		return 0;
	}

	if (sub && (!strcmp(sub, "set") || !strcmp(sub, "unset"))) {
		int setting = !strcmp(sub, "set");
		const char *key = cli_cmd_arg(argc, argv, 2);
		const char *value = setting ? cli_cmd_arg(argc, argv, 3) : NULL;
		const char *scope = cli_cmd_arg(argc, argv, setting ? 4 : 3);
		int rc;

		if (!key || (setting && !value) || argc > (setting ? 5 : 4)) {
			CMD_ERROR("/memory set <key> <value> [personal|project|session]"
				"; /memory unset <key> [personal|project|session]");
			MORPH_RETURN(-EINVAL);
		}
		rc = runtime_preference_set(ctx->runtime, scope ? scope : "personal", key, value);
		if (rc != 0) {
			CMD_ERROR("Failed to update preference: %s", morph_strerror(rc));
			return rc;
		}
		if (setting)
			CMD_OK("Preference saved (%s); effective from the next model request.",
				scope ? scope : "personal");
		else
			CMD_OK("Preference unset (%s); inheritance restored for the next model request.",
				scope ? scope : "personal");
		{
			char *effective = runtime_preferences_render(ctx->runtime, 0);

			(void)cli_print_untrusted_text(effective ? effective : "",
				UTF8_TERMINAL_TEXT_MULTILINE);
			free(effective);
		}
		return 0;
	}
	if (sub && (!strcmp(sub, "history") || !strcmp(sub, "explain"))) {
		char *history = runtime_preferences_render(ctx->runtime, 1);
		char *effective = runtime_preferences_render(ctx->runtime, 0);

		if (!history || !effective) {
			free(history);
			free(effective);
			CMD_ERROR("Failed to read preferences");
			MORPH_RETURN(MORPH_ERR_DB);
		}
		CMD_HEADER("Effective preferences (session > project > personal; "
			"explicit settings override legacy records)");
		(void)cli_print_untrusted_text(effective, UTF8_TERMINAL_TEXT_MULTILINE);
		CMD_HEADER("Preference history and sources; "
			"unset removes a scope override and restores inheritance");
		(void)cli_print_untrusted_text(history, UTF8_TERMINAL_TEXT_MULTILINE);
		free(history);
		free(effective);
		return 0;
	}

	if (!sub || strcmp(sub, "show") == 0 || strcmp(sub, "view") == 0) {
		/*
		 * Pass 0 to render every stored episode/change so /mem
		 * gives the user the full picture; max_episodes is only
		 * a hint for the React-loop context window.
		 */
		char *rendered = runtime_memory_render_current(ctx->runtime, 0);
		CMD_HEADER("memory (%s)", runtime_session_current_name(ctx->runtime));
		(void)cli_print_untrusted_text(rendered ? rendered :
		       "No long-term memory stored for this session.",
		       UTF8_TERMINAL_TEXT_MULTILINE);
		printf("\n");
		free(rendered);
		return 0;
	}

	if (strcmp(sub, "clear") == 0) {
		enum memory_clear_scope scope = MEMORY_CLEAR_ALL;
		const char *target = cli_cmd_arg(argc, argv, 2);
		if (memory_parse_scope(target, &scope) != 0) {
			CMD_ERROR("usage: /memory clear [all|facts|episodes|procedures]");
			return -EINVAL;
		}
		if (runtime_memory_clear_current(ctx->runtime, scope) != 0) {
			CMD_ERROR("failed to clear memory");
			return -EIO;
		}
		CMD_OK("cleared %s memory for session: %s (personal/project preferences retained)",
		       memory_scope_display(scope), runtime_session_current_name(ctx->runtime));
		return 0;
	}

	CMD_ERROR("usage: /memory [show|history|explain|set|unset|clear|jobs]");
	return -EINVAL;
}


static const struct cli_command memory_commands[] = {
	{ "/memory",  cmd_memory,  "Show or clear long-term memory",    "/memory [show|history|explain|set|unset|clear|jobs]" },
	{ "/mem",     cmd_memory,  "Alias for /memory",                 "/mem [show|clear] [all|facts|episodes|procedures]" },
};

int cli_register_memory_commands(void)
{
	return cli_command_register_many(memory_commands,
					 (int)(sizeof(memory_commands) /
					 sizeof(memory_commands[0])),
					 "Memory");
}
