#include <gtest/gtest.h>

extern "C" {
#include "runtime/runtime.h"
#include "sapi/cli/cli.h"
#include "sapi/cli/commands/registry.h"
#include "sapi/cli/list_ui.h"
#include "sapi/cli/composer.h"
#include "util/file.h"
#include "event/event.h"

int cli_handle_media_path(struct cli_context *ctx, const char *input,
			  int *handled);
int cli_event_callback(const struct morph_event *event, void *user_data);
}

#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

static std::vector<cJSON *> cli_test_parse_ndjson(const std::string &output)
{
	std::vector<cJSON *> events;
	size_t start = 0;

	while (start < output.size()) {
		size_t end = output.find('\n', start);
		std::string line = output.substr(start,
			end == std::string::npos ? std::string::npos : end - start);

		if (!line.empty())
			events.push_back(cJSON_Parse(line.c_str()));
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return events;
}

static void cli_test_delete_events(std::vector<cJSON *> &events)
{
	for (cJSON *event : events)
		cJSON_Delete(event);
	events.clear();
}

static int cli_test_write_png(const char *path)
{
	static const unsigned char png[] = {
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
		0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
		0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08,
		0x08, 0x02, 0x00, 0x00, 0x00, 0x4b, 0x6d, 0x6b,
		0xc4, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41,
		0x54, 0x18, 0xd3, 0x63, 0xf8, 0xcf, 0xc0, 0x80,
		0x15, 0x71, 0xd1, 0x41, 0x2b, 0x00, 0x28, 0x3f,
		0x4f, 0xc1, 0x6e, 0xec, 0xdf, 0x61, 0x00, 0x00,
		0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42,
		0x60, 0x82
	};
	FILE *file = std::fopen(path, "wb");

	if (!file)
		return -1;
	size_t written = std::fwrite(png, 1, sizeof(png), file);
	std::fclose(file);
	return written == sizeof(png) ? 0 : -1;
}

TEST(CliMediaInputTest, RemovesLegacyMediaCommands)
{
	cli_command_registry_clear();
	ASSERT_EQ(cli_register_media_commands(), 0);
	EXPECT_EQ(cli_command_find("/img"), nullptr);
	EXPECT_EQ(cli_command_find("/paste"), nullptr);
	EXPECT_EQ(cli_command_find("/video"), nullptr);
	EXPECT_EQ(cli_command_find("/vid"), nullptr);
	EXPECT_NE(cli_command_find("/image"), nullptr);
	cli_command_registry_clear();
}

TEST(CliMediaInputTest, QuotedImagePathAttachesWithoutCommand)
{
	char pattern[] = "/tmp/morph-cli-media-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	std::string database;
	std::string image;
	std::string input;
	int handled = 0;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	image = std::string(directory) + "/sample image.png";
	input = "\"" + image + "\"";
	ASSERT_EQ(cli_test_write_png(image.c_str()), 0);
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;

	testing::internal::CaptureStdout();
	int rc = cli_handle_media_path(&context, input.c_str(), &handled);
	(void)testing::internal::GetCapturedStdout();
	EXPECT_EQ(rc, 0);
	EXPECT_EQ(handled, 1);
	EXPECT_STREQ(context.image_path, image.c_str());

	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, CompactsWhitespaceWithoutBreakingUtf8)
{
	char output[128];

	ASSERT_GT(cli_list_compact_text(
		output, sizeof(output), "  飞书\n\tMarkdown   renderer  "), 0u);
	EXPECT_STREQ(output, "飞书 Markdown renderer");
}

TEST(CliListTest, CompactionRemovesTerminalControlSequences)
{
	char output[128];

	ASSERT_GT(cli_list_compact_text(output, sizeof(output),
		"safe\033[31mred\033[0m\033]52;c;secret\a end"), 0u);
	EXPECT_STREQ(output, "safered end");
	EXPECT_EQ(std::strchr(output, '\033'), nullptr);
}

TEST(CliListTest, UsesConfiguredColumnsWhenStdoutIsNotTerminal)
{
	const char *previous = std::getenv("COLUMNS");
	std::string saved = previous ? previous : "";
	bool had_previous = previous != nullptr;

	ASSERT_EQ(setenv("COLUMNS", "72", 1), 0);
	EXPECT_EQ(cli_list_columns(), 72);
	if (had_previous)
		ASSERT_EQ(setenv("COLUMNS", saved.c_str(), 1), 0);
	else
		ASSERT_EQ(unsetenv("COLUMNS"), 0);
}

TEST(CliListTest, LongItemUsesCompactContinuation)
{
	testing::internal::CaptureStdout();
	cli_list_item("", 1, nullptr, "a_very_long_registered_tool_name",
		      "First line\nSecond line with more details", 12, 40);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("a_very_long_registered_tool_name"),
		  std::string::npos);
	EXPECT_NE(output.find("First line Second line"), std::string::npos);
	EXPECT_NE(output.find("…"), std::string::npos);
}

TEST(CliListTest, JsonFieldRendersNestedTree)
{
	testing::internal::CaptureStdout();
	cli_list_json_field("Input schema",
			    "{\"type\":\"object\",\"properties\":{"
			    "\"query\":{\"type\":\"string\"}}}", 1, 80);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Input schema"), std::string::npos);
	EXPECT_NE(output.find("properties"), std::string::npos);
	EXPECT_NE(output.find("query"), std::string::npos);
	EXPECT_NE(output.find("string"), std::string::npos);
}

TEST(CliListTest, SessionRowKeepsRelatedFieldsTogether)
{
	testing::internal::CaptureStdout();
	cli_list_row("ae2c6f9a", "查一下开放平台、前端架构绩效",
		     "3,086 tokens · 07-29 15:49", 0, 0, 80);
	cli_list_row("a6f7b14c", "default", "0 tokens · 07-29 15:52",
		     1, 1, 80);
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("├"), std::string::npos);
	EXPECT_NE(output.find("└"), std::string::npos);
	EXPECT_NE(output.find("ae2c6f9a"), std::string::npos);
	EXPECT_NE(output.find("3,086 tokens"), std::string::npos);
	EXPECT_NE(output.find("07-29 15:49"), std::string::npos);
	EXPECT_NE(output.find("●"), std::string::npos);
	EXPECT_EQ(output.find("current"), std::string::npos);
	EXPECT_EQ(output.find("Model"), std::string::npos);
}

TEST(CliListTest, SessionCommandShowsUpdatedTimeWithoutCurrentLabel)
{
	char pattern[] = "/tmp/morph-cli-list-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	std::string database;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	cli_command_registry_clear();
	ASSERT_EQ(cli_register_session_commands(), 0);

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/list");
	std::string output = testing::internal::GetCapturedStdout();

	ASSERT_EQ(rc, 0);
	EXPECT_TRUE(std::regex_search(
		output, std::regex("[0-9]{2}-[0-9]{2} "
				   "[0-9]{2}:[0-9]{2}")));
	EXPECT_NE(output.find("●"), std::string::npos);
	EXPECT_EQ(output.find("current"), std::string::npos);

	cli_command_registry_clear();
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliListTest, HelpUsesTreeLayout)
{
	ASSERT_EQ(cli_commands_init(), 0);

	testing::internal::CaptureStdout();
	cli_print_help();
	std::string output = testing::internal::GetCapturedStdout();

	EXPECT_NE(output.find("Commands"), std::string::npos);
	EXPECT_NE(output.find("├"), std::string::npos);
	EXPECT_NE(output.find("└"), std::string::npos);
	EXPECT_NE(output.find("Core"), std::string::npos);
	EXPECT_NE(output.find("/help"), std::string::npos);
	cli_command_registry_clear();
}

TEST(CliListTest, JsonModeWrapsHelpAsCommandEvents)
{
	struct cli_context context{};

	context.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	context.event_cb = cli_event_callback;
	context.event_user_data = &context;
	ASSERT_EQ(cli_commands_init(), 0);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/help");
	std::string output = testing::internal::GetCapturedStdout();
	std::vector<cJSON *> events = cli_test_parse_ndjson(output);

	ASSERT_EQ(rc, 0);
	ASSERT_EQ(events.size(), 2u);
	ASSERT_NE(events[0], nullptr);
	ASSERT_NE(events[1], nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[0], "type")), "command");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[0], "name")), "command.started");
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[1], "name")), "command.completed");
	cJSON *data = cJSON_GetObjectItem(events[1], "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	const char *captured = cJSON_GetStringValue(
		cJSON_GetObjectItem(data, "output"));
	ASSERT_NE(captured, nullptr);
	EXPECT_NE(std::string(captured).find("Commands"), std::string::npos);
	EXPECT_EQ(output.find("\033"), std::string::npos);

	cli_test_delete_events(events);
	cli_command_registry_clear();
}

TEST(CliListTest, JsonModeReportsUnknownCommandWithoutBareText)
{
	struct cli_context context{};

	context.presentation_mode = CLI_PRESENT_EVENTS_JSON;
	context.event_cb = cli_event_callback;
	context.event_user_data = &context;
	cli_command_registry_clear();
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/does-not-exist");
	std::string output = testing::internal::GetCapturedStdout();
	std::vector<cJSON *> events = cli_test_parse_ndjson(output);

	ASSERT_EQ(rc, -ENOENT);
	ASSERT_EQ(events.size(), 2u);
	ASSERT_NE(events[0], nullptr);
	ASSERT_NE(events[1], nullptr);
	EXPECT_STREQ(cJSON_GetStringValue(cJSON_GetObjectItem(
		events[1], "name")), "command.failed");
	cJSON *data = cJSON_GetObjectItem(events[1], "data");
	ASSERT_TRUE(cJSON_IsObject(data));
	EXPECT_EQ(cJSON_GetNumberValue(cJSON_GetObjectItem(
		data, "error_code")), -ENOENT);
	const char *captured = cJSON_GetStringValue(
		cJSON_GetObjectItem(data, "output"));
	ASSERT_NE(captured, nullptr);
	EXPECT_NE(std::string(captured).find("unknown command"),
		  std::string::npos);

	cli_test_delete_events(events);
}

TEST(CliListTest, SyncQueriesUseGroupedTrees)
{
	char pattern[] = "/tmp/morph-cli-sync-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	struct morph_sync_backup *backups = nullptr;
	int backup_count = 0;
	std::string root;
	std::string database;
	std::string config_path;
	std::string remote;
	std::ofstream config;
	std::ofstream payload;

	ASSERT_NE(directory, nullptr);
	root = directory;
	database = root + "/data.db";
	config_path = root + "/config.toml";
	remote = root + "/remote";
	config.open(config_path);
	ASSERT_TRUE(config.is_open());
	config << "[sync]\n"
	       << "enabled = true\n"
	       << "dir = \"" << remote << "\"\n"
	       << "interval_seconds = 0\n"
	       << "retention_days = 30\n"
	       << "include = [\"data.db\", \"payload.txt\"]\n";
	config.close();
	ASSERT_TRUE(config.good());
	payload.open(root + "/payload.txt");
	payload << "first";
	payload.close();
	ASSERT_TRUE(payload.good());
	options.db_path = database.c_str();
	options.config_path = config_path.c_str();
	options.workdir = root.c_str();
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	context.runtime = runtime;
	cli_command_registry_clear();
	ASSERT_EQ(cli_register_sync_commands(), 0);

	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/sync now");
	std::string status_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(status_output.find("sync status"), std::string::npos);
	EXPECT_NE(status_output.find("state"), std::string::npos);
	EXPECT_NE(status_output.find("files"), std::string::npos);
	EXPECT_NE(status_output.find("database"), std::string::npos);
	EXPECT_NE(status_output.find("├"), std::string::npos);
	EXPECT_NE(status_output.find("└"), std::string::npos);
	EXPECT_NE(status_output.find("snapshots"), std::string::npos);

	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync conflicts");
	std::string conflict_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(conflict_output.find("sync conflicts (0)"), std::string::npos);
	EXPECT_NE(conflict_output.find("└ none"), std::string::npos);
	payload.open(root + "/payload.txt", std::ios::trunc);
	payload << "local change";
	payload.close();
	payload.open(remote + "/data/payload.txt", std::ios::trunc);
	payload << "remote change";
	payload.close();
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync now");
	(void)testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync conflicts");
	conflict_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(conflict_output.find("sync conflicts (1)"), std::string::npos);
	EXPECT_NE(conflict_output.find("#1"), std::string::npos);
	EXPECT_NE(conflict_output.find("payload.txt"), std::string::npos);

	ASSERT_EQ(runtime_sync_backups(runtime, "data.db", &backups,
		&backup_count), 0);
	ASSERT_EQ(backup_count, 1);
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, "/sync backups");
	std::string backup_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_NE(backup_output.find("database backups (1)"),
		  std::string::npos);
	EXPECT_NE(backup_output.find("data.db"), std::string::npos);
	EXPECT_NE(backup_output.find(backups[0].snapshot_id), std::string::npos);
	EXPECT_NE(backup_output.find("created"), std::string::npos);
	std::string restore_command = "/sync restore-db " +
		std::string(backups[0].snapshot_id) + " --yes";
	testing::internal::CaptureStdout();
	rc = cli_command_dispatch(&context, restore_command.c_str());
	std::string restore_output = testing::internal::GetCapturedStdout();
	ASSERT_EQ(rc, 0);
	EXPECT_EQ(context.pending_db_restore, 1);
	EXPECT_EQ(context.running, 0);
	EXPECT_NE(restore_output.find("restore prepared"), std::string::npos);
	morph_sync_backups_free(backups);
	runtime_close(runtime);
	runtime = nullptr;
	ASSERT_EQ(morph_sync_apply_db_replace(&context.db_restore_plan), 0);
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	EXPECT_FALSE(file_exists(context.db_restore_plan.journal));

	cli_command_registry_clear();
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(root, error);
}

TEST(CliListTest, TasksCommandUsesActiveAndCompactHistoryTrees)
{
	char pattern[] = "/tmp/morph-cli-tasks-XXXXXX";
	char *directory = mkdtemp(pattern);
	struct runtime_options options{};
	struct runtime *runtime = nullptr;
	struct cli_context context{};
	struct session current{};
	struct scheduled_task_input input{};
	struct scheduled_task active{};
	struct scheduled_task history{};
	std::string database;

	ASSERT_NE(directory, nullptr);
	database = std::string(directory) + "/data.db";
	options.db_path = database.c_str();
	options.workdir = directory;
	options.front_name = "test";
	ASSERT_EQ(runtime_open(&options, &runtime), 0);
	ASSERT_EQ(runtime_session_current(runtime, &current), 0);
	context.runtime = runtime;
	input.source_session_id = current.id;
	input.kind = "agent";
	input.trigger_type = "once";
	input.next_run_at = std::time(nullptr) + 3600;
	input.max_attempts = 3;
	input.action_type = "agent_run";
	input.policy_json = "{}";
	input.notify_json = "{}";
	input.title = "active task";
	input.payload_json = "{\"prompt\":\"active task details\"}";
	ASSERT_EQ(runtime_task_create(runtime, &input, &active), 0);
	input.title = "old task";
	input.payload_json = "{\"prompt\":\"old task details\"}";
	ASSERT_EQ(runtime_task_create(runtime, &input, &history), 0);
	ASSERT_EQ(runtime_task_cancel(runtime, history.id), 0);

	cli_command_registry_clear();
	ASSERT_EQ(cli_register_task_commands(), 0);
	testing::internal::CaptureStdout();
	int rc = cli_command_dispatch(&context, "/tasks");
	std::string output = testing::internal::GetCapturedStdout();

	ASSERT_EQ(rc, 0);
	EXPECT_NE(output.find("scheduled tasks (2)"), std::string::npos);
	EXPECT_NE(output.find("active"), std::string::npos);
	EXPECT_NE(output.find("history"), std::string::npos);
	EXPECT_NE(output.find("active task details"), std::string::npos);
	EXPECT_NE(output.find("old task"), std::string::npos);
	EXPECT_NE(output.find("cancelled"), std::string::npos);
	EXPECT_EQ(output.find("| ID |"), std::string::npos);

	scheduled_task_cleanup(&active);
	scheduled_task_cleanup(&history);
	cli_command_registry_clear();
	runtime_close(runtime);
	std::error_code error;
	(void)std::filesystem::remove_all(directory, error);
}

TEST(CliMediaInputTest, ComposerKeepsImageReferencesAndAtomicBoundaries)
{
	char pattern[] = "/tmp/morph-composer-XXXXXX";
	char *directory = mkdtemp(pattern);
	ASSERT_NE(directory, nullptr);
	std::string path = std::string(directory) + "/目录 image.png";
	ASSERT_EQ(cli_test_write_png(path.c_str()), 0);
	struct cli_composer composer;
	ASSERT_EQ(cli_composer_init(&composer), 0);
	const char *label;
	ASSERT_EQ(cli_composer_add_image(&composer, path.c_str(), &label), 0);
	EXPECT_STREQ(label, "[IMAGE#1]");
	ASSERT_EQ(cli_composer_add_image(&composer, path.c_str(), &label), 0);
	EXPECT_STREQ(label, "[IMAGE#2]");
	char *expanded = cli_composer_expand(&composer, "只看 [IMAGE#2]");
	ASSERT_NE(expanded, nullptr);
	EXPECT_NE(std::string(expanded).find(path), std::string::npos);
	EXPECT_EQ(std::string(expanded).find("IMAGE#1"), std::string::npos);
	free(expanded);
	int start;
	int end;
	ASSERT_EQ(cli_composer_image_span(&composer, "a[IMAGE#2]b", 10, 1,
					  &start, &end), 1);
	EXPECT_EQ(start, 1);
	EXPECT_EQ(end, 10);
	EXPECT_EQ(cli_composer_image_span(&composer, "a[IMAGE#2]b", 10, 0,
					 &start, &end), 0);
	std::string escaped = path;
	escaped.replace(escaped.find(' '), 1, "\\ ");
	std::string input = "before\n\t\"" + path + "\"  " + escaped + "\nafter";
	char *converted = nullptr;
	ASSERT_EQ(cli_composer_convert_paths(&composer, input.c_str(), &converted), 2);
	ASSERT_NE(converted, nullptr);
	EXPECT_STREQ(converted, "before\n\t[IMAGE#3]  [IMAGE#4]\nafter");
	free(converted);
	cli_composer_cleanup(&composer);
	std::filesystem::remove_all(directory);
}
