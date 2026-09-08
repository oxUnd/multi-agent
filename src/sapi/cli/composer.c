#include "sapi/cli/internal.h"
#include "sapi/cli/composer.h"

#define CLI_IMAGE_LABEL_MAX 64

struct cli_input_image {
	char *path;
	int width;
	int height;
	char label[CLI_IMAGE_LABEL_MAX];
};

int cli_composer_init(struct cli_composer *composer)
{
	struct cli_input_image image;
	int rc;

	memset(composer, 0, sizeof(*composer));
	rc = morph_array_init(&composer->images, MORPH_ARRAY_INIT_CAP, sizeof(image));
	if (rc != 0)
		MORPH_RETURN(rc);
	return 0;
}

void cli_composer_cleanup(struct cli_composer *composer)
{
	struct cli_input_image *image;

	morph_array_foreach(image, &composer->images, struct cli_input_image)
		free(image->path);
	morph_array_cleanup(&composer->images);
}

int cli_composer_add_image(struct cli_composer *composer, const char *path,
			   const char **label)
{
	struct cli_input_image *image;
	char *expanded = file_expand_path(path);
	char *absolute;
	int width;
	int height;
	int channels;

	if (!expanded)
		MORPH_RETURN(-ENOMEM);
	absolute = realpath(expanded, NULL);
	free(expanded);
	if (!absolute)
		MORPH_RETURN_ERRNO();
	if (!stbi_info(absolute, &width, &height, &channels)) {
		free(absolute);
		MORPH_RETURN(MORPH_ERR_FORMAT);
	}
	if (composer->next_image == SIZE_MAX) {
		free(absolute);
		MORPH_RETURN(-EOVERFLOW);
	}
	image = morph_array_push(&composer->images);
	if (!image) {
		free(absolute);
		MORPH_RETURN(-ENOMEM);
	}
	image->path = absolute;
	image->width = width;
	image->height = height;
	composer->next_image++;
	snprintf(image->label, sizeof(image->label), "[IMAGE#%zu]",
		 composer->next_image);
	*label = image->label;
	return 0;
}

/* Preserve source spans while decoding dragged paths (quotes and escaped
 * spaces). Ordinary text, including its whitespace, remains byte-for-byte. */
int cli_composer_convert_paths(struct cli_composer *composer, const char *input,
			       char **output)
{
	morph_buf_t path;
	morph_buf_t result;
	const char *cursor = input;
	const char *consumed = input;
	int converted = 0;
	int rc;

	*output = NULL;
	rc = morph_buf_init(&path, BUFSIZ);
	if (rc != 0)
		MORPH_RETURN(rc);
	rc = morph_buf_init(&result, strlen(input) + 1);
	if (rc != 0) {
		morph_buf_cleanup(&path);
		MORPH_RETURN(rc);
	}
	while (*cursor) {
		const char *start;
		const char *label;
		char quote = 0;
		char *expanded;

		while (isspace((unsigned char)*cursor))
			cursor++;
		if (!*cursor)
			break;
		start = cursor;
		morph_buf_reset(&path);
		while (*cursor && (quote || !isspace((unsigned char)*cursor))) {
			char ch = *cursor++;

			if (ch == '\\' && quote != '\'' && *cursor) {
				(void)morph_buf_putc(&path, *cursor++);
			} else if (ch == quote) {
				quote = 0;
			} else if (!quote && (ch == '\'' || ch == '"')) {
				quote = ch;
			} else {
				(void)morph_buf_putc(&path, ch);
			}
		}
		if (quote || path.failed)
			break;
		expanded = file_expand_path(morph_buf_cstr(&path));
		if (!expanded)
			break;
		if (!file_exists(expanded)) {
			free(expanded);
			continue;
		}
		rc = cli_composer_add_image(composer, expanded, &label);
		free(expanded);
		if (rc != 0)
			continue;
		(void)morph_buf_append(&result, consumed, (size_t)(start - consumed));
		(void)morph_buf_puts(&result, label);
		consumed = cursor;
		converted++;
	}
	(void)morph_buf_puts(&result, consumed);
	if (result.failed || path.failed)
		rc = -ENOMEM;
	else {
		*output = morph_buf_detach(&result);
		rc = converted;
	}
	morph_buf_cleanup(&result);
	morph_buf_cleanup(&path);
	if (rc < 0)
		MORPH_RETURN(rc);
	return rc;
}

void cli_composer_record_images(struct cli_composer *composer,
				struct cli_context *ctx, const char *input)
{
	const struct config *config = runtime_config_get(ctx->runtime);
	struct cli_input_image *image;

	if (!config)
		return;
	morph_array_foreach(image, &composer->images, struct cli_input_image) {
		if (strstr(input, image->label))
			cli_record_media_credits(ctx, "image_input",
				credit_image_units_from_size(image->width, image->height), 0,
				config->models.image.provider, config->models.image.model, NULL);
	}
}

char *cli_composer_expand(struct cli_composer *composer, const char *input)
{
	morph_buf_t result;
	const char *cursor = input;

	if (morph_buf_init(&result, strlen(input) + 1) != 0)
		return NULL;
	while (*cursor) {
		struct cli_input_image *image;
		struct cli_input_image *matched = NULL;
		const char *next = NULL;

		morph_array_foreach(image, &composer->images, struct cli_input_image) {
			const char *found = strstr(cursor, image->label);

			if (found && (!next || found < next)) {
				next = found;
				matched = image;
			}
		}
		if (!matched) {
			(void)morph_buf_puts(&result, cursor);
			break;
		}
		(void)morph_buf_append(&result, cursor, (size_t)(next - cursor));
		(void)morph_buf_printf(&result, "%s [Image: %s]", matched->label,
				       matched->path);
		cursor = next + strlen(matched->label);
	}
	if (result.failed) {
		morph_buf_cleanup(&result);
		return NULL;
	}
	return morph_buf_detach(&result);
}

int cli_composer_image_span(struct cli_composer *composer, const char *input,
			    int point, int backward, int *start, int *end)
{
	struct cli_input_image *image;

	morph_array_foreach(image, &composer->images, struct cli_input_image) {
		const char *found = input;

		while ((found = strstr(found, image->label))) {
			int left = (int)(found - input);
			int right = left + (int)strlen(image->label);

			if (backward ? point > left && point <= right :
			    point >= left && point < right) {
				*start = left;
				*end = right;
				return 1;
			}
			found += strlen(image->label);
		}
	}
	return 0;
}
