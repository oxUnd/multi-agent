#ifndef CLI_COMPOSER_H
#define CLI_COMPOSER_H

#include "util/array.h"

struct cli_context;

struct cli_composer {
	morph_array_t images;
	size_t next_image;
};

int cli_composer_init(struct cli_composer *composer);
void cli_composer_cleanup(struct cli_composer *composer);
int cli_composer_add_image(struct cli_composer *composer, const char *path,
			   const char **label);
int cli_composer_convert_paths(struct cli_composer *composer, const char *input,
			       char **output);
void cli_composer_record_images(struct cli_composer *composer,
				struct cli_context *ctx, const char *input);
char *cli_composer_expand(struct cli_composer *composer, const char *input);
int cli_composer_image_span(struct cli_composer *composer, const char *input,
			    int point, int backward, int *start, int *end);

#endif
