if(NOT DEFINED MCP_EXE OR NOT DEFINED MCP_ROOT_FLAG)
  message(FATAL_ERROR "MCP_EXE and MCP_ROOT_FLAG are required")
endif()
set(root "${CMAKE_CURRENT_BINARY_DIR}/mcp-smoke-root")
set(input "${CMAKE_CURRENT_BINARY_DIR}/mcp-smoke-input.jsonl")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${input}"
  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"server/discover\",\"params\":{}}\n"
  "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
  "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}\n"
  "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2099-01-01\",\"capabilities\":{},\"clientInfo\":{\"name\":\"test\",\"version\":\"1\"}}}\n"
  "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\",\"params\":{\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\"}}}\n")
execute_process(
  COMMAND "${MCP_EXE}" "${MCP_ROOT_FLAG}" "${root}"
  INPUT_FILE "${input}"
  OUTPUT_VARIABLE output
  ERROR_VARIABLE errors
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "MCP exited ${rc}: ${errors}")
endif()
foreach(fragment
    "\"supportedVersions\":[\"2026-07-28\",\"2025-06-18\"]"
    "\"protocolVersion\":\"2025-06-18\""
    "\"code\":-32022"
    "\"resultType\":\"complete\""
    "io.modelcontextprotocol/serverInfo")
  string(FIND "${output}" "${fragment}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "missing ${fragment} in MCP output: ${output}")
  endif()
endforeach()
