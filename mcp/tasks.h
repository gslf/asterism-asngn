#ifndef ASNGN_MCP_TASKS_H
#define ASNGN_MCP_TASKS_H
#include "asngn.h"
#include "asmodel_json.h"
typedef struct mcp_job mcp_job;
asngn_err mcp_job_submit(asngn_session *session, const char *message, mcp_job **out);
const char *mcp_job_id(const mcp_job *job);
asngn_err mcp_job_poll(mcp_job *job, unsigned long long cursor, asmodel_json_value **out);
asngn_err mcp_job_cancel(mcp_job *job);
void mcp_job_free(mcp_job *job);
int mcp_job_uses_session(const mcp_job *job, const asngn_session *session);
#endif
