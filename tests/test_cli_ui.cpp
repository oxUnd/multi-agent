#include <gtest/gtest.h>

extern "C" {
#include "sapi/cli/cli.h"
#include "sapi/cli/interaction.h"
#include "sapi/cli/command_job.h"
#include "sapi/cli/terminal.h"
#include "sapi/cli/ui_event.h"
#include "agent/tool_context.h"
#include "event/event.h"

int cli_event_callback(const struct morph_event *event, void *user_data);
int cli_presentation_init(struct cli_context *ctx);
void cli_presentation_cleanup(struct cli_context *ctx);
enum hitl_verdict hitl_approval_callback(const char *tool_name,
					 const char *tool_args,
					 void *user_data);
int cli_ask_user_callback(const char *question,
			  const char *const *choices,
			  int choices_count,
			  const char *selection_mode,
			  int min_choices,
			  int max_choices,
			  char ***answers,
			  int *answers_count,
			  void *user_data);
enum tool_operation_verdict operation_approval_callback(
	const struct tool_operation *op, void *user_data);
}

#include <poll.h>
#include <cerrno>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class CliUiTest : public ::testing::Test {
protected:
	struct cli_context ctx{};

	void SetUp() override
	{
		cli_set_color_enabled(0);
		ctx.presentation_mode = CLI_PRESENT_INTERACTIVE;
		ctx.presentation_ready = 1;
		ctx.turn_active = 1;
		ASSERT_EQ(cli_terminal_init(&ctx, stdout, STDOUT_FILENO), 0);
		ASSERT_EQ(cli_presentation_init(&ctx), 0);
		ASSERT_EQ(cli_ui_init(&ctx), 0);
	}

	void TearDown() override
	{
		cli_ui_cleanup(&ctx);
		cli_presentation_cleanup(&ctx);
		cli_terminal_cleanup(&ctx);
		cli_set_color_enabled(1);
	}
};

struct cli_owner_call_data {
	struct cli_context *ctx;
	int ran_on_owner;
};

static int cli_owner_call(void *opaque)
{
	cli_owner_call_data *data =
		static_cast<cli_owner_call_data *>(opaque);

	data->ran_on_owner = cli_ui_is_owner(data->ctx);
	return 73;
}

static int cli_command_with_owner_call(struct cli_context *ctx,
			       const char *input, void *opaque)
{
	(void)input;
	return cli_ui_call_owner(ctx, cli_owner_call, opaque);
}

TEST_F(CliUiTest, CrossThreadEventWaitsForOwnerDrain)
{
	cJSON *data = cJSON_CreateObject();
	ASSERT_NE(data, nullptr);
	cJSON_AddStringToObject(data, "detail", "owned copy");
	struct morph_event event{
		MORPH_EVENT_ERROR,
		"background.error",
		"end",
		"queued error",
		data,
		"turn-ui",
	};
	int callback_rc = -1;

	testing::internal::CaptureStdout();
	std::thread worker([&] {
		callback_rc = cli_event_callback(&event, &ctx);
	});
	worker.join();
	EXPECT_EQ(callback_rc, 0);
	EXPECT_TRUE(testing::internal::GetCapturedStdout().empty());
	cJSON_Delete(data);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("queued error"), std::string::npos);
}

TEST_F(CliUiTest, WakePipeSignalsQueuedWorkAndDrains)
{
	struct notification notification{};
	notification.id = 42;
	std::snprintf(notification.title, sizeof(notification.title),
		      "nightly task");
	notification.body = const_cast<char *>("finished safely");
	int callback_rc = -1;

	std::thread worker([&] {
		callback_rc = cli_ui_post_notification(&ctx, &notification);
	});
	worker.join();
	ASSERT_EQ(callback_rc, 0);

	struct pollfd fd{};
	fd.fd = cli_ui_wake_fd(&ctx);
	fd.events = POLLIN;
	ASSERT_EQ(poll(&fd, 1, 0), 1);
	EXPECT_NE(fd.revents & POLLIN, 0);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("nightly task"), std::string::npos);
	EXPECT_NE(output.find("finished safely"), std::string::npos);
	EXPECT_NE(output.find("#42"), std::string::npos);

	fd.revents = 0;
	EXPECT_EQ(poll(&fd, 1, 0), 0);
}

TEST_F(CliUiTest, OwnerThreadEventsRemainImmediate)
{
	struct morph_event event{
		MORPH_EVENT_ERROR,
		"owner.error",
		"end",
		"immediate error",
		nullptr,
		"turn-ui",
	};

	ASSERT_TRUE(cli_ui_is_owner(&ctx));
	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_event_callback(&event, &ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	EXPECT_NE(output.find("immediate error"), std::string::npos);
}

TEST_F(CliUiTest, QueuedEventsRenderBeforeFollowingOwnerEvent)
{
	struct morph_event queued_event{
		MORPH_EVENT_ERROR,
		"background.error",
		"end",
		"first queued error",
		nullptr,
		"turn-ui",
	};
	struct morph_event owner_event{
		MORPH_EVENT_ERROR,
		"owner.error",
		"end",
		"second owner error",
		nullptr,
		"turn-ui",
	};
	int callback_rc = -1;

	std::thread worker([&] {
		callback_rc = cli_event_callback(&queued_event, &ctx);
	});
	worker.join();
	ASSERT_EQ(callback_rc, 0);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_event_callback(&owner_event, &ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	size_t queued_offset = output.find("first queued error");
	size_t owner_offset = output.find("second owner error");
	ASSERT_NE(queued_offset, std::string::npos);
	ASSERT_NE(owner_offset, std::string::npos);
	EXPECT_LT(queued_offset, owner_offset);
}

TEST_F(CliUiTest, ConcurrentProducersPreserveEveryNotification)
{
	constexpr int worker_count = 4;
	constexpr int events_per_worker = 12;
	std::vector<std::thread> workers;
	std::vector<int> results(worker_count, 0);

	for (int worker = 0; worker < worker_count; worker++) {
		workers.emplace_back([&, worker] {
			for (int index = 0; index < events_per_worker; index++) {
				struct notification notification{};

				notification.id = worker * events_per_worker + index;
				std::snprintf(notification.title,
					      sizeof(notification.title),
					      "queued-item-%d-%d", worker, index);
				int rc = cli_ui_post_notification(&ctx, &notification);
				if (rc != 0)
					results[worker] = rc;
			}
		});
	}
	for (auto &worker : workers)
		worker.join();
	for (int rc : results)
		ASSERT_EQ(rc, 0);

	testing::internal::CaptureStdout();
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	std::string output = testing::internal::GetCapturedStdout();
	int found = 0;
	size_t offset = 0;
	while ((offset = output.find("queued-item-", offset)) !=
	       std::string::npos) {
		found++;
		offset += sizeof("queued-item-") - 1;
	}
	EXPECT_EQ(found, worker_count * events_per_worker);
}

TEST_F(CliUiTest, NonOwnerCannotDrainPresentationState)
{
	int drain_rc = 0;
	std::thread worker([&] {
		drain_rc = cli_ui_drain(&ctx);
	});
	worker.join();
	EXPECT_EQ(drain_rc, -EPERM);
}

TEST_F(CliUiTest, SynchronousOwnerCallRunsOnUiThread)
{
	cli_owner_call_data data{&ctx, 0};
	int call_rc = 0;
	std::thread worker([&] {
		call_rc = cli_ui_call_owner(&ctx, cli_owner_call, &data);
	});

	struct pollfd fd{};
	fd.fd = cli_ui_wake_fd(&ctx);
	fd.events = POLLIN;
	ASSERT_EQ(poll(&fd, 1, 1000), 1);
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	worker.join();
	EXPECT_EQ(call_rc, 73);
	EXPECT_EQ(data.ran_on_owner, 1);
}

TEST_F(CliUiTest, CommandWaitPumpsSynchronousOwnerCall)
{
	struct cli_command_job job{};
	cli_owner_call_data data{&ctx, 0};

	ASSERT_EQ(cli_command_job_init(&job), 0);
	ASSERT_EQ(cli_command_job_start_fn(&job, &ctx, "test",
		cli_command_with_owner_call, &data), 0);
	EXPECT_EQ(cli_command_job_wait(&job), 73);
	EXPECT_EQ(data.ran_on_owner, 1);
	cli_command_job_cleanup(&job);
}

TEST_F(CliUiTest, PendingOwnerCallSurvivesConsumedWakeSignal)
{
	cli_owner_call_data data{&ctx, 0};
	int call_rc = 0;
	std::thread worker([&] {
		call_rc = cli_ui_call_owner(&ctx, cli_owner_call, &data);
	});

	struct pollfd fd{};
	fd.fd = cli_ui_wake_fd(&ctx);
	fd.events = POLLIN;
	ASSERT_EQ(poll(&fd, 1, 1000), 1);
	unsigned char wake_byte = 0;
	ASSERT_EQ(read(fd.fd, &wake_byte, sizeof(wake_byte)), 1);
	fd.revents = 0;
	EXPECT_EQ(poll(&fd, 1, 0), 0);
	ASSERT_EQ(cli_ui_drain(&ctx), 0);
	worker.join();
	EXPECT_EQ(call_rc, 73);
	EXPECT_EQ(data.ran_on_owner, 1);
}

static std::string run_structured_interaction(
	struct cli_context *ctx, const std::string &input,
	const std::function<void()> &worker_fn)
{
	int input_pipe[2];
	int saved_stdin;

	if (pipe(input_pipe) != 0)
		return {};
	saved_stdin = dup(STDIN_FILENO);
	if (saved_stdin < 0) {
		close(input_pipe[0]);
		close(input_pipe[1]);
		return {};
	}
	if (write(input_pipe[1], input.data(), input.size()) !=
	    static_cast<ssize_t>(input.size())) {
		close(input_pipe[0]);
		close(input_pipe[1]);
		close(saved_stdin);
		return {};
	}
	close(input_pipe[1]);
	if (dup2(input_pipe[0], STDIN_FILENO) < 0) {
		close(input_pipe[0]);
		close(saved_stdin);
		return {};
	}
	close(input_pipe[0]);
	clearerr(stdin);

	testing::internal::CaptureStdout();
	std::thread worker(worker_fn);
	struct pollfd fd{};
	fd.fd = cli_ui_wake_fd(ctx);
	fd.events = POLLIN;
	(void)poll(&fd, 1, 1000);
	(void)cli_ui_drain(ctx);
	worker.join();
	std::string output = testing::internal::GetCapturedStdout();

	(void)dup2(saved_stdin, STDIN_FILENO);
	close(saved_stdin);
	clearerr(stdin);
	return output;
}

static cJSON *find_json_event(const std::string &output, const char *name)
{
	size_t offset = 0;

	while (offset < output.size()) {
		size_t end = output.find('\n', offset);
		std::string line = output.substr(
			offset, end == std::string::npos ? end : end - offset);
		cJSON *event = cJSON_Parse(line.c_str());

		if (event) {
			cJSON *event_name = cJSON_GetObjectItemCaseSensitive(
				event, "name");
			if (cJSON_IsString(event_name) && event_name->valuestring &&
			    std::strcmp(event_name->valuestring, name) == 0)
				return event;
			cJSON_Delete(event);
		}
		if (end == std::string::npos)
			break;
		offset = end + 1;
	}
	return nullptr;
}

TEST_F(CliUiTest, StructuredInteractionRejectsMismatchedResponseThenResolves)
{
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	ctx.event_cb = cli_event_callback;
	ctx.event_user_data = &ctx;
	cJSON *request = cJSON_CreateObject();
	cJSON *response = nullptr;
	int call_rc = -1;
	ASSERT_NE(request, nullptr);
	cJSON_AddStringToObject(request, "prompt", "continue?");
	std::string input =
		"{\"type\":\"interaction.response\",\"request_id\":"
		"\"wrong\",\"data\":{\"answer\":\"yes\"}}\n"
		"{\"type\":\"interaction.response\",\"request_id\":"
		"\"interaction-1\",\"data\":{\"answer\":\"yes\"}}\n";

	std::string output = run_structured_interaction(&ctx, input, [&] {
		call_rc = cli_interaction_request(&ctx, "test", request,
			nullptr, nullptr, &response);
	});
	EXPECT_EQ(call_rc, 0);
	ASSERT_NE(response, nullptr);
	EXPECT_STREQ(cJSON_GetObjectItem(response, "answer")->valuestring,
		     "yes");
	EXPECT_NE(output.find("interaction.invalid_response"),
		  std::string::npos);
	EXPECT_NE(output.find("interaction.resolved"), std::string::npos);
	cJSON_Delete(response);
	cJSON_Delete(request);
}

TEST_F(CliUiTest, StructuredAskUserReturnsValidatedAnswers)
{
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	ctx.event_cb = cli_event_callback;
	ctx.event_user_data = &ctx;
	const char *choices[] = {"red", "blue"};
	char **answers = nullptr;
	int answers_count = 0;
	int call_rc = -1;
	std::string input =
		"{\"type\":\"interaction.response\",\"request_id\":"
		"\"interaction-1\",\"data\":{\"answers\":[\"blue\"]}}\n";

	std::string output = run_structured_interaction(&ctx, input, [&] {
		call_rc = cli_ask_user_callback("pick a color", choices, 2,
			"single", 1, 1, &answers, &answers_count, &ctx);
	});
	EXPECT_EQ(call_rc, 0);
	ASSERT_EQ(answers_count, 1);
	EXPECT_STREQ(answers[0], "blue");
	cJSON *event = find_json_event(output, "interaction.request");
	ASSERT_NE(event, nullptr);
	cJSON *data = cJSON_GetObjectItem(event, "data");
	EXPECT_STREQ(cJSON_GetObjectItem(data, "kind")->valuestring,
		     "ask_user");
	cJSON *payload = cJSON_GetObjectItem(data, "request");
	EXPECT_STREQ(cJSON_GetObjectItem(payload, "question")->valuestring,
		     "pick a color");
	cJSON_Delete(event);
	for (int i = 0; i < answers_count; i++)
		free(answers[i]);
	free(answers);
}

TEST_F(CliUiTest, StructuredOperationApprovalMapsSessionDecision)
{
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	ctx.event_cb = cli_event_callback;
	ctx.event_user_data = &ctx;
	struct tool_directory_capability directory{};
	std::snprintf(directory.path, sizeof(directory.path), "/tmp/output");
	directory.create = 1;
	struct tool_operation operation{
		TOOL_OP_COMMAND,
		"bash_exec",
		"agent",
		"make test",
		nullptr,
		"/workspace",
		"{}",
		&directory,
		1,
	};
	enum tool_operation_verdict verdict = TOOL_OP_DENY;
	std::string input =
		"{\"type\":\"interaction.response\",\"request_id\":"
		"\"interaction-1\",\"data\":{\"decision\":\"session\"}}\n";

	std::string output = run_structured_interaction(&ctx, input, [&] {
		verdict = operation_approval_callback(&operation, &ctx);
	});
	EXPECT_EQ(verdict, TOOL_OP_SESSION);
	cJSON *event = find_json_event(output, "interaction.request");
	ASSERT_NE(event, nullptr);
	cJSON *data = cJSON_GetObjectItem(event, "data");
	EXPECT_STREQ(cJSON_GetObjectItem(data, "kind")->valuestring,
		     "operation_approval");
	cJSON *payload = cJSON_GetObjectItem(data, "request");
	EXPECT_STREQ(cJSON_GetObjectItem(payload, "operation")->valuestring,
		     "command");
	EXPECT_EQ(cJSON_GetArraySize(
		cJSON_GetObjectItem(payload, "directories")), 1);
	cJSON_Delete(event);
}

TEST_F(CliUiTest, StructuredToolApprovalMapsPersistentDecision)
{
	ctx.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	ctx.event_cb = cli_event_callback;
	ctx.event_user_data = &ctx;
	enum hitl_verdict verdict = HITL_DENY;
	std::string input =
		"{\"type\":\"interaction.response\",\"request_id\":"
		"\"interaction-1\",\"data\":{\"decision\":\"always\"}}\n";

	std::string output = run_structured_interaction(&ctx, input, [&] {
		verdict = hitl_approval_callback(
			"remote_tool", "{\"query\":\"status\"}", &ctx);
	});
	EXPECT_EQ(verdict, HITL_ALWAYS);
	cJSON *event = find_json_event(output, "interaction.request");
	ASSERT_NE(event, nullptr);
	cJSON *data = cJSON_GetObjectItem(event, "data");
	EXPECT_STREQ(cJSON_GetObjectItem(data, "kind")->valuestring,
		     "tool_approval");
	cJSON *payload = cJSON_GetObjectItem(data, "request");
	EXPECT_STREQ(cJSON_GetObjectItem(payload, "tool_name")->valuestring,
		     "remote_tool");
	EXPECT_EQ(cJSON_GetArraySize(
		cJSON_GetObjectItem(payload, "allowed_decisions")), 3);
	cJSON_Delete(event);
}

TEST(CliCommandPrompts, PreservesOrderPayloadLifetimeAndCompletionTail)
{
	struct cli_command_job job;
	struct react_action action;
	ASSERT_EQ(cli_command_job_init(&job), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "改成中文🙂"), 0);
	ASSERT_EQ(cli_command_job_prompt(&job, "second\nline"), 0);
	EXPECT_EQ(cli_command_job_prompt_pending(&job), 1);
	ASSERT_EQ(cli_command_job_drain(&job, &action, 0), 1);
	EXPECT_STREQ(action.type, "prompt");
	std::string delivered = action.payload_json;
	const char *borrowed = action.payload_json;
	/* Grow the queue while the consumer still owns the previous payload. */
	for (int i = 0; i < 100; i++)
		ASSERT_EQ(cli_command_job_prompt(&job, "tail"), 0);
	EXPECT_EQ(delivered, borrowed);
	char *pending = cli_command_job_take_prompt(&job);
	ASSERT_NE(pending, nullptr);
	EXPECT_STREQ(pending, "second\nline");
	free(pending);
	for (int i = 0; i < 100; i++) {
		pending = cli_command_job_take_prompt(&job);
		ASSERT_NE(pending, nullptr);
		EXPECT_STREQ(pending, "tail");
		free(pending);
	}
	EXPECT_EQ(cli_command_job_prompt_pending(&job), 0);
	EXPECT_EQ(cli_command_job_take_prompt(&job), nullptr);
	cli_command_job_cleanup(&job);
}
