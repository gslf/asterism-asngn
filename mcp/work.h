#ifndef ASNGN_MCP_WORK_H
#define ASNGN_MCP_WORK_H
#include "asngn.h"
#include "asmodel_json.h"
extern const char MCP_WORK_SCHEMA[];
asngn_err mcp_work_request(asngn_session *s, const asmodel_json_value *args, asmodel_json_value **out);
asngn_err mcp_work_status(asngn_session *s, uint64_t revision, asmodel_json_value *out);
#endif
