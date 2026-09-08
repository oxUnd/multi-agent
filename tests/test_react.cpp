#include <gtest/gtest.h>
#include "agent/react.h"
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/tokenizer.h"
#include "models/llm.h"
#include "util/arena.h"
#include "http/client.h"
#include "http/sse.h"
#include "config/config.h"
#include "db/database.h"
#include "session.h"
#include "util/error.h"
#include "util/file.h"
#include "util/utf8.h"
#include <string.h>
#include <signal.h>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>
#include <atomic>
#include <cstdio>
#include <limits.h>
#include <unistd.h>

struct sse_test_info {
	int count;
	std::string last_data;
};

static int global_sse_write_adapter(const char *data, size_t len, void *user_data)
{
	struct sse_parser *parser = (struct sse_parser *)user_data;
	sse_parser_feed(parser, data, len);
	return 0;
}

/* ---- mock tools ---- */

static int test_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_success_json_text(result, strdup("{\"result\":\"test\"}"));
	return 0;
}

static int large_tool_fn(const char *args_json, struct tool_result *result,
			 void *user_data)
{
	size_t bytes = *(size_t *)user_data;
	char *text;

	(void)args_json;
	text = (char *)malloc(bytes + 1);
	if (!text)
		return -ENOMEM;
	for (size_t i = 0; i < bytes; i++)
		text[i] = "word "[i % 5];
	text[bytes] = '\0';
	(void)tool_result_success_json_text(result, text);
	return 0;
}

static int test_compress_cb(const char *text, void *user_data, char **out);

static int streaming_tool_fn(const char *args_json, struct tool_result *result,
			     void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_runtime_emit_stream("stream_tool", "text", "alpha ");
	(void)tool_runtime_emit_stream("stream_tool", "text", "beta");
	(void)tool_result_success_json_text(result, strdup("{\"result\":\"done\"}"));
	return 0;
}

static int failing_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_success_json_text(result, strdup("tool failed"));
	return -EIO;
}

static int json_object_tool_fn(const char *args_json,
			       struct tool_result *result, void *user_data)
{
	cJSON *root;

	(void)user_data;
	root = args_json ? cJSON_Parse(args_json) : nullptr;
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		(void)tool_result_error(result, "invalid_arguments",
			"arguments must be a valid JSON object; correct them and retry");
		return -EINVAL;
	}
	cJSON_Delete(root);
	(void)tool_result_success_json_text(result,
		strdup("{\"result\":\"ok\"}"));
	return 0;
}

struct text_tool_capture {
	int calls;
	std::string last_input;
};

static int text_tool_fn(const char *input, struct tool_result *result,
			void *user_data)
{
	struct text_tool_capture *capture =
		(struct text_tool_capture *)user_data;

	capture->calls++;
	capture->last_input = input ? input : "";
	if (!input || strstr(input, "*** Begin Patch") == nullptr) {
		(void)tool_result_error(result, "invalid_patch",
			"input must be a patch; correct it and retry");
		return -EINVAL;
	}
	(void)tool_result_success_json_text(result,
		strdup("{\"result\":\"ok\"}"));
	return 0;
}

static int slow_tool_fn(const char *args_json, struct tool_result *result,
			void *user_data)
{
	(void)args_json;
	(void)user_data;
	std::this_thread::sleep_for(std::chrono::milliseconds(2500));
	(void)tool_result_success_json_text(result, strdup("{\"result\":\"late\"}"));
	return 0;
}

static int not_configured_tool_fn(const char *args_json,
				  struct tool_result *result,
				  void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_success_json_text(result,
				    strdup("{\"error\":\"missing api key\"}"));
	return MORPH_ERR_NOT_CONFIGURED;
}

static int call_count_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	int *count = (int *)user_data;
	(*count)++;
	char buf[128];
	snprintf(buf, sizeof(buf), "{\"calls\":%d}", *count);
	(void)tool_result_success_json_text(result, strdup(buf));
	return 0;
}

static int artifact_tool_fn(const char *args_json, struct tool_result *result,
			    void *user_data)
{
	(void)args_json;
	(void)user_data;
	(void)tool_result_success_json_text(result,
		strdup("{\"message\":\"artifact ready\"}"));
	(void)tool_result_add_image(result, "/tmp/morph-event-test.png",
				    640, 480);
	return 0;
}

static int tool_register(enum tool_origin origin, struct tool_registry *reg,
			 const char *name, const char *description,
			 const char *input_schema, tool_exec_fn exec,
			 void *user_data,
			 tool_user_data_destroy_fn user_data_destroy)
{
	struct tool_spec spec = {};
	spec.origin = origin;
	spec.name = name;
	spec.description = description;
	if (!input_schema || strcmp(input_schema, "{}") == 0 ||
	    strcmp(input_schema, "{\"type\":\"object\"}") == 0)
		spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	else
		spec.input_schema = input_schema;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.exec = exec;
	spec.user_data = user_data;
	spec.user_data_destroy = user_data_destroy;
	return ::tool_register(reg, &spec);
}

/* ---- mock LLM helpers ---- */

struct mock_collect_data {
	char *buf;
	size_t len;
	size_t cap;
};

static int mock_collect_cb(const char *token, void *user_data)
{
	struct mock_collect_data *cd = (struct mock_collect_data *)user_data;
	size_t tlen = strlen(token);
	if (cd->len + tlen + 1 >= cd->cap) {
		cd->cap = (cd->len + tlen + 1) * 2;
		char *new_b = (char *)realloc(cd->buf, cd->cap);
		if (!new_b) return -ENOMEM;
		cd->buf = new_b;
	}
	memcpy(cd->buf + cd->len, token, tlen);
	cd->len += tlen;
	cd->buf[cd->len] = '\0';
	return 0;
}

/* ---- mock LLM ---- */

struct mock_llm_data {
	const char *response;
	int call_count;
	int fail_after;
	int should_fail;
	int sleep_ms;
};

static int mock_llm_chat(struct model *self, struct arena *arena,
			  const char *system_prompt,
			  const char **messages, int n,
			  const struct model_chat_options *opts,
			  sse_callback cb, void *user_data)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	(void)opts;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
	if (data->sleep_ms > 0)
		std::this_thread::sleep_for(
			std::chrono::milliseconds(data->sleep_ms));
	if (cb && data->response)
		cb(data->response, user_data);
	return 200;
}

static int mock_llm_streaming_chat(struct model *self, struct arena *arena,
				    const char *system_prompt,
				    const char **messages, int n,
				    const struct model_chat_options *opts,
				    sse_callback cb, void *user_data)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	(void)opts;
	struct mock_llm_data *data = (struct mock_llm_data *)self->handle;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (data->fail_after > 0 && data->call_count > data->fail_after)
		return -EIO;
	if (data->sleep_ms > 0)
		std::this_thread::sleep_for(
			std::chrono::milliseconds(data->sleep_ms));
	if (cb && data->response) {
		const char *r = data->response;
		size_t len = strlen(r);
		size_t chunk = len > 20 ? 20 : len;
		for (size_t i = 0; i < len; i += chunk) {
			size_t remaining = len - i;
			size_t this_chunk = remaining < chunk ? remaining : chunk;
			char *piece = (char *)malloc(this_chunk + 1);
			memcpy(piece, r + i, this_chunk);
			piece[this_chunk] = '\0';
			cb(piece, user_data);
			free(piece);
		}
	}
	return 200;
}

static char *strcasestr_local(const char *haystack, const char *needle)
{
	size_t nlen = strlen(needle);
	while (*haystack) {
		if (strncasecmp(haystack, needle, nlen) == 0)
			return (char *)haystack;
		haystack++;
	}
	return nullptr;
}

static void mock_emit_legacy_thought(const char *buf, char *final_pos,
				     sse_callback thought_cb, void *thought_ud)
{
	if (!buf || !final_pos || !thought_cb || final_pos <= buf)
		return;
	size_t len = (size_t)(final_pos - buf);
	char *thought = (char *)malloc(len + 1);
	if (!thought)
		return;
	memcpy(thought, buf, len);
	thought[len] = '\0';
	while (len > 0 && isspace((unsigned char)thought[len - 1]))
		thought[--len] = '\0';
	char *t = thought;
	if (strncasecmp(t, "Thought:", 8) == 0) {
		t += 8;
		while (*t == ' ')
			t++;
	}
	if (*t)
		thought_cb(t, thought_ud);
	free(thought);
}

static int mock_chat_with_tools(struct model *self, struct arena *arena,
				const char *system_prompt,
				struct chat_message *messages, int msg_count,
				struct tool_desc *tools, int tool_count,
				struct chat_response *response,
				sse_callback thought_cb, void *thought_ud)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)msg_count;
	(void)tools;
	(void)tool_count;

	struct mock_collect_data cd = {nullptr, 0, 0};
	cd.buf = (char *)malloc(8192);
	cd.cap = 8192;
	cd.buf[0] = '\0';

	int status = self->chat(self, arena, nullptr, nullptr, 0, nullptr,
				mock_collect_cb, &cd);
	if (status < 0) {
		free(cd.buf);
		return status;
	}

	memset(response, 0, sizeof(*response));

	char *action_pos = strcasestr_local(cd.buf, "Action:");
	if (action_pos && tool_count > 0) {
		if (action_pos > cd.buf) {
			size_t tlen = action_pos - cd.buf;
			char *thought = (char *)malloc(tlen + 1);
			memcpy(thought, cd.buf, tlen);
			thought[tlen] = '\0';
			while (tlen > 0 && isspace((unsigned char)thought[tlen-1]))
				thought[--tlen] = '\0';
			char *t = thought;
			if (strncasecmp(t, "Thought:", 8) == 0) {
				t += 8;
				while (*t == ' ') t++;
			}
			if (*t) {
				response->content = strdup(t);
				if (thought_cb)
					thought_cb(t, thought_ud);
			}
			free(thought);
		}

		const char *ap = action_pos + 7;
		while (*ap == ' ') ap++;
		char tool_name[64] = {0};
		int ni = 0;
		while (*ap && *ap != '(' && *ap != '\n' && ni < 63)
			tool_name[ni++] = *ap++;
		char *args = strdup("{}");
		if (*ap == '(') {
			ap++;
			const char *args_start = ap;
			int depth = 1;
			while (*ap && depth > 0) {
				if (*ap == '(') depth++;
				else if (*ap == ')') depth--;
				ap++;
			}
			size_t alen = (size_t)((ap - 1) - args_start);
			free(args);
			args = (char *)malloc(alen + 1);
			memcpy(args, args_start, alen);
			args[alen] = '\0';
		}
		response->tool_calls = (struct tool_call *)calloc(1, sizeof(*response->tool_calls));
		response->tool_call_count = 1;
		snprintf(response->tool_calls[0].id, sizeof(response->tool_calls[0].id),
			 "call_mock_%d", 0);
		strncpy(response->tool_calls[0].name, tool_name,
			sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = args;
	} else {
		const char *content = cd.buf;
		char *final_pos = strcasestr_local(cd.buf, "Final:");
		if (final_pos) {
			mock_emit_legacy_thought(cd.buf, final_pos,
						 thought_cb, thought_ud);
		} else {
			char *thought_pos = strcasestr_local(cd.buf, "Thought:");
			if (thought_pos) {
				thought_pos += 8;
				while (*thought_pos == ' ') thought_pos++;
				content = thought_pos;
			}
		}
		response->content = strdup(content);
		if (!final_pos && thought_cb && response->content &&
		    *response->content)
			thought_cb(response->content, thought_ud);
	}

	free(cd.buf);
	return 200;
}

static void mock_llm_destroy(struct model *self)
{
	if (self && self->handle) {
		free(self->handle);
		self->handle = NULL;
	}
	free(self);
}

static struct model *create_mock_llm(const char *response)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	if (!m) return NULL;
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "mock-model", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	strncpy(m->api_base, "http://localhost:1", sizeof(m->api_base) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = mock_llm_chat;
	m->chat_with_tools = mock_chat_with_tools;
	m->generate = NULL;
	m->destroy = mock_llm_destroy;
	struct mock_llm_data *data = (struct mock_llm_data *)calloc(1, sizeof(*data));
	data->response = response;
	data->call_count = 0;
	data->fail_after = 0;
	data->should_fail = 0;
	data->sleep_ms = 0;
	m->handle = data;
	return m;
}

static struct model *create_mock_streaming_llm(const char *response)
{
	struct model *m = create_mock_llm(response);
	m->chat = mock_llm_streaming_chat;
	return m;
}

struct direct_stream_data {
	const char **chunks;
	int chunk_count;
	const char *content;
	const char *final_content;
	int tool_on_first_call;
	int call_count;
};

static int direct_stream_chat_with_tools(struct model *self,
					 struct arena *arena,
					 const char *system_prompt,
					 struct chat_message *messages,
					 int msg_count,
					 struct tool_desc *tools,
					 int tool_count,
					 struct chat_response *response,
					 llm_stream_callback stream_cb,
					 void *stream_ud)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)msg_count;
	(void)tools;
	(void)tool_count;
	struct direct_stream_data *data =
		(struct direct_stream_data *)self->handle;
	memset(response, 0, sizeof(*response));
	data->call_count++;
	if (data->tool_on_first_call && data->call_count > 1) {
		int rc = stream_cb(LLM_STREAM_CONTENT, data->final_content,
				   stream_ud);
		if (rc != 0)
			return rc;
		response->content = strdup(data->final_content);
		return 200;
	}
	for (int i = 0; i < data->chunk_count; i++) {
		int rc = stream_cb(LLM_STREAM_CONTENT, data->chunks[i], stream_ud);
		if (rc != 0)
			return rc;
	}
	response->content = strdup(data->content);
	if (data->tool_on_first_call) {
		response->tool_calls =
			(struct tool_call *)calloc(1, sizeof(*response->tool_calls));
		response->tool_call_count = 1;
		strncpy(response->tool_calls[0].name, "test_tool",
			sizeof(response->tool_calls[0].name) - 1);
		snprintf(response->tool_calls[0].id,
			 sizeof(response->tool_calls[0].id), "typed_call_1");
		response->tool_calls[0].arguments = strdup("{}");
	}
	return 200;
}

static struct model *create_direct_stream_llm(const char **chunks,
					      int chunk_count,
					      const char *content)
{
	struct model *m = create_mock_llm(content);
	m->chat_with_tools_stream = direct_stream_chat_with_tools;
	struct direct_stream_data *data =
		(struct direct_stream_data *)calloc(1, sizeof(*data));
	data->chunks = chunks;
	data->chunk_count = chunk_count;
	data->content = content;
	free(m->handle);
	m->handle = data;
	return m;
}

static struct model *create_direct_stream_tool_llm(const char **chunks,
						    int chunk_count,
						    const char *thought,
						    const char *final_content)
{
	struct model *m = create_direct_stream_llm(chunks, chunk_count, thought);
	struct direct_stream_data *data =
		(struct direct_stream_data *)m->handle;
	data->tool_on_first_call = 1;
	data->final_content = final_content;
	return m;
}

/* ---- Multi-response mock: returns different responses per call ---- */

struct multi_mock_data {
	const char **responses;
	int count;
	int call_count;
	int should_fail;
	int replay_argument_checks;
	int invalid_replayed_arguments;
};

static int multi_mock_chat(struct model *self, struct arena *arena,
			   const char *system_prompt,
			   const char **messages, int n,
			   const struct model_chat_options *opts,
			   sse_callback cb, void *user_data)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	(void)opts;
	struct multi_mock_data *data = (struct multi_mock_data *)self->handle;
	int idx = data->call_count;
	data->call_count++;
	if (data->should_fail)
		return -EIO;
	if (idx < data->count && cb && data->responses[idx])
		cb(data->responses[idx], user_data);
	return 200;
}

static int multi_mock_chat_with_tools(struct model *self, struct arena *arena,
				      const char *system_prompt,
				      struct chat_message *messages, int msg_count,
				      struct tool_desc *tools, int tool_count,
				      struct chat_response *response,
				      sse_callback thought_cb, void *thought_ud)
{
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)msg_count;
	(void)tools;
	(void)tool_count;

	struct multi_mock_data *data = (struct multi_mock_data *)self->handle;
	int idx = data->call_count;
	for (int i = 0; i < msg_count; i++) {
		if (!messages[i].tool_calls)
			continue;
		for (int j = 0; j < messages[i].tool_call_count; j++) {
			cJSON *arguments = cJSON_Parse(
				messages[i].tool_calls[j].arguments);

			data->replay_argument_checks++;
			if (!cJSON_IsObject(arguments))
				data->invalid_replayed_arguments++;
			cJSON_Delete(arguments);
		}
	}

	struct mock_collect_data cd = {nullptr, 0, 0};
	cd.buf = (char *)malloc(8192);
	cd.cap = 8192;
	cd.buf[0] = '\0';

	if (data->should_fail) {
		free(cd.buf);
		return -EIO;
	}
	if (idx < data->count && data->responses[idx])
		mock_collect_cb(data->responses[idx], &cd);
	data->call_count++;

	memset(response, 0, sizeof(*response));

	char *action_pos = strcasestr_local(cd.buf, "Action:");
	if (action_pos && tool_count > 0) {
		if (action_pos > cd.buf) {
			size_t tlen = action_pos - cd.buf;
			char *thought = (char *)malloc(tlen + 1);
			memcpy(thought, cd.buf, tlen);
			thought[tlen] = '\0';
			while (tlen > 0 && isspace((unsigned char)thought[tlen-1]))
				thought[--tlen] = '\0';
			char *t = thought;
			if (strncasecmp(t, "Thought:", 8) == 0) {
				t += 8;
				while (*t == ' ') t++;
			}
			if (*t) {
				response->content = strdup(t);
				if (thought_cb)
					thought_cb(t, thought_ud);
			}
			free(thought);
		}

		const char *ap = action_pos + 7;
		while (*ap == ' ') ap++;
		char tool_name[64] = {0};
		int ni = 0;
		while (*ap && *ap != '(' && *ap != '\n' && ni < 63)
			tool_name[ni++] = *ap++;
		char *args = strdup("{}");
		if (*ap == '(') {
			ap++;
			const char *args_start = ap;
			int depth = 1;
			while (*ap && depth > 0) {
				if (*ap == '(') depth++;
				else if (*ap == ')') depth--;
				ap++;
			}
			size_t alen = (size_t)((ap - 1) - args_start);
			free(args);
			args = (char *)malloc(alen + 1);
			memcpy(args, args_start, alen);
			args[alen] = '\0';
		}
		response->tool_calls = (struct tool_call *)calloc(1, sizeof(*response->tool_calls));
		response->tool_call_count = 1;
		snprintf(response->tool_calls[0].id, sizeof(response->tool_calls[0].id),
			 "call_mock_%d", idx);
		strncpy(response->tool_calls[0].name, tool_name,
			sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = args;
	} else {
		const char *content = cd.buf;
		char *final_pos = strcasestr_local(cd.buf, "Final:");
		if (final_pos)
			mock_emit_legacy_thought(cd.buf, final_pos,
						 thought_cb, thought_ud);
		response->content = strdup(content);
		if (!final_pos && thought_cb && response->content &&
		    *response->content)
			thought_cb(response->content, thought_ud);
	}

	free(cd.buf);
	return 200;
}

static void multi_mock_destroy(struct model *self)
{
	if (self && self->handle) {
		free(self->handle);
		self->handle = NULL;
	}
	free(self);
}

static struct model *create_multi_mock_llm(const char **responses, int count)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	if (!m) return NULL;
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "mock-model", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	strncpy(m->api_base, "http://localhost:1", sizeof(m->api_base) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = multi_mock_chat;
	m->chat_with_tools = multi_mock_chat_with_tools;
	m->generate = NULL;
	m->destroy = multi_mock_destroy;
	struct multi_mock_data *d = (struct multi_mock_data *)calloc(1, sizeof(*d));
	d->responses = responses;
	d->count = count;
	d->call_count = 0;
	d->should_fail = 0;
	m->handle = d;
	return m;
}

struct slot_mock_data {
	int call_count;
	int last_msg_count;
	int duplicate_provider_ids;
	int empty_provider_ids;
	char assistant_tool_call_ids[2][128];
	char tool_message_ids[2][128];
};

static int slot_mock_chat(struct model *self, struct arena *arena,
			  const char *system_prompt,
			  const char **messages, int n,
			  const struct model_chat_options *opts,
			  sse_callback cb, void *user_data)
{
	(void)self;
	(void)arena;
	(void)system_prompt;
	(void)messages;
	(void)n;
	(void)opts;
	(void)cb;
	(void)user_data;
	return 200;
}

static int slot_mock_chat_with_tools(struct model *self, struct arena *arena,
				     const char *system_prompt,
				     struct chat_message *messages,
				     int msg_count,
				     struct tool_desc *tools, int tool_count,
				     struct chat_response *response,
				     sse_callback thought_cb, void *thought_ud)
{
	struct slot_mock_data *d = (struct slot_mock_data *)self->handle;

	(void)arena;
	(void)system_prompt;
	(void)tools;
	(void)tool_count;
	memset(response, 0, sizeof(*response));
	d->last_msg_count = msg_count;
	d->call_count++;
	if (d->call_count == 1) {
		if (thought_cb)
			thought_cb("run two tools", thought_ud);
		response->content = strdup("run two tools");
		response->tool_call_count = 2;
		response->tool_calls = (struct tool_call *)calloc(
			2, sizeof(*response->tool_calls));
		if (!response->tool_calls)
			return -ENOMEM;
		if (!d->empty_provider_ids) {
			snprintf(response->tool_calls[0].id,
				 sizeof(response->tool_calls[0].id), "%s",
				 d->duplicate_provider_ids ?
				 "slot_call_dup" : "slot_call_0");
		}
		strncpy(response->tool_calls[0].name, "slot_a",
			sizeof(response->tool_calls[0].name) - 1);
		response->tool_calls[0].arguments = strdup("{\"n\":1}");
		if (!d->empty_provider_ids) {
			snprintf(response->tool_calls[1].id,
				 sizeof(response->tool_calls[1].id), "%s",
				 d->duplicate_provider_ids ?
				 "slot_call_dup" : "slot_call_1");
		}
		strncpy(response->tool_calls[1].name, "slot_b",
			sizeof(response->tool_calls[1].name) - 1);
		response->tool_calls[1].arguments = strdup("{\"n\":2}");
		return 200;
	}
	for (int i = 0; i < msg_count; i++) {
		if (messages[i].tool_calls && messages[i].tool_call_count >= 2) {
			snprintf(d->assistant_tool_call_ids[0],
				 sizeof(d->assistant_tool_call_ids[0]), "%s",
				 messages[i].tool_calls[0].id);
			snprintf(d->assistant_tool_call_ids[1],
				 sizeof(d->assistant_tool_call_ids[1]), "%s",
				 messages[i].tool_calls[1].id);
		}
		if (messages[i].tool_call_id) {
			int out = d->tool_message_ids[0][0] ? 1 : 0;
			if (out < 2) {
				snprintf(d->tool_message_ids[out],
					 sizeof(d->tool_message_ids[out]), "%s",
					 messages[i].tool_call_id);
			}
		}
	}
	response->content = strdup("done with both tools");
	if (thought_cb)
		thought_cb(response->content, thought_ud);
	return 200;
}

static void slot_mock_destroy(struct model *self)
{
	if (!self)
		return;
	free(self->handle);
	free(self);
}

static struct model *create_slot_mock_llm(struct slot_mock_data **out)
{
	struct model *m = (struct model *)calloc(1, sizeof(*m));
	struct slot_mock_data *d;

	if (!m)
		return NULL;
	d = (struct slot_mock_data *)calloc(1, sizeof(*d));
	if (!d) {
		free(m);
		return NULL;
	}
	strncpy(m->provider, "mock", sizeof(m->provider) - 1);
	strncpy(m->model_id, "slot-mock", sizeof(m->model_id) - 1);
	strncpy(m->api_key, "mock-key", sizeof(m->api_key) - 1);
	m->context_limit = 128000;
	m->timeout_seconds = 60;
	m->chat = slot_mock_chat;
	m->chat_with_tools = slot_mock_chat_with_tools;
	m->destroy = slot_mock_destroy;
	m->handle = d;
	if (out)
		*out = d;
	return m;
}

/* ---- basic fixture ---- */

class ReactTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct tokenizer *tok;
	struct compress_config cfg;
	struct arena *ar;
	void SetUp() override {
		tool_registry_init(&tools);
		tok = tokenizer_create("gpt-4o", 128000);
		memset(&cfg, 0, sizeof(cfg));
		cfg.max_context_tokens = 128000;
		cfg.max_history_rounds = 6;
		cfg.summarize_threshold_ratio = 0.8;
		cfg.compress_target_ratio = 0.5;
		ar = arena_create(0);
	}
	void TearDown() override {
		tokenizer_destroy(tok);
		arena_destroy(ar);
		tool_registry_cleanup(&tools);
	}
};

/* ---- mock LLM fixture ---- */

class MockLlmTest : public ::testing::Test {
protected:
	struct tool_registry tools;
	struct tokenizer *tok;
	struct compress_config cfg;
	struct model *llm;
	struct mock_llm_data *llm_data;
	void SetUp() override {
		tool_registry_init(&tools);
		tok = tokenizer_create("gpt-4o", 128000);
		memset(&cfg, 0, sizeof(cfg));
		cfg.max_context_tokens = 128000;
		cfg.max_history_rounds = 6;
		cfg.summarize_threshold_ratio = 0.8;
		cfg.compress_target_ratio = 0.5;
		llm = NULL;
		llm_data = NULL;
	}
	void TearDown() override {
		if (llm) model_destroy(llm);
		tokenizer_destroy(tok);
		tool_registry_cleanup(&tools);
	}
	void setup_llm_with_response(const char *response) {
		llm = create_mock_llm(response);
		llm_data = (struct mock_llm_data *)llm->handle;
	}
	void setup_streaming_llm_with_response(const char *response) {
		llm = create_mock_streaming_llm(response);
		llm_data = (struct mock_llm_data *)llm->handle;
	}
};

/* ============================================= */
/* Basic React tests                             */
/* ============================================= */

TEST_F(ReactTest, CreateDestroy) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_NONE);
	EXPECT_EQ(ctx->last_error_code, 0);
	EXPECT_EQ(ctx->max_iterations, 10);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, DefaultToolTimeoutAndRetries) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tool_timeout_seconds, 300);
	EXPECT_EQ(ctx->tool_max_retries, 3);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, Reset) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->state = REACT_STATE_THINKING;
	react_reset(ctx);
	EXPECT_EQ(ctx->state, REACT_STATE_INIT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_NONE);
	EXPECT_EQ(ctx->last_error_code, 0);
	EXPECT_EQ(ctx->steps, nullptr);
	EXPECT_EQ(ctx->step_count, 0);
	EXPECT_EQ(ctx->cancelled, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CancelAndAbort) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	react_cancel(ctx);
	EXPECT_EQ(ctx->cancelled, 1);
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CancelFn) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->cancelled, 0);
	react_cancel(ctx);
	EXPECT_EQ(ctx->cancelled, 1);
	react_cancel(nullptr);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, CreateStep) {
	struct arena *arena = arena_create(1024);
	struct react_step *s = react_step_create(arena, REACT_STEP_THOUGHT, "thinking", nullptr, nullptr, nullptr);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, REACT_STEP_THOUGHT);
	EXPECT_STREQ(s->content, "thinking");
	arena_destroy(arena);
}

TEST_F(ReactTest, CreateStepWithTool) {
	struct arena *arena = arena_create(1024);
	struct react_step *s = react_step_create(arena, REACT_STEP_ACTION, "calling tool",
						 "test_tool", "{\"prompt\":\"hi\"}", nullptr);
	ASSERT_NE(s, nullptr);
	EXPECT_EQ(s->type, REACT_STEP_ACTION);
	EXPECT_STREQ(s->tool_name, "test_tool");
	EXPECT_STREQ(s->tool_args, "{\"prompt\":\"hi\"}");
	arena_destroy(arena);
}

TEST_F(ReactTest, StepNames) {
	EXPECT_STREQ(react_step_type_name(REACT_STEP_THOUGHT), "Thought");
	EXPECT_STREQ(react_step_type_name(REACT_STEP_ACTION), "Action");
	EXPECT_STREQ(react_step_type_name(REACT_STEP_OBSERVATION), "Observation");
	EXPECT_STREQ(react_step_type_name(REACT_STEP_FINAL), "Final");
}

TEST_F(ReactTest, StateNames) {
	EXPECT_STREQ(react_state_name(REACT_STATE_INIT), "INIT");
	EXPECT_STREQ(react_state_name(REACT_STATE_THINKING), "THINKING");
	EXPECT_STREQ(react_state_name(REACT_STATE_DONE), "DONE");
	EXPECT_STREQ(react_state_name(REACT_STATE_ABORT), "ABORT");
	EXPECT_STREQ(react_state_name(REACT_STATE_TOOL_FAIL), "TOOL_FAIL");
	EXPECT_STREQ(react_state_name(REACT_STATE_ACTING), "ACTING");
	EXPECT_STREQ(react_state_name(REACT_STATE_OBSERVING), "OBSERVING");
}

TEST_F(ReactTest, OutcomeNames) {
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_NONE), "none");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_SUCCESS), "success");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_CANCELLED), "cancelled");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_TIMEOUT), "timeout");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_MAX_ITERATIONS),
		     "max_iterations");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_LLM_ERROR),
		     "llm_error");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_TOOL_ERROR),
		     "tool_error");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_GUARDRAIL_DENIED),
		     "guardrail_denied");
	EXPECT_STREQ(react_outcome_name(REACT_OUTCOME_INTERNAL_ERROR),
		     "internal_error");
}

TEST_F(ReactTest, DestroyNull) {
	EXPECT_NO_FATAL_FAILURE(react_context_destroy(nullptr));
}

TEST_F(ReactTest, ResetNull) {
	EXPECT_NO_FATAL_FAILURE(react_reset(nullptr));
}

TEST_F(ReactTest, StepDestroyNull) {
	EXPECT_NO_FATAL_FAILURE(react_step_destroy(nullptr));
}

TEST_F(ReactTest, ToolRegistryIntegration) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "Generate text",
		      "{\"type\":\"object\"}", test_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tools->count, 1);
	struct tool_entry *e = tool_lookup(ctx->tools, "test_tool");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->desc.name, "test_tool");
	react_context_destroy(ctx);
}

TEST_F(ReactTest, RunBasic) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	int rc = react_run(ctx, "hello world", nullptr, nullptr);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, RunNullInput) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	int rc = react_run(ctx, nullptr, nullptr, nullptr);
	EXPECT_NE(rc, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, NullContext) {
	EXPECT_NE(react_context_create(nullptr, tok, &cfg, nullptr), nullptr);
}

TEST_F(ReactTest, RunWithCallback) {
	static int callback_count = 0;
	callback_count = 0;
	auto cb = [](const struct react_output_event *event, void *ud) -> int {
		(void)event;
		(void)ud;
		callback_count++;
		return 0;
	};
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	struct model *m = create_mock_llm("Thought: ok\nFinal: done");
	ASSERT_NE(m, nullptr);
	ctx->llm_model = m;
	react_run(ctx, "test input", cb, nullptr);
	react_context_destroy(ctx);
	model_destroy(m);
	EXPECT_GT(callback_count, 0);
}

TEST_F(MockLlmTest, CallbackReceivesStructuredToolStatus) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}",
		      test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: use tool.\nAction: test_tool({\"prompt\":\"hi\"})\n",
		"Final: done"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	struct callback_state {
		int action_start_count;
		int saw_observation_done;
	} state = {0, 0};
	auto cb = [](const struct react_output_event *event, void *ud) -> int {
		struct callback_state *state = (struct callback_state *)ud;
		if (event->type == REACT_STEP_ACTION &&
		    event->status == REACT_OUTPUT_STARTED &&
		    event->tool_name &&
		    strcmp(event->tool_name, "test_tool") == 0 &&
		    event->tool_args &&
		    strcmp(event->tool_args, "{\"prompt\":\"hi\"}") == 0) {
			state->action_start_count++;
		}
		if (event->type == REACT_STEP_OBSERVATION &&
		    event->status == REACT_OUTPUT_COMPLETED &&
		    event->error_code == 0) {
			state->saw_observation_done = 1;
		}
		return 0;
	};

	EXPECT_EQ(react_run(ctx, "use tool", cb, &state), 0);
	EXPECT_EQ(state.action_start_count, 1);
	EXPECT_EQ(state.saw_observation_done, 1);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, MaxIterationsAbort) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->max_iterations = 1;
	react_run(ctx, "test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, ToolFailThresholdThree) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "failing_tool", "Always fails",
		      "{}", failing_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->tool_max_retries, 3);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Mock LLM tests: full ReAct cycle              */
/* ============================================= */

TEST_F(MockLlmTest, LlmFinalDirectly) {
	setup_llm_with_response("Final: Hello, I can help with that.");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "Hello"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ContinuationWordingWithoutCompactionRemainsFinal) {
	setup_llm_with_response("Let me understand the requested explanation.");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	EXPECT_EQ(react_run(ctx, "explain this", nullptr, nullptr), 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_STREQ(ctx->final_answer,
		"Let me understand the requested explanation.");
	EXPECT_EQ(ctx->incomplete_final_retry_count, 0);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmThoughtAndFinal) {
	setup_llm_with_response("Thought: Let me think.\nFinal: Here is my answer.");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "what is 2+2?", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "Here is my answer"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmActionToolCall) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: Using test tool.\nAction: test_tool({\"prompt\":\"hi\"})\n\nFinal: Done.");
	struct react_context *ctx2 = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx2, nullptr);
	ctx2->llm_model = llm;
	ctx2->max_iterations = 5;
	int cb_count = 0;
	auto cb = [](const struct react_output_event *event, void *ud) -> int {
		(void)event;
		int *n = (int *)ud;
		(*n)++;
		return 0;
	};
	react_run(ctx2, "use the test tool", cb, &cb_count);
	EXPECT_TRUE(ctx2->state == REACT_STATE_DONE ||
		    ctx2->state == REACT_STATE_ABORT);
	react_context_destroy(ctx2);
}

TEST_F(MockLlmTest, LlmStreamingFinal) {
	setup_streaming_llm_with_response("Final: streamed answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "stream test", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmToolFailRetries) {
	int call_count = 0;
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "fail_tool", "Fails every time", "{}",
		      failing_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: try fail_tool.\nAction: fail_tool({\"q\":\"test\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "call failing tool", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_DONE ||
		    ctx->state == REACT_STATE_ABORT);
	EXPECT_GE(ctx->step_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmToolFailMaxRetries) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "fail_tool", "Fails every time", "{}",
		      failing_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: try again.\nAction: fail_tool({\"q\":\"test\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 10;
	ctx->tool_max_retries = 3;
	react_run(ctx, "keep calling failing tool", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "failed"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmCallCount) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple question", nullptr, nullptr);
	EXPECT_EQ(llm_data->call_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmMultiStepCallCount) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	setup_llm_with_response(
		"Thought: Step 1.\nAction: test_tool({\"prompt\":\"hi\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "multi-step question", nullptr, nullptr);
	EXPECT_GE(llm_data->call_count, 1);
	react_context_destroy(ctx);
}

/* ---- Multi-response termination tests ---- */

TEST_F(MockLlmTest, ActionThenFinal) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Using tool.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Tool done.\nFinal: The answer is here."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "answer is here") != nullptr);
	react_context_destroy(ctx);
}

static bool event_recorder_has_name(struct morph_event_recorder *rec,
				    const char *name)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0;
		cJSON_Delete(root);
		if (matched)
			return true;
	}
	return false;
}

static int event_recorder_count_name(struct morph_event_recorder *rec,
				     const char *name)
{
	int count = 0;

	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		if (cJSON_IsString(name_item) &&
		    strcmp(name_item->valuestring, name) == 0)
			count++;
		cJSON_Delete(root);
	}
	return count;
}

static int event_recorder_index_name(struct morph_event_recorder *rec,
				     const char *name)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0;
		cJSON_Delete(root);
		if (matched)
			return (int)i;
	}
	return -1;
}

static bool event_recorder_observation_has_artifacts(
	struct morph_event_recorder *rec)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *artifacts = cJSON_IsObject(data) ?
			cJSON_GetObjectItem(data, "artifacts") : NULL;
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, "react.observation") == 0 &&
			cJSON_IsArray(artifacts) &&
			cJSON_GetArraySize(artifacts) > 0;
		cJSON_Delete(root);
		if (matched)
			return true;
	}
	return false;
}

static int event_recorder_nth_index_name(struct morph_event_recorder *rec,
					 const char *name, int nth)
{
	int seen = 0;

	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0;
		cJSON_Delete(root);
		if (!matched)
			continue;
		if (seen == nth)
			return (int)i;
		seen++;
	}
	return -1;
}

static bool event_recorder_has_outcome(struct morph_event_recorder *rec,
				       const char *name,
				       const char *outcome)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *outcome_item = data ?
			cJSON_GetObjectItem(data, "outcome") : nullptr;
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0 &&
			cJSON_IsString(outcome_item) &&
			strcmp(outcome_item->valuestring, outcome) == 0;
		cJSON_Delete(root);
		if (matched)
			return true;
	}
	return false;
}

static bool event_recorder_has_auth_required(struct morph_event_recorder *rec,
					     const char *backend,
					     const char *tool)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *type_item = cJSON_GetObjectItem(root, "type");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *kind_item = data ?
			cJSON_GetObjectItem(data, "kind") : nullptr;
		cJSON *backend_item = data ?
			cJSON_GetObjectItem(data, "backend") : nullptr;
		cJSON *tool_item = data ?
			cJSON_GetObjectItem(data, "tool") : nullptr;
		cJSON *retryable = data ?
			cJSON_GetObjectItem(data, "retryable") : nullptr;
		bool matched = cJSON_IsString(type_item) &&
			strcmp(type_item->valuestring, "hitl") == 0 &&
			cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, "auth.required") == 0 &&
			cJSON_IsString(kind_item) &&
			strcmp(kind_item->valuestring, "auth_required") == 0 &&
			cJSON_IsString(backend_item) &&
			strcmp(backend_item->valuestring, backend) == 0 &&
			cJSON_IsString(tool_item) &&
			strcmp(tool_item->valuestring, tool) == 0 &&
			cJSON_IsTrue(retryable);
		cJSON_Delete(root);
		if (matched)
			return true;
	}
	return false;
}

static std::string event_recorder_turn_id(struct morph_event_recorder *rec,
					  const char *name)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *turn_id = cJSON_GetObjectItem(root, "turn_id");
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0 &&
			cJSON_IsString(turn_id);
		std::string out = matched ? turn_id->valuestring : "";
		cJSON_Delete(root);
		if (matched)
			return out;
	}
	return "";
}

static std::string event_recorder_tool_call_id(
	struct morph_event_recorder *rec, const char *name, int nth)
{
	int seen = 0;

	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *tool_call_id = cJSON_IsObject(data) ?
			cJSON_GetObjectItem(data, "tool_call_id") : nullptr;
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0;
		std::string out = matched && seen == nth &&
			cJSON_IsString(tool_call_id) ? tool_call_id->valuestring : "";
		cJSON_Delete(root);
		if (!matched)
			continue;
		if (seen == nth)
			return out;
		seen++;
	}
	return "";
}

static std::string event_recorder_tool_input(
	struct morph_event_recorder *rec, const char *tool)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);

		if (!root)
			continue;
		cJSON *name = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *tool_item = cJSON_IsObject(data) ?
			cJSON_GetObjectItem(data, "tool") : nullptr;
		cJSON *args = cJSON_IsObject(data) ?
			cJSON_GetObjectItem(data, "args") : nullptr;
		cJSON *input = cJSON_IsObject(args) ?
			cJSON_GetObjectItem(args, "input") : nullptr;
		bool matched = cJSON_IsString(name) &&
			strcmp(name->valuestring, "tool.call") == 0 &&
			cJSON_IsString(tool_item) &&
			strcmp(tool_item->valuestring, tool) == 0;
		std::string out = matched && cJSON_IsString(input) ?
			input->valuestring : "";

		cJSON_Delete(root);
		if (matched)
			return out;
	}
	return "";
}

static bool event_recorder_all_turn_ids_match(struct morph_event_recorder *rec,
					      const char *turn_id)
{
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *type = cJSON_GetObjectItem(root, "type");
		cJSON *id = cJSON_GetObjectItem(root, "turn_id");
		bool checked = cJSON_IsString(type) &&
			(strcmp(type->valuestring, "react") == 0 ||
			 strcmp(type->valuestring, "tool") == 0 ||
			 strcmp(type->valuestring, "artifact") == 0 ||
			 strcmp(type->valuestring, "hitl") == 0);
		bool ok = !checked || (cJSON_IsString(id) &&
			strcmp(id->valuestring, turn_id) == 0);
		cJSON_Delete(root);
		if (!ok)
			return false;
	}
	return true;
}

static bool event_recorder_named_text_valid_utf8(struct morph_event_recorder *rec,
						 const char *name)
{
	bool seen = false;

	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		if (utf8valid(json) != nullptr)
			return false;
		cJSON *root = cJSON_Parse(json);
		if (!root)
			return false;
		cJSON *name_item = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *text = cJSON_IsObject(data) ?
			cJSON_GetObjectItem(data, "text") : nullptr;
		bool matched = cJSON_IsString(name_item) &&
			strcmp(name_item->valuestring, name) == 0;
		if (matched) {
			seen = true;
			if (cJSON_IsString(text) &&
			    utf8valid(text->valuestring) != nullptr) {
				cJSON_Delete(root);
				return false;
			}
		}
		cJSON_Delete(root);
	}
	return seen;
}

static std::string event_recorder_join_text(struct morph_event_recorder *rec,
					    const char *expected_name)
{
	std::string out;
	for (size_t i = 0; i < morph_event_recorder_count(rec); i++) {
		const char *json = morph_event_recorder_get(rec, i);
		cJSON *root = cJSON_Parse(json);
		if (!root)
			continue;
		cJSON *name = cJSON_GetObjectItem(root, "name");
		cJSON *data = cJSON_GetObjectItem(root, "data");
		cJSON *text = cJSON_IsObject(data) ?
			cJSON_GetObjectItem(data, "text") : nullptr;
		if (cJSON_IsString(name) && cJSON_IsString(text) &&
		    strcmp(name->valuestring, expected_name) == 0)
			out += text->valuestring;
		cJSON_Delete(root);
	}
	return out;
}

TEST_F(MockLlmTest, CompactsWithinTurnAfterLargeToolResult)
{
	const char *responses[] = {
		"Thought: gather data.\nAction: large_tool({})\n",
		"Let me understand the current task before continuing.",
		"Final: continued after compaction"
	};
	char db_path[PATH_MAX];
	struct db db;
	struct session session;
	struct morph_event_recorder rec;
	size_t result_bytes = 80000;
	int active_count = 0;

	snprintf(db_path, sizeof(db_path),
		 "/tmp/morph_react_compact_%d.db", getpid());
	std::remove(db_path);
	ASSERT_EQ(db_open(&db, db_path), 0);
	ASSERT_EQ(db_init_schema(&db), 0);
	ASSERT_EQ(session_create(&db, "compact", "mock-model", &session), 0);
	ASSERT_EQ(morph_event_recorder_init(&rec), 0);
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &tools, "large_tool",
		"Return a large result", "{}", large_tool_fn, &result_bytes,
		nullptr), 0);
	cfg.max_context_tokens = 12000;
	cfg.max_history_rounds = 6;
	cfg.in_turn_compaction = 1;
	cfg.protocol_reserve_tokens = 0;
	cfg.summarize_threshold_ratio = 0.8;
	cfg.compress_target_ratio = 0.5;
	cfg.tool_result_max_tokens = 20000;
	cfg.compaction_user_message_tokens = 2000;
	cfg.compaction_summary_max_tokens = 1000;
	llm = create_multi_mock_llm(responses, 3);
	ASSERT_NE(llm, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
		nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->history_enabled = 1;
	ctx->history_db = &db;
	ctx->history_session_id = session.id;
	ctx->compress.summarize = test_compress_cb;
	ctx->compress.summarize_user_data = nullptr;
	ASSERT_EQ(react_set_turn_id(ctx, "turn_in_turn_compaction"), 0);
	ASSERT_EQ(react_set_event_callback(ctx, morph_event_recorder_cb, &rec),
		0);

	EXPECT_EQ(react_run(ctx, "collect the large result", nullptr, nullptr),
		0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_STREQ(ctx->final_answer, "continued after compaction");
	EXPECT_EQ(ctx->compress.in_turn_compaction, 1);
	EXPECT_TRUE(ctx->history_enabled);
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.call"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.observation"));
	EXPECT_GT(model_history_count(&db, session.id, 0), 1);
	EXPECT_EQ(ctx->in_turn_compaction_count, 1);
	EXPECT_EQ(ctx->incomplete_final_retry_count, 1);
	EXPECT_TRUE(event_recorder_has_name(&rec,
		"react.compaction.completed"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final.retry"));
	struct model_history_item *active = model_history_list(&db,
		session.id, 1, &active_count);
	ASSERT_NE(active, nullptr);
	ASSERT_GE(active_count, 2);
	bool found_summary = false;
	for (struct model_history_item *item = active; item;
	     item = item->next) {
		if (std::strcmp(item->kind, "compaction_summary") == 0)
			found_summary = true;
	}
	EXPECT_TRUE(found_summary);
	model_history_free_list(active);

	react_context_destroy(ctx);
	morph_event_recorder_cleanup(&rec);
	db_close(&db);
	std::remove(db_path);
}

TEST_F(MockLlmTest, StreamsNativeContentAsProvisionalThoughtUntilFinal) {
	const char *chunks[] = {
		"# ",
		"Title\n\nword",
		" between",
		" words"
	};
	llm = create_direct_stream_llm(chunks, 4,
				       "# Title\n\nword between words");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb, &rec));

	int rc = react_run(ctx, "stream final", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(event_recorder_count_name(&rec, "react.thought.delta"), 4);
	EXPECT_EQ(event_recorder_join_text(&rec, "react.thought.delta"),
		  "# Title\n\nword between words");
	EXPECT_EQ(event_recorder_count_name(&rec, "react.final.delta"), 0);
	EXPECT_LT(event_recorder_index_name(&rec, "react.thought.delta"),
		  event_recorder_index_name(&rec, "react.final"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, StreamsNativeContentBeforeToolsAndPromotesLastResponse) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool",
		      "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *chunks[] = {"I will ", "inspect first."};
	llm = create_direct_stream_tool_llm(
		chunks, 2, "I will inspect first.", "Done.");
	struct react_context *ctx =
		react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(
		ctx, morph_event_recorder_cb, &rec));

	int rc = react_run(ctx, "inspect", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(event_recorder_join_text(&rec, "react.thought.delta"),
		  "I will inspect first.Done.");
	EXPECT_EQ(event_recorder_count_name(&rec, "react.thought.delta"), 3);
	EXPECT_EQ(event_recorder_count_name(&rec, "react.final.delta"), 0);
	EXPECT_LT(event_recorder_index_name(&rec, "react.thought.delta"),
		  event_recorder_index_name(&rec, "tool.call"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.thought.end"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, StreamingDeltasDoNotSplitUtf8Codepoints) {
	const char *chunks[] = {
		"Thought: \xE5\x8D",
		"\x95词释义\nFi",
		"nal: 答案是\xE8\x8B",
		"\xB9果"
	};
	llm = create_direct_stream_llm(chunks, 4, "答案是苹果");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb, &rec));

	int rc = react_run(ctx, "stream utf8", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(event_recorder_named_text_valid_utf8(&rec,
							"react.thought.delta"));
	EXPECT_EQ(event_recorder_count_name(&rec, "react.final.delta"), 0);
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmitsStructuredToolEvents) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Using tool.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Tool done.\nFinal: The answer is here."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb, &rec));

	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.turn.begin"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.thought.delta"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.action"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.call"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.running"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.result"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.turn.end"));
	EXPECT_EQ(event_recorder_count_name(&rec, "react.final.delta"), 0);
	EXPECT_LT(event_recorder_index_name(&rec, "tool.call"),
		  event_recorder_index_name(&rec, "tool.running"));
	EXPECT_LT(event_recorder_index_name(&rec, "tool.running"),
		  event_recorder_index_name(&rec, "tool.result"));
	EXPECT_TRUE(event_recorder_has_outcome(&rec, "react.turn.end",
					       "success"));
	std::string turn_id = event_recorder_turn_id(&rec, "react.turn.begin");
	ASSERT_FALSE(turn_id.empty());
	EXPECT_TRUE(event_recorder_all_turn_ids_match(&rec, turn_id.c_str()));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmitsToolStreamEventsFromToolThread) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "stream_tool", "Streams from tool", "{}",
		      streaming_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Using streaming tool.\nAction: stream_tool({})\n",
		"Thought: Tool done.\nFinal: Streamed."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(event_recorder_count_name(&rec, "tool.stream.delta"), 2);
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.result"));
	std::string tool_call_id =
		event_recorder_tool_call_id(&rec, "tool.call", 0);
	ASSERT_FALSE(tool_call_id.empty());
	EXPECT_EQ(event_recorder_tool_call_id(&rec, "tool.stream.delta", 0),
		  tool_call_id);
	EXPECT_EQ(event_recorder_tool_call_id(&rec, "tool.stream.delta", 1),
		  tool_call_id);
	EXPECT_EQ(event_recorder_tool_call_id(&rec, "tool.result", 0),
		  tool_call_id);
	std::string turn_id = event_recorder_turn_id(&rec, "react.turn.begin");
	ASSERT_FALSE(turn_id.empty());
	EXPECT_TRUE(event_recorder_all_turn_ids_match(&rec, turn_id.c_str()));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, UsesInjectedTurnIdForEvents) {
	const char *responses[] = {
		"Thought: Done.\nFinal: Injected turn."
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb, &rec));
	ASSERT_EQ(0, react_set_turn_id(ctx, "turn_external"));

	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_STREQ("turn_external", react_get_turn_id(ctx));
	EXPECT_TRUE(event_recorder_all_turn_ids_match(&rec, "turn_external"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmitsAuthRequiredWhenLlmKeyMissing) {
	llm = create_mock_llm("Thought: Done.\nFinal: no key");
	ASSERT_NE(llm, nullptr);
	llm->api_key[0] = '\0';
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_NOT_CONFIGURED);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_LLM_ERROR);
	EXPECT_TRUE(event_recorder_has_auth_required(&rec, "text", ""));
	EXPECT_TRUE(event_recorder_has_outcome(&rec, "react.turn.end",
					       "llm_error"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmitsAuthRequiredWhenToolKeyMissing) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "img_qa", "Needs key", "{}",
		      not_configured_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Need tool.\nAction: img_qa({})\n",
		"Thought: Tool failed.\nFinal: cannot continue."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "do it", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(event_recorder_has_auth_required(&rec, "text",
						     "img_qa"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "tool.failed"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "react.final"));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmitsStructuredArtifactEvents) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "artifact_tool", "Returns artifact", "{}",
		      artifact_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Creating artifact.\nAction: artifact_tool({})\n",
		"Thought: Done.\nFinal: Artifact is ready."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "make an artifact", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_TRUE(event_recorder_has_name(&rec, "artifact.ready"));
	EXPECT_TRUE(event_recorder_observation_has_artifacts(&rec));

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MultipleToolCallsUseSlotArray) {
	int slot_a_count = 0;
	int slot_b_count = 0;
	struct slot_mock_data *slot_data = nullptr;

	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slot_a", "Slot A", "{}",
		      call_count_tool_fn, &slot_a_count, nullptr);
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slot_b", "Slot B", "{}",
		      call_count_tool_fn, &slot_b_count, nullptr);
	llm = create_slot_mock_llm(&slot_data);
	ASSERT_NE(llm, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "run both", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_EQ(slot_a_count, 1);
	EXPECT_EQ(slot_b_count, 1);
	ASSERT_NE(slot_data, nullptr);
	EXPECT_EQ(slot_data->call_count, 2);
	EXPECT_EQ(event_recorder_count_name(&rec, "tool.result"), 2);
	EXPECT_EQ(event_recorder_count_name(&rec, "tool.running"), 2);
	{
		std::string id0 = event_recorder_tool_call_id(&rec, "tool.call", 0);
		std::string id1 = event_recorder_tool_call_id(&rec, "tool.call", 1);
		ASSERT_FALSE(id0.empty());
		ASSERT_FALSE(id1.empty());
		EXPECT_NE(id0, id1);
		EXPECT_EQ(id0.rfind("tc_", 0), 0u);
		EXPECT_EQ(id1.rfind("tc_", 0), 0u);
	}
	EXPECT_STREQ(slot_data->assistant_tool_call_ids[0], "slot_call_0");
	EXPECT_STREQ(slot_data->assistant_tool_call_ids[1], "slot_call_1");
	EXPECT_STREQ(slot_data->tool_message_ids[0], "slot_call_0");
	EXPECT_STREQ(slot_data->tool_message_ids[1], "slot_call_1");
	EXPECT_LT(event_recorder_nth_index_name(&rec, "tool.call", 1),
		  event_recorder_nth_index_name(&rec, "tool.running", 0));
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "both tools"), nullptr);

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LocalToolCallIdsSurviveDuplicateProviderIds) {
	int slot_a_count = 0;
	int slot_b_count = 0;
	struct slot_mock_data *slot_data = nullptr;

	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slot_a", "Slot A", "{}",
		      call_count_tool_fn, &slot_a_count, nullptr);
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slot_b", "Slot B", "{}",
		      call_count_tool_fn, &slot_b_count, nullptr);
	llm = create_slot_mock_llm(&slot_data);
	ASSERT_NE(llm, nullptr);
	ASSERT_NE(slot_data, nullptr);
	slot_data->duplicate_provider_ids = 1;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "run both", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	std::string id0 = event_recorder_tool_call_id(&rec, "tool.call", 0);
	std::string id1 = event_recorder_tool_call_id(&rec, "tool.call", 1);
	ASSERT_FALSE(id0.empty());
	ASSERT_FALSE(id1.empty());
	EXPECT_NE(id0, id1);
	EXPECT_EQ(id0.rfind("tc_", 0), 0u);
	EXPECT_EQ(id1.rfind("tc_", 0), 0u);
	EXPECT_STREQ(slot_data->assistant_tool_call_ids[0], "slot_call_dup");
	EXPECT_STREQ(slot_data->assistant_tool_call_ids[1], "slot_call_dup");
	EXPECT_STREQ(slot_data->tool_message_ids[0], "slot_call_dup");
	EXPECT_STREQ(slot_data->tool_message_ids[1], "slot_call_dup");

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MissingProviderIdsGetProtocolFallbacks) {
	int slot_a_count = 0;
	int slot_b_count = 0;
	struct slot_mock_data *slot_data = nullptr;

	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slot_a", "Slot A", "{}",
		      call_count_tool_fn, &slot_a_count, nullptr);
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slot_b", "Slot B", "{}",
		      call_count_tool_fn, &slot_b_count, nullptr);
	llm = create_slot_mock_llm(&slot_data);
	ASSERT_NE(llm, nullptr);
	ASSERT_NE(slot_data, nullptr);
	slot_data->empty_provider_ids = 1;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "run both", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	std::string local0 = event_recorder_tool_call_id(&rec, "tool.call", 0);
	std::string local1 = event_recorder_tool_call_id(&rec, "tool.call", 1);
	ASSERT_FALSE(local0.empty());
	ASSERT_FALSE(local1.empty());
	EXPECT_NE(local0, local1);
	EXPECT_EQ(local0.rfind("tc_", 0), 0u);
	EXPECT_EQ(local1.rfind("tc_", 0), 0u);
	std::string provider0 = slot_data->assistant_tool_call_ids[0];
	std::string provider1 = slot_data->assistant_tool_call_ids[1];
	ASSERT_FALSE(provider0.empty());
	ASSERT_FALSE(provider1.empty());
	EXPECT_NE(provider0, provider1);
	EXPECT_EQ(provider0.rfind("pc_", 0), 0u);
	EXPECT_EQ(provider1.rfind("pc_", 0), 0u);
	EXPECT_EQ(provider0, slot_data->tool_message_ids[0]);
	EXPECT_EQ(provider1, slot_data->tool_message_ids[1]);

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MessageArrayGrowsBeyondInitialCapacity) {
	struct slot_mock_data *slot_data = nullptr;
	llm = create_slot_mock_llm(&slot_data);
	ASSERT_NE(llm, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 1;

	for (int i = 0; i < 70; i++) {
		char content[64];
		snprintf(content, sizeof(content), "history %d", i);
		msg_list_append(&ctx->messages,
				msg_list_create(ctx->session_arena,
						i % 2 == 0 ? "user" :
						"assistant",
						content, 1));
	}

	int rc = react_run(ctx, "final prompt", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(slot_data, nullptr);
	EXPECT_EQ(slot_data->last_msg_count, 71);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionActionThenFinal) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Step 1.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Step 2.\nAction: test_tool({\"p\":\"2\"})\n",
		"Thought: Done.\nFinal: Final result after two steps."
	};
	llm = create_multi_mock_llm(responses, 3);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "multi step", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "after two steps") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionNeverFinal) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Step 1.\nAction: test_tool({\"p\":\"1\"})\n",
		"Thought: Step 2.\nAction: test_tool({\"p\":\"2\"})\n",
		"Thought: Step 3.\nAction: test_tool({\"p\":\"3\"})\n",
		"Thought: Step 4.\nAction: test_tool({\"p\":\"4\"})\n",
		"Thought: Step 5.\nAction: test_tool({\"p\":\"5\"})\n",
	};
	llm = create_multi_mock_llm(responses, 5);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 3;
	int rc = react_run(ctx, "never final", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_REACT_MAX_ITERATIONS);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_MAX_ITERATIONS);
	EXPECT_EQ(ctx->last_error_code, MORPH_ERR_REACT_MAX_ITERATIONS);
	EXPECT_STREQ(ctx->outcome_reason, "max_iterations");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolFailThenFinal) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "fail_tool", "Fails always", "{}",
		      failing_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: Try failing tool.\nAction: fail_tool({\"q\":\"x\"})\n",
		"Thought: It failed, giving up.\nFinal: Sorry, tool failed."
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "call tool", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "failed") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, InvalidToolArgumentsAreReplayedSafelyAndRetried) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "json_tool",
		"Requires a JSON object", "{}", json_object_tool_fn,
		nullptr, nullptr);
	const char *responses[] = {
		"Thought: malformed.\nAction: json_tool({not-json)\n",
		"Thought: correct arguments.\nAction: json_tool({\"ok\":true})\n",
		"Final: recovered after correcting arguments"
	};
	llm = create_multi_mock_llm(responses, 3);
	struct multi_mock_data *data =
		(struct multi_mock_data *)llm->handle;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
		nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;

	int rc = react_run(ctx, "write the file", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(data->call_count, 3);
	EXPECT_GT(data->replay_argument_checks, 0);
	EXPECT_EQ(data->invalid_replayed_arguments, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "recovered"), nullptr);
	bool saw_invalid_arguments = false;
	bool saw_success = false;
	for (struct react_step *step = ctx->steps; step; step = step->next) {
		if (step->type != REACT_STEP_OBSERVATION)
			continue;
		if (step->error_code == -EINVAL)
			saw_invalid_arguments = true;
		if (step->error_code == 0)
			saw_success = true;
	}
	EXPECT_TRUE(saw_invalid_arguments);
	EXPECT_TRUE(saw_success);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, TextToolFallbackFailureIsObservedAndRetried) {
	struct text_tool_capture capture{};
	struct tool_spec spec{};
	struct morph_event_recorder rec;
	const char *responses[] = {
		"Thought: malformed.\nAction: apply_text({not-json)\n",
		"Thought: retry.\nAction: apply_text({\"input\":\"*** Begin Patch\\n*** End Patch\"})\n",
		"Final: recovered after correcting the patch"
	};

	spec.origin = TOOL_ORIGIN_BUILTIN;
	spec.name = "apply_text";
	spec.description = "Apply raw text";
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.input_kind = TOOL_INPUT_TEXT;
	spec.exec = text_tool_fn;
	spec.user_data = &capture;
	ASSERT_EQ(::tool_register(&tools, &spec), 0);
	llm = create_multi_mock_llm(responses, 3);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
		nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb, &rec));

	int rc = react_run(ctx, "apply the patch", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(capture.calls, 2);
	EXPECT_EQ(capture.last_input,
		"*** Begin Patch\n*** End Patch");
	EXPECT_EQ(event_recorder_tool_input(&rec, "apply_text"),
		"{not-json");
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "recovered"), nullptr);
	bool saw_invalid_patch = false;
	bool saw_success = false;
	for (struct react_step *step = ctx->steps; step; step = step->next) {
		if (step->type != REACT_STEP_OBSERVATION)
			continue;
		if (step->error_code == -EINVAL)
			saw_invalid_patch = true;
		if (step->error_code == 0)
			saw_success = true;
	}
	EXPECT_TRUE(saw_invalid_patch);
	EXPECT_TRUE(saw_success);

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MaxIterationsDoesNotUseToolErrorAsFinal) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "fail_tool", "Fails always", "{}",
		      failing_tool_fn, nullptr, nullptr);
	setup_llm_with_response("Thought: Try failing tool.\nAction: fail_tool({\"q\":\"x\"})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 1;
	int rc = react_run(ctx, "call tool", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_REACT_MAX_ITERATIONS);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_MAX_ITERATIONS);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_EQ(strstr(ctx->final_answer, "tool error:"), nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "Maximum iterations reached"),
		  nullptr);
	struct react_step *obs = ctx->steps;
	while (obs && obs->type != REACT_STEP_OBSERVATION)
		obs = obs->next;
	ASSERT_NE(obs, nullptr);
	EXPECT_LT(obs->error_code, 0);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, LlmFailureReturnsAbort) {
	llm = create_mock_llm("should not matter");
	llm_data = (struct mock_llm_data *)llm->handle;
	llm_data->should_fail = 1;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "trigger LLM failure", nullptr, nullptr);
	EXPECT_EQ(rc, -EIO);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_LLM_ERROR);
	EXPECT_EQ(ctx->last_error_code, -EIO);
	EXPECT_STREQ(ctx->outcome_reason, "llm_error");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, StepCountAfterFinal) {
	setup_llm_with_response("Final: quick answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple", nullptr, nullptr);
	EXPECT_GE(ctx->step_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CallbackReceivesFinalStep) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "simple question", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(ctx->final_answer && strstr(ctx->final_answer, "direct answer"));
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CancelDuringRun) {
	setup_llm_with_response("Thought: thinking...\nFinal: answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_cancel(ctx);
	react_run(ctx, "cancelled test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

static int drain_cancel_once(void *user_data, struct react_action *out,
			     int block)
{
	int *count = (int *)user_data;

	(void)block;
	if (*count > 0)
		return 0;
	(*count)++;
	out->type = "cancel";
	out->payload_json = NULL;
	return 1;
}

static int drain_prompt_once(void *user_data, struct react_action *out,
			     int block)
{
	int *count = (int *)user_data;

	(void)block;
	if (*count > 0)
		return 0;
	(*count)++;
	out->type = "prompt";
	out->payload_json = "{\"text\":\"also add tests\"}";
	return 1;
}

static int drain_prompt_after_first_call(void *user_data,
					 struct react_action *out, int block)
{
	int *count = (int *)user_data;

	(void)block;
	(*count)++;
	if (*count != 2)
		return 0;
	out->type = "prompt";
	out->payload_json = "{\"text\":\"change the implementation\"}";
	return 1;
}

TEST_F(MockLlmTest, ActionDrainCancelBeforeLlmCall) {
	setup_llm_with_response("Final: should not be called");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int drain_count = 0;
	ASSERT_EQ(react_set_action_drain(ctx, drain_cancel_once,
					 &drain_count), 0);

	int rc = react_run(ctx, "cancel through drain", nullptr, nullptr);
	EXPECT_EQ(rc, -ECANCELED);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_CANCELLED);
	EXPECT_EQ(llm_data->call_count, 0);
	EXPECT_EQ(drain_count, 1);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionDrainInjectsPromptIntoCurrentLoop) {
	setup_llm_with_response("Final: updated implementation");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int drain_count = 0;
	ASSERT_EQ(react_set_action_drain(ctx, drain_prompt_once,
					 &drain_count), 0);

	ASSERT_EQ(react_run(ctx, "implement feature", nullptr, nullptr), 0);
	ASSERT_NE(ctx->messages, nullptr);
	EXPECT_STREQ(ctx->messages->content, "implement feature");
	ASSERT_NE(ctx->messages->next, nullptr);
	EXPECT_STREQ(ctx->messages->next->role, "user");
	EXPECT_STREQ(ctx->messages->next->content, "also add tests");
	EXPECT_EQ(llm_data->call_count, 1);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, PromptArrivingDuringModelCallContinuesCurrentLoop) {
	setup_llm_with_response("Final: implementation");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int drain_count = 0;
	ASSERT_EQ(react_set_action_drain(ctx, drain_prompt_after_first_call,
					 &drain_count), 0);

	ASSERT_EQ(react_run(ctx, "build it", nullptr, nullptr), 0);
	EXPECT_EQ(llm_data->call_count, 2);
	ASSERT_NE(ctx->messages, nullptr);
	ASSERT_NE(ctx->messages->next, nullptr);
	EXPECT_STREQ(ctx->messages->next->content,
		     "change the implementation");

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SteeringDiscardsToolsChosenBeforeRequirementChanged) {
	setup_llm_with_response("Thought: old plan.\nAction: test_tool({})");
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "Test", "{}",
		      test_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 1;
	int drain_count = 0;
	ASSERT_EQ(react_set_action_drain(ctx, drain_prompt_after_first_call,
					 &drain_count), 0);
	(void)react_run(ctx, "build it", nullptr, nullptr);
	EXPECT_EQ(ctx->steer_count, 1);
	for (struct react_step *step = ctx->steps; step; step = step->next)
		EXPECT_NE(step->type, REACT_STEP_ACTION);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ModelTimeoutField) {
	llm = create_mock_llm("Final: test");
	ASSERT_NE(llm, nullptr);
	EXPECT_EQ(llm->timeout_seconds, 60);
	llm->timeout_seconds = 30;
	EXPECT_EQ(llm->timeout_seconds, 30);
	model_destroy(llm);
	llm = nullptr;
}

TEST_F(MockLlmTest, ConfigurableMaxIterations) {
	setup_llm_with_response("Thought: looping.\nAction: test_tool({})");
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "Test", "{}", test_tool_fn, nullptr, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 3;
	react_run(ctx, "loop test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE ||
		    ctx->state == REACT_STATE_DONE);
	EXPECT_LE(ctx->step_count, 3 * 4 + 2);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolCallCount) {
	int call_count = 0;
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "counter", "Counts calls", "{}",
		      call_count_tool_fn, &call_count, nullptr);
	setup_llm_with_response("Thought: call counter.\nAction: counter({})");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "count calls", nullptr, nullptr);
	EXPECT_GE(call_count, 1);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GlobalToolTimeoutLetsLoopContinue) {
	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slow_tool",
				"Slow tool", "{}", slow_tool_fn, nullptr,
				nullptr), 0);
	const char *responses[] = {
		"Thought: call slow.\nAction: slow_tool({})",
		"Final: done after timeout"
	};
	llm = create_multi_mock_llm(responses, 2);
	ASSERT_NE(llm, nullptr);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->tool_timeout_seconds = 1;
	struct callback_state {
		int action_timeout_count;
		int observation_failed_count;
	} state = {0, 0};
	auto cb = [](const struct react_output_event *event, void *ud) -> int {
		struct callback_state *state = (struct callback_state *)ud;
		if (event->type == REACT_STEP_ACTION &&
		    event->status == REACT_OUTPUT_TIMEOUT &&
		    event->error_code == -ETIMEDOUT &&
		    event->tool_name &&
		    strcmp(event->tool_name, "slow_tool") == 0 &&
		    event->text &&
		    strstr(event->text, "timed out")) {
			state->action_timeout_count++;
		}
		if (event->type == REACT_STEP_OBSERVATION &&
		    event->status == REACT_OUTPUT_FAILED &&
		    event->error_code == -ETIMEDOUT &&
		    event->tool_name &&
		    strcmp(event->tool_name, "slow_tool") == 0 &&
		    event->text &&
		    strstr(event->text, "timed out")) {
			state->observation_failed_count++;
		}
		return 0;
	};
	auto start = std::chrono::steady_clock::now();
	int rc = react_run(ctx, "run slow tool", cb, &state);
	auto elapsed = std::chrono::steady_clock::now() - start;

	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "done after timeout"), nullptr);
	EXPECT_EQ(state.action_timeout_count, 1);
	EXPECT_EQ(state.observation_failed_count, 1);
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
			  elapsed).count(), 2000);
	react_context_destroy(ctx);
}

/* ============================================= */
/* HTTP timeout tests                             */
/* ============================================= */

class HttpTimeoutTest : public ::testing::Test {
protected:
	void SetUp() override { http_init(); }
	void TearDown() override { http_cleanup(); }
};

TEST_F(HttpTimeoutTest, TimeoutFnNullUrl) {
	struct sse_parser parser;
	sse_parser_init(&parser, nullptr, nullptr);
	int rc = http_post_sse_ex_timeout(nullptr, "", 0,
					  "application/json", nullptr, 0, 10,
					  global_sse_write_adapter, &parser);
	EXPECT_NE(rc, 0);
	sse_parser_free(&parser);
}

TEST_F(HttpTimeoutTest, TimeoutFnNullCallback) {
	int rc = http_post_sse_ex_timeout("http://127.0.0.1:1/test", "", 0,
					  "application/json", nullptr, 0, 10,
					  nullptr, nullptr);
	EXPECT_NE(rc, 0);
}

TEST_F(HttpTimeoutTest, TimeoutFnConnectRefused) {
	struct sse_parser parser;
	sse_parser_init(&parser, nullptr, nullptr);
	int rc = http_post_sse_ex_timeout("http://127.0.0.1:1/test",
					  "{}", 2,
					  "application/json", nullptr, 0, 2,
					  global_sse_write_adapter, &parser);
	EXPECT_NE(rc, 0);
	sse_parser_free(&parser);
}

static int global_global_sse_write_adapter(const char *data, size_t len, void *user_data) {
	struct sse_parser *parser = (struct sse_parser *)user_data;
	sse_parser_feed(parser, data, len);
	return 0;
}

TEST_F(HttpTimeoutTest, ZeroTimeoutUsesDefault) {
	struct sse_parser parser;
	sse_parser_init(&parser, nullptr, nullptr);
	int rc = http_post_sse_ex_timeout("http://127.0.0.1:1/test",
					  "{}", 2,
					  "application/json", nullptr, 0, 0,
					  global_sse_write_adapter, &parser);
	EXPECT_NE(rc, 0);
	sse_parser_free(&parser);
}

/* ============================================= */
/* Mock HTTP server for integration tests         */
/* ============================================= */

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

struct mock_server {
	int port;
	int server_fd;
	pthread_t thread;
	int thread_started;
	volatile int running;
	const char *response_body;
	int response_status;
	const char *transient_error_body;
	int transient_error_status;
	int transient_failures;
	int transient_sse_body;
	int request_count;
	char request_method[16];
	char last_request[8192];
};

static void *mock_http_server_thread(void *arg)
{
	struct mock_server *srv = (struct mock_server *)arg;
	while (srv->running) {
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client_fd = accept(srv->server_fd,
				       (struct sockaddr *)&client_addr,
				       &client_len);
		if (client_fd < 0)
			continue;
		char buf[8192];
		ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
		if (n <= 0) {
			close(client_fd);
			continue;
		}
		buf[n] = '\0';
		srv->request_count++;
		snprintf(srv->last_request, sizeof(srv->last_request), "%s", buf);
		sscanf(buf, "%15s", srv->request_method);
		int is_sse = (strstr(buf, "Accept: text/event-stream") != NULL ||
			      strstr(buf, "text/event-stream") != NULL);
		int transient = srv->request_count <= srv->transient_failures;
		if (transient) {
			const char *data = srv->transient_error_body
				? srv->transient_error_body : "{}";
			if (srv->transient_sse_body) {
				char header[512];
				int hlen = snprintf(header, sizeof(header),
					"HTTP/1.1 %d Error\r\n"
					"Content-Type: text/event-stream\r\n"
					"Connection: close\r\n\r\n",
					srv->transient_error_status);
				send(client_fd, header, hlen, 0);
				char event_buf[2048];
				int elen = snprintf(event_buf, sizeof(event_buf),
					"data: %s\n\n", data);
				send(client_fd, event_buf, elen, 0);
			} else {
				int body_len = strlen(data);
				char header[512];
				int hlen = snprintf(header, sizeof(header),
					"HTTP/1.1 %d Error\r\n"
					"Content-Type: application/json\r\n"
					"Content-Length: %d\r\n"
					"Connection: close\r\n\r\n",
					srv->transient_error_status, body_len);
				send(client_fd, header, hlen, 0);
				send(client_fd, data, body_len, 0);
			}
		} else if (srv->response_body && is_sse &&
			   (srv->response_status == 0 || srv->response_status == 200)) {
			const char *data = srv->response_body;
			char header[512];
			int hlen = snprintf(header, sizeof(header),
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/event-stream\r\n"
				"Cache-Control: no-cache\r\n"
				"Connection: close\r\n"
				"\r\n");
			send(client_fd, header, hlen, 0);
			char event_buf[2048];
			int elen = snprintf(event_buf, sizeof(event_buf),
				"data: %s\n\n", data);
			send(client_fd, event_buf, elen, 0);
			const char *done = "data: [DONE]\n\n";
			send(client_fd, done, strlen(done), 0);
		} else if (srv->response_body) {
			int body_len = strlen(srv->response_body);
			char header[512];
			int hlen = snprintf(header, sizeof(header),
				"HTTP/1.1 %d OK\r\n"
				"Content-Type: application/json\r\n"
				"Content-Length: %d\r\n"
				"Connection: close\r\n"
				"\r\n",
				srv->response_status > 0 ? srv->response_status : 200,
				body_len);
			send(client_fd, header, hlen, 0);
			send(client_fd, srv->response_body, body_len, 0);
		} else {
			const char *resp_404 = "HTTP/1.1 404 Not Found\r\n"
					       "Content-Length: 0\r\n"
					       "Connection: close\r\n"
					       "\r\n";
			send(client_fd, resp_404, strlen(resp_404), 0);
		}
		close(client_fd);
	}
	return nullptr;
}

static int mock_server_start(struct mock_server *srv, int suggested_port)
{
	const char *saved_body = srv->response_body;
	int saved_status = srv->response_status;
	const char *saved_transient_body = srv->transient_error_body;
	int saved_transient_status = srv->transient_error_status;
	int saved_transient_failures = srv->transient_failures;
	int saved_transient_sse_body = srv->transient_sse_body;
	memset(srv, 0, sizeof(*srv));
	srv->response_body = saved_body;
	srv->response_status = saved_status;
	srv->transient_error_body = saved_transient_body;
	srv->transient_error_status = saved_transient_status;
	srv->transient_failures = saved_transient_failures;
	srv->transient_sse_body = saved_transient_sse_body;
	srv->server_fd = -1;
	srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (srv->server_fd < 0)
		return -1;
	int opt = 1;
	setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(suggested_port);
	if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(srv->server_fd);
		srv->server_fd = -1;
		return -1;
	}
	if (listen(srv->server_fd, 5) < 0) {
		close(srv->server_fd);
		srv->server_fd = -1;
		return -1;
	}
	socklen_t addr_len = sizeof(addr);
	getsockname(srv->server_fd, (struct sockaddr *)&addr, &addr_len);
	srv->port = ntohs(addr.sin_port);
	srv->running = 1;
	if (pthread_create(&srv->thread, nullptr, mock_http_server_thread, srv) != 0) {
		srv->running = 0;
		close(srv->server_fd);
		srv->server_fd = -1;
		return -1;
	}
	srv->thread_started = 1;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	return 0;
}

static void mock_server_stop(struct mock_server *srv)
{
	srv->running = 0;
	if (srv->server_fd >= 0) {
		shutdown(srv->server_fd, SHUT_RDWR);
		close(srv->server_fd);
		srv->server_fd = -1;
	}
	if (srv->thread_started) {
		pthread_join(srv->thread, nullptr);
		srv->thread_started = 0;
	}
}

/* ---- Integration tests with mock server ---- */

class MockServerTest : public ::testing::Test {
protected:
	struct mock_server srv;
	void SetUp() override {
		memset(&srv, 0, sizeof(srv));
		srv.server_fd = -1;
		http_init();
	}
	void TearDown() override {
		if (srv.running || srv.thread_started || srv.server_fd >= 0)
			mock_server_stop(&srv);
		http_cleanup();
	}
};

#define START_MOCK_OR_SKIP(srv)						\
	do {								\
		if (mock_server_start((srv), 0) != 0)			\
			GTEST_SKIP() << "local HTTP mock server unavailable";	\
	} while (0)

TEST_F(MockServerTest, HttpGetSuccess) {
	srv.response_body = "{\"status\":\"ok\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp3 = {0};
	char url4[256];
	snprintf(url4, sizeof(url4), "http://127.0.0.1:%d/test", srv.port);
	int rc4 = http_get(url4, &resp3);
	EXPECT_EQ(rc4, 0);
	EXPECT_EQ(resp3.status_code, 200);
	EXPECT_NE(resp3.body.data, nullptr);
	EXPECT_TRUE(resp3.body.data && strstr(resp3.body.data, "ok"));
	http_response_free(&resp3);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostSuccess) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp = {0};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/test", srv.port);
	int rc = http_post(url, "{\"data\":1}", 10,
			   "application/json", &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	EXPECT_NE(resp.body.data, nullptr);
	EXPECT_TRUE(resp.body.data && strstr(resp.body.data, "posted"));
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostEmptyBodyUsesPost) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp = {0};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/empty", srv.port);
	int rc = http_post(url, "", 0, "application/json", &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpPostExAddsExtraHeadersAndClearsResponse) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_response resp = {0};
	resp.status_code = 999;
	resp.body.len = 123;
	resp.headers.len = 456;
	const char *headers[] = { "X-Morph-Test: yes" };
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/headers", srv.port);
	int rc = http_post_ex(url, "{}", 2, "application/json",
			      headers, 1, &resp);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(resp.status_code, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	EXPECT_NE(strstr(srv.last_request, "X-Morph-Test: yes"), nullptr);
	EXPECT_GT(resp.body.len, 0u);
	http_response_free(&resp);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, HttpSessionPostReusesHandleWithHeaders) {
	srv.response_body = "{\"result\":\"posted\"}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct http_session session;
	ASSERT_EQ(http_session_init(&session), 0);
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/api/session",
		 srv.port);
	const char *headers1[] = { "X-Morph-Session-Test: one" };
	int rc = http_session_post(&session, url, "{\"n\":1}", 7,
				   "application/json", headers1, 1, 5);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(http_session_status(&session), 200);
	EXPECT_NE(strstr(srv.last_request, "X-Morph-Session-Test: one"),
		  nullptr);

	const char *headers2[] = { "X-Morph-Session-Test: two" };
	rc = http_session_post(&session, url, "{\"n\":2}", 7,
			       "application/json", headers2, 1, 5);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(http_session_status(&session), 200);
	EXPECT_NE(strstr(srv.last_request, "X-Morph-Session-Test: two"),
		  nullptr);
	http_session_cleanup(&session);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSEStreamingResponse) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct sse_test_info info = {0, ""};
	auto sse_cb = [](const char *event, const char *data, void *ud) -> int {
		auto *i = (struct sse_test_info *)ud;
		i->count++;
		if (data) i->last_data = data;
		return 0;
	};
	struct sse_parser parser;
	sse_parser_init(&parser, sse_cb, &info);
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc = http_post_sse_ex(url, "{}", 2,
				   "application/json", nullptr, 0,
				   global_sse_write_adapter, &parser);
	EXPECT_EQ(rc, 200);
	EXPECT_GT(info.count, 0);
	EXPECT_STREQ(srv.request_method, "POST");
	sse_parser_free(&parser);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSEWithTimeout) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"test\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	struct sse_parser parser2;
	sse_parser_init(&parser2, nullptr, nullptr);
	char url3[256];
	snprintf(url3, sizeof(url3), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc3 = http_post_sse_ex_timeout(url3, "{}", 2,
					   "application/json", nullptr, 0, 30,
					   global_sse_write_adapter, &parser2);
	EXPECT_EQ(rc3, 200);
	EXPECT_STREQ(srv.request_method, "POST");
	sse_parser_free(&parser2);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmStreamsReasoningWithoutAccumulatingIt) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}\n\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);

	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);

	struct chat_message msg = {
		(char *)"user",
		(char *)"hello",
		NULL,
		NULL,
		0,
	};
	struct chat_response response;
	std::string streamed;

	int rc = model->chat_with_tools_stream(
		model, arena, NULL, &msg, 1, NULL, 0, &response,
		[](enum llm_stream_kind kind, const char *token, void *ud) -> int {
			auto *out = static_cast<std::string *>(ud);
			if (kind == LLM_STREAM_REASONING)
				out->append("R:");
			else
				out->append("C:");
			if (token)
				out->append(token);
			return 0;
		},
		&streamed);
	EXPECT_EQ(rc, 200);
	EXPECT_EQ(streamed, "R:thinkC:answer");
	ASSERT_NE(response.content, nullptr);
	EXPECT_STREQ(response.content, "answer");

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, DeepSeekAdapterCapturesReasoningContent) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"reasoning_content\":\"plan \"}}]}"
		"\n\ndata: {\"choices\":[{\"delta\":{\"reasoning_content\":\"next\","
		"\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{"
		"\"name\":\"lookup\",\"arguments\":\"{}\"}}]}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("deepseek", "deepseek-v4-flash",
		api_base, "test-key");
	ASSERT_NE(model, nullptr);
	EXPECT_STREQ(model->adapter, "deepseek");
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message message{};
	message.role = const_cast<char *>("user");
	message.content = const_cast<char *>("lookup");
	struct chat_response response{};

	int rc = model->chat_with_tools_stream(model, arena, nullptr,
		&message, 1, nullptr, 0, &response, nullptr, nullptr);
	EXPECT_EQ(rc, 200);
	ASSERT_NE(response.reasoning_content, nullptr);
	EXPECT_STREQ(response.reasoning_content, "plan next");
	ASSERT_EQ(response.tool_call_count, 1);
	EXPECT_STREQ(response.tool_calls[0].id, "call_1");

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, DeepSeekThinkingReplaysReasoningWithoutToolChoice) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"content\":\"done\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("deepseek", "deepseek-v4-flash",
		api_base, "test-key");
	ASSERT_NE(model, nullptr);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct tool_call prior_call{};
	std::strcpy(prior_call.id, "call_1");
	std::strcpy(prior_call.name, "lookup");
	prior_call.arguments = const_cast<char *>("{}");
	struct chat_message messages[3]{};
	messages[0].role = const_cast<char *>("user");
	messages[0].content = const_cast<char *>("lookup");
	messages[1].role = const_cast<char *>("assistant");
	messages[1].reasoning_content = const_cast<char *>("plan next");
	messages[1].tool_calls = &prior_call;
	messages[1].tool_call_count = 1;
	messages[2].role = const_cast<char *>("tool");
	messages[2].content = const_cast<char *>("result");
	messages[2].tool_call_id = const_cast<char *>("call_1");
	struct tool_desc tool{};
	std::strcpy(tool.name, "lookup");
	std::strcpy(tool.description, "Lookup data");
	struct chat_response response{};

	int rc = model->chat_with_tools_stream(model, arena, nullptr,
		messages, 3, &tool, 1, &response, nullptr, nullptr);
	EXPECT_EQ(rc, 200);
	const char *body = std::strstr(srv.last_request, "\r\n\r\n");
	ASSERT_NE(body, nullptr);
	cJSON *request = cJSON_Parse(body + 4);
	ASSERT_NE(request, nullptr);
	EXPECT_EQ(cJSON_GetObjectItem(request, "tool_choice"), nullptr);
	cJSON *sent_messages = cJSON_GetObjectItem(request, "messages");
	cJSON *assistant = cJSON_GetArrayItem(sent_messages, 1);
	EXPECT_STREQ(cJSON_GetStringValue(
		cJSON_GetObjectItem(assistant, "content")), "");
	EXPECT_STREQ(cJSON_GetStringValue(
		cJSON_GetObjectItem(assistant, "reasoning_content")), "plan next");
	cJSON_Delete(request);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, DeepSeekDisabledThinkingUsesToolChoice) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"content\":\"done\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("deepseek", "deepseek-v4-flash",
		api_base, "test-key");
	ASSERT_NE(model, nullptr);
	std::strcpy(model->extra_body_json,
		"{\"thinking\":{\"type\":\"disabled\"}}");
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message message{};
	message.role = const_cast<char *>("user");
	message.content = const_cast<char *>("lookup");
	struct tool_desc tool{};
	std::strcpy(tool.name, "lookup");
	std::strcpy(tool.description, "Lookup data");
	struct chat_response response{};

	int rc = model->chat_with_tools_stream(model, arena, nullptr,
		&message, 1, &tool, 1, &response, nullptr, nullptr);
	EXPECT_EQ(rc, 200);
	EXPECT_NE(std::strstr(srv.last_request,
		"\"tool_choice\":\"auto\""), nullptr);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmMergesExtraBodyJsonIntoToolRequest) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	strncpy(model->extra_body_json,
		"{\"reasoning_effort\":\"high\",\"chat_template_kwargs\":"
		"{\"enable_thinking\":true}}",
		sizeof(model->extra_body_json) - 1);

	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;
	int rc = model->chat_with_tools_stream(
		model, arena, NULL, &msg, 1, NULL, 0, &response, NULL, NULL);

	EXPECT_EQ(rc, 200);
	EXPECT_NE(strstr(srv.last_request,
		"\"reasoning_effort\":\"high\""), nullptr);
	EXPECT_NE(strstr(srv.last_request,
		"\"chat_template_kwargs\":{\"enable_thinking\":true}"),
		nullptr);
	EXPECT_NE(strstr(srv.last_request, "\"stream\":true"), nullptr);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, ResponsesAdapterStreamsNativeCustomToolInput) {
	srv.response_body =
		"{\"type\":\"response.output_item.added\",\"item\":{"
		"\"type\":\"custom_tool_call\",\"id\":\"item_patch\","
		"\"call_id\":\"call_patch\",\"name\":\"apply_patch\","
		"\"input\":\"\"}}\n\n"
		"data: {\"type\":\"response.custom_tool_call_input.delta\","
		"\"item_id\":\"item_patch\","
		"\"delta\":\"*** Begin Patch\\n*** End Patch\"}\n\n"
		"data: {\"type\":\"response.output_item.done\",\"item\":{"
		"\"type\":\"custom_tool_call\",\"id\":\"item_patch\","
		"\"call_id\":\"call_patch\",\"name\":\"apply_patch\","
		"\"input\":\"*** Begin Patch\\n*** End Patch\"}}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("openai", "gpt-test",
		api_base, "test-key");
	ASSERT_NE(model, nullptr);
	std::strcpy(model->adapter, "openai-responses");
	struct arena *arena = arena_create(16384);
	ASSERT_NE(arena, nullptr);
	struct chat_message message{};
	message.role = const_cast<char *>("user");
	message.content = const_cast<char *>("edit the file");
	struct tool_desc tool{};
	std::strcpy(tool.name, "apply_patch");
	std::strcpy(tool.description, "Apply a source patch");
	tool.input_kind = TOOL_INPUT_TEXT;
	struct chat_response response{};

	int rc = model->chat_with_tools_stream(model, arena, nullptr,
		&message, 1, &tool, 1, &response, nullptr, nullptr);
	ASSERT_EQ(rc, 200);
	ASSERT_EQ(response.tool_call_count, 1);
	EXPECT_STREQ(response.tool_calls[0].id, "call_patch");
	EXPECT_STREQ(response.tool_calls[0].name, "apply_patch");
	EXPECT_EQ(response.tool_calls[0].input_kind, TOOL_INPUT_TEXT);
	EXPECT_STREQ(response.tool_calls[0].arguments,
		"*** Begin Patch\n*** End Patch");
	EXPECT_NE(std::strstr(srv.last_request, "POST /v1/responses"), nullptr);
	EXPECT_NE(std::strstr(srv.last_request,
		"\"type\":\"custom\",\"name\":\"apply_patch\""), nullptr);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, ResponsesAdapterRejectsMissingApiKeyBeforeRequest) {
	struct model *model = model_llm_create("openai", "gpt-test",
		"http://127.0.0.1:1/v1", "");
	ASSERT_NE(model, nullptr);
	std::strcpy(model->adapter, "openai-responses");
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message message{};
	message.role = const_cast<char *>("user");
	message.content = const_cast<char *>("edit");
	struct chat_response response{};

	int rc = model->chat_with_tools_stream(model, arena, nullptr,
		&message, 1, nullptr, 0, &response, nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_NOT_CONFIGURED);
	EXPECT_NE(strstr(model->last_error, "no API key"), nullptr);

	arena_destroy(arena);
	model_destroy(model);
}

TEST_F(MockServerTest, ChatAdapterFallsBackToStringFunctionArgument) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
		api_base, "test-key");
	ASSERT_NE(model, nullptr);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct tool_call prior_call{};
	std::strcpy(prior_call.id, "call_patch");
	std::strcpy(prior_call.name, "apply_patch");
	prior_call.arguments = const_cast<char *>(
		"*** Begin Patch\n*** End Patch");
	prior_call.input_kind = TOOL_INPUT_TEXT;
	struct chat_message messages[3]{};
	messages[0].role = const_cast<char *>("user");
	messages[0].content = const_cast<char *>("edit");
	messages[1].role = const_cast<char *>("assistant");
	messages[1].tool_calls = &prior_call;
	messages[1].tool_call_count = 1;
	messages[2].role = const_cast<char *>("tool");
	messages[2].content = const_cast<char *>("invalid patch");
	messages[2].tool_call_id = const_cast<char *>("call_patch");
	struct tool_desc tool{};
	std::strcpy(tool.name, "apply_patch");
	std::strcpy(tool.description, "Apply patch");
	tool.input_kind = TOOL_INPUT_TEXT;
	struct chat_response response{};

	int rc = model->chat_with_tools_stream(model, arena, nullptr,
		messages, 3, &tool, 1, &response, nullptr, nullptr);
	EXPECT_EQ(rc, 200);
	EXPECT_NE(std::strstr(srv.last_request,
		"\"required\":[\"input\"]"), nullptr);
	EXPECT_NE(std::strstr(srv.last_request,
		"\"additionalProperties\":false"), nullptr);
	const char *body = std::strstr(srv.last_request, "\r\n\r\n");
	ASSERT_NE(body, nullptr);
	cJSON *request = cJSON_Parse(body + 4);
	ASSERT_NE(request, nullptr);
	cJSON *sent_messages = cJSON_GetObjectItem(request, "messages");
	cJSON *assistant = cJSON_GetArrayItem(sent_messages, 1);
	cJSON *sent_calls = cJSON_GetObjectItem(assistant, "tool_calls");
	cJSON *sent_call = cJSON_GetArrayItem(sent_calls, 0);
	cJSON *function = cJSON_GetObjectItem(sent_call, "function");
	const char *arguments = cJSON_GetStringValue(
		cJSON_GetObjectItem(function, "arguments"));
	ASSERT_NE(arguments, nullptr);
	cJSON *wrapper = cJSON_Parse(arguments);
	ASSERT_NE(wrapper, nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(wrapper, "input")),
		"*** Begin Patch\n*** End Patch");
	cJSON_Delete(wrapper);
	cJSON_Delete(request);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmRetriesTransientHttpErrorsBeforeStreaming) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"content\":\"recovered\"}}]}";
	srv.response_status = 200;
	srv.transient_error_body =
		"{\"error\":{\"message\":\"engine overloaded\"}}";
	srv.transient_error_status = 429;
	srv.transient_failures = 2;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	EXPECT_EQ(model->retry_count, 3);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;
	std::string streamed;

	int rc = model->chat_with_tools(
		model, arena, NULL, &msg, 1, NULL, 0, &response,
		[](const char *token, void *ud) -> int {
			static_cast<std::string *>(ud)->append(token ? token : "");
			return 0;
		}, &streamed);
	EXPECT_EQ(rc, 200);
	EXPECT_EQ(srv.request_count, 3);
	EXPECT_EQ(streamed, "recovered");
	ASSERT_NE(response.content, nullptr);
	EXPECT_STREQ(response.content, "recovered");

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmStopsAfterConfiguredRetriesAndKeepsLastError) {
	srv.response_body = "{}";
	srv.response_status = 200;
	srv.transient_error_body =
		"{\"error\":{\"message\":\"engine overloaded\"}}";
	srv.transient_error_status = 503;
	srv.transient_failures = 10;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	model->retry_count = 1;
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;

	int rc = model->chat_with_tools(model, arena, NULL, &msg, 1,
					NULL, 0, &response, NULL, NULL);
	EXPECT_EQ(rc, MORPH_ERR_API);
	EXPECT_EQ(srv.request_count, 2);
	EXPECT_NE(strstr(model->last_error, "HTTP 503"), nullptr);
	EXPECT_NE(strstr(model->last_error, "engine overloaded"), nullptr);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmDoesNotRetryNonTransientHttpErrors) {
	srv.response_body = "{\"error\":{\"message\":\"bad request\"}}";
	srv.response_status = 400;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;

	int rc = model->chat_with_tools(model, arena, NULL, &msg, 1,
					NULL, 0, &response, NULL, NULL);
	EXPECT_EQ(rc, MORPH_ERR_API);
	EXPECT_EQ(srv.request_count, 1);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmDoesNotRetryAfterStreamContentWasEmitted) {
	srv.response_body = "{}";
	srv.response_status = 200;
	srv.transient_error_body =
		"{\"choices\":[{\"delta\":{\"content\":\"partial\"}}]}";
	srv.transient_error_status = 429;
	srv.transient_failures = 10;
	srv.transient_sse_body = 1;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("test", "mock-model",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;
	std::string streamed;

	int rc = model->chat_with_tools(
		model, arena, NULL, &msg, 1, NULL, 0, &response,
		[](const char *token, void *ud) -> int {
			static_cast<std::string *>(ud)->append(token ? token : "");
			return 0;
		}, &streamed);
	EXPECT_EQ(rc, MORPH_ERR_API);
	EXPECT_EQ(srv.request_count, 1);
	EXPECT_EQ(streamed, "partial");

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmParsesKimiTopLevelCachedTokens) {
	srv.response_body =
		"{\"choices\":[{\"delta\":{\"content\":\"answer\"},"
		"\"finish_reason\":\"stop\"}],\"usage\":{"
		"\"prompt_tokens\":100,\"completion_tokens\":10,"
		"\"total_tokens\":110,\"cached_tokens\":80}}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("moonshot", "kimi-k3",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;

	int rc = model->chat_with_tools(model, arena, NULL, &msg, 1, NULL, 0,
					&response, NULL, NULL);
	EXPECT_EQ(rc, 200);
	EXPECT_EQ(response.usage.input_tokens, 100);
	EXPECT_EQ(response.usage.cached_tokens, 80);
	EXPECT_EQ(response.usage.output_tokens, 10);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, LlmUsesLargestCompatibleCachedTokenDetailOnce) {
	srv.response_body =
		"{\"choices\":[],\"usage\":{\"prompt_tokens\":100,"
		"\"cached_tokens\":60,\"prompt_tokens_details\":{"
		"\"cached_tokens\":70},\"input_tokens_details\":{"
		"\"cached_tokens\":80}}}\n\n"
		"data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}],"
		"\"usage\":{\"prompt_tokens\":100,"
		"\"prompt_tokens_details\":{\"cached_tokens\":80}}}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);

	char api_base[256];
	snprintf(api_base, sizeof(api_base), "http://127.0.0.1:%d/v1",
		 srv.port);
	struct model *model = model_llm_create("openai", "gpt-test",
					       api_base, "test-key");
	ASSERT_NE(model, nullptr);
	struct arena *arena = arena_create(8192);
	ASSERT_NE(arena, nullptr);
	struct chat_message msg = {
		(char *)"user", (char *)"hello", NULL, NULL, 0,
	};
	struct chat_response response;

	int rc = model->chat_with_tools(model, arena, NULL, &msg, 1, NULL, 0,
					&response, NULL, NULL);
	EXPECT_EQ(rc, 200);
	EXPECT_EQ(response.usage.input_tokens, 100);
	EXPECT_EQ(response.usage.cached_tokens, 80);

	arena_destroy(arena);
	model_destroy(model);
	mock_server_stop(&srv);
}

TEST_F(MockServerTest, SSECallbackErrorPropagates) {
	srv.response_body = "{\"choices\":[{\"delta\":{\"content\":\"test\"}}]}";
	srv.response_status = 200;
	START_MOCK_OR_SKIP(&srv);
	auto failing_cb = [](const char *data, size_t len, void *ud) -> int {
		(void)data;
		(void)len;
		(void)ud;
		return -EIO;
	};
	char url[256];
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", srv.port);
	int rc = http_post_sse_ex(url, "{}", 2,
				  "application/json", nullptr, 0,
				  failing_cb, nullptr);
	EXPECT_EQ(rc, -EIO);
	mock_server_stop(&srv);
}

/* ============================================= */
/* System prompt tests                           */
/* ============================================= */

TEST_F(ReactTest, SystemPromptCreateDestroy) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->system_prompt = strdup("Be creative and concise.");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptNoCrash) {
	setup_llm_with_response("Final: done");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->system_prompt = strdup("Always rhyme.");
	react_run(ctx, "say something", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	react_context_destroy(ctx);
}

struct capt_prompt_data {
	char *prompt;
	char *system_prompt;
	char *tool_descs;
	const char *resp;
	int tool_count;
};

static void capt_store_structured_messages(struct capt_prompt_data *d,
					   struct chat_message *messages,
					   int msg_count)
{
	std::ostringstream os;

	if (!d)
		return;
	for (int i = 0; i < msg_count; i++) {
		os << (messages[i].role ? messages[i].role : "") << ":"
		   << (messages[i].content ? messages[i].content : "")
		   << "\n";
	}
	free(d->prompt);
	d->prompt = strdup(os.str().c_str());
}

static int capt_prompt_chat(struct model *self, struct arena *arena,
			    const char *system_prompt,
			    const char **messages, int n,
			    const struct model_chat_options *opts,
			    sse_callback cb, void *user_data)
{
	(void)arena;
	(void)user_data;
	(void)opts;
	struct capt_prompt_data *d = (struct capt_prompt_data *)self->handle;
	free(d->prompt);
	d->prompt = (n > 0 && messages[0]) ? strdup(messages[0]) : nullptr;
	free(d->system_prompt);
	d->system_prompt = system_prompt ? strdup(system_prompt) : nullptr;
	if (cb && d->resp)
		cb(d->resp, user_data);
	return 200;
}

static int capt_prompt_chat_with_tools(struct model *self, struct arena *arena,
				       const char *system_prompt,
				       struct chat_message *messages, int msg_count,
				       struct tool_desc *tools, int tool_count,
				       struct chat_response *response,
				       sse_callback thought_cb, void *thought_ud)
{
	(void)arena;
	(void)messages;
	(void)msg_count;
	struct capt_prompt_data *d = (struct capt_prompt_data *)self->handle;
	free(d->system_prompt);
	d->system_prompt = system_prompt ? strdup(system_prompt) : nullptr;
	free(d->tool_descs);
	d->tool_descs = nullptr;
	d->tool_count = tool_count;
	if (tools && tool_count > 0) {
		std::ostringstream os;
		for (int i = 0; i < tool_count; i++) {
			os << tools[i].name << ":" << tools[i].description << "\n";
		}
		d->tool_descs = strdup(os.str().c_str());
	}
	capt_store_structured_messages(d, messages, msg_count);

	memset(response, 0, sizeof(*response));
	const char *content = d->resp ? d->resp : "";
	char *final_pos = strcasestr_local((char *)content, "Final:");
	if (final_pos) {
		final_pos += 6;
		while (*final_pos == ' ') final_pos++;
		content = final_pos;
	}
	response->content = strdup(content);
	if (thought_cb && response->content)
		thought_cb(response->content, thought_ud);
	return 200;
}

static void capt_prompt_destroy(struct model *self)
{
	if (!self) return;
	struct capt_prompt_data *d = (struct capt_prompt_data *)self->handle;
	free(d->prompt);
	free(d->system_prompt);
	free(d->tool_descs);
	free(d);
	free(self);
}

TEST_F(MockLlmTest, SystemPromptAppearsInPrompt) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->system_prompt = strdup("Custom instruction here.");
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	const char *found = nullptr;
	if (cd->system_prompt)
		found = strstr(cd->system_prompt, "Custom instruction here.");
	EXPECT_NE(found, nullptr) << "system_prompt should appear in the LLM prompt";
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptRequiresSmallCompleteMorphPatches) {
	struct capt_prompt_data *cd =
		(struct capt_prompt_data *)calloc(1, sizeof(*cd));
	struct tool_spec spec{};

	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;
	spec.origin = TOOL_ORIGIN_BUILTIN;
	spec.name = "apply_patch";
	spec.description = "Apply a patch";
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.input_kind = TOOL_INPUT_TEXT;
	spec.exec = test_tool_fn;
	ASSERT_EQ(::tool_register(&tools, &spec), 0);

	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
		nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "edit a large file", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "complete Codex patch"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "below 4 KiB"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "at most 80 changed lines"),
		nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "continuation marker"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "Do not emit unified-diff"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
		"must never have a leading +, space, or -"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
		"+/* MORPH_CONTINUE */\n*** End Patch"), nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptOmitsDisabledApplyPatchInstructions) {
	struct capt_prompt_data *cd =
		(struct capt_prompt_data *)calloc(1, sizeof(*cd));
	struct tool_spec spec{};

	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;
	spec.origin = TOOL_ORIGIN_BUILTIN;
	spec.name = "apply_patch";
	spec.description = "Apply a patch";
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.input_kind = TOOL_INPUT_TEXT;
	spec.exec = test_tool_fn;
	ASSERT_EQ(::tool_register(&tools, &spec), 0);
	ASSERT_EQ(tool_disable(&tools, "apply_patch"), 0);

	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
		nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "edit a file", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_EQ(strstr(cd->system_prompt, "Source editing:"), nullptr);
	EXPECT_EQ(strstr(cd->system_prompt, "complete Codex patch"), nullptr);
	EXPECT_EQ(cd->tool_count, 0);
	EXPECT_EQ(cd->tool_descs, nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, DisabledToolsAreAbsentFromPromptAndFunctionSurface) {
	struct capt_prompt_data *cd =
		(struct capt_prompt_data *)calloc(1, sizeof(*cd));
	struct tool_spec spec{};

	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;
	spec.origin = TOOL_ORIGIN_BUILTIN;
	spec.description = "Disabled test tool";
	spec.input_schema = TOOL_EMPTY_INPUT_SCHEMA;
	spec.output_schema = TOOL_OBJECT_OUTPUT_SCHEMA;
	spec.exec = test_tool_fn;
	spec.name = "file_list";
	ASSERT_EQ(::tool_register(&tools, &spec), 0);
	spec.name = "bash_exec";
	ASSERT_EQ(::tool_register(&tools, &spec), 0);
	ASSERT_EQ(tool_disable(&tools, "file_list"), 0);
	ASSERT_EQ(tool_disable(&tools, "bash_exec"), 0);

	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
		nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "inspect files", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_EQ(strstr(cd->system_prompt, "file_list"), nullptr);
	EXPECT_EQ(strstr(cd->system_prompt, "Shell filesystem permissions:"),
		nullptr);
	EXPECT_EQ(cd->tool_count, 0);
	EXPECT_EQ(cd->tool_descs, nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptRequiresMarkdownLinksForUrls) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Format web URLs as Markdown links"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "do not leave bare http(s) URLs in final answers"),
		  nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptRoutesMorphUsageQuestionsToSkill) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "activate the morph-usage skill before answering"),
		  nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolDescriptionsIncludeOriginPrefixes) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	ASSERT_EQ(tool_register(TOOL_ORIGIN_BUILTIN, &tools, "builtin_tool",
				"builtin desc", "{}", test_tool_fn, nullptr,
				nullptr), 0);
	ASSERT_EQ(tool_register(TOOL_ORIGIN_DYNAMIC_SESSION, &tools,
				"session_tool", "session desc", "{}",
				test_tool_fn, nullptr, nullptr), 0);
	ASSERT_EQ(tool_register(TOOL_ORIGIN_DYNAMIC_PERSISTENT, &tools,
				"persistent_tool", "persistent desc", "{}",
				test_tool_fn, nullptr, nullptr), 0);

	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							 nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->tool_descs, nullptr);
	EXPECT_NE(strstr(cd->tool_descs,
			 "builtin_tool:[system built-in] builtin desc"),
		  nullptr);
	EXPECT_NE(strstr(cd->tool_descs,
			 "session_tool:[dynamic session] session desc"),
		  nullptr);
	EXPECT_NE(strstr(cd->tool_descs,
			 "persistent_tool:[dynamic persistent] persistent desc"),
		  nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SystemPromptIncludesMarkdownOutputRules) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "MARKDOWN OUTPUT"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Do not wrap the entire response in a code block"),
		  nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Do not use Markdown tables on mobile clients"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt,
			 "Chinese prose may use normal Chinese punctuation"),
		  nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MemoryContextAppearsInPrompt) {
	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	cd->resp = "Final: answer";
	llm = (struct model *)calloc(1, sizeof(*llm));
	strncpy(llm->provider, "mock", sizeof(llm->provider) - 1);
	strncpy(llm->model_id, "mock", sizeof(llm->model_id) - 1);
	strncpy(llm->api_key, "k", sizeof(llm->api_key) - 1);
	llm->context_limit = 128000;
	llm->chat = capt_prompt_chat;
	llm->chat_with_tools = capt_prompt_chat_with_tools;
	llm->destroy = capt_prompt_destroy;
	llm->handle = cd;
	llm_data = nullptr;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ASSERT_EQ(react_set_memory_context(ctx,
			"Persistent memory for this session.\n- Preferred language: Chinese\n"),
		  0);
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(cd->system_prompt, nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "Preferred language: Chinese"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "latest explicit language instruction"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "progress updates before"), nullptr);
	EXPECT_NE(strstr(cd->system_prompt, "Do not switch to English"), nullptr);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Multi-turn conversation (message accumulation)*/
/* ============================================= */

TEST_F(MockLlmTest, MultiTurnMessageAccumulation) {
	setup_llm_with_response("Final: ok");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "turn 1", nullptr, nullptr);
	int after_first = msg_list_count(ctx->messages);
	EXPECT_EQ(after_first, 2);

	react_run(ctx, "turn 2", nullptr, nullptr);
	int after_second = msg_list_count(ctx->messages);
	EXPECT_EQ(after_second, 4);

	react_run(ctx, "turn 3", nullptr, nullptr);
	int after_third = msg_list_count(ctx->messages);
	EXPECT_EQ(after_third, 6);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MultiTurnFinalAnswerUpdated) {
	setup_llm_with_response("Final: first answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "first", nullptr, nullptr);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "first answer") != nullptr);

	llm_data->response = "Final: second answer";
	react_run(ctx, "second", nullptr, nullptr);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "second answer") != nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, MultiTurnMessageRolesAlternate) {
	setup_llm_with_response("Final: answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "hello", nullptr, nullptr);
	struct message_list *cur = ctx->messages;
	EXPECT_STREQ(cur->role, "user");
	EXPECT_STREQ(cur->content, "hello");
	cur = cur->next;
	ASSERT_NE(cur, nullptr);
	EXPECT_STREQ(cur->role, "assistant");
	EXPECT_TRUE(strstr(cur->content, "answer") != nullptr);

	react_context_destroy(ctx);
}

/* ============================================= */
/* Compression integration within react_run      */
/* ============================================= */

static int test_compress_cb(const char *text, void *user_data, char **out)
{
	(void)text;
	(void)user_data;
	*out = strdup("[COMPRESSED SUMMARY]");
	return *out ? 0 : -ENOMEM;
}

struct compress_probe {
	int calls;
	std::string summarized_text;
};

static int test_compress_probe_cb(const char *text, void *user_data,
				  char **out)
{
	struct compress_probe *probe = (struct compress_probe *)user_data;

	if (probe) {
		probe->calls++;
		probe->summarized_text = text ? text : "";
	}
	*out = strdup("[COMPRESSED SUMMARY]");
	return *out ? 0 : -ENOMEM;
}

TEST_F(MockLlmTest, CompressTriggerInReact) {
	cfg.max_context_tokens = 6;
	cfg.summarize_threshold_ratio = 0.5;

	setup_llm_with_response("Final: after compress");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->compress.summarize = test_compress_cb;
	ctx->compress.summarize_user_data = nullptr;
	ctx->compress.max_history_rounds = 1;

	react_run(ctx, "first", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);

	react_run(ctx, "second", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, CompressBeforePreparingLlmMessages) {
	cfg.max_context_tokens = 12;
	cfg.summarize_threshold_ratio = 0.5;
	cfg.max_history_rounds = 1;

	struct capt_prompt_data *cd = (struct capt_prompt_data *)calloc(1, sizeof(*cd));
	ASSERT_NE(cd, nullptr);
	cd->resp = "Final: ok";
	struct model *capt = (struct model *)calloc(1, sizeof(*capt));
	ASSERT_NE(capt, nullptr);
	strncpy(capt->provider, "mock", sizeof(capt->provider) - 1);
	strncpy(capt->model_id, "mock-model", sizeof(capt->model_id) - 1);
	strncpy(capt->api_key, "mock-key", sizeof(capt->api_key) - 1);
	capt->chat = capt_prompt_chat;
	capt->chat_with_tools = capt_prompt_chat_with_tools;
	capt->destroy = capt_prompt_destroy;
	capt->handle = cd;
	llm = capt;

	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	struct compress_probe probe = {0, ""};
	ctx->compress.summarize = test_compress_probe_cb;
	ctx->compress.summarize_user_data = &probe;

	ASSERT_EQ(react_run(ctx,
		"first turn has enough repeated words to exceed the tiny test context",
		nullptr, nullptr), 0);
	ASSERT_EQ(probe.calls, 0);

	ASSERT_EQ(react_run(ctx, "second turn should trigger compression",
			    nullptr, nullptr), 0);
	EXPECT_GE(probe.calls, 1);
	ASSERT_NE(cd->prompt, nullptr);
	EXPECT_NE(strstr(cd->prompt, "[COMPRESSED SUMMARY]"), nullptr);
	EXPECT_NE(strstr(ctx->messages->content, "[COMPRESSED SUMMARY]"),
		  nullptr);
	EXPECT_NE(probe.summarized_text.find("first turn"), std::string::npos);

	react_context_destroy(ctx);
	model_destroy(llm);
	llm = nullptr;
}

TEST_F(MockLlmTest, CompressPreservesAfterFlow) {
	cfg.max_context_tokens = 6;
	cfg.summarize_threshold_ratio = 0.5;

	setup_llm_with_response("Final: ok");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->compress.summarize = test_compress_cb;
	ctx->compress.summarize_user_data = nullptr;
	ctx->compress.max_history_rounds = 1;

	for (int i = 0; i < 4; i++) {
		char input[32];
		snprintf(input, sizeof(input), "run %d", i);
		react_run(ctx, input, nullptr, nullptr);
		EXPECT_EQ(ctx->state, REACT_STATE_DONE);
		EXPECT_NE(ctx->final_answer, nullptr);
	}
	react_context_destroy(ctx);
}

/* ============================================= */
/* Tool execution edge cases                     */
/* ============================================= */

TEST_F(MockLlmTest, ActionToolNotFound) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "good_tool", "Works", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: try bad tool.\nAction: bad_tool({\"x\":1})\n",
		"Thought: it failed.\nFinal: tool not available"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "not available") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionInvalidFormat) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "Test", "{}", test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: bad format.\nAction: test_tool({\"x\")\n",
		"Thought: adjusting.\nFinal: recovered"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "recovered") != nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ActionNoToolsRegistered) {
	setup_llm_with_response("Action: some_tool({})\n");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

static int null_result_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	(void)user_data;
	tool_result_clear(result);
	return 0;
}

TEST_F(MockLlmTest, ToolNullResult) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "null_tool", "Returns null result", "{}",
		      null_result_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: calling null tool.\nAction: null_tool({})\n",
		"Thought: got null.\nFinal: handled null result"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "handled null result") != nullptr);
	react_context_destroy(ctx);
}

/* ============================================= */
/* LLM response edge cases                       */
/* ============================================= */

TEST_F(MockLlmTest, EmptyLlmResponse) {
	setup_llm_with_response("");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_LLM);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "empty response"), nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ResponseOnlyFinalPrefix) {
	setup_llm_with_response("Final:");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_LLM);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "empty response"), nullptr);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ThoughtOnly) {
	setup_llm_with_response("Thought: just thinking\n");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	int rc = react_run(ctx, "test", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Cancellation during tool execution            */
/* ============================================= */

static int self_cancel_tool_fn(const char *args_json, struct tool_result *result, void *user_data)
{
	(void)args_json;
	struct react_context *ctx = (struct react_context *)user_data;
	ctx->cancelled = 1;
	(void)tool_result_success_json_text(result, strdup("{\"cancelled\":true}"));
	return 0;
}

struct slow_signal_tool_state {
	std::atomic<int> entered;
	std::atomic<int> finished;
};

static int slow_signal_tool_fn(const char *args_json, struct tool_result *result,
			       void *user_data)
{
	(void)args_json;
	struct slow_signal_tool_state *state =
		(struct slow_signal_tool_state *)user_data;

	state->entered.store(1);
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	state->finished.store(1);
	(void)tool_result_success_json_text(result, strdup("{\"done\":true}"));
	return 0;
}

TEST_F(MockLlmTest, CancelDuringToolExecution) {
	const char *responses[] = {
		"Thought: run self-cancel tool.\nAction: self_cancel({})\n",
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "self_cancel", "Cancels context", "{}",
		      self_cancel_tool_fn, ctx, nullptr);
	ctx->max_iterations = 5;
	int rc = react_run(ctx, "cancel me", nullptr, nullptr);
	EXPECT_EQ(rc, -ECANCELED);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_CANCELLED);
	EXPECT_EQ(ctx->last_error_code, -ECANCELED);
	EXPECT_STREQ(ctx->outcome_reason, "user_cancelled");
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, SigintCancelsBlockedToolJoinPromptly) {
	const char *responses[] = {
		"Thought: run slow tool.\nAction: slow_signal({})\n",
		"Final: recovered after cancellation",
	};
	struct slow_signal_tool_state state;
	state.entered.store(0);
	state.finished.store(0);
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "slow_signal", "Slow tool", "{}",
		      slow_signal_tool_fn, &state, nullptr);

	std::thread interrupter([&state]() {
		for (int i = 0; i < 50 && !state.entered.load(); i++)
			std::this_thread::sleep_for(
				std::chrono::milliseconds(10));
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		react_sigint_flag = 1;
		http_cancel_from_signal();
	});

	auto start = std::chrono::steady_clock::now();
	int rc = react_run(ctx, "cancel slow tool", nullptr, nullptr);
	auto elapsed = std::chrono::steady_clock::now() - start;
	interrupter.join();

	EXPECT_EQ(rc, -ECANCELED);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_CANCELLED);
	EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
			  elapsed).count(),
		  1000);

	for (int i = 0; i < 200 && !state.finished.load(); i++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_TRUE(state.finished.load());
	EXPECT_EQ(react_run(ctx, "follow-up question", nullptr, nullptr), 0);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(std::strstr(ctx->final_answer, "recovered after cancellation"),
		  nullptr);

	react_context_destroy(ctx);
	http_clear_signal_cancel();
	react_sigint_flag = 0;
}

/* ============================================= */
/* Step list traversal                           */
/* ============================================= */

TEST_F(MockLlmTest, StepLinkedListTraversal) {
	const char *responses[] = {
		"Thought: step 1.\nAction: counter({})\n",
		"Thought: step 2.\nFinal: done after two steps"
	};
	int call_count = 0;
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "counter", "Counts", "{}",
		      call_count_tool_fn, &call_count, nullptr);
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	react_run(ctx, "multi-step flow", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_GE(call_count, 1);
	ASSERT_GE(ctx->step_count, 3);
	struct react_step *s = ctx->steps;
	int type_order[] = {REACT_STEP_THOUGHT, REACT_STEP_ACTION,
			    REACT_STEP_OBSERVATION, REACT_STEP_THOUGHT,
			    REACT_STEP_FINAL};
	int idx = 0;
	while (s && idx < 5) {
		EXPECT_EQ(s->type, type_order[idx]);
		s = s->next;
		idx++;
	}
	react_context_destroy(ctx);
}

/* ============================================= */
/* Context reuse after complex interaction       */
/* ============================================= */

TEST_F(MockLlmTest, ReuseContextAfterDone) {
	setup_llm_with_response("Final: first");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;

	react_run(ctx, "first input", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);

	llm_data->response = "Final: second";
	react_run(ctx, "second input", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_TRUE(strstr(ctx->final_answer, "second") != nullptr);

	react_context_destroy(ctx);
}

/* ============================================= */
/* Message list with file paths                  */
/* ============================================= */

TEST_F(ReactTest, MsgListWithFilePaths) {
	struct message_list *m = msg_list_create(ar, "user", "check file", 2);
	ASSERT_NE(m, nullptr);
	m->file_paths = (char **)calloc(2, sizeof(char *));
	m->file_paths[0] = strdup("/tmp/test.txt");
	m->file_count = 1;
	EXPECT_STREQ(m->file_paths[0], "/tmp/test.txt");
	EXPECT_EQ(m->file_count, 1);
}

TEST_F(ReactTest, MsgListCreateNullContent) {
	struct message_list *m = msg_list_create(ar, "user", nullptr, 1);
	ASSERT_NE(m, nullptr);
	EXPECT_STREQ(m->content, "");
}

TEST_F(ReactTest, MsgListAppendToNullHead) {
	struct message_list *head = nullptr;
	msg_list_append(nullptr, nullptr);
	EXPECT_NO_FATAL_FAILURE(msg_list_append(&head, nullptr));
	EXPECT_EQ(msg_list_count(head), 0);
}

TEST_F(ReactTest, ContextNeedsCompressExactThreshold) {
	struct compress_config test_cfg = {
		.max_context_tokens = 10,
		.max_history_rounds = 2,
		.summarize_threshold_ratio = 0.5,
	};
	struct message_list *head = nullptr;
	for (int i = 0; i < 5; i++)
		msg_list_append(&head, msg_list_create(ar, "user", "msg", 1));
	int needs = context_needs_compress(head, nullptr, &test_cfg);
	EXPECT_EQ(needs, 1);
}

TEST_F(ReactTest, ContextNeedsCompressBelowThreshold) {
	struct compress_config test_cfg = {
		.max_context_tokens = 100,
		.max_history_rounds = 2,
		.summarize_threshold_ratio = 0.5,
	};
	struct message_list *head = nullptr;
	msg_list_append(&head, msg_list_create(ar, "user", "hi", 1));
	int needs = context_needs_compress(head, nullptr, &test_cfg);
	EXPECT_EQ(needs, 0);
}

/* ============================================= */
/* Guardrail tests                                */
/* ============================================= */

TEST_F(ReactTest, GuardrailStepTypeName) {
	EXPECT_STREQ(react_step_type_name(REACT_STEP_REFLECTION), "Reflection");
}

TEST_F(ReactTest, GuardrailStateName) {
	EXPECT_STREQ(react_state_name(REACT_STATE_GUARDRAIL), "GUARDRAIL");
}

TEST_F(ReactTest, GuardrailDefaultDisabled) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->guardrail.enabled, 0);
	EXPECT_EQ(ctx->guardrail.max_retries, 2);
	EXPECT_EQ(ctx->guardrail.max_empty_rounds, 3);
	EXPECT_EQ(ctx->guardrail_retry_count, 0);
	react_context_destroy(ctx);
}

TEST_F(ReactTest, GuardrailEnabledField) {
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 2;
	ctx->guardrail.max_empty_rounds = 2;
	EXPECT_EQ(ctx->guardrail.enabled, 1);
	EXPECT_EQ(ctx->guardrail.max_retries, 2);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GuardrailDisabledNoGuardrailStep) {
	setup_llm_with_response("Final: direct answer");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 0;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	bool has_guardrail = false;
	struct react_step *s = ctx->steps;
	while (s) {
		if (s->type == REACT_STEP_REFLECTION)
			has_guardrail = true;
		s = s->next;
	}
	EXPECT_FALSE(has_guardrail);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GuardrailEnabledPassesOnGoodAnswer) {
	const char *responses[] = {
		"Final: the answer with saved: output.png",
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	react_run(ctx, "hello", nullptr, nullptr);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, GuardrailCancelDuringRetry) {
	const char *responses[] = {
		"Final: answer",
	};
	llm = create_multi_mock_llm(responses, 1);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg, nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 3;
	react_cancel(ctx);
	react_run(ctx, "test", nullptr, nullptr);
	EXPECT_TRUE(ctx->state == REACT_STATE_ABORT ||
		    ctx->state == REACT_STATE_DONE);
	react_context_destroy(ctx);
}

static enum guardrail_verdict reject_blocked_input(
	const struct guardrail_eval_ctx *ctx, char *reason, size_t cap)
{
	if (ctx->user_input && strstr(ctx->user_input, "blocked")) {
		snprintf(reason, cap, "blocked input");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict reject_bad_answer(
	const struct guardrail_eval_ctx *ctx, char *reason, size_t cap)
{
	if (ctx->proposed_answer && strstr(ctx->proposed_answer, "bad")) {
		snprintf(reason, cap, "bad answer");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

static enum guardrail_verdict reject_tool_output(
	const struct guardrail_eval_ctx *ctx, char *reason, size_t cap)
{
	if (ctx->tool_result && strstr(ctx->tool_result, "test")) {
		snprintf(reason, cap, "tool output rejected");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

TEST_F(MockLlmTest, InputGuardrailRejectsBeforeLlmCall) {
	setup_llm_with_response("Final: should not be called");
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ASSERT_EQ(guardrail_rule_register(&ctx->guardrail, "reject_blocked",
		GUARDRAIL_HOOK_INPUT, GUARDRAIL_RULE_C, reject_blocked_input,
		NULL, NULL, "Use a different request."), 0);

	int rc = react_run(ctx, "this is blocked", nullptr, nullptr);
	EXPECT_EQ(rc, -EPERM);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_GUARDRAIL_DENIED);
	EXPECT_EQ(llm_data->call_count, 0);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "blocked input"), nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, OutputGuardrailRetriesAndFinalizes) {
	const char *responses[] = {
		"Final: bad answer",
		"Final: good answer"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct multi_mock_data *data = (struct multi_mock_data *)llm->handle;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ASSERT_EQ(guardrail_rule_register(&ctx->guardrail, "reject_bad",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C, reject_bad_answer,
		NULL, NULL, "Try again."), 0);

	int rc = react_run(ctx, "answer carefully", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(data->call_count, 2);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "good answer"), nullptr);
	bool saw_reflection = false;
	for (struct react_step *s = ctx->steps; s; s = s->next) {
		if (s->type == REACT_STEP_REFLECTION &&
		    s->content && strstr(s->content, "bad answer"))
			saw_reflection = true;
	}
	EXPECT_TRUE(saw_reflection);
	EXPECT_EQ(msg_list_count(ctx->messages), 2);
	ASSERT_NE(ctx->messages, nullptr);
	EXPECT_STREQ(ctx->messages->role, "user");
	ASSERT_NE(ctx->messages->next, nullptr);
	EXPECT_STREQ(ctx->messages->next->role, "assistant");
	EXPECT_NE(strstr(ctx->messages->next->content, "good answer"), nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmptyOutputRetriesThenFails) {
	const char *responses[] = { "", "" };
	llm = create_multi_mock_llm(responses, 2);
	struct multi_mock_data *data = (struct multi_mock_data *)llm->handle;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ctx->guardrail.max_empty_rounds = 2;

	int rc = react_run(ctx, "answer carefully", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_LLM);
	EXPECT_EQ(data->call_count, 2);
	EXPECT_EQ(ctx->state, REACT_STATE_ABORT);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_LLM_ERROR);
	EXPECT_STREQ(ctx->outcome_reason, "empty_response");
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "empty response"), nullptr);
	EXPECT_EQ(msg_list_count(ctx->messages), 1);
	ASSERT_NE(ctx->messages, nullptr);
	EXPECT_STREQ(ctx->messages->role, "user");
	EXPECT_EQ(ctx->messages->next, nullptr);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, EmptyOutputHonorsMaxEmptyRounds) {
	const char *responses[] = { "", "unused" };
	llm = create_multi_mock_llm(responses, 2);
	struct multi_mock_data *data = (struct multi_mock_data *)llm->handle;
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 3;
	ctx->guardrail.max_empty_rounds = 1;

	int rc = react_run(ctx, "answer carefully", nullptr, nullptr);
	EXPECT_EQ(rc, MORPH_ERR_LLM);
	EXPECT_EQ(data->call_count, 1);
	EXPECT_EQ(ctx->outcome, REACT_OUTCOME_LLM_ERROR);

	react_context_destroy(ctx);
}

TEST_F(MockLlmTest, ToolOutputGuardrailRewritesObservation) {
	tool_register(TOOL_ORIGIN_BUILTIN, &tools, "test_tool", "A test tool", "{}",
		      test_tool_fn, nullptr, nullptr);
	const char *responses[] = {
		"Thought: use tool.\nAction: test_tool({})\n",
		"Final: done after guarded observation"
	};
	llm = create_multi_mock_llm(responses, 2);
	struct react_context *ctx = react_context_create(&tools, tok, &cfg,
							nullptr);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->guardrail.enabled = 1;
	ctx->guardrail.max_retries = 1;
	ASSERT_EQ(guardrail_rule_register(&ctx->guardrail, "reject_tool",
		GUARDRAIL_HOOK_TOOL_OUTPUT, GUARDRAIL_RULE_C,
		reject_tool_output, NULL, NULL,
		"Inspect the tool output."), 0);

	int rc = react_run(ctx, "guard tool output", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	bool saw_guarded_observation = false;
	bool saw_reflection = false;
	for (struct react_step *s = ctx->steps; s; s = s->next) {
		if (s->type == REACT_STEP_OBSERVATION &&
		    s->content && strstr(s->content,
					"guardrail: tool output rejected"))
			saw_guarded_observation = true;
		if (s->type == REACT_STEP_REFLECTION &&
		    s->content && strstr(s->content, "tool output rejected"))
			saw_reflection = true;
	}
	EXPECT_TRUE(saw_guarded_observation);
	EXPECT_TRUE(saw_reflection);

	react_context_destroy(ctx);
}

/* ---- HITL (Human-in-the-Loop) tests ---- */

static int hitl_deny_count = 0;

static enum hitl_verdict hitl_deny_callback(const char *tool_name,
					     const char *tool_args,
					     void *user_data)
{
	(void)tool_args;
	(void)user_data;
	hitl_deny_count++;
	if (strcmp(tool_name, "dangerous_tool") == 0)
		return HITL_DENY;
	return HITL_APPROVE;
}

static enum hitl_verdict hitl_always_callback(const char *tool_name,
					      const char *tool_args,
					      void *user_data)
{
	(void)tool_args;
	(void)user_data;
	if (strcmp(tool_name, "dangerous_tool") == 0)
		return HITL_ALWAYS;
	return HITL_APPROVE;
}

static enum hitl_verdict hitl_approve_all_callback(const char *tool_name,
						   const char *tool_args,
						   void *user_data)
{
	(void)tool_name;
	(void)tool_args;
	(void)user_data;
	return HITL_APPROVE;
}

TEST(HitlTest, NeedsApprovalDisabled) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "test_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "test_tool"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalEnabledAllTools) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "test_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "test_tool"), 1);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalSpecificTool) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "bash_exec", "desc", "{}", test_tool_fn, NULL, NULL);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.tools_count = 1;
	strncpy(ctx->hitl.tools[0], "bash_exec", HITL_TOOL_NAME_MAX - 1);
	EXPECT_EQ(hitl_needs_approval(ctx, "bash_exec"), 1);
	EXPECT_EQ(hitl_needs_approval(ctx, "file_read"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalInternalApprovalTool) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "bash_exec", "desc", "{}", test_tool_fn, NULL, NULL);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dangerous_tool", "desc", "{}", test_tool_fn, NULL,
		      NULL);
	struct tool_entry *e = tool_lookup(&reg, "bash_exec");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_INTERNAL_APPROVAL;
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.tools_count = 2;
	strncpy(ctx->hitl.tools[0], "bash_exec", HITL_TOOL_NAME_MAX - 1);
	strncpy(ctx->hitl.tools[1], "dangerous_tool", HITL_TOOL_NAME_MAX - 1);
	EXPECT_EQ(hitl_needs_approval(ctx, "bash_exec"), 0);
	EXPECT_EQ(hitl_needs_approval(ctx, "dangerous_tool"), 1);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalAutoApproved) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "bash_exec", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	hitl_add_auto_approved(&ctx->hitl, "bash_exec");
	EXPECT_EQ(hitl_needs_approval(ctx, "bash_exec"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalReadonlyAutoApprove) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	struct tool_entry *e = tool_lookup(&reg, "file_read");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_READONLY;
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 1;
	ctx->hitl.tools_count = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "file_read"), 0);
	react_context_destroy(ctx);
}

TEST(HitlTest, NeedsApprovalReadonlyNoAutoApprove) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	struct tool_entry *e = tool_lookup(&reg, "file_read");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_READONLY;
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	EXPECT_EQ(hitl_needs_approval(ctx, "file_read"), 1);
	react_context_destroy(ctx);
}

TEST(HitlTest, AddAutoApprovedIdempotent) {
	struct hitl_config h;
	memset(&h, 0, sizeof(h));
	h.auto_approved_count = 0;
	hitl_add_auto_approved(&h, "tool_a");
	EXPECT_EQ(h.auto_approved_count, 1);
	hitl_add_auto_approved(&h, "tool_a");
	EXPECT_EQ(h.auto_approved_count, 1);
	hitl_add_auto_approved(&h, "tool_b");
	EXPECT_EQ(h.auto_approved_count, 2);
}

TEST(HitlTest, ConfigDefaultsDisabled) {
	struct config cfg;
	config_set_defaults(&cfg);
	EXPECT_EQ(cfg.react.hitl_enabled, 0);
	EXPECT_EQ(cfg.react.hitl_tools_count, 0);
	EXPECT_EQ(cfg.react.hitl_auto_approve_readonly, 1);
}

TEST(HitlTest, ToolIsReadonly) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "file_read", "desc", "{}", test_tool_fn, NULL, NULL);
	EXPECT_EQ(tool_is_readonly(&reg, "file_read"), 0);
	struct tool_entry *e = tool_lookup(&reg, "file_read");
	ASSERT_NE(e, nullptr);
	e->flags |= TOOL_FLAG_READONLY;
	EXPECT_EQ(tool_is_readonly(&reg, "file_read"), 1);
	EXPECT_EQ(tool_is_readonly(&reg, "nonexistent"), 0);
}

TEST(HitlTest, DenyCallbackPreventsExecution) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dangerous_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "safe_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.approval_cb = hitl_deny_callback;
	ctx->hitl.approval_user_data = NULL;
	hitl_deny_count = 0;
	enum hitl_verdict v1 = ctx->hitl.approval_cb("dangerous_tool", "{}", NULL);
	EXPECT_EQ(v1, HITL_DENY);
	enum hitl_verdict v2 = ctx->hitl.approval_cb("safe_tool", "{}", NULL);
	EXPECT_EQ(v2, HITL_APPROVE);
	EXPECT_EQ(hitl_deny_count, 2);
	react_context_destroy(ctx);
}

TEST(HitlTest, DenyDuringReactRunSkipsToolExecution) {
	struct tool_registry reg;
	struct tokenizer *tok = tokenizer_create("gpt-4o", 128000);
	struct compress_config ccfg = {0};
	int dangerous_count = 0;
	const char *responses[] = {
		"Thought: try dangerous.\nAction: dangerous_tool({})\n",
		"Final: denied and done"
	};
	struct model *llm = create_multi_mock_llm(responses, 2);
	struct multi_mock_data *data = (struct multi_mock_data *)llm->handle;

	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dangerous_tool", "desc", "{}",
		      call_count_tool_fn, &dangerous_count, NULL);
	ccfg.max_context_tokens = 128000;
	ccfg.max_history_rounds = 6;
	ccfg.summarize_threshold_ratio = 0.8;
	ccfg.compress_target_ratio = 0.5;
	struct react_context *ctx = react_context_create(&reg, tok, &ccfg,
							NULL);
	ASSERT_NE(ctx, nullptr);
	ctx->llm_model = llm;
	ctx->max_iterations = 5;
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.approval_cb = hitl_deny_callback;
	hitl_deny_count = 0;

	struct morph_event_recorder rec;
	ASSERT_EQ(0, morph_event_recorder_init(&rec));
	ASSERT_EQ(0, react_set_event_callback(ctx, morph_event_recorder_cb,
					      &rec));

	int rc = react_run(ctx, "run dangerous", nullptr, nullptr);
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(ctx->state, REACT_STATE_DONE);
	EXPECT_EQ(dangerous_count, 0);
	EXPECT_EQ(hitl_deny_count, 1);
	EXPECT_EQ(data->call_count, 2);
	EXPECT_TRUE(event_recorder_has_name(&rec, "hitl.request"));
	EXPECT_TRUE(event_recorder_has_name(&rec, "hitl.denied"));
	ASSERT_NE(ctx->final_answer, nullptr);
	EXPECT_NE(strstr(ctx->final_answer, "denied and done"), nullptr);

	morph_event_recorder_cleanup(&rec);
	react_context_destroy(ctx);
	model_destroy(llm);
	tokenizer_destroy(tok);
	tool_registry_cleanup(&reg);
}

TEST(HitlTest, AlwaysCallbackAutoApproves) {
	struct tool_registry reg;
	tool_registry_init(&reg);
	tool_register(TOOL_ORIGIN_BUILTIN, &reg, "dangerous_tool", "desc", "{}", test_tool_fn, NULL, NULL);
	struct compress_config ccfg = {0};
	struct guardrail_config gcfg = {0};
	struct react_context *ctx = react_context_create(&reg, NULL, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	ctx->hitl.enabled = 1;
	ctx->hitl.auto_approve_readonly = 0;
	ctx->hitl.tools_count = 0;
	ctx->hitl.approval_cb = hitl_always_callback;
	ctx->hitl.approval_user_data = NULL;
	enum hitl_verdict v = ctx->hitl.approval_cb("dangerous_tool", "{}", NULL);
	EXPECT_EQ(v, HITL_ALWAYS);
	hitl_add_auto_approved(&ctx->hitl, "dangerous_tool");
	EXPECT_EQ(hitl_needs_approval(ctx, "dangerous_tool"), 0);
	react_context_destroy(ctx);
}

/* ============================================= */
/* Extensible Guardrail tests                    */
/* ============================================= */

TEST(Guardrail, RegisterBuiltinRules) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	EXPECT_EQ(cfg.rule_count, 6);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "empty_answer"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "consecutive_empty"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "tools_all_failed"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "creative_no_media"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "creative_file_missing"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&cfg, "final_local_file_missing"), nullptr);
}

TEST(Guardrail, RuleEnableDisable) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	EXPECT_EQ(guardrail_rule_disable(&cfg, "empty_answer"), 0);
	EXPECT_EQ(guardrail_rule_lookup(&cfg, "empty_answer")->enabled, 0);
	EXPECT_EQ(guardrail_rule_enable(&cfg, "empty_answer"), 0);
	EXPECT_EQ(guardrail_rule_lookup(&cfg, "empty_answer")->enabled, 1);
	EXPECT_EQ(guardrail_rule_disable(&cfg, "nonexistent"), -ENOENT);
}

TEST(Guardrail, EmptyAnswerFail) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "empty_answer");
	arena_destroy(a);
}

TEST(Guardrail, EmptyAnswerPass) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "Hello world";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, CreativeNoMediaIgnoresTextFileReferences) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct react_step action;
	struct react_step obs;
	memset(&action, 0, sizeof(action));
	memset(&obs, 0, sizeof(obs));
	action.type = REACT_STEP_ACTION;
	action.tool_name = (char *)"img_gen";
	action.next = &obs;
	obs.type = REACT_STEP_OBSERVATION;
	obs.error_code = 0;
	obs.content = (char *)"image generated: /tmp/looks-real.png";

	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "done";
	ctx.steps = &action;
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "creative_no_media");
	arena_destroy(a);
}

TEST(Guardrail, CreativeFileMissingUsesStructuredArtifactsOnly) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.tool_name = "img_gen";
	ctx.tool_result = "image generated: /tmp/missing-from-text.png";
	ctx.tool_error_code = 0;
	ctx.arena = a;
	auto pass = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_TOOL_OUTPUT, &ctx);
	EXPECT_EQ(pass.verdict, GUARDRAIL_PASS);

	struct tool_artifact_list artifacts;
	memset(&artifacts, 0, sizeof(artifacts));
	artifacts.count = 1;
	artifacts.items[0].kind = TOOL_ARTIFACT_IMAGE;
	snprintf(artifacts.items[0].path, sizeof(artifacts.items[0].path),
		 "/tmp/morph_missing_structured_artifact.png");
	ctx.tool_artifacts = &artifacts;
	auto fail = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_TOOL_OUTPUT, &ctx);
	EXPECT_EQ(fail.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(fail.triggered_rule, nullptr);
	EXPECT_STREQ(fail.triggered_rule->name, "creative_file_missing");
	arena_destroy(a);
}

TEST(Guardrail, FinalLocalFileMissingFailsForFileUrl) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"done [artifact](file:///tmp/morph_missing_final_file.txt)";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "final_local_file_missing");
	EXPECT_NE(strstr(r.reason, "Referenced local file does not exist"),
		  nullptr);
	arena_destroy(a);
}

TEST(Guardrail, FinalLocalFileMissingFailsForAbsolutePath) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"done ![image](/tmp/morph_missing_final_image.png)";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "final_local_file_missing");
	arena_destroy(a);
}

TEST(Guardrail, FinalLocalFileMissingPassesForExistingFile) {
	const char *path = "/tmp/morph_existing_final_file.txt";
	ASSERT_EQ(file_write_all(path, "ok", 2), 0);

	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "done [artifact](/tmp/morph_existing_final_file.txt)";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	std::remove(path);
	arena_destroy(a);
}

TEST(Guardrail, FinalLocalFileMissingIgnoresNonLocalLinks) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"[site](https://example.com) [mail](mailto:a@example.com) "
		"[anchor](#section) [data](data:text/plain,hello)";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, FinalLocalFileMissingIgnoresPlainTextPaths) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "The file is /tmp/morph_missing_bare_path.txt";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, CustomCRule) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	auto my_check = [](const struct guardrail_eval_ctx *ctx,
			   char *reason, size_t cap) -> enum guardrail_verdict {
		if (ctx->user_input && strstr(ctx->user_input, "forbidden"))
			{ snprintf(reason, cap, "Forbidden word"); return GUARDRAIL_FAIL; }
		return GUARDRAIL_PASS;
	};
	guardrail_rule_register(&cfg, "no_forbidden", GUARDRAIL_HOOK_INPUT,
		GUARDRAIL_RULE_C, my_check, NULL, NULL, "Do not use forbidden words.");
	EXPECT_EQ(cfg.rule_count, 7);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.user_input = "this is forbidden text";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_INPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "no_forbidden");
	arena_destroy(a);
}

TEST(Guardrail, DisabledConfigPasses) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 0;
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, DuplicateNameRejected) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	int rc = guardrail_rule_register(&cfg, "empty_answer",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_C, NULL, NULL, NULL, NULL);
	EXPECT_EQ(rc, -EEXIST);
}

TEST(Guardrail, ConsecutiveEmptyFail) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.max_retries = 1;
	cfg.max_empty_rounds = 2;
	guardrail_register_builtin_rules(&cfg);
	guardrail_rule_disable(&cfg, "empty_answer");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.empty_round_count = 3;
	ctx.max_empty_rounds = 3;
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "consecutive_empty");
	arena_destroy(a);
}

TEST(Guardrail, RuleLookupNull) {
	EXPECT_EQ(guardrail_rule_lookup(NULL, "test"), nullptr);
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	EXPECT_EQ(guardrail_rule_lookup(&cfg, "test"), nullptr);
}

TEST(Guardrail, HookIsolation) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_register_builtin_rules(&cfg);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_INPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, LlmRuleNoModelPasses) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	cfg.llm = NULL;
	guardrail_rule_register(&cfg, "llm_check", GUARDRAIL_HOOK_INPUT,
		GUARDRAIL_RULE_LLM, NULL, "Check for bad content", NULL, "Fix it.");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.user_input = "bad stuff";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_INPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, ExtRuleNoEntryPasses) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_rule_register(&cfg, "ext_check", GUARDRAIL_HOOK_OUTPUT,
		GUARDRAIL_RULE_EXT, NULL, NULL, "", "Fix it.");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "answer";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	arena_destroy(a);
}

TEST(Guardrail, SetLlm) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_set_llm(&cfg, NULL);
	EXPECT_EQ(cfg.llm, nullptr);
	struct model m;
	memset(&m, 0, sizeof(m));
	guardrail_set_llm(&cfg, &m);
	EXPECT_EQ(cfg.llm, &m);
}

TEST(Guardrail, BuiltinRulesAutoRegistered) {
	struct guardrail_config gcfg;
	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.enabled = 1;
	struct compress_config ccfg = {0};
	struct react_context *ctx = react_context_create(nullptr, nullptr, &ccfg, &gcfg);
	ASSERT_NE(ctx, nullptr);
	EXPECT_EQ(ctx->guardrail.rule_count, 6);
	EXPECT_NE(guardrail_rule_lookup(&ctx->guardrail, "empty_answer"), nullptr);
	EXPECT_NE(guardrail_rule_lookup(&ctx->guardrail, "final_local_file_missing"), nullptr);
	react_context_destroy(ctx);
}

static std::string build_agent_ui_tags_so()
{
	std::string src = std::string(MORPH_TEST_SOURCE_DIR) +
		"/exts/guardrail-agent-ui-tags/agent_ui_tags.c";
	std::string out = "/tmp/morph_agent_ui_tags_" +
		std::to_string((long long)getpid()) + ".so";
	std::string cmd = "cc -shared -fPIC -o " + out + " " + src;
	int status = system(cmd.c_str());
	if (status != 0)
		return "";
	return out;
}

static void register_agent_ui_tags_ext(struct guardrail_config *cfg,
				       const std::string &so_path)
{
	guardrail_rule_register(cfg, "guardrail-agent-ui-tags",
		GUARDRAIL_HOOK_OUTPUT, GUARDRAIL_RULE_EXT, NULL,
		"Validate supported Agent UI tags.", so_path.c_str(),
		"Regenerate using only supported Agent UI tags.");
	struct guardrail_rule *rule = guardrail_rule_lookup(cfg, "guardrail-agent-ui-tags");
	ASSERT_NE(rule, nullptr);
	rule->ext_type = GUARDRAIL_EXT_SO;
	ASSERT_EQ(guardrail_ext_so_load(rule), 0);
}

TEST(Guardrail, AgentUiTagsExtSupportedTagsPass) {
	std::string so = build_agent_ui_tags_so();
	ASSERT_FALSE(so.empty());
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	register_agent_ui_tags_ext(&cfg, so);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"<m:vocab word=\"access\" lang=\"en-US\">获取</m:vocab>\n"
		"<m:sentence lang=\"en-US\">I can access it.</m:sentence>\n"
		"<m:button label=\"继续\" action=\"practice.next\" />";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_PASS);
	guardrail_ext_so_unload(guardrail_rule_lookup(&cfg, "guardrail-agent-ui-tags"));
	arena_destroy(a);
	unlink(so.c_str());
}

TEST(Guardrail, AgentUiTagsExtUnsupportedTagFails) {
	std::string so = build_agent_ui_tags_so();
	ASSERT_FALSE(so.empty());
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	register_agent_ui_tags_ext(&cfg, so);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer =
		"<m:ask_user question=\"认识 access 吗?\" />\n"
		"<m:vocab word=\"access\">获取</m:vocab>";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "guardrail-agent-ui-tags");
	EXPECT_NE(strstr(r.reason, "ask_user"), nullptr);
	guardrail_ext_so_unload(guardrail_rule_lookup(&cfg, "guardrail-agent-ui-tags"));
	arena_destroy(a);
	unlink(so.c_str());
}

TEST(Guardrail, AgentUiTagsExtUnknownTagFails) {
	std::string so = build_agent_ui_tags_so();
	ASSERT_FALSE(so.empty());
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	register_agent_ui_tags_ext(&cfg, so);
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "Choose: <m:quiz id=\"q1\">...</m:quiz>";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	ASSERT_NE(r.triggered_rule, nullptr);
	EXPECT_STREQ(r.triggered_rule->name, "guardrail-agent-ui-tags");
	EXPECT_NE(strstr(r.reason, "quiz"), nullptr);
	guardrail_ext_so_unload(guardrail_rule_lookup(&cfg, "guardrail-agent-ui-tags"));
	arena_destroy(a);
	unlink(so.c_str());
}

static enum guardrail_verdict mock_so_check_fn(
	const struct guardrail_eval_ctx *ctx,
	char *reason_out, size_t reason_cap)
{
	if (ctx->proposed_answer && strstr(ctx->proposed_answer, "BLOCK"))
	{
		snprintf(reason_out, reason_cap, "Blocked by mock .so rule.");
		return GUARDRAIL_FAIL;
	}
	return GUARDRAIL_PASS;
}

TEST(Guardrail, ExtSoRuleWithCheckFn) {
	struct guardrail_config cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.enabled = 1;
	guardrail_rule_register(&cfg, "so_check", GUARDRAIL_HOOK_OUTPUT,
		GUARDRAIL_RULE_C, mock_so_check_fn, NULL, NULL, "Fix it.");
	struct arena *a = arena_create(4096);
	struct guardrail_eval_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.proposed_answer = "This is BLOCK content";
	ctx.arena = a;
	auto r = guardrail_run_hook(&cfg, GUARDRAIL_HOOK_OUTPUT, &ctx);
	EXPECT_EQ(r.verdict, GUARDRAIL_FAIL);
	EXPECT_STREQ(r.triggered_rule->name, "so_check");
	arena_destroy(a);
}

TEST(Guardrail, ExtSoLoadBadPathFails) {
	struct guardrail_rule rule;
	memset(&rule, 0, sizeof(rule));
	strncpy(rule.name, "bad_so", sizeof(rule.name) - 1);
	strncpy(rule.ext_entry, "/nonexistent/path.so", sizeof(rule.ext_entry) - 1);
	int rc = guardrail_ext_so_load(&rule);
	EXPECT_NE(rc, 0);
	EXPECT_EQ(rule.dl_handle, nullptr);
	EXPECT_EQ(rule.ext_check, nullptr);
}

TEST(Guardrail, ExtSoUnloadNullSafe) {
	guardrail_ext_so_unload(nullptr);
	struct guardrail_rule rule;
	memset(&rule, 0, sizeof(rule));
	guardrail_ext_so_unload(&rule);
}
