add_library(morph-persistence STATIC
	credit_store.c
	memory_store.c
	preferences.c
)

target_include_directories(morph-persistence PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(morph-persistence PUBLIC morph-util morph-db morph-session)
