/*
 * digest.c — digestion of oversized working items and blob slicing.
 *
 * Any single working item larger than context.digest_threshold_chars is
 * digested: the compressor produces a short digest that stays in the
 * working zone, and the full text becomes a blob on disk that OPEN B<n>
 * can re-inject slice by slice. Redaction applies to blob content before
 * it is written — always, independent of safety.redact_context.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"

/* core, digest wording. */
static const char DIGEST_INSTRUCTION[] =
    "You compress one oversized tool output so an assistant can keep "
    "working with less text.\n"
    "\n"
    "Write a digest that:\n"
    "- keeps numbers, file paths, identifiers, and error text;\n"
    "- drops repetition and boilerplate;\n"
    "- never invents anything that is not in the input;\n"
    "- stays under the given length, as plain prose, no headers.\n"
    "\n"
    "Answer with the digest text only.";

/* Cut a byte length back to a UTF-8 boundary. */
static size_t utf8_floor(const char *s, size_t len) {
  while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;
  return len;
}

/* Human size like "41 KiB" / "512 B". */
static void human_size(size_t bytes, char out[16]) {
  if (bytes >= 1024 * 1024)
    snprintf(out, 16, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
  else if (bytes >= 1024)
    snprintf(out, 16, "%zu KiB", bytes / 1024);
  else
    snprintf(out, 16, "%zu B", bytes);
}

/* Extractive fallback digest: the head of the text, one marker line. */
static char *digest_extract(const char *text, size_t len) {
  size_t keep = len < 4096 ? len : utf8_floor(text, 4096);
  asngn_buf b;
  asngn_buf_init(&b);
  if (asngn_buf_append(&b, text, keep) != ASNGN_OK ||
      asngn_buf_appends(&b, "\n[digest fallback: head only]") != ASNGN_OK) {
    asngn_buf_free(&b);
    return NULL;
  }
  {
    char *out = asngn_buf_detach(&b);
    asngn_buf_free(&b);
    return out;
  }
}

asngn_err asngn_digest_item(asngn_ctx *c, asngn_session *s,
                            asngn_turn_state *t, const char *label,
                            const char *text, size_t len,
                            size_t *saved_tokens, size_t *aux_tokens,
                            char **out) {
  char *blob_text = NULL;
  size_t blob_len;
  char *digest = NULL;
  asngn_blob *blob = NULL;
  char id[37];
  char size_str[16];
  asngn_buf line;
  asngn_err e;
  size_t masked = 0;

  *out = NULL;
  if (len <= (size_t)c->cfg.digest_threshold_chars) return ASNGN_OK;

  /* blob content is redacted always */
  e = asngn_redact(text, len, &blob_text, &masked);
  if (e != ASNGN_OK) return e;
  blob_len = blob_text != NULL ? strlen(blob_text) : len;

  asngn_uuid_v4(id);
  e = asngn_session_add_blob(s, id, label,
                             blob_text != NULL ? blob_text : text,
                             blob_len, &blob);
  if (e != ASNGN_OK) {
    free(blob_text);
    return e;
  }

  /* compressor input: redacted when the context is redacted, else raw */
  {
    const char *src = (s->redact_context && blob_text != NULL)
                          ? blob_text
                          : text;
    size_t src_len = (s->redact_context && blob_text != NULL)
                         ? blob_len
                         : len;
    size_t cap = src_len < 65536 ? src_len : utf8_floor(src, 65536);
    char *clipped = asngn_strndup(src, cap);
    int slot = asngn_models_slot_for_role(c, ASNGN_ROLE_COMPRESSOR);
    if (clipped == NULL) {
      free(blob_text);
      return ASNGN_ERR_NOMEM;
    }
    if (slot >= 0) {
      char *gen = NULL;
      int tin = 0, tout = 0;
      asngn_buf up;
      asngn_buf_init(&up);
      if (asngn_buf_printf(&up,
                           "Tool output (%s):\n%s\n\nLength limit: about "
                           "%d tokens.",
                           label, clipped, c->cfg.digest_tokens) ==
          ASNGN_OK) {
        if (asngn_models_generate(c, slot, ASNGN_TASK_COMPRESS,
                                  DIGEST_INSTRUCTION, up.data, NULL,
                                  c->cfg.digest_tokens, NULL, NULL,
                                  t != NULL ? &t->cancel : NULL, &gen,
                                  &tin, &tout) == ASNGN_OK &&
            gen != NULL && gen[0] != '\0') {
          digest = gen;
          if (aux_tokens != NULL)
            *aux_tokens += (size_t)(tout > 0 ? tout : 0);
        } else {
          free(gen);
        }
      }
      asngn_buf_free(&up);
    }
    if (digest == NULL) digest = digest_extract(clipped, strlen(clipped));
    free(clipped);
  }
  free(blob_text);
  if (digest == NULL) return ASNGN_ERR_NOMEM;

  /* "[B3 · fs.read · 41 KiB · digested] ..." */
  human_size(len, size_str);
  asngn_buf_init(&line);
  e = asngn_buf_printf(&line, "[B%zu \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 "
                              "digested] %s",
                       s->blobs_n, label, size_str, digest);
  free(digest);
  if (e != ASNGN_OK) {
    asngn_buf_free(&line);
    return e;
  }
  *out = asngn_buf_detach(&line);
  asngn_buf_free(&line);
  if (*out == NULL) return ASNGN_ERR_NOMEM;

  if (saved_tokens != NULL) {
    int slot = t != NULL ? t->gen_slot
                         : asngn_models_slot_for_role(
                               c, ASNGN_ROLE_GENERATOR);
    int full = asngn_models_count_tokens(c, slot, text);
    int kept = asngn_models_count_tokens(c, slot, *out);
    if (full > kept) *saved_tokens += (size_t)(full - kept);
  }
  {
    char data[128];
    snprintf(data, sizeof data, "{label: \"%s\", bytes: %zu}", label, len);
    asngn_tele_emit(c, "digest", NULL, NULL, s->slug,
                    t != NULL ? t->led.turn : 0, data);
  }
  (void)blob;
  return ASNGN_OK;
}

asngn_err asngn_digest_open_slice(asngn_ctx *c, asngn_session *s,
                                  asngn_blob *b, size_t max_chars,
                                  char **out) {
  char *text = NULL;
  size_t len = 0;
  asngn_err e;
  size_t start, take;
  asngn_buf line;

  (void)s;
  *out = NULL;
  if (b->slice_off >= b->size) return ASNGN_OK; /* exhausted */
  e = os_read_file(b->path, &text, &len);
  if (e != ASNGN_OK)
    return asngn_seterr(c, e, "blob %s: cannot read %s", b->id, b->path);
  if (b->slice_off >= len) {
    free(text);
    b->slice_off = b->size;
    return ASNGN_OK;
  }
  start = b->slice_off;
  take = len - start;
  if (max_chars > 0 && take > max_chars)
    take = utf8_floor(text + start, max_chars);
  if (take == 0) {
    free(text);
    b->slice_off = b->size;
    return ASNGN_OK;
  }
  asngn_buf_init(&line);
  e = asngn_buf_printf(&line, "[B?] %s \xC2\xB7 bytes %zu-%zu of %zu\n",
                       b->label, start, start + take, len);
  if (e == ASNGN_OK) e = asngn_buf_append(&line, text + start, take);
  free(text);
  if (e != ASNGN_OK) {
    asngn_buf_free(&line);
    return e;
  }
  b->slice_off = start + take;
  *out = asngn_buf_detach(&line);
  asngn_buf_free(&line);
  return *out != NULL ? ASNGN_OK : ASNGN_ERR_NOMEM;
}
