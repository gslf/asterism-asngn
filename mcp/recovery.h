#ifndef ASNGN_MCP_RECOVERY_H
#define ASNGN_MCP_RECOVERY_H
#include "asmodel_json.h"
#include "asngn.h"
asngn_err mcp_task_recover(asngn_session *session, const char *id, asmodel_json_value **out);
#endif
