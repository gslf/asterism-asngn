# asngn tests: one CTest executable per test_*.c, linked against
# asngn_static, using the in-house harness (asngn_test.h) and the scripted
# fakes (fakes.c). Golden values are inline C string literals.

set(ASNGN_TEST_PROGRAMS
  test_scheduler
  test_util
  test_time
  test_sha_uuid
  test_stream
  test_config
  test_log
  test_redact
  test_verification
  test_workspace_tree
  test_retrieval
  test_workspace_git
  test_work
  test_work_replay
  test_steps
  test_grammar
  test_route
  test_detail
  test_session
  test_task_record
  test_action_events
  test_operations
  test_ledger
  test_context_golden
  test_request_trace
  test_cache
  test_toolcache
  test_telemetry
  test_json
  test_engine_turns
  test_engine_guards
  test_tool_selection
  test_native_actions
  test_approvals
  test_approval_replay
  test_evidence
)

foreach(test_name ${ASNGN_TEST_PROGRAMS})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/unit/${test_name}.c)
    set(ASNGN_TEST_SRC unit/${test_name}.c)
  elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/integration/${test_name}.c)
    set(ASNGN_TEST_SRC integration/${test_name}.c)
  else()
    message(WARNING "test ${test_name} not written yet; skipped")
    continue()
  endif()
  add_executable(${test_name} ${ASNGN_TEST_SRC} fakes.c)
  target_include_directories(${test_name} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASNGN_ASPER_DIR}/include
    ${ASNGN_ASTOOLS_DIR}/include
    ${ASNGN_XCDN_DIR}/src)
  if(MSVC)
    target_compile_options(${test_name} PRIVATE /W4 /WX)
  else()
    target_compile_options(${test_name} PRIVATE -Wall -Wextra -Wpedantic
                           -Werror)
  endif()
  target_link_libraries(${test_name} PRIVATE asngn_static)
  if(ASNGN_SANITIZERS)
    # Sanitizer runtimes reserve a very large virtual address range.  The
    # integration fixture therefore runs its instrumented helper without
    # astools' RLIMIT_AS; the normal test build still exercises the sandbox.
    target_compile_definitions(${test_name} PRIVATE ASNGN_TEST_SANITIZERS)
  endif()
  add_test(NAME ${test_name} COMMAND ${test_name})
  set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endforeach()

if(ASNGN_WITH_LLAMA AND TARGET test_grammar)
  target_sources(test_grammar PRIVATE grammar_validate.cpp)
  target_include_directories(test_grammar PRIVATE ${ASNGN_ASPER_DIR}/deps/llama.cpp/src)
  target_compile_definitions(test_grammar PRIVATE ASNGN_TEST_LLAMA)
endif()

if(TARGET test_config)
  target_compile_definitions(test_config PRIVATE
    "ASNGN_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/..\"")
endif()

if(TARGET asngn-mcp)
  add_test(NAME test_mcp_protocol
    COMMAND ${CMAKE_COMMAND}
      "-DMCP_EXE=$<TARGET_FILE:asngn-mcp>"
      "-DMCP_ROOT_FLAG=--root"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/mcp_smoke.cmake")
  set_tests_properties(test_mcp_protocol PROPERTIES TIMEOUT 60)
endif()

add_test(NAME test_model_manifest
  COMMAND ${CMAKE_COMMAND}
    "-DMANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/../scripts/models.manifest.tsv"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/model_manifest_smoke.cmake")

# ── asngn_fake_tool: scripted protocol tool (xcdn only, like astools') ───────
# Spawned by the engine integration tests through the real astools registry;
# mirrors astools' tests/tools/tool_fake pattern.
add_executable(asngn_fake_tool tools/fake_tool.c)
target_include_directories(asngn_fake_tool PRIVATE ${ASNGN_XCDN_DIR}/src)
if(MSVC)
  target_compile_options(asngn_fake_tool PRIVATE /W4 /WX)
else()
  target_compile_options(asngn_fake_tool PRIVATE -Wall -Wextra -Wpedantic
                         -Werror)
endif()
target_link_libraries(asngn_fake_tool PRIVATE xcdn)

# The engine integration tests use the fixture writers (fixtures.c) and
# invoke the fake tool by absolute path.
foreach(t test_engine_turns test_engine_guards test_tool_selection test_native_actions test_approvals test_approval_replay test_evidence test_task_record test_action_events)
  if(TARGET ${t})
    target_sources(${t} PRIVATE fixtures.c)
    target_compile_definitions(${t} PRIVATE
      "ASNGN_TEST_TOOL_PATH=\"$<TARGET_FILE:asngn_fake_tool>\"")
    add_dependencies(${t} asngn_fake_tool)
  endif()
endforeach()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(TARGET asngn_acp_host)
  add_executable(test_acp_io acp/io.c)
  target_include_directories(test_acp_io PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(test_acp_io PRIVATE ${ASNGN_C_FLAGS})
  target_link_libraries(test_acp_io PRIVATE asngn_acp_host)
  add_test(NAME test_acp_io COMMAND test_acp_io)
  set_tests_properties(test_acp_io PROPERTIES TIMEOUT 20)
endif()
if(Python3_Interpreter_FOUND)
  if(TARGET asngn_acp_host)
    add_executable(asngn_acp_test_driver acp/driver.c fakes.c fixtures.c)
    target_include_directories(asngn_acp_test_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(asngn_acp_test_driver PRIVATE ${ASNGN_C_FLAGS})
    target_link_libraries(asngn_acp_test_driver PRIVATE asngn_acp_host)
    target_compile_definitions(asngn_acp_test_driver PRIVATE
      "ASNGN_TEST_TOOL_PATH=\"$<TARGET_FILE:asngn_fake_tool>\"")
    if(ASNGN_SANITIZERS)
      target_compile_definitions(asngn_acp_test_driver PRIVATE ASNGN_TEST_SANITIZERS)
    endif()
    add_dependencies(asngn_acp_test_driver asngn_fake_tool)
    add_test(NAME test_acp_protocol COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/acp/protocol.py $<TARGET_FILE:asngn-acp>
      $<TARGET_FILE:asngn_acp_test_driver>)
    set_tests_properties(test_acp_protocol PROPERTIES TIMEOUT 90)
    add_test(NAME test_acp_lifecycle COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/acp/lifecycle.py $<TARGET_FILE:asngn_acp_test_driver>)
    set_tests_properties(test_acp_lifecycle PROPERTIES TIMEOUT 90)
  endif()
  add_test(NAME test_runtime_archive COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_archive.py)
  set_tests_properties(test_runtime_archive PROPERTIES TIMEOUT 30)
  if(ASNGN_BUILD_DISTRIBUTION)
    add_test(NAME test_distribution COMMAND ${Python3_EXECUTABLE} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/distribution.py --build ${CMAKE_BINARY_DIR})
    set_tests_properties(test_distribution PROPERTIES TIMEOUT 90)
  endif()
  add_executable(asngn_operation_replay_probe operation_replay_probe.c)
  target_include_directories(asngn_operation_replay_probe PRIVATE ../src)
  target_link_libraries(asngn_operation_replay_probe PRIVATE asngn_static)
  add_executable(asngn_workspace_probe workspace_probe.c)
  target_include_directories(asngn_workspace_probe PRIVATE ../src)
  target_link_libraries(asngn_workspace_probe PRIVATE asngn_static)
  add_test(NAME test_workspace_git_live
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/workspace_git.py
      $<TARGET_FILE:asngn_workspace_probe>)
  set_tests_properties(test_workspace_git test_workspace_git_live PROPERTIES TIMEOUT 120)
  add_test(NAME test_release_admission
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_release.py)
  set_tests_properties(test_release_admission PROPERTIES TIMEOUT 120)
  if(TARGET asngn)
    add_test(NAME test_confirmation_tui
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/confirmation_tui.py
        $<TARGET_FILE:asngn>)
  endif()
  add_test(NAME test_quality_oracle
    COMMAND ${Python3_EXECUTABLE} -B -m unittest discover
      -s ${CMAKE_CURRENT_SOURCE_DIR}/quality -p "test_*.py")
  set_tests_properties(test_quality_oracle PROPERTIES TIMEOUT 120)
  if(TARGET asngn-mcp)
    add_test(NAME test_approval_mcp
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/approval_mcp.py
        $<TARGET_FILE:asngn-mcp>)
    add_test(NAME test_work_mcp
      COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/work_smoke.py
        $<TARGET_FILE:asngn-mcp>)
    # Imported CURL targets can be scoped to the asmodel subdirectory.
    get_target_property(ASNGN_MODEL_LINKS asmodel_static INTERFACE_LINK_LIBRARIES)
    if(WIN32 OR "CURL::libcurl" IN_LIST ASNGN_MODEL_LINKS)
      add_test(NAME test_native_wire
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/native_wire.py
          $<TARGET_FILE:asngn-mcp> $<TARGET_FILE:asngn_fake_tool>)
    endif()
  endif()
endif()

if(TARGET test_engine_turns)
  target_sources(test_engine_turns PRIVATE ../mcp/tasks.c ../mcp/work.c)
endif()

if(TARGET test_work)
  target_sources(test_work PRIVATE ../mcp/work.c)
endif()

foreach(target test_stream test_operations test_native_actions)
  if(TARGET ${target})
    target_sources(${target} PRIVATE wal_fixture.c)
  endif()
endforeach()

if(TARGET test_task_record)
  target_sources(test_task_record PRIVATE ../mcp/recovery.c ../mcp/work.c)
endif()

if(Python3_Interpreter_FOUND)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../sdk/tests ${CMAKE_CURRENT_BINARY_DIR}/sdk)
endif()
