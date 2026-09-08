#include <gtest/gtest.h>

#include "db/database.h"
#include "agent/memory.h"
#include "session.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include "models/llm.h"

class MemoryTest : public ::testing::Test {
protected:
	struct db db;
	char db_path[256];
	struct memory_options opts;

	void SetUp() override
	{
		std::snprintf(db_path, sizeof(db_path),
			      "/tmp/ma_test_memory_%d.db", getpid());
		std::remove(db_path);
		std::memset(&db, 0, sizeof(db));
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		std::memset(&opts, 0, sizeof(opts));
		opts.enabled = 1;
		opts.hot_path_enabled = 1;
		opts.cold_path_enabled = 1;
		opts.max_facts = 6;
		opts.max_episodes = 4;
		opts.max_procedures = 4;
		opts.max_context_chars = 4096;
	}

	void TearDown() override
	{
		memory_async_shutdown();
		db_close(&db);
		std::remove(db_path);
	}
};

static int consolidate_accepted_turn(struct db *database, int64_t session_id,
	const char *input, const char *answer, const struct react_step *steps,
	int success, const struct memory_options *options)
{
	int rc = memory_accept_input(database, session_id, input, nullptr, options);
	if (rc != 0)
		return rc;
	return memory_consolidate_turn(database, session_id, input, answer, steps, success, options);
}

TEST_F(MemoryTest, PreferencesHaveOneAuthorityAndNoDuplicateRules)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "memory_session", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id,
		"Please always reply in Chinese and be concise.", "input1", &opts), 0);
	char *context = memory_build_context(&db, session.id, "always", &opts);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_NE(strstr(context, "response.detail: concise"), nullptr);
	EXPECT_EQ(strstr(context, "Standing rules"), nullptr);
	EXPECT_EQ(strstr(context, "Profile"), nullptr);
	free(context);
}

TEST_F(MemoryTest, UpdatedFactSupersedesOlderValue)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "memory_temporal", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id,
		"以后都用英文回答", "input1", &opts), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id,
		"以后都用中文回答", "input2", &opts), 0);
	char *effective = preference_render(&db, session.id, 0);
	char *history = preference_render(&db, session.id, 1);
	ASSERT_NE(effective, nullptr);
	ASSERT_NE(history, nullptr);
	EXPECT_NE(strstr(effective, "Chinese"), nullptr);
	EXPECT_EQ(strstr(effective, "English"), nullptr);
	EXPECT_NE(strstr(history, "English"), nullptr);
	EXPECT_NE(strstr(history, "Chinese"), nullptr);
	free(effective);
	free(history);
}

TEST_F(MemoryTest, BuildContextIncludesEpisodesAndChanges)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_context", "gpt-4o", &s), 0);
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id,
			  "My name is Alice. I live in London.",
			  "Sure, I will reply in English.",
			  &action, 1, &opts),
		  0);
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id,
			  "My name is Alice. I live in Tokyo.",
			  "好的，之后默认使用中文。",
			  &action, 1, &opts),
		  0);

	char *temporal = memory_build_context(&db, s.id,
					      "现在还是英文吗，什么时候改的？",
					      &opts);
	ASSERT_NE(temporal, nullptr);
	EXPECT_NE(std::strstr(temporal, "Recent changes"), nullptr);
	EXPECT_NE(std::strstr(temporal, "location"), nullptr);
	free(temporal);

	char *episodic = memory_build_context(&db, s.id,
					      "上次你是怎么做的？",
					      &opts);
	ASSERT_NE(episodic, nullptr);
	EXPECT_NE(std::strstr(episodic, "Relevant episodes"), nullptr);
	EXPECT_NE(std::strstr(episodic, "file_read"), nullptr);
	free(episodic);
}

TEST_F(MemoryTest, RenderSessionShowsStoredSections)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_render", "gpt-4o", &s), 0);
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id,
			  "Please always reply in Chinese and be concise.",
			  "好的，我之后会用中文简洁回答。",
			  &action, 1, &opts),
		  0);

	char *rendered = memory_render_session(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_NE(std::strstr(rendered, "Effective preferences"), nullptr);
	EXPECT_NE(std::strstr(rendered, "response.language"), nullptr);
	EXPECT_NE(std::strstr(rendered, "response.detail"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Recent episodes"), nullptr);
	free(rendered);
}

TEST_F(MemoryTest, ClearFactsKeepsEpisodes)
{
	struct session s;
	struct react_step action = {};
	action.type = REACT_STEP_ACTION;
	action.tool_name = const_cast<char *>("file_read");

	ASSERT_EQ(session_create(&db, "memory_clear_facts", "gpt-4o", &s), 0);
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id,
			  "Please reply in English.",
			  "Sure, I will reply in English.",
			  &action, 1, &opts),
		  0);

	ASSERT_EQ(memory_clear(&db, s.id, MEMORY_CLEAR_FACTS), 0);

	char *rendered = memory_render_session(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_EQ(std::strstr(rendered, "Current facts"), nullptr);
	EXPECT_NE(std::strstr(rendered, "Recent episodes"), nullptr);
	free(rendered);
}

TEST_F(MemoryTest, ClearAllRemovesEverything)
{
	struct session s;

	ASSERT_EQ(session_create(&db, "memory_clear_all", "gpt-4o", &s), 0);
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id,
			  "这个会话用中文回答，默认简洁。",
			  "好的，我之后会用中文简洁回答。",
			  nullptr, 1, &opts),
		  0);

	ASSERT_EQ(memory_clear(&db, s.id, MEMORY_CLEAR_ALL), 0);

	char *rendered = memory_render_session(&db, s.id, 4);
	ASSERT_NE(rendered, nullptr);
	EXPECT_STREQ(rendered, "No long-term memory stored for this session.");
	free(rendered);
}

static int memory_count_fact(struct db *db, int64_t session_id,
			     const char *key_name)
{
	sqlite3_stmt *stmt = nullptr;
	const char *sql =
		"SELECT COUNT(*) FROM memory_facts "
		"WHERE session_id=? AND key_name=? AND is_current=1";
	if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, nullptr) !=
	    SQLITE_OK)
		return -1;
	sqlite3_bind_int64(stmt, 1, session_id);
	sqlite3_bind_text(stmt, 2, key_name, -1, SQLITE_TRANSIENT);
	int count = -1;
	if (sqlite3_step(stmt) == SQLITE_ROW)
		count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return count;
}

TEST_F(MemoryTest, AmbiguousAnchorsDoNotPolluteFacts)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "memory_precision", "gpt-4o", &s), 0);

	/* "I am in Tokyo" used to also land as user_name="in Tokyo".
	 * It should now only set location. */
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id, "I am in Tokyo.",
			  "Got it.", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "user_name"), 0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "location"), 1);

	/* "我想知道..." should not be misread as a goal anchor. */
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id, "我想知道天气如何。",
			  "天气晴朗。", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "goal"), 0);

	/* "我在想..." should not be misread as a location anchor. */
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id, "我在想这个问题。",
			  "嗯。", nullptr, 1, &opts),
		  0);
	/* Still only one location fact, from the Tokyo turn above. */
	EXPECT_EQ(memory_count_fact(&db, s.id, "location"), 1);

	/* "我叫了一辆出租车" should not be misread as user_name. */
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id, "我叫了一辆出租车。",
			  "好的。", nullptr, 1, &opts),
		  0);
	EXPECT_EQ(memory_count_fact(&db, s.id, "user_name"), 0);

	/* Sanity check: a clean anchor still works. */
	ASSERT_EQ(consolidate_accepted_turn(
			  &db, s.id, "Please call me Alice.",
			  "Sure, Alice.", nullptr, 1, &opts),
		  0);
	char *name = preference_render(&db, s.id, 0);
	ASSERT_NE(name, nullptr);
	EXPECT_NE(strstr(name, "user.preferred_name: Alice"), nullptr);
	free(name);
}

TEST_F(MemoryTest, BuildContextReturnsNullWhenEmpty)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "memory_empty_ctx", "gpt-4o", &s), 0);

	/* No turns consolidated; build_context should return NULL (i.e.
	 * skip the prompt injection entirely) instead of emitting just the
	 * intro line. */
	char *ctx_str = memory_build_context(&db, s.id, "anything", &opts);
	EXPECT_EQ(ctx_str, nullptr);
	free(ctx_str);
}

TEST_F(MemoryTest, LanguageMentionsComplaintsAndNegationsDoNotChangePreference)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "language_guard", "mock", &s), 0);
	ASSERT_EQ(consolidate_accepted_turn(&db, s.id, "你现在回答为啥过程中都是英文？",
		"我会检查", nullptr, 1, &opts), 0);
	sqlite3_stmt *check = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle,
		"SELECT COUNT(*) FROM memory_facts WHERE session_id=? "
		"AND key_name='preferred_language' AND is_current=1",
		-1, &check, nullptr), SQLITE_OK);
	sqlite3_bind_int64(check, 1, s.id);
	ASSERT_EQ(sqlite3_step(check), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(check, 0), 0);
	sqlite3_finalize(check);
	ASSERT_EQ(consolidate_accepted_turn(&db, s.id, "后续回答我的语言改成中文",
		"好的", nullptr, 1, &opts), 0);
	const char *inputs[] = {
		"你现在回答为啥过程中都是英文？",
		"不要用英文回答我。",
		"Please do not reply in English.",
		"Why do you reply in English?",
		"请把这个词翻译成英文。",
		"今天学习中文和英文。",
		"请解释“用英文回答”这句话。",
	};
	for (const char *input : inputs) {
		SCOPED_TRACE(input);
		ASSERT_EQ(consolidate_accepted_turn(&db, s.id, input, "好的",
			nullptr, 1, &opts), 0);
		char *effective = preference_render(&db, s.id, 0);
		ASSERT_NE(effective, nullptr);
		EXPECT_NE(strstr(effective, "Chinese"), nullptr);
		EXPECT_EQ(strstr(effective, "English"), nullptr);
		free(effective);
	}
}

TEST_F(MemoryTest, ExplicitLanguageSwitchReplacesConflictingStandingRule)
{
	struct session s;
	ASSERT_EQ(session_create(&db, "language_switch", "mock", &s), 0);
	ASSERT_EQ(consolidate_accepted_turn(&db, s.id, "Please always reply in English.",
		"OK", nullptr, 1, &opts), 0);
	ASSERT_EQ(consolidate_accepted_turn(&db, s.id, "以后请用中文回答，不要用英文。",
		"好的", nullptr, 1, &opts), 0);
	char *context = memory_build_context(&db, s.id, "How should you always respond?", &opts);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "Respond in English"), nullptr);
	free(context);
}

static std::string effective_preferences(struct db *database, int64_t session_id)
{
	char *text = preference_render(database, session_id, 0);
	std::string result = text ? text : "<error>";
	free(text);
	return result;
}

TEST_F(MemoryTest, PersonalPreferencesCrossSessionsButNeverUsers)
{
	struct session first, second, stranger;
	ASSERT_EQ(session_create(&db, "first", "mock", &first), 0);
	ASSERT_EQ(session_create(&db, "second", "mock", &second), 0);
	ASSERT_EQ(session_create(&db, "stranger", "mock", &stranger), 0);
	ASSERT_EQ(preference_bind(&db, first.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, second.id, "alice", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, stranger.id, "bob", "/project"), 0);
	ASSERT_EQ(memory_accept_input(&db, first.id, "以后都用中文", "first", &opts), 0);
	EXPECT_NE(effective_preferences(&db, second.id).find("Chinese"), std::string::npos);
	EXPECT_EQ(effective_preferences(&db, stranger.id), "");
	EXPECT_EQ(preference_bind(&db, first.id, "bob", "/project"), -EPERM);
	ASSERT_EQ(memory_accept_input(&db, second.id, "以后都用英文", "second", &opts), 0);
	EXPECT_EQ(effective_preferences(&db, first.id).find("Chinese"), std::string::npos);
	EXPECT_NE(effective_preferences(&db, first.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, ScopePrecedenceAndUnsetRestoreInheritance)
{
	struct session first, same_project, other_project;
	ASSERT_EQ(session_create(&db, "first", "mock", &first), 0);
	ASSERT_EQ(session_create(&db, "same", "mock", &same_project), 0);
	ASSERT_EQ(session_create(&db, "other", "mock", &other_project), 0);
	ASSERT_EQ(preference_bind(&db, first.id, "alice", "/one"), 0);
	ASSERT_EQ(preference_bind(&db, same_project.id, "alice", "/one"), 0);
	ASSERT_EQ(preference_bind(&db, other_project.id, "alice", "/two"), 0);
	ASSERT_EQ(preference_set(&db, first.id, "personal", "language", "Chinese", "a", "1"), 0);
	ASSERT_EQ(preference_set(&db, first.id, "project", "language", "English", "b", "2"), 0);
	ASSERT_EQ(preference_set(&db, first.id, "session", "language", "Japanese", "c", "3"), 0);
	EXPECT_NE(effective_preferences(&db, first.id).find("Japanese"), std::string::npos);
	EXPECT_NE(effective_preferences(&db, same_project.id).find("English"), std::string::npos);
	EXPECT_NE(effective_preferences(&db, other_project.id).find("Chinese"), std::string::npos);
	ASSERT_EQ(preference_set(&db, first.id, "session", "language", nullptr, "unset", "4"), 0);
	EXPECT_NE(effective_preferences(&db, first.id).find("English"), std::string::npos);
	ASSERT_EQ(preference_set(&db, first.id, "project", "language", nullptr, "unset", "5"), 0);
	EXPECT_NE(effective_preferences(&db, first.id).find("Chinese"), std::string::npos);
}

TEST_F(MemoryTest, TemporaryQuestionsAndQuotesNeverOverwriteDurablePreference)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "temporary", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后默认用中文，回答简洁", "1", &opts), 0);
	std::string before = effective_preferences(&db, session.id);
	const char *inputs[] = {
		"这次用英文详细回答", "这封邮件用英文", "Please reply in English.",
		"以后用英文回答吗？", "我希望了解为什么回答总是详细的？",
		"请解释：‘以后用英文’是什么意思？", "Translate always use English into Chinese.",
		"以后不要用英文", "Why do you always use English?",
	};
	for (const char *input : inputs) {
		SCOPED_TRACE(input);
		ASSERT_EQ(memory_accept_input(&db, session.id, input, input, &opts), 0);
		EXPECT_EQ(effective_preferences(&db, session.id), before);
	}
}

TEST_F(MemoryTest, ReplayCannotResurrectDeletedPreferenceOrOverrideNewValue)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "replay", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用英文", "old", &opts), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用中文", "new", &opts), 0);
	std::string current = effective_preferences(&db, session.id);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用英文", "old", &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), current);
	ASSERT_EQ(preference_set(&db, session.id, "personal", "language", nullptr, "forget", "delete"), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用英文", "old", &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), "");
	ASSERT_EQ(memory_consolidate_turn(&db, session.id, "以后都用英文", "OK", nullptr, 1, &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id), "");
}

TEST_F(MemoryTest, ClearedMemoryRejectsOlderBackgroundGeneration)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "generation", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "input", &opts), 0);
	struct memory_options old_job = opts;
	old_job.hot_path_enabled = 0;
	old_job.generation_bound = 1;
	old_job.generation = 0;
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	EXPECT_EQ(memory_consolidate_turn(&db, session.id, "old work", "done", nullptr, 1, &old_job), -ESTALE);
	char *render = memory_render_session(&db, session.id, 0);
	ASSERT_NE(render, nullptr);
	EXPECT_STREQ(render, "No long-term memory stored for this session.");
	free(render);
}

TEST_F(MemoryTest, OldProfilesAndRulesCannotCompeteWithEffectivePreference)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "legacy", "mock", &session), 0);
	ASSERT_EQ(db_exec(&db,
		"INSERT INTO memory_profiles VALUES(1,'Respond in English',1);"
		"INSERT INTO memory_procedures(session_id,rule_text,trigger_text,updated_at) "
		"VALUES(1,'Always speak English','complaint',1);"
		"INSERT INTO memory_facts(session_id,key_name,value_text,source_text,category,"
		"valid_from,created_at,updated_at) VALUES(1,'preferred_language','English',"
		"'你现在回答为啥过程中都是英文？','preference',1,1,1);"), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用中文", "new", &opts), 0);
	char *context = memory_build_context(&db, session.id, "always reply", &opts);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_EQ(strstr(context, "English"), nullptr);
	EXPECT_EQ(strstr(context, "preferred_language"), nullptr);
	free(context);
	EXPECT_EQ(effective_preferences(&db, session.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, LegacyMigrationIsIdempotentAndCannotOverrideNewExplicitSetting)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "legacy_good", "mock", &session), 0);
	ASSERT_EQ(db_exec(&db,
		"INSERT INTO memory_facts(session_id,key_name,value_text,source_text,category,"
		"valid_from,created_at,updated_at) VALUES(1,'preferred_language','English',"
		"'以后都用英文','preference',1,1,1);"), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "first", &opts), 0);
	EXPECT_NE(effective_preferences(&db, session.id).find("English [session"), std::string::npos);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用中文", "second", &opts), 0);
	EXPECT_NE(effective_preferences(&db, session.id).find("Chinese [personal"), std::string::npos);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "hello", "third", &opts), 0);
	EXPECT_EQ(effective_preferences(&db, session.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, DurableJobsRecoverAndDoNotDuplicateEpisodes)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "recovery", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	ASSERT_EQ(db_exec(&db,
		"INSERT INTO memory_jobs(session_id,generation,payload) VALUES(1,0,"
		"'{\"input\":\"old task\",\"answer\":\"done\",\"success\":true,"
		"\"episodes\":true,\"llm\":false,\"tools\":[\"file_read\"]}');"), 0);
	ASSERT_EQ(memory_async_resume(&db), 0);
	memory_async_shutdown();
	char *jobs = memory_background_render(&db, session.id);
	ASSERT_NE(jobs, nullptr);
	EXPECT_NE(strstr(jobs, "completed"), nullptr);
	free(jobs);
	/* Simulate a crash after episode commit but before the completion marker. */
	ASSERT_EQ(db_exec(&db, "UPDATE memory_jobs SET state='queued'"), 0);
	ASSERT_EQ(memory_async_resume(&db), 0);
	memory_async_shutdown();
	sqlite3_stmt *stmt = nullptr;
	ASSERT_EQ(sqlite3_prepare_v2(db.handle, "SELECT COUNT(*) FROM memory_episodes", -1, &stmt, nullptr), SQLITE_OK);
	ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
	EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
	sqlite3_finalize(stmt);
	ASSERT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	ASSERT_EQ(memory_async_resume(&db), 0);
	memory_async_shutdown();
	char *render = memory_render_session(&db, session.id, 0);
	ASSERT_NE(render, nullptr);
	EXPECT_STREQ(render, "No long-term memory stored for this session.");
	free(render);
}

struct DelayedMemoryModel {
	std::mutex mutex;
	std::condition_variable cv;
	bool entered = false;
	bool released = false;
};

static int delayed_memory_chat(struct model *self, struct arena *, const char *,
	const char **, int, const struct model_chat_options *, sse_callback cb, void *data)
{
	auto *state = static_cast<DelayedMemoryModel *>(self->handle);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->entered = true;
		state->cv.notify_all();
		state->cv.wait_for(lock, std::chrono::seconds(5), [&] { return state->released; });
	}
	return cb("{\"facts\":[{\"key\":\"language\",\"value\":\"English\",\"category\":\"preference\"}],"
		"\"rules\":[{\"rule_text\":\"Always speak English\"}]}", data);
}

TEST_F(MemoryTest, InFlightExtractionCannotOverrideOrResurrectPreference)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "async_old", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用英文", "old", &opts), 0);
	DelayedMemoryModel state;
	struct model model = {};
	model.handle = &state;
	model.chat = delayed_memory_chat;
	std::strcpy(model.api_key, "mock");
	memory_set_llm(&model);
	opts.llm_extract_enabled = 1;
	ASSERT_EQ(memory_consolidate_turn_async(&db, session.id, "以后都用英文", "OK", nullptr, 1, &opts), 0);
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		EXPECT_TRUE(state.cv.wait_for(lock, std::chrono::seconds(5), [&] { return state.entered; }));
	}
	EXPECT_EQ(memory_accept_input(&db, session.id, "以后都用中文", "new", &opts), 0);
	EXPECT_EQ(preference_set(&db, session.id, "personal", "language", nullptr, "forget", "delete"), 0);
	EXPECT_EQ(memory_clear(&db, session.id, MEMORY_CLEAR_ALL), 0);
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.released = true;
		state.cv.notify_all();
	}
	memory_async_shutdown();
	memory_set_llm(nullptr);
	EXPECT_EQ(effective_preferences(&db, session.id), "");
	char *context = memory_build_context(&db, session.id, "上次 always", &opts);
	EXPECT_EQ(context, nullptr);
	free(context);
}

TEST_F(MemoryTest, IndependentWritersResolveOneValuePerCanonicalKey)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "writers", "mock", &session), 0);
	ASSERT_EQ(preference_bind(&db, session.id, "local", "/project"), 0);
	std::atomic<int> failures{0};
	auto write = [&](const char *key, const char *value) {
		struct db connection = {};
		if (db_open(&connection, db_path) != 0) { failures++; return; }
		for (int i = 0; i < 20; i++) {
			if (preference_set(&connection, session.id, "personal", key, value, "writer", nullptr))
				failures++;
		}
		db_close(&connection);
	};
	std::thread first(write, "preferred_language", "Chinese");
	std::thread second(write, "response.language", "English");
	first.join();
	second.join();
	EXPECT_EQ(failures, 0);
	std::string effective = effective_preferences(&db, session.id);
	EXPECT_NE(effective.find("response.language:"), std::string::npos);
	EXPECT_EQ(effective.find("response.language:", effective.find("response.language:") + 1), std::string::npos);
}

TEST_F(MemoryTest, SourceMessageOrderWinsEvenWhenOlderInputCommitsLater)
{
	struct session older, newer;
	ASSERT_EQ(session_create(&db, "older", "mock", &older), 0);
	ASSERT_EQ(session_create(&db, "newer", "mock", &newer), 0);
	ASSERT_EQ(preference_bind(&db, older.id, "local", "/project"), 0);
	ASSERT_EQ(preference_bind(&db, newer.id, "local", "/project"), 0);
	struct model_history_insert input = {};
	input.kind = "user_message";
	input.role = "user";
	input.active = 1;
	input.session_id = older.id;
	input.turn_id = "older-input";
	input.content = "以后都用英文";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	input.session_id = newer.id;
	input.turn_id = "newer-input";
	input.content = "以后都用中文";
	ASSERT_EQ(model_history_add(&db, &input, nullptr), 0);
	ASSERT_EQ(memory_accept_input(&db, newer.id, input.content, input.turn_id, &opts), 0);
	ASSERT_EQ(memory_accept_input(&db, older.id, "以后都用英文", "older-input", &opts), 0);
	EXPECT_NE(effective_preferences(&db, older.id).find("Chinese"), std::string::npos);
	EXPECT_EQ(effective_preferences(&db, newer.id).find("English"), std::string::npos);
}

TEST_F(MemoryTest, PreferenceReadFailureIsNotTreatedAsEmptyMemory)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "read-failure", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用中文", "input", &opts), 0);
	ASSERT_EQ(db_exec(&db, "DROP TABLE memory_preferences"), 0);
	char *context = nullptr;
	EXPECT_EQ(memory_build_context_checked(&db, session.id, "hello", &opts, &context), MORPH_ERR_DB);
	EXPECT_EQ(context, nullptr);
}

TEST_F(MemoryTest, EssentialLanguageSurvivesSmallGeneralMemoryBudget)
{
	struct session session;
	ASSERT_EQ(session_create(&db, "budget", "mock", &session), 0);
	ASSERT_EQ(memory_accept_input(&db, session.id, "以后都用中文", "input", &opts), 0);
	opts.max_context_chars = 32;
	char *context = memory_build_context(&db, session.id, "hello", &opts);
	ASSERT_NE(context, nullptr);
	EXPECT_NE(strstr(context, "response.language: Chinese"), nullptr);
	EXPECT_LE(strlen(context), 512u);
	free(context);
}
