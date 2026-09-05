#ifndef ASNGN_MCP_WORK_H
#define ASNGN_MCP_WORK_H
#include "asngn.h"
#include "json.h"
extern const char MCP_WORK_SCHEMA[];
asngn_err mcp_work_request(asngn_session *s, const jx_value *args, jx_value **out);
asngn_err mcp_work_status(asngn_session *s, uint64_t revision, jx_value *out);
#endif
