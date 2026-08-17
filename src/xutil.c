/*
 * xutil.c — checked xCDN AST builders and typed field accessors.
 *
 * xcdn-c's void mutators cannot report allocation failure, so growth and
 * key duplication are done here over the public AST structs (the journal.c
 * discipline of the siblings): every builder pre-grows with realloc and
 * consumes/frees its value/node argument on failure, so durable writes are
 * never silently dropped. Keys in our writers are unique by construction,
 * so replace semantics (xcdn_object_set) are deliberately avoided.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_internal.h"

#include <stdlib.h>
#include <string.h>

#include "xcdn.h"

/* ---- checked builders ---------------------------------------------------- */

bool asngn_xobj_put_node(xcdn_value_t *obj, const char *key,
                         xcdn_node_t *node) {
  char *k;
  if (!obj || !node) {
    xcdn_node_free(node);
    return false;
  }
  if (obj->data.object.len >= obj->data.object.cap) {
    size_t ncap = obj->data.object.cap ? obj->data.object.cap * 2 : 8;
    xcdn_object_entry_t *ne = (xcdn_object_entry_t *)realloc(
        obj->data.object.entries, ncap * sizeof(*ne));
    if (!ne) {
      xcdn_node_free(node);
      return false;
    }
    obj->data.object.entries = ne;
    obj->data.object.cap = ncap;
  }
  k = asngn_strdup(key);
  if (!k) {
    xcdn_node_free(node);
    return false;
  }
  obj->data.object.entries[obj->data.object.len].key = k;
  obj->data.object.entries[obj->data.object.len].node = node;
  obj->data.object.len++;
  return true;
}

bool asngn_xobj_put(xcdn_value_t *obj, const char *key, xcdn_value_t *val) {
  xcdn_node_t *node;
  if (!val) return false;
  node = xcdn_node_new(val);
  if (!node) {
    xcdn_value_free(val);
    return false;
  }
  return asngn_xobj_put_node(obj, key, node);
}

bool asngn_xarr_push(xcdn_value_t *arr, xcdn_value_t *val) {
  xcdn_node_t *node;
  if (!arr || !val) {
    if (val) xcdn_value_free(val);
    return false;
  }
  node = xcdn_node_new(val);
  if (!node) {
    xcdn_value_free(val);
    return false;
  }
  if (arr->data.array.len >= arr->data.array.cap) {
    size_t ncap = arr->data.array.cap ? arr->data.array.cap * 2 : 8;
    xcdn_node_t **ni =
        (xcdn_node_t **)realloc(arr->data.array.items, ncap * sizeof(*ni));
    if (!ni) {
      xcdn_node_free(node);
      return false;
    }
    arr->data.array.items = ni;
    arr->data.array.cap = ncap;
  }
  arr->data.array.items[arr->data.array.len++] = node;
  return true;
}

bool asngn_xnode_tag(xcdn_node_t *node, const char *name) {
  char *n;
  if (!node || !name) return false;
  if (node->tags_len >= node->tags_cap) {
    size_t ncap = node->tags_cap ? node->tags_cap * 2 : 2;
    xcdn_tag_t *nt = (xcdn_tag_t *)realloc(node->tags, ncap * sizeof(*nt));
    if (!nt) return false;
    node->tags = nt;
    node->tags_cap = ncap;
  }
  n = asngn_strdup(name);
  if (!n) return false;
  node->tags[node->tags_len].name = n;
  node->tags_len++;
  return true;
}

bool asngn_xdoc_push(xcdn_document_t *doc, xcdn_node_t *node) {
  if (!doc || !node) {
    xcdn_node_free(node);
    return false;
  }
  if (doc->values_len >= doc->values_cap) {
    size_t ncap = doc->values_cap ? doc->values_cap * 2 : 2;
    xcdn_node_t **nv =
        (xcdn_node_t **)realloc(doc->values, ncap * sizeof(*nv));
    if (!nv) {
      xcdn_node_free(node);
      return false;
    }
    doc->values = nv;
    doc->values_cap = ncap;
  }
  doc->values[doc->values_len++] = node;
  return true;
}

/* ---- typed field access -------------------------------------------------- */

const xcdn_value_t *asngn_xfield(const xcdn_value_t *obj, const char *key) {
  size_t i;
  if (!obj || !key || obj->type != XCDN_VAL_OBJECT) return NULL;
  for (i = 0; i < obj->data.object.len; i++) {
    const xcdn_object_entry_t *e = &obj->data.object.entries[i];
    if (e->key && strcmp(e->key, key) == 0)
      return e->node ? e->node->value : NULL;
  }
  return NULL;
}

const char *asngn_xstr(const xcdn_value_t *v) {
  if (!v || v->type != XCDN_VAL_STRING) return NULL;
  return v->data.string;
}

bool asngn_xint(const xcdn_value_t *v, int64_t *out) {
  if (!v || !out || v->type != XCDN_VAL_INT) return false;
  *out = v->data.integer;
  return true;
}

bool asngn_xnum(const xcdn_value_t *v, double *out) {
  if (!v || !out) return false;
  if (v->type == XCDN_VAL_INT) {
    *out = (double)v->data.integer;
    return true;
  }
  if (v->type == XCDN_VAL_FLOAT) {
    *out = v->data.floating;
    return true;
  }
  return false;
}

bool asngn_xbool(const xcdn_value_t *v, bool *out) {
  if (!v || !out || v->type != XCDN_VAL_BOOL) return false;
  *out = v->data.boolean;
  return true;
}

bool asngn_xtime(const xcdn_value_t *v, asngn_time *out) {
  if (!v || !out || v->type != XCDN_VAL_DATETIME) return false;
  return asngn_time_parse_rfc3339(v->data.string, out);
}

bool asngn_xdur(const xcdn_value_t *v, int64_t *out_s) {
  if (!v || !out_s || v->type != XCDN_VAL_DURATION) return false;
  return asngn_duration_parse(v->data.string, out_s);
}

bool asngn_xuuid(const xcdn_value_t *v, char out[37]) {
  if (!v || !out || v->type != XCDN_VAL_UUID) return false;
  if (!v->data.string || strlen(v->data.string) != 36) return false;
  memcpy(out, v->data.string, 36);
  out[36] = '\0';
  return true;
}

/* ---- borrowed-node serialization ----------------------------------------
 * Wrap the node in a throwaway document (1-slot values array), serialize,
 * then drop the borrow before freeing the document so the node survives
 * (the store_node_pretty trick of the siblings). */

asngn_err asngn_xnode_write(const xcdn_node_t *node, bool pretty,
                            asngn_buf *out) {
  xcdn_document_t *doc;
  char *s;
  asngn_err e;
  if (!node || !out) return ASNGN_ERR_INVALID;
  doc = xcdn_document_new();
  if (!doc) return ASNGN_ERR_NOMEM;
  doc->values = (xcdn_node_t **)malloc(sizeof(*doc->values));
  if (!doc->values) {
    xcdn_document_free(doc);
    return ASNGN_ERR_NOMEM;
  }
  doc->values[0] = (xcdn_node_t *)node; /* borrowed */
  doc->values_len = 1;
  doc->values_cap = 1;
  s = pretty ? xcdn_to_string_pretty(doc) : xcdn_to_string_compact(doc);
  doc->values_len = 0; /* borrowed: do not free node */
  free(doc->values);
  doc->values = NULL;
  doc->values_cap = 0;
  xcdn_document_free(doc);
  if (!s) return ASNGN_ERR_NOMEM;
  e = asngn_buf_append(out, s, strlen(s));
  free(s);
  return e;
}
