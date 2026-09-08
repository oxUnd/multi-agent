#include "sapi/cli/commands/registry.h"

int cli_attach_image(struct cli_context *ctx, const char *path)
{
	char *expanded;
	const struct config *config;
	int w = 0;
	int h = 0;
	int ch = 0;

	if (!ctx || !path || !path[0])
		MORPH_RETURN(-EINVAL);
	expanded = file_expand_path(path);
	if (!expanded)
		MORPH_RETURN(-ENOMEM);
	if (!file_exists(expanded)) {
		CMD_ERROR("file not found: %s", expanded);
		free(expanded);
		MORPH_RETURN(-ENOENT);
	}
	if (!stbi_info(expanded, &w, &h, &ch)) {
		CMD_ERROR("not a valid image file: %s", expanded);
		free(expanded);
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}
	strncpy(ctx->image_path, expanded, sizeof(ctx->image_path) - 1);
	ctx->image_path[sizeof(ctx->image_path) - 1] = '\0';
	config = runtime_config_get(ctx->runtime);
	cli_record_media_credits(ctx, "image_input",
				 credit_image_units_from_size(w, h), 0,
				 config->models.image.provider,
				 config->models.image.model, NULL);
	CMD_OK("[IMAGE#1]");
	free(expanded);
	return 0;
}

static int cmd_image(struct cli_context *ctx, int argc, char **argv)
{
	const char *path = cli_cmd_arg(argc, argv, 1);

	if (!path) {
		CMD_ERROR("usage: /image <file_path>");
		MORPH_RETURN(-EINVAL);
	}
	return cli_attach_image(ctx, path);
}

static int media_path_is_video(const char *path)
{
	const char *ext = strrchr(path, '.');

	if (!ext)
		return 0;
	ext++;
	return strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
		strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
		strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0;
}

static int media_play_video(struct cli_context *ctx, const char *path)
{
	const struct config *config = runtime_config_get(ctx->runtime);

	if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
	    video_play(path, config->render.mpv_args) != 0) {
		CMD_ERROR("failed to play video: %s", path);
		MORPH_RETURN(-EIO);
	}
	cli_record_media_credits(ctx, "video_input", 0, 1,
				 config->models.video.provider,
				 config->models.video.model,
				 "{\"estimated\":true}");
	CMD_OK("video loaded: %s", path);
	return 0;
}

int cli_handle_media_path(struct cli_context *ctx, const char *input,
			  int *handled)
{
	char *copy;
	char *expanded;
	char *argv[3];
	int argc;
	int width;
	int height;
	int channels;
	int rc = 0;

	if (!ctx || !input || !handled)
		MORPH_RETURN(-EINVAL);
	*handled = 0;
	copy = strdup(input);
	if (!copy)
		MORPH_RETURN(-ENOMEM);
	argc = cli_argv_split(copy, argv, 3);
	if (argc != 1) {
		free(copy);
		return 0;
	}
	expanded = file_expand_path(argv[0]);
	free(copy);
	if (!expanded)
		MORPH_RETURN(-ENOMEM);
	if (!file_exists(expanded)) {
		free(expanded);
		return 0;
	}
	if (stbi_info(expanded, &width, &height, &channels)) {
		*handled = 1;
		rc = cli_attach_image(ctx, expanded);
	} else if (media_path_is_video(expanded)) {
		*handled = 1;
		rc = media_play_video(ctx, expanded);
	}
	free(expanded);
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}
static int cmd_render(struct cli_context *ctx, int argc, char **argv)
{
	const char *path = cli_cmd_arg(argc, argv, 1);
	if (!path) {
		CMD_ERROR("usage: /render <file_path>");
		return -EINVAL;
	}
	char *expanded = file_expand_path(path);
	if (!file_exists(expanded)) {
		CMD_ERROR("file not found: %s", expanded);
		free(expanded);
		return -ENOENT;
	}
	const char *ext = strrchr(expanded, '.');
	if (ext) ext++;
	if (ext && (strcasecmp(ext, "mp4") == 0 || strcasecmp(ext, "mov") == 0 ||
		    strcasecmp(ext, "avi") == 0 || strcasecmp(ext, "mkv") == 0 ||
		    strcasecmp(ext, "webm") == 0 || strcasecmp(ext, "flv") == 0)) {
		const struct config *config = runtime_config_get(ctx->runtime);
		if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON &&
		    video_play(expanded, config->render.mpv_args) != 0) {
			CMD_ERROR("failed to play video: %s", expanded);
			free(expanded);
			return -EIO;
		}
		CMD_OK("video: %s", expanded);
	} else if (ext && (strcasecmp(ext, "png") == 0 || strcasecmp(ext, "jpg") == 0 ||
			   strcasecmp(ext, "jpeg") == 0 || strcasecmp(ext, "gif") == 0 ||
			   strcasecmp(ext, "webp") == 0 || strcasecmp(ext, "bmp") == 0 ||
			   strcasecmp(ext, "tga") == 0 || strcasecmp(ext, "hdr") == 0)) {
		int w = 0, h = 0, ch = 0;
		if (!stbi_info(expanded, &w, &h, &ch)) {
			CMD_ERROR("not a valid image file: %s", expanded);
			free(expanded);
			MORPH_RETURN(MORPH_ERR_FORMAT);
		}
		if (ctx->presentation_mode != CLI_PRESENT_EVENTS_JSON)
			image_render_terminal(expanded);
		CMD_OK("image: %s (%dx%d)", expanded, w, h);
	} else {
		size_t len = 0;
		char *text = file_read_all(expanded, &len);
		if (!text) {
			CMD_ERROR("failed to read file: %s", expanded);
			free(expanded);
			return -EIO;
		}
		if (ctx->presentation_mode == CLI_PRESENT_EVENTS_JSON)
			printf("%s", text);
		else
			cli_markdown_render_ansi(text);
		free(text);
	}
	free(expanded);
	return 0;
}


static const struct cli_command media_commands[] = {
	{ "/image",   cmd_image,   "Inject an image into context",      "/image <file_path>" },
	{ "/render",  cmd_render,  "Render a file (image/video/markdown)", "/render <file_path>" },
	{ "/r",       cmd_render,  "Alias for /render",                 "/r <file_path>" },
};

int cli_register_media_commands(void)
{
	return cli_command_register_many(media_commands,
					 (int)(sizeof(media_commands) /
					 sizeof(media_commands[0])),
					 "Media");
}
