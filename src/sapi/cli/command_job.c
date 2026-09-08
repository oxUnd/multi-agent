#include "sapi/cli/internal.h"

#define CLI_COMMAND_POLL_TIMEOUT_MS 100

struct cli_prompt {
	char *text;
	char *payload;
};

static void *cli_command_job_run(void *opaque)
{
	struct cli_command_job *job = opaque;
	int result;

	result = job->fn(job->ctx, job->input, job->user_data);
	pthread_mutex_lock(&job->mutex);
	job->result = result;
	job->done = 1;
	pthread_mutex_unlock(&job->mutex);
	cli_ui_notify(job->ctx);
	return NULL;
}

static int cli_command_handle(struct cli_context *ctx, const char *input,
			      void *user_data)
{
	(void)user_data;
	return cli_handle_command(ctx, input);
}

int cli_command_job_init(struct cli_command_job *job)
{
	struct cli_prompt prompt;
	int rc;

	if (!job)
		MORPH_RETURN(-EINVAL);
	memset(job, 0, sizeof(*job));
	rc = pthread_mutex_init(&job->mutex, NULL);
	if (rc != 0)
		MORPH_RETURN(-rc);
	rc = morph_array_init(&job->prompts, MORPH_ARRAY_INIT_CAP,
			      sizeof(prompt));
	if (rc != 0) {
		pthread_mutex_destroy(&job->mutex);
		MORPH_RETURN(rc);
	}
	return 0;
}

void cli_command_job_cleanup(struct cli_command_job *job)
{
	if (!job)
		return;
	if (job->active)
		(void)cli_command_job_finish(job);
	for (size_t i = 0; i < job->prompts.nelts; i++) {
		struct cli_prompt *prompt = morph_array_get(&job->prompts, i);

		free(prompt->text);
		free(prompt->payload);
	}
	morph_array_cleanup(&job->prompts);
	free(job->delivered_prompt);
	pthread_mutex_destroy(&job->mutex);
}

int cli_command_job_start(struct cli_command_job *job,
			  struct cli_context *ctx, const char *input)
{
	return cli_command_job_start_fn(job, ctx, input,
					cli_command_handle, NULL);
}

int cli_command_job_start_fn(struct cli_command_job *job,
			     struct cli_context *ctx, const char *input,
			     cli_command_job_fn fn, void *user_data)
{
	int thread_rc;

	if (!job || !ctx || !input || !fn || job->active)
		MORPH_RETURN(-EINVAL);
	job->input = strdup(input);
	if (!job->input)
		MORPH_RETURN(-ENOMEM);
	job->ctx = ctx;
	job->fn = fn;
	job->user_data = user_data;
	job->done = 0;
	job->result = 0;
	job->active = 1;
	thread_rc = pthread_create(&job->thread, NULL,
				   cli_command_job_run, job);
	if (thread_rc != 0) {
		job->active = 0;
		free(job->input);
		job->input = NULL;
		MORPH_RETURN(-thread_rc);
	}
	return 0;
}

int cli_command_job_done(struct cli_command_job *job)
{
	int done;

	if (!job || !job->active)
		return 0;
	pthread_mutex_lock(&job->mutex);
	done = job->done;
	pthread_mutex_unlock(&job->mutex);
	return done;
}

int cli_command_job_finish(struct cli_command_job *job)
{
	int result;

	if (!job || !job->active)
		MORPH_RETURN(-EINVAL);
	pthread_join(job->thread, NULL);
	result = job->result;
	free(job->input);
	job->input = NULL;
	job->fn = NULL;
	job->user_data = NULL;
	job->active = 0;
	job->done = 0;
	return result;
}

int cli_command_job_wait(struct cli_command_job *job)
{
	struct cli_context *ctx;
	int warned = 0;

	if (!job || !job->active || !job->ctx)
		MORPH_RETURN(-EINVAL);
	ctx = job->ctx;
	while (!cli_command_job_done(job)) {
		struct pollfd fd;
		int wake_fd = cli_ui_wake_fd(ctx);
		int nfds = wake_fd >= 0 ? 1 : 0;
		int rc;

		fd.fd = wake_fd;
		fd.events = POLLIN;
		fd.revents = 0;
		rc = poll(nfds > 0 ? &fd : NULL, (nfds_t)nfds,
			  CLI_COMMAND_POLL_TIMEOUT_MS);
		if (rc < 0 && errno == EINTR)
			continue;
		if (rc < 0 && !warned) {
			log_warn("command UI polling failed: %s", strerror(errno));
			warned = 1;
		}
		(void)cli_ui_drain(ctx);
	}
	(void)cli_ui_drain(ctx);
	return cli_command_job_finish(job);
}

int cli_command_job_prompt(struct cli_command_job *job, const char *text)
{
	cJSON *payload;
	char *json;
	struct cli_prompt prompt;
	struct cli_prompt *slot;

	if (!job || !text || !text[0])
		MORPH_RETURN(-EINVAL);
	payload = cJSON_CreateObject();
	if (!payload)
		MORPH_RETURN(-ENOMEM);
	if (!cJSON_AddStringToObject(payload, "text", text)) {
		cJSON_Delete(payload);
		MORPH_RETURN(-ENOMEM);
	}
	json = cJSON_PrintUnformatted(payload);
	cJSON_Delete(payload);
	if (!json)
		MORPH_RETURN(-ENOMEM);
	prompt.text = strdup(text);
	prompt.payload = json;
	if (!prompt.text) {
		free(json);
		MORPH_RETURN(-ENOMEM);
	}
	pthread_mutex_lock(&job->mutex);
	slot = morph_array_push(&job->prompts);
	if (slot)
		*slot = prompt;
	pthread_mutex_unlock(&job->mutex);
	if (!slot) {
		free(prompt.text);
		free(json);
		MORPH_RETURN(-ENOMEM);
	}
	return 0;
}

/* The returned payload stays valid until the next drain, including when the
 * producer appends another prompt while ReAct is processing this one. */
int cli_command_job_drain(void *opaque, struct react_action *out, int timeout)
{
	struct cli_command_job *job = opaque;
	struct cli_prompt *items;

	(void)timeout;
	pthread_mutex_lock(&job->mutex);
	free(job->delivered_prompt);
	job->delivered_prompt = NULL;
	if (job->prompts.nelts > 0) {
		items = job->prompts.elts;
		job->delivered_prompt = items[0].payload;
		free(items[0].text);
		job->prompts.nelts--;
		memmove(items, items + 1, job->prompts.nelts * sizeof(*items));
	}
	pthread_mutex_unlock(&job->mutex);
	out->type = "prompt";
	out->payload_json = job->delivered_prompt;
	return out->payload_json ? 1 : 0;
}

/* Called after joining the worker: submissions that missed the final drain
 * become the next turn instead of disappearing at the completion boundary. */
char *cli_command_job_take_prompt(struct cli_command_job *job)
{
	struct cli_prompt *items;
	char *text = NULL;

	pthread_mutex_lock(&job->mutex);
	if (job->prompts.nelts > 0) {
		items = job->prompts.elts;
		text = items[0].text;
		free(items[0].payload);
		job->prompts.nelts--;
		memmove(items, items + 1, job->prompts.nelts * sizeof(*items));
	}
	pthread_mutex_unlock(&job->mutex);
	return text;
}

int cli_command_job_prompt_pending(void *opaque)
{
	struct cli_command_job *job = opaque;
	int pending;

	pthread_mutex_lock(&job->mutex);
	pending = job->prompts.nelts > 0;
	pthread_mutex_unlock(&job->mutex);
	return pending;
}
