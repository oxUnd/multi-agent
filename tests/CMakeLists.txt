include(GoogleTest)

find_package(Threads REQUIRED)

set(TEST_SOURCES
	test_arena.cpp
	test_error.cpp
	test_log.cpp
	test_file.cpp
	test_database.cpp
	test_scheduled_task.cpp
	test_config.cpp
	test_config_write.cpp
	test_credits.cpp
	test_session.cpp
	test_memory.cpp
	test_runtime_query.cpp
	test_runtime.cpp
	test_runtime_services.cpp
	test_runtime_internal.cpp
	test_runtime_sync.cpp
	test_runtime_execute.cpp
	test_runtime_output.cpp
	test_dynamic_tools.cpp
	test_tool.cpp
	test_plan.cpp
	test_patch.cpp
	test_apply_patch_codex.cpp
	test_context.cpp
	test_tokenizer.cpp
	test_compress.cpp
	test_react.cpp
	test_agent_turn.cpp
	test_sse.cpp
	test_http.cpp
	test_image.cpp
	test_video.cpp
	test_skill.cpp
	test_sync.cpp
	test_bash_exec.cpp
	test_bash_parse.cpp
	test_spin.cpp
	test_util_containers.cpp
	test_render.cpp
	test_mcp.cpp
	test_ask_user.cpp
	test_sandbox.cpp
	test_tool_context.cpp
	test_sub_agent.cpp
	test_ext.cpp
	test_event.cpp
	test_agent_ui.cpp
	test_cli_list.cpp
	test_cli_presentation.cpp
	test_cli_terminal.cpp
	test_cli_ui.cpp
	test_utf8.cpp
	test_kitty_protocol.cpp
	test_fastcgi_security.cpp
	test_fastcgi_runtime.cpp
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi/security.c
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi/agent_bridge.c
)

add_executable(morph-tests ${TEST_SOURCES})
set_target_properties(morph-tests PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)
add_dependencies(morph-tests morph-data)

target_include_directories(morph-tests PRIVATE
	${CMAKE_SOURCE_DIR}/src
	${CMAKE_SOURCE_DIR}/src/util
	${CMAKE_SOURCE_DIR}/src/db
	${CMAKE_SOURCE_DIR}/src/agent
	${CMAKE_SOURCE_DIR}/src/agent/tools
	${CMAKE_SOURCE_DIR}/src/sandbox
	${CMAKE_SOURCE_DIR}/src/ext
	${CMAKE_SOURCE_DIR}/src/ipc
	${CMAKE_SOURCE_DIR}/src/models
	${CMAKE_SOURCE_DIR}/src/http
	${CMAKE_SOURCE_DIR}/src/render
	${CMAKE_SOURCE_DIR}/src/skill
	${CMAKE_SOURCE_DIR}/src/mcp
	${CMAKE_SOURCE_DIR}/src/event
	${CMAKE_SOURCE_DIR}/src/agent_ui
	${CMAKE_SOURCE_DIR}/src/sapi/fastcgi
)

target_compile_definitions(morph-tests PRIVATE
	MORPH_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(morph-tests PRIVATE
	morph-cli
	GTest::gtest_main
)

gtest_discover_tests(morph-tests)

# PTY integration driver uses the production CLI with a disposable database.
add_executable(morph-cli-pty-driver cli_pty_driver.c)
target_include_directories(morph-cli-pty-driver PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(morph-cli-pty-driver PRIVATE morph-cli)
set_target_properties(morph-cli-pty-driver PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND UNIX AND MORPH_READLINE_LIBRARY)
	add_test(NAME cli_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	add_test(NAME cli_image_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_image_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	add_test(NAME cli_memory_pty_integration
		COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_cli_memory_pty.py
			$<TARGET_FILE:morph-cli-pty-driver>)
	set_tests_properties(cli_memory_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	set_tests_properties(cli_image_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
	set_tests_properties(cli_pty_integration PROPERTIES
		SKIP_RETURN_CODE 77 TIMEOUT 90)
endif()
