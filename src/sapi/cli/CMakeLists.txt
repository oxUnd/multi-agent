add_library(morph-cli STATIC
	cancel_input.c
	clipboard.c
	command_job.c
	composer.c
	core.c
	events.c
	init.c
	interaction.c
	list_ui.c
	once.c
	output.c
	presentation.c
	run.c
	scheduler.c
	terminal.c
	ui_event.c
	commands/ext.c
	commands/mcp.c
	commands/media.c
	commands/memory.c
	commands/permissions.c
	commands/registry.c
	commands/runtime.c
	commands/session.c
	commands/skill.c
	commands/sync.c
	commands/tasks.c
)
target_include_directories(morph-cli PUBLIC
	${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(morph-cli PUBLIC MORPH_VERSION="${PROJECT_VERSION}")
target_link_libraries(morph-cli PUBLIC
	morph-runtime
	morph-util
	morph-db
	morph-agent
	morph-http
	morph-models
	morph-config
	morph-session
	morph-credits
	morph-render
	MorphMarkdown::Kitty
	morph-skill
	morph-tools
	morph-ext
	morph-mcp
	morph-ipc
	morph-sandbox
	morph-event
	morph-agent-ui
	morph-sync
)
add_executable(morph main.c)
set_target_properties(morph PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR}
)
target_include_directories(morph PRIVATE
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph PRIVATE morph-cli)
add_dependencies(morph morph-data)

install(TARGETS morph
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

find_path(MORPH_READLINE_INCLUDE_DIR readline/readline.h
	/opt/homebrew/opt/readline/include
	/usr/local/opt/readline/include
	/usr/include
	NO_DEFAULT_PATH
)
find_library(MORPH_READLINE_LIBRARY readline
	/opt/homebrew/opt/readline/lib
	/usr/local/opt/readline/lib
	/usr/lib
	NO_DEFAULT_PATH
)
if(NOT MORPH_READLINE_INCLUDE_DIR)
	find_path(MORPH_READLINE_INCLUDE_DIR readline/readline.h)
endif()
if(NOT MORPH_READLINE_LIBRARY)
	find_library(MORPH_READLINE_LIBRARY readline)
endif()
if(MORPH_READLINE_INCLUDE_DIR AND MORPH_READLINE_LIBRARY)
	include(CheckCSourceCompiles)
	set(CMAKE_REQUIRED_INCLUDES ${MORPH_READLINE_INCLUDE_DIR})
	set(CMAKE_REQUIRED_LIBRARIES ${MORPH_READLINE_LIBRARY})
	check_c_source_compiles([=[
#include <stdio.h>
#include <readline/readline.h>
int main(void)
{
	return rl_bind_keyseq("", 0);
}
]=] MORPH_HAVE_RL_BIND_KEYSEQ)
	check_c_source_compiles([=[
#include <stdio.h>
#include <readline/readline.h>
int main(void) { return rl_bracketed_paste_begin(1, 0); }
]=] MORPH_HAVE_RL_BRACKETED_PASTE_BEGIN)
	unset(CMAKE_REQUIRED_INCLUDES)
	unset(CMAKE_REQUIRED_LIBRARIES)
	target_include_directories(morph-cli PRIVATE ${MORPH_READLINE_INCLUDE_DIR})
	target_compile_definitions(morph-cli PRIVATE HAVE_READLINE=1)
	if(MORPH_HAVE_RL_BRACKETED_PASTE_BEGIN)
		target_compile_definitions(morph-cli PRIVATE HAVE_RL_BRACKETED_PASTE_BEGIN=1)
	endif()
	if(MORPH_HAVE_RL_BIND_KEYSEQ)
		target_compile_definitions(morph-cli PRIVATE HAVE_RL_BIND_KEYSEQ=1)
	endif()
	target_link_libraries(morph-cli PUBLIC ${MORPH_READLINE_LIBRARY})
	message(STATUS "readline found: ${MORPH_READLINE_LIBRARY}")
else()
	message(STATUS "readline not found, using fgets")
endif()
