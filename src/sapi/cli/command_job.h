#ifndef CLI_COMMAND_JOB_H
#define CLI_COMMAND_JOB_H

#include <pthread.h>
#include "util/array.h"

struct react_action;

struct cli_context;
typedef int (*cli_command_job_fn)(struct cli_context *ctx,
				  const char *input, void *user_data);

struct cli_command_job {
	struct cli_context *ctx;
	char *input;
	cli_command_job_fn fn;
	void *user_data;
	pthread_t thread;
	pthread_mutex_t mutex;
	int active;
	int done;
	int result;
	morph_array_t prompts;
	char *delivered_prompt;
};

int cli_command_job_init(struct cli_command_job *job);
void cli_command_job_cleanup(struct cli_command_job *job);
int cli_command_job_start(struct cli_command_job *job,
			  struct cli_context *ctx, const char *input);
int cli_command_job_start_fn(struct cli_command_job *job,
			     struct cli_context *ctx, const char *input,
			     cli_command_job_fn fn, void *user_data);
int cli_command_job_done(struct cli_command_job *job);
int cli_command_job_finish(struct cli_command_job *job);
int cli_command_job_prompt_pending(void *opaque);
int cli_command_job_prompt(struct cli_command_job *job, const char *text);
char *cli_command_job_take_prompt(struct cli_command_job *job);
int cli_command_job_drain(void *opaque, struct react_action *out, int timeout);
int cli_command_job_wait(struct cli_command_job *job);

#endif
