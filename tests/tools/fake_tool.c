/*
 * fake_tool.c — scripted oneshot protocol tool for the asngn integration
 * tests. Ported from astools' tests/tools/
 * tool_fake.c: one tiny xcdn-linked binary; the FIRST argv argument
 * selects the behavior:
 *
 *   echo   parse the #tool_request, respond ok with result = the args
 *          object it received (verbatim clone)
 *   sleep  parse {ms: N} from args, sleep, respond ok {}
 *   crash  read stdin, exit 3 without a response
 *   big    respond ok with result {data: "<8000 x's>"} so the engine's
 *          digestion path has something oversized to chew on
 *
 * Deliberately self-contained: it copies the minimal request parsing
 * inline (invocation_id + args are enough) instead of including any SDK,
 * so a defect elsewhere can never mask an engine defect. Responses are
 * hand-formatted printf of a compact #tool_response.
 */

#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#include "xcdn.h"

#define FAKE_BIG_BYTES 8000

/* ---- small helpers ------------------------------------------------------- */

static char *read_all_stdin(size_t *out_len) {
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  for (;;) {
    size_t got;
    if (len == cap) {
      char *nb;
      if (cap > (size_t)-1 / 2) {
        free(buf);
        return NULL;
      }
      cap *= 2;
      nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        return NULL;
      }
      buf = nb;
    }
    got = fread(buf + len, 1, cap - len, stdin);
    len += got;
    if (got == 0) break;
  }
  if (len == cap) {
    char *nb = realloc(buf, cap + 1);
    if (!nb) {
      free(buf);
      return NULL;
    }
    buf = nb;
  }
  buf[len] = '\0';
  if (out_len) *out_len = len;
  return buf;
}

static void sleep_ms(long ms) {
#if defined(_WIN32)
  Sleep((DWORD)ms);
#else
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0) {
    /* retry on EINTR with the remaining time */
  }
#endif
}

/* ---- minimal #tool_request access ---------------------------------------- */

static int node_has_tag(const xcdn_node_t *n, const char *name) {
  size_t i;
  if (!n) return 0;
  for (i = 0; i < n->tags_len; i++)
    if (n->tags[i].name && strcmp(n->tags[i].name, name) == 0) return 1;
  return 0;
}

/* First top-level #tool_request (or, leniently, the first object). */
static const xcdn_node_t *find_request(const xcdn_document_t *doc) {
  size_t i;
  for (i = 0; i < doc->values_len; i++)
    if (node_has_tag(doc->values[i], "tool_request")) return doc->values[i];
  for (i = 0; i < doc->values_len; i++) {
    const xcdn_node_t *n = doc->values[i];
    if (n && n->value && n->value->type == XCDN_VAL_OBJECT) return n;
  }
  return NULL;
}

static const char *req_id(const xcdn_node_t *req) {
  const xcdn_node_t *n;
  if (!req || !req->value) return NULL;
  n = xcdn_object_get(req->value, "invocation_id");
  if (!n || !n->value) return NULL;
  if (n->value->type != XCDN_VAL_UUID && n->value->type != XCDN_VAL_STRING)
    return NULL;
  return n->value->data.string;
}

/*
 * Detach the args node from the parsed request so it can be re-serialized
 * standalone (the xcdn serializer only accepts whole documents). The entry
 * keeps a NULL node, which every xcdn destructor tolerates.
 */
static xcdn_node_t *detach_args(const xcdn_node_t *req) {
  xcdn_value_t *obj;
  size_t i;
  if (!req || !req->value || req->value->type != XCDN_VAL_OBJECT) return NULL;
  obj = req->value;
  for (i = 0; i < obj->data.object.len; i++) {
    if (obj->data.object.entries[i].key &&
        strcmp(obj->data.object.entries[i].key, "args") == 0) {
      xcdn_node_t *n = obj->data.object.entries[i].node;
      obj->data.object.entries[i].node = NULL;
      return n;
    }
  }
  return NULL;
}

/* Serialize one owned node compact; consumes the node. NULL on failure. */
static char *node_to_compact(xcdn_node_t *owned) {
  xcdn_document_t *doc = xcdn_document_new();
  char *text;
  if (!doc) {
    xcdn_node_free(owned);
    return NULL;
  }
  xcdn_document_push_value(doc, owned); /* doc owns the node now */
  text = xcdn_to_string_compact(doc);
  xcdn_document_free(doc);
  return text;
}

/* ---- responses ----------------------------------------------------------- */

static const char FALLBACK_ID[] = "00000000-0000-4000-8000-000000000000";

static void respond_ok(const char *id, const char *result_text) {
  printf("#tool_response {protocol: 1, invocation_id: u\"%s\", ok: true, "
         "result: %s}\n",
         id != NULL ? id : FALLBACK_ID,
         result_text != NULL ? result_text : "{}");
  fflush(stdout);
}

/* ---- behaviors ----------------------------------------------------------- */

static int do_request_behavior(const char *behavior) {
  size_t len = 0;
  char *input = read_all_stdin(&len);
  xcdn_document_t *doc = NULL;
  const xcdn_node_t *req = NULL;
  const char *id = NULL;
  xcdn_error_t err;
  int rc = 0;

  memset(&err, 0, sizeof err);
  if (input != NULL) {
    doc = xcdn_parse_str(input, len, &err);
    if (doc) {
      req = find_request(doc);
      id = req_id(req);
    }
  }

  if (strcmp(behavior, "crash") == 0) {
    rc = 3; /* silent: no response, no stderr */
  } else if (strcmp(behavior, "sleep") == 0) {
    long ms = 0;
    if (req && req->value) {
      const xcdn_node_t *args = xcdn_object_get(req->value, "args");
      if (args && args->value && args->value->type == XCDN_VAL_OBJECT) {
        const xcdn_node_t *mn = xcdn_object_get(args->value, "ms");
        if (mn && mn->value && mn->value->type == XCDN_VAL_INT)
          ms = (long)mn->value->data.integer;
      }
    }
    if (ms < 0) ms = 0;
    sleep_ms(ms);
    respond_ok(id, "{}");
  } else if (strcmp(behavior, "big") == 0) {
    char *result = malloc(FAKE_BIG_BYTES + 32);
    if (result != NULL) {
      memcpy(result, "{data: \"", 8);
      memset(result + 8, 'x', FAKE_BIG_BYTES);
      memcpy(result + 8 + FAKE_BIG_BYTES, "\"}", 3);
      respond_ok(id, result);
      free(result);
    } else {
      respond_ok(id, "{}");
    }
  } else { /* echo */
    xcdn_node_t *args = detach_args(req);
    char *text = NULL;
    if (args != NULL) text = node_to_compact(args); /* consumes args */
    respond_ok(id, text);
    free(text);
  }

  xcdn_document_free(doc);
  free(input);
  return rc;
}

int main(int argc, char **argv) {
  const char *behavior = argc > 1 ? argv[1] : "echo";

  if (strcmp(behavior, "echo") == 0 || strcmp(behavior, "sleep") == 0 ||
      strcmp(behavior, "crash") == 0 || strcmp(behavior, "big") == 0)
    return do_request_behavior(behavior);

  fprintf(stderr, "fake_tool: unknown behavior '%s'\n", behavior);
  return 2;
}
