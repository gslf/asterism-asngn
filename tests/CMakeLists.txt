# asngn tests: one CTest executable per test_*.c, linked against
# asngn_static, using the in-house harness (asngn_test.h) and the scripted
# fakes (fakes.c). Golden values are inline C string literals.

set(ASNGN_TEST_PROGRAMS
  test_util
  test_time
  test_sha_uuid
  test_stream
  test_config
  test_log
  test_redact
  test_steps
  test_grammar
  test_route
  test_detail
  test_session
  test_ledger
  test_context_golden
  test_cache
  test_toolcache
  test_telemetry
  test_json
  test_engine_turns
  test_engine_guards
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

# test_json compiles the MCP-only codec directly (it is not in libasngn).
if(TARGET test_json)
  target_sources(test_json PRIVATE ../src/json.c)
endif()

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
foreach(t test_engine_turns test_engine_guards)
  if(TARGET ${t})
    target_sources(${t} PRIVATE fixtures.c)
    target_compile_definitions(${t} PRIVATE
      "ASNGN_TEST_TOOL_PATH=\"$<TARGET_FILE:asngn_fake_tool>\"")
    add_dependencies(${t} asngn_fake_tool)
  endif()
endforeach()
