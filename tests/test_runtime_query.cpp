#include <gtest/gtest.h>
#include "agent/tool.h"
#include "agent/tool_runtime.h"
#include "agent/tools/runtime_query.h"
#include "agent/memory.h"
#include "credits.h"
#include "session.h"
#include "cJSON.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

static int CountOccurrences(const char *text, const char *needle)
{
	int count = 0;
	size_t needle_len;

	if (!text || !needle || !*needle)
		return 0;
	needle_len = std::strlen(needle);
	while ((text = std::strstr(text, needle)) != nullptr) {
		count++;
		text += needle_len;
	}
	return count;
}

class RuntimeQueryTest : public ::testing::Test {
protected:
	struct db db;
	struct config cfg;
	struct tool_registry tools;
	char db_path[256];

	void SetUp() override {
		memset(&db, 0, sizeof(db));
		config_set_defaults(&cfg);
		tool_registry_init(&tools);
		snprintf(db_path, sizeof(db_path),
			 "/tmp/morph_runtime_query_%d.db", getpid());
		std::remove(db_path);
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		ASSERT_EQ(runtime_query_tools_init(&tools), 0);
	}

	void TearDown() override {
		tool_runtime_set_current(nullptr);
		tool_registry_cleanup(&tools);
		db_close(&db);
		std::remove(db_path);
	}

	void SetRuntime(const struct session &s) {
		struct tool_runtime_context rt;

		memset(&rt, 0, sizeof(rt));
		rt.db = &db;
		rt.config = &cfg;
		rt.user_id = "local";
		rt.credit_session_id = s.display_id[0] ? s.display_id : s.name;
		rt.memory_session_id = s.id;
		rt.restrict_memory_to_user = 0;
		tool_runtime_set_current(&rt);
	}
};

TEST_F(RuntimeQueryTest, RegistersReadOnlyTools)
{
	struct tool_entry *credits = tool_lookup(&tools, "credits");
	struct tool_entry *memory_tool = tool_lookup(&tools, "memory");

	ASSERT_NE(credits, nullptr);
	ASSERT_NE(memory_tool, nullptr);
	EXPECT_TRUE((credits->flags & TOOL_FLAG_READONLY) != 0);
	EXPECT_TRUE((memory_tool->flags & TOOL_FLAG_READONLY) != 0);
	EXPECT_NE(std::strstr(memory_tool->desc.description,
			      "only when the user explicitly asks"),
		  nullptr);

	cJSON *schema = cJSON_Parse(memory_tool->desc.input_schema);
	ASSERT_NE(schema, nullptr);
	cJSON *required = cJSON_GetObjectItem(schema, "required");
	ASSERT_TRUE(cJSON_IsArray(required));
	ASSERT_EQ(cJSON_GetArraySize(required), 2);
	EXPECT_STREQ(cJSON_GetArrayItem(required, 0)->valuestring, "type");
	EXPECT_STREQ(cJSON_GetArrayItem(required, 1)->valuestring, "scope");
	cJSON_Delete(schema);
}

TEST_F(RuntimeQueryTest, CreditsReportsTodaySessionAndTotal)
{
	struct session s;
	struct credit_event event;
	struct tool_result result;

	ASSERT_EQ(session_create(&db, "runtime_credits", "gpt-test", &s), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &s), 0);
	SetRuntime(s);
	cfg.credits.input_token_credit_coef = 1.0;
	cfg.credits.daily_limit = 5;

	memset(&event, 0, sizeof(event));
	event.user_id = "local";
	event.session_id = s.display_id;
	event.kind = "model_text";
	event.provider = "openai";
	event.model = "gpt-test";
	event.input_tokens = 7;
	ASSERT_EQ(credit_record_event(&db, &cfg.credits, &event, nullptr), 0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "credits", "{}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root = result.data;
	ASSERT_NE(root, nullptr);
	cJSON *today = cJSON_GetObjectItem(root, "today");
	cJSON *session = cJSON_GetObjectItem(root, "session");
	cJSON *total = cJSON_GetObjectItem(root, "total");
	ASSERT_TRUE(cJSON_IsObject(today));
	ASSERT_TRUE(cJSON_IsObject(session));
	ASSERT_TRUE(cJSON_IsObject(total));
	EXPECT_EQ(cJSON_GetObjectItem(today, "credits")->valueint, 7);
	EXPECT_EQ(cJSON_GetObjectItem(session, "credits")->valueint, 7);
	EXPECT_EQ(cJSON_GetObjectItem(total, "credits")->valueint, 7);
	EXPECT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(root, "over_daily_limit")));
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, MemoryQueriesSessionAndType)
{
	struct session s;
	struct memory_options opts;
	struct react_step action = {};
	struct tool_result result;

	ASSERT_EQ(session_create(&db, "runtime_memory", "gpt-test", &s), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &s), 0);
	SetRuntime(s);
	memset(&opts, 0, sizeof(opts));
	opts.enabled = 1;
	opts.hot_path_enabled = 1;
	opts.cold_path_enabled = 1;
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(memory_accept_input(&db, s.id, "My name is Alice.", "input", &opts), 0);
	ASSERT_EQ(memory_consolidate_turn(
			  &db, s.id,
			  "Please always reply in Chinese and be concise.",
			  "好的，我之后会用中文简洁回答。",
			  &action, 1, &opts),
		  0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"session\",\"type\":\"facts\"}",
			    &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_NE(std::strstr(text->valuestring, "facts"), nullptr);
	EXPECT_EQ(std::strstr(text->valuestring, "episodes"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, MemoryAllScopeIncludesMultipleSessions)
{
	struct session first;
	struct session second;
	struct memory_options opts;
	struct tool_result result;

	ASSERT_EQ(session_create(&db, "runtime_memory_one", "gpt-test", &first),
		  0);
	ASSERT_EQ(session_create(&db, "runtime_memory_two", "gpt-test", &second),
		  0);
	ASSERT_EQ(session_ensure_display_id(&db, &first), 0);
	ASSERT_EQ(session_ensure_display_id(&db, &second), 0);
	SetRuntime(first);
	memset(&opts, 0, sizeof(opts));
	opts.enabled = 1;
	opts.hot_path_enabled = 1;
	opts.cold_path_enabled = 1;

	ASSERT_EQ(memory_consolidate_turn(&db, first.id,
					  "Call me Ada.",
					  "Okay, Ada.", nullptr, 1, &opts),
		  0);
	ASSERT_EQ(memory_consolidate_turn(&db, second.id,
					  "Call me Grace.",
					  "Okay, Grace.", nullptr, 1, &opts),
		  0);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory", "{}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	cJSON *root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	cJSON *scope = cJSON_GetObjectItem(root, "scope");
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(scope));
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_STREQ(scope->valuestring, "session");
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_one"), nullptr);
	EXPECT_EQ(std::strstr(text->valuestring, "runtime_memory_two"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"all\",\"type\":\"all\"}", &result),
		  0);
	ASSERT_NE(result.text.data, nullptr);
	root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_one"), nullptr);
	EXPECT_NE(std::strstr(text->valuestring, "runtime_memory_two"), nullptr);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, MemoryUsesConfiguredEpisodeLimitByDefault)
{
	struct session s;
	struct tool_result result;
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"INSERT INTO memory_episodes"
		"(session_id,summary_text,created_at) VALUES(?,?,?)";

	ASSERT_EQ(session_create(&db, "runtime_memory_limit", "gpt-test", &s),
		  0);
	ASSERT_EQ(session_ensure_display_id(&db, &s), 0);
	SetRuntime(s);
	cfg.memory.max_episodes = 2;

	ASSERT_EQ(sqlite3_prepare_v2(db.handle, sql, -1, &stmt, nullptr),
		  SQLITE_OK);
	for (int i = 1; i <= 3; i++) {
		char summary[32];

		std::snprintf(summary, sizeof(summary), "episode-%d", i);
		sqlite3_bind_int64(stmt, 1, s.id);
		sqlite3_bind_text(stmt, 2, summary, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int64(stmt, 3, i);
		ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
		ASSERT_EQ(sqlite3_reset(stmt), SQLITE_OK);
		ASSERT_EQ(sqlite3_clear_bindings(stmt), SQLITE_OK);
	}
	sqlite3_finalize(stmt);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory",
			    "{\"scope\":\"session\",\"type\":\"episodes\"}",
			    &result), 0);
	cJSON *root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	cJSON *text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_EQ(CountOccurrences(text->valuestring, "episode-"), 2);
	cJSON_Delete(root);
	tool_result_cleanup(&result);

	tool_result_init(&result);
	ASSERT_EQ(tool_exec(
			  &tools, "memory",
			  "{\"scope\":\"session\",\"type\":\"episodes\","
			  "\"max_episodes\":1}",
			  &result),
		  0);
	root = cJSON_Parse(result.text.data);
	ASSERT_NE(root, nullptr);
	text = cJSON_GetObjectItem(root, "text");
	ASSERT_TRUE(cJSON_IsString(text));
	EXPECT_EQ(CountOccurrences(text->valuestring, "episode-"), 1);
	cJSON_Delete(root);
	tool_result_cleanup(&result);
}

TEST_F(RuntimeQueryTest, ExplicitPreferenceToolReplacesGenericPreferenceAndRejectsOldEvidence)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "preferences", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	struct model_history_insert input = {};
	input.session_id = session.id;
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.turn_id = "one";
	input.content = "以后代码缩进使用四个空格";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory_preference",
		"{\"key\":\"code.indent\",\"value\":\"four spaces\",\"scope\":\"personal\","
		"\"evidence\":\"以后代码缩进使用四个空格\"}", &result), 0);
	ASSERT_NE(result.text.data, nullptr);
	EXPECT_NE(strstr(result.text.data, "four spaces"), nullptr);
	tool_result_cleanup(&result);
	input.turn_id = "two";
	input.content = "以后代码缩进使用 Tab";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory_preference",
		"{\"key\":\"code.indent\",\"value\":\"tab\",\"scope\":\"personal\","
		"\"evidence\":\"以后代码缩进使用 Tab\"}", &result), 0);
	tool_result_cleanup(&result);
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory_preference",
		"{\"key\":\"code.indent\",\"value\":\"four spaces\",\"scope\":\"personal\","
		"\"evidence\":\"以后代码缩进使用四个空格\"}", &result), 0);
	tool_result_cleanup(&result);
	char *effective = preference_render(&db, session.id, 0);
	ASSERT_NE(effective, nullptr);
	EXPECT_NE(strstr(effective, "code.indent: tab"), nullptr);
	EXPECT_EQ(strstr(effective, "four spaces"), nullptr);
	free(effective);
}

TEST_F(RuntimeQueryTest, PreferenceToolCannotPromoteComplaintOrTemporaryRequest)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "no-inference", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	for (const char *text : {"Why do you always use English?", "这次用英文回答"}) {
		struct model_history_insert input = {};
		input.session_id = session.id;
		input.kind = "user_message";
		input.role = "user";
		input.active = 1;
		input.content = text;
		ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
		cJSON *args = cJSON_CreateObject();
		cJSON_AddStringToObject(args, "key", "language");
		cJSON_AddStringToObject(args, "value", "English");
		cJSON_AddStringToObject(args, "scope", "personal");
		cJSON_AddStringToObject(args, "evidence", text);
		char *json = cJSON_PrintUnformatted(args);
		struct tool_result result;
		tool_result_init(&result);
		EXPECT_EQ(tool_exec(&tools, "memory_preference", json, &result), 0);
		tool_result_cleanup(&result);
		free(json);
		cJSON_Delete(args);
		char *effective = preference_render(&db, session.id, 0);
		ASSERT_NE(effective, nullptr);
		EXPECT_STREQ(effective, "");
		free(effective);
	}
}

TEST_F(RuntimeQueryTest, ExtractionOfSameInputCannotContradictCommittedLanguage)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "same-event", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	cfg.memory.enabled = 1;
	SetRuntime(session);
	struct model_history_insert input = {};
	input.session_id = session.id;
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.turn_id = "same-input";
	input.content = "以后都用中文";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	struct memory_options options = {};
	options.enabled = 1;
	options.hot_path_enabled = 1;
	ASSERT_EQ(memory_accept_input(&db, session.id, input.content, input.turn_id, &options), 0);
	struct tool_result result;
	tool_result_init(&result);
	ASSERT_EQ(tool_exec(&tools, "memory_preference",
		"{\"key\":\"preferred_language\",\"value\":\"English\",\"scope\":\"personal\","
		"\"evidence\":\"以后都用中文\"}", &result), 0);
	tool_result_cleanup(&result);
	char *effective = preference_render(&db, session.id, 0);
	ASSERT_NE(effective, nullptr);
	EXPECT_NE(strstr(effective, "Chinese"), nullptr);
	EXPECT_EQ(strstr(effective, "English"), nullptr);
	free(effective);
}
