add_library(morph-agent STATIC
	react.c
	history.c
	turn.c
	context.c
	compress.c
	tokenizer.c
	tool.c
	tool_context.c
	tool_runtime.c
	patch.c
	plan.c
	memory.c
	memory_jobs.c
	guardrail.c
	sub_agent.c
)

target_include_directories(morph-agent PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-agent PUBLIC morph-util morph-db morph-session morph-persistence morph-models morph-http morph-event Threads::Threads)

add_subdirectory(tools)
