/* Acceptance is an executable contract, independent of answer text and commits. */
#include "asngn_test.h"
#include "work_state.h"
#include "mcp/work.h"
#include <string.h>

typedef struct { char dir[256], workspace[256]; asngn_ctx *ctx; asngn_session s; } fixture;
static bool open_fixture(fixture *f) {
  memset(f,0,sizeof *f);
  if (!asngn_test_tmpdir(f->dir) || !asngn_test_tmpdir(f->workspace)) return false;
  f->ctx = calloc(1,sizeof *f->ctx);
  if (!f->ctx) return false;
  os_mutex_init(&f->ctx->err_mu); os_mutex_init(&f->ctx->log_mu);
  f->ctx->clock = asngn_clock_system();
  f->s.ctx = f->ctx; f->s.dir = f->dir; os_rwlock_init(&f->s.lock);
  snprintf(f->s.workspace.canonical_root,sizeof f->s.workspace.canonical_root,"%s",f->workspace);
  snprintf(f->s.workspace.repository_root,sizeof f->s.workspace.repository_root,"%s",f->workspace);
  return asngn_work_load(&f->s) == ASNGN_OK;
}
static void close_fixture(fixture *f) {
  asngn_work_free(f->s.work); os_rwlock_destroy(&f->s.lock);
  os_mutex_destroy(&f->ctx->err_mu); os_mutex_destroy(&f->ctx->log_mu); free(f->ctx);
  asngn_test_rmtree(f->dir); asngn_test_rmtree(f->workspace);
}
static asngn_work_definition definition(void) {
  asngn_work_definition d = {0};
  strcpy(d.goal,"Repair recursive configuration merging");
  strcpy(d.constraints,"Do not modify the acceptance suite.");
  d.count = 2;
  for (size_t i = 0; i < d.count; i++) {
    strcpy(d.criteria[i].id,i ? "regression" : "build");
    strcpy(d.criteria[i].requirement,i ? "Protected regression suite passes" : "Sources compile");
    strcpy(d.criteria[i].command,i ? "test" : "build");
    strcpy(d.criteria[i].path,"."); strcpy(d.criteria[i].adapter,"cmake");
    d.criteria[i].depends_on = i ? 1 : 0;
  }
  return d;
}
static asngn_err observe(fixture *f, const char *command, const char *status, int collected) {
  asngn_turn_state t = {0};
  asngn_workspace_info ws = f->s.workspace;
  asngn_err e = asngn_workspace_snapshot(&ws,NULL);
  if (e != ASNGN_OK) return e;
  t.s = &f->s; asngn_uuid_v4(t.action_id);
  asngn_buf b; asngn_buf_init(&b);
  e = asngn_buf_printf(&b,"{verification_schema:1, verification_status:\"%s\", exit_code:%d, "
      "action:\"%s\",cwd:\"%s\",adapter:\"cmake\",tests_collected:%d,tests_skipped:0}",
      status,!strcmp(status,"failed"),command,f->workspace,collected);
  if (e == ASNGN_OK) e = asngn_work_observe(&t,"project",command,"{}",true,b.data,ws.fingerprint,ws.fingerprint);
  asngn_buf_free(&b); return e;
}
static int proof_is(fixture *f, size_t i, asngn_proof_status status, int complete) {
  asngn_work_state *w = NULL;
  int ok = asngn_session_work_get(&f->s,&w) == ASNGN_OK &&
      w->proofs[i].status == status && w->succeeded == complete;
  free(w); return ok;
}
TEST(acceptance_dependencies_and_recovery) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  asngn_work_definition d = definition();
  asngn_work_state *empty = NULL;
  ASSERT_ERR(asngn_session_work_get(&f.s,&empty),ASNGN_ERR_NOT_FOUND);
  ASSERT_OK(asngn_session_work_define(&f.s,0,&d));
  ASSERT_ERR(asngn_session_work_define(&f.s,0,&d),ASNGN_ERR_BUSY);
  ASSERT_OK(observe(&f,"test","passed",3));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_BLOCKED,0));
  ASSERT_OK(observe(&f,"build","passed",0));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_BLOCKED,0));
  ASSERT_OK(observe(&f,"test","passed",3));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_PASSED,1));
  asngn_work_free(f.s.work); f.s.work = NULL;
  ASSERT_OK(asngn_work_load(&f.s));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_PASSED,1));
  ASSERT_OK(observe(&f,"build","failed",0));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_BLOCKED,0));
  ASSERT_OK(observe(&f,"build","passed",0));
  ASSERT_OK(observe(&f,"test","passed",0));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_INCONCLUSIVE,0));
  close_fixture(&f);
}
TEST(revision_and_external_changes) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  asngn_work_definition d = definition();
  ASSERT_OK(asngn_session_work_define(&f.s,0,&d));
  ASSERT_OK(observe(&f,"build","passed",0)); ASSERT_OK(observe(&f,"test","passed",3));
  strcpy(d.goal,"A clearer display title");
  ASSERT_OK(asngn_session_work_define(&f.s,1,&d));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_PASSED,1));
  strcpy(d.criteria[1].requirement,"Expanded regression requirement");
  ASSERT_OK(asngn_session_work_define(&f.s,2,&d));
  ASSERT_TRUE(proof_is(&f,0,ASNGN_PROOF_PASSED,0));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_NOT_RUN,0));
  ASSERT_OK(observe(&f,"test","passed",3));
  char path[512]; snprintf(path,sizeof path,"%s/changed.c",f.workspace);
  ASSERT_OK(os_write_file(path,"changed",7));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_STALE,0));
  ASSERT_OK(observe(&f,"build","passed",0)); ASSERT_OK(observe(&f,"test","passed",3));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_PASSED,1));
  ASSERT_OK(asngn_session_work_invalidate(&f.s,3));
  ASSERT_TRUE(proof_is(&f,0,ASNGN_PROOF_NOT_RUN,0));
  d.criteria[0].depends_on = 2;
  ASSERT_ERR(asngn_session_work_define(&f.s,4,&d),ASNGN_ERR_INVALID);
  d.criteria[0].depends_on = 0; strcpy(d.criteria[0].path,"../outside");
  ASSERT_ERR(asngn_session_work_define(&f.s,4,&d),ASNGN_ERR_INVALID);
  close_fixture(&f);
}
static int fail(void *ud, const char *point) { return !strcmp(ud,point); }
TEST(incomplete_receipts_and_io_failure) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  asngn_work_definition d = definition();
  ASSERT_OK(asngn_session_work_define(&f.s,0,&d));
  ASSERT_OK(observe(&f,"build","passed",0)); ASSERT_OK(observe(&f,"test","passed",3));
  asngn_turn_state t = {0}; t.s = &f.s; asngn_uuid_v4(t.action_id);
  asngn_workspace_info ws = f.s.workspace; ASSERT_OK(asngn_workspace_snapshot(&ws,NULL));
  ASSERT_OK(asngn_work_observe(&t,"fs","read","{path:\"test.c\"}",true,
      "{verification_status:\"passed\"}",ws.fingerprint,ws.fingerprint));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_PASSED,1));
  ASSERT_OK(asngn_work_observe(&t,"project","test","{}",true,
      "{verification_status:\"passed\"}",ws.fingerprint,ws.fingerprint));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_INCONCLUSIVE,0));
  ASSERT_OK(observe(&f,"test","passed",3));
  f.ctx->fault = fail; f.ctx->fault_ud = "stream_short_write";
  ASSERT_ERR(asngn_session_work_invalidate(&f.s,1),ASNGN_ERR_IO);
  asngn_work_state *state = NULL;
  ASSERT_ERR(asngn_session_work_get(&f.s,&state),ASNGN_ERR_IO);
  f.ctx->fault = NULL; f.s.recovery_required = false;
  asngn_work_free(f.s.work); f.s.work = NULL; ASSERT_OK(asngn_work_load(&f.s));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_PASSED,1));
  ASSERT_OK(asngn_work_revoke(&f.s));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_STALE,0));
  asngn_work_free(f.s.work); f.s.work = NULL; ASSERT_OK(asngn_work_load(&f.s));
  ASSERT_TRUE(proof_is(&f,1,ASNGN_PROOF_STALE,0));
  close_fixture(&f);
}
TEST(host_contract_transport) {
  fixture f; ASSERT_TRUE(open_fixture(&f));
  jx_value *schema = NULL, *args = NULL, *out = NULL;
  ASSERT_EQ_INT(jx_parse(MCP_WORK_SCHEMA,strlen(MCP_WORK_SCHEMA),&schema),0);
  jx_free(schema);
  const char *input = "{\"mode\":\"define\",\"expected_revision\":0,\"definition\":{"
      "\"goal\":\"Verifica Unicode è valida\",\"constraints\":\"\",\"criteria\":[{"
      "\"id\":\"regression\",\"requirement\":\"Regression passes\",\"command\":\"test\","
      "\"adapter\":\"cmake\",\"path\":\".\",\"depends_on\":0}]}}";
  ASSERT_EQ_INT(jx_parse(input,strlen(input),&args),0);
  ASSERT_OK(mcp_work_request(&f.s,args,&out));
  ASSERT_EQ_STR(jx_string_value(jx_object_get(out,"task_state")),"incomplete");
  jx_free(out); out = NULL;
  ASSERT_ERR(mcp_work_request(&f.s,args,&out),ASNGN_ERR_BUSY);
  ASSERT_EQ_INT(jx_object_set(args,"proof",jx_string("passed")),0);
  ASSERT_ERR(mcp_work_request(&f.s,args,&out),ASNGN_ERR_INVALID);
  jx_free(args);
  ASSERT_OK(observe(&f,"test","passed",3));
  out = jx_object(); ASSERT_OK(mcp_work_status(&f.s,1,out));
  ASSERT_EQ_STR(jx_string_value(jx_object_get(out,"task_state")),"succeeded");
  ASSERT_OK(asngn_session_work_invalidate(&f.s,1));
  ASSERT_OK(mcp_work_status(&f.s,1,out));
  ASSERT_EQ_STR(jx_string_value(jx_object_get(out,"task_state")),"superseded");
  jx_free(out); close_fixture(&f);
}
TEST_LIST = {TEST_ENTRY(host_contract_transport), TEST_ENTRY(acceptance_dependencies_and_recovery),
  TEST_ENTRY(revision_and_external_changes), TEST_ENTRY(incomplete_receipts_and_io_failure)};
RUN_ALL_TESTS()
