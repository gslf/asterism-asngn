#include "asngn_test.h"
#include "wal_fixture.h"
#include "asngn_internal.h"
#include "xcdn.h"
#include "operation_record.h"
#include "asmodel_json.h"

static asngn_ctx *open_fixture(char root[256]) {
  asngn_ctx *c = calloc(1,sizeof *c);
  if (!c || !asngn_test_tmpdir(root)) { free(c); return NULL; }
  c->root = asngn_strdup(root);
  os_rwlock_init(&c->lock); os_mutex_init(&c->err_mu);
  return c;
}
static void close_fixture(asngn_ctx *c, const char *root) {
  os_mutex_destroy(&c->err_mu); os_rwlock_destroy(&c->lock);
  free(c->root); free(c); asngn_test_rmtree(root);
}

TEST(reservations_survive_failure_and_reopen) {
  char root[256], lock_path[512];
  asngn_ctx *c = calloc(1, sizeof *c);
  asngn_operation failed, passed, pending;
  FILE *lock;
  ASSERT_TRUE(c && asngn_test_tmpdir(root));
  c->root = asngn_strdup(root);
  os_rwlock_init(&c->lock);
  os_mutex_init(&c->err_mu);
  ASSERT_OK(asngn_operations_load(c));
  ASSERT_OK(asngn_operation_begin(c, "model", "answer", "cancelled-request", 100, &failed));
  ASSERT_OK(asngn_operation_end(c, &failed, 0, 0, false, ASNGN_ERR_CANCELLED));
  ASSERT_OK(asngn_operation_begin(c, "model", "judge", "judge-request", 50, &passed));
  ASSERT_OK(asngn_operation_end(c, &passed, 10, 5, true, ASNGN_OK));
  ASSERT_ERR(asngn_operation_end(c, &passed, 10, 5, true, ASNGN_OK), ASNGN_ERR_INVALID);
  ASSERT_OK(asngn_operation_begin(c, "model", "answer", NULL, 25, &pending));
  ASSERT_EQ_INT(c->consumption.today.charged_tokens, 140);
  char *path = os_path_join(root,"operations.xcdn");
  xcdn_document_t *doc = NULL;
  ASSERT_TRUE(path);
  ASSERT_OK(asngn_test_wal_load(c,path,&doc));
  ASSERT_TRUE(doc && doc->values_len == 5);
  const char *expected[] = {"cancelled-request","cancelled-request","judge-request","judge-request",""};
  for (size_t i = 0; i < 5; i++)
    ASSERT_EQ_STR(asngn_xstr(asngn_xfield(doc->values[i]->value,"request_id")),expected[i]);
  free(path); xcdn_document_free(doc);
  c->consumption.today.charged_tokens = 0;
  ASSERT_OK(asngn_operations_load(c));
  ASSERT_EQ_INT(c->consumption.today.charged_tokens, 140);
  c->cfg.daily_tokens = 150;
  ASSERT_ERR(asngn_operation_begin(c, "model", "answer", NULL, 20, &pending), ASNGN_ERR_LIMIT);
  snprintf(lock_path, sizeof lock_path, "%s/lock", root);
  lock = os_store_lock(lock_path);
  ASSERT_TRUE(lock != NULL);
  ASSERT_TRUE(os_store_lock(lock_path) == NULL);
  fclose(lock);
  lock = os_store_lock(lock_path);
  ASSERT_TRUE(lock != NULL);
  fclose(lock);
  os_mutex_destroy(&c->err_mu);
  os_rwlock_destroy(&c->lock);
  free(c->root); free(c);
  asngn_test_rmtree(root);
}
TEST(replay_rejects_reassigned_operations_and_ambiguous_metadata) {
  const char *changes[] = {"request_id", "model", "kind", "schema", "duplicate", "outcome"};
  for (size_t test = 0; test < 6; test++) {
    char root[256], payload[1024];
    asngn_ctx *c = open_fixture(root);
    asngn_operation op;
    ASSERT_TRUE(c); ASSERT_OK(asngn_operations_load(c));
    ASSERT_OK(asngn_operation_begin(c,"model","answer","request",100,&op));
    snprintf(payload,sizeof payload,"{\"schema\":%d,\"id\":\"%s\",\"request_id\":\"%s\","
        "\"model\":\"%s\",\"kind\":\"%s\",\"state\":\"settled\",\"day\":%lld,\"budget_delta\":-90,"
        "\"input_tokens\":10,\"output_tokens\":0,\"usage_known\":true,\"outcome\":\"%s\"%s}",
        !strcmp(changes[test],"schema") ? 1 : 2,op.id,
        !strcmp(changes[test],"request_id") ? "different" : "request",
        !strcmp(changes[test],"model") ? "different" : "model",
        !strcmp(changes[test],"kind") ? "different" : "answer",(long long)op.day,
        !strcmp(changes[test],"outcome") ? "invented" : "ASNGN_OK",
        !strcmp(changes[test],"duplicate") ? ",\"request_id\":\"request\"" : "");
    char *path = os_path_join(root,"operations.xcdn");
    asngn_stream stream;
    ASSERT_TRUE(path); ASSERT_OK(asngn_stream_open(c,&stream,path,true));
    ASSERT_OK(asngn_wal_append(c,&stream,payload,strlen(payload)));
    asngn_stream_close(&stream);
    FILE *tail = os_fopen(path,"ab");
    ASSERT_TRUE(tail); ASSERT_TRUE(fputs("// incomplete",tail) >= 0); fclose(tail);
    uint64_t before, after;
    ASSERT_OK(os_file_size(path,&before));
    c->consumption.today.charged_tokens = 777;
    ASSERT_ERR(asngn_operations_load(c),ASNGN_ERR_PARSE);
    ASSERT_TRUE(c->usage_recovery_required);
    ASSERT_EQ_INT(c->consumption.today.charged_tokens,777); /* Failed replay cannot publish a prefix. */
    ASSERT_OK(os_file_size(path,&after)); ASSERT_EQ_INT(before,after);
    free(path);
    close_fixture(c,root);
  }
}

TEST(reservations_do_not_overflow_and_copy_request_identity) {
  char root[256], request[129];
  asngn_ctx *c = open_fixture(root);
  asngn_operation op, next;
  ASSERT_TRUE(c); ASSERT_OK(asngn_operations_load(c));
  memset(request,'x',128); request[128] = 0;
  ASSERT_OK(asngn_operation_begin(c,"model","answer",request,INT64_MAX,&op));
  request[0] = 'y';
  ASSERT_EQ_INT(op.request_id[0],'x');
  ASSERT_ERR(asngn_operation_begin(c,"model","answer",NULL,1,&next),ASNGN_ERR_LIMIT);
  ASSERT_EQ_INT(c->consumption.today.charged_tokens,INT64_MAX);
  ASSERT_OK(asngn_operation_end(c,&op,10,5,true,ASNGN_ERR_MODEL));
  ASSERT_EQ_INT(c->consumption.today.charged_tokens,15);
  ASSERT_OK(asngn_operations_load(c)); ASSERT_EQ_INT(c->consumption.today.charged_tokens,15);
  ASSERT_ERR(asngn_operation_begin(c,"model","answer","",1,&next),ASNGN_ERR_INVALID);
  ASSERT_ERR(asngn_operation_begin(c,"model","answer",NULL,-1,&next),ASNGN_ERR_INVALID);
  close_fixture(c,root);
}

static int fail_sync(void *ud, const char *point) { (void)ud; return !strcmp(point,"stream_sync"); }
TEST(uncertain_settlement_blocks_further_appends_until_recovery) {
  char root[256];
  asngn_ctx *c = open_fixture(root);
  asngn_operation first, second;
  ASSERT_TRUE(c); ASSERT_OK(asngn_operations_load(c));
  ASSERT_OK(asngn_operation_begin(c,"model","answer","first",100,&first));
  ASSERT_OK(asngn_operation_begin(c,"model","answer","second",100,&second));
  c->fault = fail_sync;
  ASSERT_ERR(asngn_operation_end(c,&first,2,1,true,ASNGN_OK),ASNGN_ERR_IO);
  c->fault = NULL;
  char *path = os_path_join(root,"operations.xcdn");
  uint64_t before, after;
  ASSERT_TRUE(path); ASSERT_OK(os_file_size(path,&before));
  ASSERT_ERR(asngn_operation_end(c,&second,5,1,true,ASNGN_OK),ASNGN_ERR_IO);
  ASSERT_OK(os_file_size(path,&after)); ASSERT_EQ_INT(before,after);
  ASSERT_OK(asngn_operations_load(c)); ASSERT_EQ_INT(c->consumption.today.charged_tokens,103);
  free(path); close_fixture(c,root);
}

TEST(streaming_replay_preserves_interleaved_reservations_across_growth) {
  char root[256];
  asngn_ctx *c = open_fixture(root);
  asngn_operation *ops = calloc(600,sizeof *ops);
  ASSERT_TRUE(c && ops); ASSERT_OK(asngn_operations_load(c));
  for (size_t i = 0; i < 600; i++) {
    char request[32]; snprintf(request,sizeof request,"request-%zu",i);
    ASSERT_OK(asngn_operation_begin(c,"model","answer",request,100,&ops[i]));
  }
  for (size_t i = 600; i > 0; i--)
    ASSERT_OK(asngn_operation_end(c,&ops[i-1],5,5,i%2 != 0,ASNGN_ERR_CANCELLED));
  ASSERT_EQ_INT(c->consumption.today.charged_tokens,33000);
  ASSERT_OK(asngn_operations_load(c)); ASSERT_EQ_INT(c->consumption.today.charged_tokens,33000);
  free(ops); close_fixture(c,root);
}

TEST(operation_records_preserve_strict_scalar_boundaries) {
  asngn_operation op = {.model="model",.kind="answer"};
  asngn_operation_record record;
  asngn_uuid_v4(op.id); strcpy(op.request_id,"request");
  char *text = asngn_operation_encode(&op,"reserved",100,0,0,false,ASNGN_OK);
  ASSERT_TRUE(text); ASSERT_OK(asngn_operation_decode(text,strlen(text),&record));
  const char *keys[] = {"request_id","model","usage_known","input_tokens","output_tokens","schema","day","day","day","outcome","extra"};
  const char *values[] = {"\"request\\u0000hidden\"","\"model\\u0000hidden\"","0","-1","2147483648","2.5","false","9223372036854775807","-9223372036854775808","\"ASNGN_ERR_MODEL\"","null"};
  for (size_t i = 0; i < sizeof keys/sizeof keys[0]; i++) {
    asmodel_json_value *v = NULL, *value = NULL;
    ASSERT_EQ_INT(asmodel_json_parse(text,strlen(text),&v),0);
    ASSERT_EQ_INT(asmodel_json_parse(values[i],strlen(values[i]),&value),0);
    ASSERT_EQ_INT(asmodel_json_object_set(v,keys[i],value),0);
    char *bad = asmodel_json_write(v,0);
    ASSERT_TRUE(bad); ASSERT_ERR(asngn_operation_decode(bad,strlen(bad),&record),ASNGN_ERR_PARSE);
    free(bad); asmodel_json_free(v);
  }
  char oversized[4097]; memset(oversized,' ',sizeof oversized);
  ASSERT_ERR(asngn_operation_decode(oversized,sizeof oversized,&record),ASNGN_ERR_LIMIT);
  free(text);
}

TEST_LIST = {TEST_ENTRY(reservations_survive_failure_and_reopen),
  TEST_ENTRY(replay_rejects_reassigned_operations_and_ambiguous_metadata),
  TEST_ENTRY(reservations_do_not_overflow_and_copy_request_identity),
  TEST_ENTRY(uncertain_settlement_blocks_further_appends_until_recovery),
  TEST_ENTRY(streaming_replay_preserves_interleaved_reservations_across_growth),
  TEST_ENTRY(operation_records_preserve_strict_scalar_boundaries)};
RUN_ALL_TESTS()
