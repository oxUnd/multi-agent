#include <gtest/gtest.h>

#include "agent/react.h"
#include "agent/history.h"
#include "agent/tokenizer.h"
#include "agent/turn.h"
#include "db/database.h"
#include "session.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <unistd.h>

class AgentTurnTest : public ::testing::Test {
protected:
	struct db db;
	struct session sess;
	struct tokenizer *tok;
	struct react_context *react;
	char db_path[PATH_MAX];

	void SetUp() override
	{
		snprintf(db_path, sizeof(db_path), "/tmp/ma_test_agent_turn_%d.db",
			 getpid());
		std::remove(db_path);
		ASSERT_EQ(db_open(&db, db_path), 0);
		ASSERT_EQ(db_init_schema(&db), 0);
		ASSERT_EQ(session_create(&db, "agent_turn", "gpt-test",
					 &sess), 0);
		tok = tokenizer_create("gpt-test", 4096);
		ASSERT_NE(tok, nullptr);
		react = react_context_create(nullptr, tok, nullptr, nullptr);
		ASSERT_NE(react, nullptr);
	}

	void TearDown() override
	{
		react_context_destroy(react);
		tokenizer_destroy(tok);
		db_close(&db);
		std::remove(db_path);
	}
};

static int agent_turn_summary(const char *, void *, char **out)
{
	*out = strdup("old work checkpoint");
	return *out ? 0 : -ENOMEM;
}

static int agent_turn_failing_summary(const char *, void *, char **out)
{
	*out = nullptr;
	return -EIO;
}

TEST_F(AgentTurnTest, BeginLoadsHistoryAndFinishPersistsTurn)
{
	ASSERT_EQ(message_add(&db, sess.id, "user", "old question", 2), 0);
	struct agent_session_runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;

	struct agent_turn_input input;
	memset(&input, 0, sizeof(input));
	input.model_input = "full model prompt";
	input.stored_user_input = "display prompt";
	input.turn_id = "turn_test";

	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	ASSERT_TRUE(turn.begun);
	ASSERT_NE(react->messages, nullptr);
	EXPECT_STREQ(react->messages->content, "old question");

	struct react_step *step =
		static_cast<struct react_step *>(calloc(1, sizeof(*step)));
	ASSERT_NE(step, nullptr);
	step->type = REACT_STEP_FINAL;
	step->content = strdup("done");
	ASSERT_NE(step->content, nullptr);
	react->steps = step;
	react->final_answer = strdup("hello from assistant");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_DONE;

	struct agent_turn_result result;
	ASSERT_EQ(agent_turn_finish(&turn, &result), 0);
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), -EALREADY);
	EXPECT_TRUE(result.trace_saved);
	EXPECT_EQ(result.trace_rc, 0);
	EXPECT_TRUE(result.user_saved);
	EXPECT_TRUE(result.assistant_saved);
	EXPECT_EQ(result.user_rc, 0);
	EXPECT_EQ(result.assistant_rc, 0);
	EXPECT_EQ(result.message_persistence_rc, 0);
	EXPECT_GT(result.user_tokens, 0);
	EXPECT_GT(result.assistant_tokens, 0);

	int count = 0;
	struct message *msgs = message_list(&db, sess.id, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(msgs, nullptr);
	EXPECT_STREQ(msgs->role, "user");
	EXPECT_STREQ(msgs->content, "old question");
	ASSERT_NE(msgs->next, nullptr);
	EXPECT_STREQ(msgs->next->role, "user");
	EXPECT_STREQ(msgs->next->content, "display prompt");
	EXPECT_STREQ(msgs->next->turn_id, "turn_test");
	ASSERT_NE(msgs->next->next, nullptr);
	EXPECT_STREQ(msgs->next->next->role, "assistant");
	EXPECT_STREQ(msgs->next->next->content, "hello from assistant");
	EXPECT_STREQ(msgs->next->next->turn_id, "turn_test");
	message_free_list(msgs);

	struct session loaded;
	ASSERT_EQ(session_get_by_id(&db, sess.id, &loaded), 0);
	EXPECT_EQ(loaded.token_used,
		  result.user_tokens + result.assistant_tokens);

	int round_no = 0;
	int aborted = 0;
	char *trace = trace_load_latest(&db, sess.id, &round_no, &aborted);
	ASSERT_NE(trace, nullptr);
	EXPECT_EQ(round_no, 1);
	EXPECT_EQ(aborted, 0);
	EXPECT_NE(strstr(trace, "\"type\":\"Final\""), nullptr);
	free(trace);
}

TEST_F(AgentTurnTest, AssistantFailureKeepsPreModelUserCheckpoint)
{
	ASSERT_EQ(db_exec(&db,
		"CREATE TRIGGER reject_assistant_message "
		"BEFORE INSERT ON messages WHEN NEW.role = 'assistant' "
		"BEGIN SELECT RAISE(ABORT, 'reject assistant'); END;"), 0);

	struct agent_session_runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;

	struct agent_turn_input input;
	memset(&input, 0, sizeof(input));
	input.model_input = "remember this";

	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	react->final_answer = strdup("answer");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_DONE;

	struct agent_turn_result result;
	EXPECT_LT(agent_turn_finish(&turn, &result), 0);
	EXPECT_LT(result.message_persistence_rc, 0);
	EXPECT_FALSE(result.user_saved);
	EXPECT_FALSE(result.assistant_saved);

	int count = 0;
	struct message *messages = message_list(&db, sess.id, &count);
	ASSERT_NE(messages, nullptr);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(messages->role, "user");
	EXPECT_STREQ(messages->content, "remember this");
	message_free_list(messages);
	struct model_history_item *history =
		model_history_list(&db, sess.id, 1, &count);
	ASSERT_NE(history, nullptr);
	ASSERT_EQ(count, 1);
	EXPECT_STREQ(history->kind, "user_message");
	model_history_free_list(history);
}

TEST_F(AgentTurnTest, FinishDoesNotConsolidateMemoryForAbortedTurn)
{
	struct agent_session_runtime runtime;
	memset(&runtime, 0, sizeof(runtime));
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;

	struct agent_turn_input input;
	memset(&input, 0, sizeof(input));
	input.model_input = "failing prompt";

	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);

	struct react_step *step =
		static_cast<struct react_step *>(calloc(1, sizeof(*step)));
	ASSERT_NE(step, nullptr);
	step->type = REACT_STEP_OBSERVATION;
	step->content = strdup("tool error");
	ASSERT_NE(step->content, nullptr);
	react->steps = step;
	react->final_answer = strdup("tool error");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_MAX_ITERATIONS;

	struct agent_turn_result result;
	ASSERT_EQ(agent_turn_finish(&turn, &result), 0);
	EXPECT_TRUE(result.trace_saved);
	EXPECT_TRUE(result.user_saved);
	EXPECT_TRUE(result.assistant_saved);
	EXPECT_FALSE(result.memory_queued);
	EXPECT_FALSE(result.memory_ran_inline);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 1);
	EXPECT_EQ(model_history_count(&db, sess.id, 0), 1);

	char *rendered = memory_render_session(&db, sess.id, 0);
	ASSERT_NE(rendered, nullptr);
	EXPECT_NE(strstr(rendered,
			 "No long-term memory stored for this session."),
		  nullptr);
	free(rendered);
}

TEST_F(AgentTurnTest, FailedTurnKeepsUserWithoutPoisoningNextTurn)
{
	struct agent_session_runtime runtime = {};
	struct agent_turn_input failed_input = {};
	struct agent_turn failed_turn;
	struct tool_call call = {};
	int count = 0;

	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;
	failed_input.model_input = "run malformed tool";
	failed_input.turn_id = "turn_failed";
	ASSERT_EQ(agent_turn_begin(&failed_turn, &runtime, &failed_input), 0);
	std::strcpy(call.id, "provider_failed");
	std::strcpy(call.tool_call_id, "local_failed");
	std::strcpy(call.name, "bash_exec");
	call.arguments = const_cast<char *>("{not-json");
	ASSERT_EQ(agent_history_record_tool_calls(react, nullptr, nullptr,
		&call, 1), 0);
	ASSERT_EQ(agent_history_record_tool_result(react, "local_failed",
		"provider_failed", "bash_exec", "invalid arguments", -EINVAL), 0);
	react->final_answer = strdup(
		"LLM repeatedly returned an empty response.");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_LLM_ERROR;
	ASSERT_EQ(agent_turn_finish(&failed_turn, nullptr), 0);
	EXPECT_EQ(model_history_count(&db, sess.id, 1), 1);
	EXPECT_EQ(model_history_count(&db, sess.id, 0), 3);

	struct message *transcript = message_list(&db, sess.id, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(transcript, nullptr);
	EXPECT_STREQ(transcript->role, "user");
	ASSERT_NE(transcript->next, nullptr);
	EXPECT_STREQ(transcript->next->role, "assistant");
	EXPECT_NE(std::strstr(transcript->next->content, "empty response"),
		nullptr);
	message_free_list(transcript);

	struct agent_turn_input next_input = {};
	struct agent_turn next_turn;
	next_input.model_input = "next request";
	next_input.turn_id = "turn_next";
	ASSERT_EQ(agent_turn_begin(&next_turn, &runtime, &next_input), 0);
	struct model_history_item *active =
		model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->turn_id, "turn_failed");
	EXPECT_STREQ(active->kind, "user_message");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->turn_id, "turn_next");
	EXPECT_STREQ(active->next->kind, "user_message");
	model_history_free_list(active);
	react->final_answer = strdup("recovered");
	ASSERT_NE(react->final_answer, nullptr);
	react->state = REACT_STATE_DONE;
	react->outcome = REACT_OUTCOME_SUCCESS;
	ASSERT_EQ(agent_turn_finish(&next_turn, nullptr), 0);
	active = model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->turn_id, "turn_failed");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->turn_id, "turn_next");
	ASSERT_NE(active->next->next, nullptr);
	EXPECT_STREQ(active->next->next->kind, "assistant_message");
	EXPECT_STREQ(active->next->next->content, "recovered");
	model_history_free_list(active);
}

TEST_F(AgentTurnTest, MissingConfigurationKeepsCompleteToolHistory)
{
	struct agent_session_runtime runtime = {};
	struct agent_turn_input input = {};
	struct agent_turn turn;
	struct tool_call call = {};
	int count = 0;

	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;
	input.model_input = "generate an image";
	input.turn_id = "turn_missing_key";
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	std::strcpy(call.id, "provider_missing_key");
	std::strcpy(call.tool_call_id, "local_missing_key");
	std::strcpy(call.name, "img_gen");
	call.arguments = const_cast<char *>("{\"prompt\":\"sunrise\"}");
	ASSERT_EQ(agent_history_record_tool_calls(react, nullptr, nullptr,
		&call, 1), 0);
	ASSERT_EQ(agent_history_record_tool_result(react, "local_missing_key",
		"provider_missing_key", "img_gen", "missing api key",
		MORPH_ERR_NOT_CONFIGURED), 0);
	struct react_step *step = static_cast<struct react_step *>(
		calloc(1, sizeof(*step)));
	ASSERT_NE(step, nullptr);
	step->type = REACT_STEP_OBSERVATION;
	step->content = strdup("missing api key");
	ASSERT_NE(step->content, nullptr);
	step->error_code = MORPH_ERR_NOT_CONFIGURED;
	react->steps = step;
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_LLM_ERROR;
	react->last_error_code = MORPH_ERR_LLM;
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);

	struct model_history_item *active = model_history_list(
		&db, sess.id, 1, &count);
	ASSERT_EQ(count, 3);
	ASSERT_NE(active, nullptr);
	EXPECT_STREQ(active->kind, "user_message");
	ASSERT_NE(active->next, nullptr);
	EXPECT_STREQ(active->next->kind, "assistant_tool_calls");
	ASSERT_NE(active->next->next, nullptr);
	EXPECT_STREQ(active->next->next->kind, "tool_result");
	EXPECT_STREQ(active->next->next->tool_name, "img_gen");
	model_history_free_list(active);
}

TEST_F(AgentTurnTest, BeginCompactsBeforePersistingCurrentUser)
{
	struct model_history_insert old = {};
	old.session_id = sess.id;
	old.turn_id = "turn_old";
	old.kind = "user_message";
	old.role = "user";
	old.content = "old large context";
	old.token_count = 10;
	old.active = 1;
	ASSERT_EQ(model_history_add(&db, &old, nullptr), 0);
	react->compress.max_context_tokens = 2;
	react->compress.summarize_threshold_ratio = 0.5;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_user_message_tokens = 0;
	react->compress.compaction_summary_max_tokens = 10;
	react->compress.summarize = agent_turn_summary;

	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.flags = AGENT_TURN_DEFAULT_FLAGS &
		~AGENT_TURN_SAVE_TRACE &
		~AGENT_TURN_CONSOLIDATE_MEMORY &
		~AGENT_TURN_ASYNC_MEMORY &
		~AGENT_TURN_BUILD_MEMORY_CONTEXT;
	struct agent_turn_input input = {};
	input.model_input = "current exact request";
	input.turn_id = "turn_current";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);

	int count = 0;
	struct model_history_item *items =
		model_history_list(&db, sess.id, 1, &count);
	ASSERT_EQ(count, 2);
	ASSERT_NE(items, nullptr);
	EXPECT_STREQ(items->kind, "compaction_summary");
	ASSERT_NE(items->next, nullptr);
	EXPECT_STREQ(items->next->kind, "user_message");
	EXPECT_STREQ(items->next->turn_id, "turn_current");
	EXPECT_STREQ(items->next->content, "current exact request");
	model_history_free_list(items);
	react->state = REACT_STATE_ABORT;
	EXPECT_EQ(agent_turn_finish(&turn, nullptr), 0);
}

TEST_F(AgentTurnTest, ChatHistoryPlacesSummaryBeforePreservedUser)
{
	struct model_history_item user = {};
	struct model_history_item summary = {};
	morph_array_t messages = {};
	struct arena *arena = arena_create(0);

	ASSERT_NE(arena, nullptr);
	std::strcpy(user.kind, "user_message");
	user.content = const_cast<char *>("exact current request");
	user.next = &summary;
	std::strcpy(summary.kind, "compaction_summary");
	summary.content = const_cast<char *>("earlier context summary");
	ASSERT_EQ(morph_array_init(&messages, 2,
		sizeof(struct chat_message)), 0);
	ASSERT_EQ(agent_history_build_chat_messages(&user, &messages, arena), 0);
	ASSERT_EQ(messages.nelts, 2U);
	struct chat_message *built =
		static_cast<struct chat_message *>(messages.elts);
	EXPECT_STREQ(built[0].role, "system");
	EXPECT_STREQ(built[0].content, "earlier context summary");
	EXPECT_STREQ(built[1].role, "user");
	EXPECT_STREQ(built[1].content, "exact current request");
	morph_array_cleanup(&messages);
	arena_destroy(arena);
}

TEST_F(AgentTurnTest, InTurnCompactionFallsBackWhenSummaryFails)
{
	struct model_history_insert old = {};

	old.session_id = sess.id;
	old.turn_id = "turn_fallback";
	old.kind = "user_message";
	old.role = "user";
	old.content = "important active context";
	old.token_count = 10;
	old.active = 1;
	ASSERT_EQ(model_history_add(&db, &old, nullptr), 0);
	react->history_enabled = 1;
	react->history_db = &db;
	react->history_session_id = sess.id;
	react->compress.max_context_tokens = 100;
	react->compress.max_history_rounds = 2;
	react->compress.compress_target_ratio = 0.5;
	react->compress.compaction_user_message_tokens = 20;
	react->compress.compaction_summary_max_tokens = 20;
	react->compress.summarize = agent_turn_failing_summary;
	ASSERT_EQ(react_set_turn_id(react, "turn_fallback"), 0);
	ASSERT_EQ(agent_history_reload(react), 0);
	EXPECT_EQ(agent_history_compact_with_trigger(react, 1, "in_turn_1"),
		1);

	int count = 0;
	struct model_history_item *items = model_history_list(&db, sess.id, 1,
		&count);
	bool found_fallback = false;
	for (struct model_history_item *item = items; item;
	     item = item->next) {
		if (std::strcmp(item->kind, "compaction_summary") == 0 &&
		    item->content && std::strstr(item->content,
			"could not be summarized"))
			found_fallback = true;
	}
	EXPECT_TRUE(found_fallback);
	model_history_free_list(items);
}

TEST_F(AgentTurnTest, PreferenceIsSavedBeforeModelAndSurvivesCancellation)
{
	struct memory_options options = {};
	options.enabled = 1;
	options.hot_path_enabled = 1;
	struct agent_session_runtime runtime = {};
	runtime.db = &db;
	runtime.session_id = sess.id;
	runtime.react = react;
	runtime.memory_options = &options;
	struct agent_turn_input input = {};
	input.model_input = "以后都用中文回答";
	input.turn_id = "preference-turn";
	struct agent_turn turn;
	ASSERT_EQ(agent_turn_begin(&turn, &runtime, &input), 0);
	ASSERT_NE(react->memory_context, nullptr);
	EXPECT_NE(strstr(react->memory_context, "response.language: Chinese"), nullptr);
	react->state = REACT_STATE_ABORT;
	react->outcome = REACT_OUTCOME_CANCELLED;
	ASSERT_EQ(agent_turn_finish(&turn, nullptr), 0);
	char *effective = preference_render(&db, sess.id, 0);
	ASSERT_NE(effective, nullptr);
	EXPECT_NE(strstr(effective, "Chinese"), nullptr);
	free(effective);
}
