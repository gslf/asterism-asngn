/* Whole-document collection is test inspection only; runtime replay is streaming. */
#ifndef ASNGN_TEST_WAL_FIXTURE_H
#define ASNGN_TEST_WAL_FIXTURE_H
#include "asngn.h"
struct xcdn_document;
asngn_err asngn_test_wal_load(asngn_ctx *ctx, const char *path, struct xcdn_document **out);
#endif
