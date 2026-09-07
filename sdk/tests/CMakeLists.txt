# SDKs are optional consumers; configuring the C core never downloads packages.
if(Python3_VERSION VERSION_LESS 3.11)
  message(STATUS "SDK checks need Python 3.11+; interpreter is ${Python3_VERSION}")
  return()
endif()
add_test(NAME test_sdk_python COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_python.py)
set_tests_properties(test_sdk_python PROPERTIES TIMEOUT 60)
find_program(ASNGN_NODE NAMES node)
if(ASNGN_NODE)
  execute_process(COMMAND ${ASNGN_NODE} -p process.versions.node OUTPUT_VARIABLE ASNGN_NODE_VERSION
                  OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE ASNGN_NODE_RESULT)
  if(NOT ASNGN_NODE_RESULT EQUAL 0 OR ASNGN_NODE_VERSION VERSION_LESS 22)
    message(STATUS "SDK JavaScript checks need Node 22+; found ${ASNGN_NODE_VERSION}")
    set(ASNGN_NODE "")
  endif()
endif()
if(ASNGN_NODE)
  add_test(NAME test_sdk_javascript COMMAND ${CMAKE_COMMAND} -E env
    "ASTERISM_TEST_PYTHON=${Python3_EXECUTABLE}" ${ASNGN_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/test_javascript.mjs)
  set_tests_properties(test_sdk_javascript PROPERTIES TIMEOUT 60)
  # npm's extensionless .bin/tsc shim is not a Windows executable. Use the
  # installed JavaScript entry point through the Node executable on every OS.
  set(ASNGN_TSC_JS "${CMAKE_CURRENT_SOURCE_DIR}/../node_modules/typescript/bin/tsc")
  if(EXISTS "${ASNGN_TSC_JS}")
    add_test(NAME test_sdk_types COMMAND ${ASNGN_NODE} ${ASNGN_TSC_JS} --noEmit --strict --module nodenext
      --target es2022 --lib es2022,dom,esnext.disposable ${CMAKE_CURRENT_SOURCE_DIR}/types.mts)
  else()
    message(STATUS "SDK TypeScript check skipped: install sdk dev dependencies to enable it")
  endif()
else()
  message(STATUS "SDK JavaScript checks skipped: Node 22+ unavailable")
endif()
if(TARGET asngn-mcp)
  add_test(NAME test_sdk_consumption COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/consumption_live.py $<TARGET_FILE:asngn-mcp> ${ASNGN_NODE})
  set_tests_properties(test_sdk_consumption PROPERTIES TIMEOUT 60)
  add_test(NAME test_sdk_recovery COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/recovery_live.py $<TARGET_FILE:asngn-mcp> ${ASNGN_NODE})
  set_tests_properties(test_sdk_recovery PROPERTIES TIMEOUT 60)
endif()
if(TARGET asngn-mcp AND (WIN32 OR "CURL::libcurl" IN_LIST ASNGN_MODEL_LINKS))
  add_test(NAME test_sdk_python_live COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/python_live.py
    $<TARGET_FILE:asngn-mcp> $<TARGET_FILE:asngn_fake_tool>)
  set_tests_properties(test_sdk_python_live PROPERTIES TIMEOUT 60)
  if(ASNGN_NODE)
    add_test(NAME test_sdk_javascript_live COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/javascript_live.py
      $<TARGET_FILE:asngn-mcp> ${ASNGN_NODE} $<TARGET_FILE:asngn_fake_tool>)
    set_tests_properties(test_sdk_javascript_live PROPERTIES TIMEOUT 60)
  endif()
endif()
