cmake_minimum_required(VERSION 3.14)
project(opencode-cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- Options ----
option(OPENCODE_BUILD_TESTS "Build tests" OFF)

# ---- Third-party libraries ----
# httplib.h and json.hpp are in third_party/

# CURL: import pre-built library from parent project's 3party
add_library(curl_lib SHARED IMPORTED)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(curl_lib PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../3party/bin/x64/bin/libcurl.dll"
        IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/../3party/bin/x64/lib/libcurl_imp.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../3party/bin/x64/include"
    )
else()
    set_target_properties(curl_lib PROPERTIES
        IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/../3party/bin/win32/bin/libcurl.dll"
        IMPORTED_IMPLIB "${CMAKE_CURRENT_SOURCE_DIR}/../3party/bin/win32/lib/libcurl_imp.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../3party/bin/win32/include"
    )
endif()

# SQLite3: build from bundled amalgamation source
add_library(sqlite3_lib STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC third_party/sqlite)
if(WIN32)
    target_compile_definitions(sqlite3_lib PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- Source files ----
set(OPENCODE_SOURCES
    src/main.cpp
    src/util/logger.cpp
    src/util/uuid.cpp
    src/util/httplib_client.cpp
    src/database/database.cpp
    src/database/schema.cpp
    src/event/event_bus.cpp
    src/session/message.cpp
    src/session/session_manager.cpp
    src/session/session_prompt.cpp
    src/session/system_prompt.cpp
    src/session/retry.cpp
    src/session/compaction.cpp
    src/permission/permission.cpp
    src/snapshot/snapshot.cpp
    src/command/command.cpp
    src/agent/agent.cpp
    src/mcp/mcp_client.cpp
    src/mcp/mcp_service.cpp
    src/question/question.cpp
    src/skill/skill.cpp
    src/provider/provider_registry.cpp
    src/provider/openai/openai_provider.cpp
    src/provider/openai/openai_stream.cpp
    src/provider/anthropic/anthropic_provider.cpp
    src/provider/anthropic/anthropic_stream.cpp
    src/provider/cost.cpp
    src/tool/tool_registry.cpp
    src/tool/truncate.cpp
    src/tool/builtin/shell_tool.cpp
    src/tool/builtin/shell_common.cpp
    src/tool/builtin/read_tool.cpp
    src/tool/builtin/write_tool.cpp
    src/tool/builtin/edit_tool.cpp
    src/tool/builtin/glob_tool.cpp
    src/tool/builtin/grep_tool.cpp
    src/tool/builtin/task_tool.cpp
    src/tool/builtin/question_tool.cpp
    src/tool/builtin/skill_tool.cpp
    src/workspace/workspace.cpp
    src/sync/sync.cpp
    src/project/project.cpp
    src/server/server.cpp
    src/server/auth.cpp
    src/server/router.cpp
    src/server/sse.cpp
    src/server/middleware.cpp
    src/config/config.cpp
    src/memory/memory_store.cpp
    src/memory/memory_collector.cpp
    src/memory/memory_refiner.cpp
    src/memory/memory_retriever.cpp
    src/memory/memory_manager.cpp
    src/memory/memory_embedder.cpp
    src/memory/knowledge_graph.cpp
)

# ---- Header files (for IDE project views) ----
set(OPENCODE_HEADERS
    src/util/logger.h
    src/util/uuid.h
    src/util/httplib_client.h
    src/database/database.h
    src/database/schema.h
    src/event/event_bus.h
    src/event/event_types.h
    src/session/message.h
    src/session/session_manager.h
    src/session/session_prompt.h
    src/session/system_prompt.h
    src/session/retry.h
    src/session/compaction.h
    src/permission/permission.h
    src/snapshot/snapshot.h
    src/command/command.h
    src/agent/agent.h
    src/mcp/mcp_client.h
    src/mcp/mcp_service.h
    src/question/question.h
    src/skill/skill.h
    src/provider/provider.h
    src/provider/provider_registry.h
    src/provider/openai/openai_provider.h
    src/provider/openai/openai_stream.h
    src/provider/anthropic/anthropic_provider.h
    src/provider/anthropic/anthropic_stream.h
    src/provider/cost.h
    src/tool/tool.h
    src/tool/tool_registry.h
    src/tool/truncate.h
    src/tool/builtin/shell_tool.h
    src/tool/builtin/shell_common.h
    src/tool/builtin/read_tool.h
    src/tool/builtin/write_tool.h
    src/tool/builtin/edit_tool.h
    src/tool/builtin/glob_tool.h
    src/tool/builtin/grep_tool.h
    src/tool/builtin/task_tool.h
    src/tool/builtin/question_tool.h
    src/tool/builtin/skill_tool.h
    src/workspace/workspace.h
    src/sync/sync.h
    src/project/project.h
    src/server/server.h
    src/server/auth.h
    src/server/router.h
    src/server/sse.h
    src/server/middleware.h
    src/config/config.h
    src/memory/memory.h
    src/memory/memory_store.h
    src/memory/memory_collector.h
    src/memory/memory_refiner.h
    src/memory/memory_retriever.h
    src/memory/memory_manager.h
    src/memory/memory_embedder.h
    src/memory/knowledge_graph.h
    src/pty/pty.h
)

# Platform-specific headers (for IDE project views)
if(WIN32)
    list(APPEND OPENCODE_HEADERS
        src/tool/builtin/cmd_tool.h
        src/tool/builtin/powershell_tool.h
    )
endif()

# Platform-specific PTY source
if(WIN32)
    list(APPEND OPENCODE_SOURCES
        src/pty/pty_win.cpp
        src/tool/builtin/cmd_tool.cpp
        src/tool/builtin/powershell_tool.cpp
    )
else()
    list(APPEND OPENCODE_SOURCES src/pty/pty_unix.cpp)
endif()

# ---- Executable ----
add_executable(opencode-cpp ${OPENCODE_SOURCES} ${OPENCODE_HEADERS})

target_include_directories(opencode-cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# Link libraries
target_link_libraries(opencode-cpp PRIVATE
    curl_lib
)

# SQLite3 linking
target_link_libraries(opencode-cpp PRIVATE sqlite3_lib)

# Platform-specific
if(WIN32)
    target_link_libraries(opencode-cpp PRIVATE ws2_32)
    target_compile_definitions(opencode-cpp PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    if(MSVC)
        # Ensure UTF-8 encoding for source files and string literals
        target_compile_options(opencode-cpp PRIVATE /utf-8)
    endif()
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(opencode-cpp PRIVATE pthread)
endif()

# ---- Copy prompts/ directory to exe output dir after build ----
add_custom_command(TARGET opencode-cpp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/prompts"
        "$<TARGET_FILE_DIR:opencode-cpp>/prompts"
    COMMENT "Copying prompts/ to output directory"
)
