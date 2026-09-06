/*
 * digest.c — bounded views of oversized working items.
 *
 * Any single working item larger than context.digest_threshold_chars is
 * digested: log recognizers or the compressor produce a short view, and
 * the full text becomes an object reopenable at an explicit byte offset.
 * Redaction applies to blob content before
 * it is written — always, independent of safety.redact_context.
 *
 * MIT License — per aspera ad astra.
 */

#include <stdlib.h>
#include <string.h>

#include "asngn_internal.h"
#include "excerpt.h"

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

/* Human size like "41 KiB" / "512 B". */
static void human_size(size_t bytes, char out[16]) {
  if (bytes >= 1024 * 1024) snprintf(out, 16, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
  else if (bytes >= 1024) snprintf(out, 16, "%zu KiB", bytes / 1024);
  else snprintf(out, 16, "%zu B", bytes);
}

/* Closed workflow tools get an extractive view without an auxiliary model. */
static bool log_label(const char *label) {
  return label && ((!strncmp(label, "project", 7) && (label[7] == '.' || label[7] == '@')) ||
                   (!strncmp(label, "proc", 4) && (label[4] == '.' || label[4] == '@')));
}

asngn_err asngn_digest_item(asngn_ctx *c, asngn_session *s, asngn_turn_state *t, const char *label,
                            const char *text, size_t len, size_t *saved_tokens, size_t *aux_tokens,
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
  bool compressor_used = false;

  if (!out) return ASNGN_ERR_INVALID;
  *out = NULL;
  if (!c || !s || !label || !text) return ASNGN_ERR_INVALID;
  if (len <= (size_t)c->cfg.digest_threshold_chars) return ASNGN_OK;
  /* Lossless large-result storage belongs to Asper.  In explicitly
   * degraded mode keep the original result in the current turn instead of
   * creating an ASNGN-owned blob or silently throwing bytes away. */
  if (!c->asper_ok) return ASNGN_OK;

  /* blob content is redacted always */
  e = asngn_redact(text, len, &blob_text, &masked);
  if (e != ASNGN_OK) return e;
  blob_len = blob_text != NULL ? strlen(blob_text) : len;

  /* Validate and select before creating a persistent object. */
  const char *src = blob_text ? blob_text : text;
  asngn_excerpt view = {0}, sample = {0};
  e = asngn_excerpt_build(src, blob_len, 4096, &view);
  if (e != ASNGN_OK) {
    free(blob_text);
    return e;
  }

  asngn_uuid_v4(id);
  e = asngn_session_add_blob(s, id, label, src, blob_len, &blob);
  if (e != ASNGN_OK) {
    asngn_excerpt_free(&view);
    free(blob_text);
    return e;
  }
  if (log_label(label)) {
    digest = view.text;
    view.text = NULL;
  } else {
    e = asngn_excerpt_build(src, blob_len, 65536, &sample);
    int slot = asngn_models_slot_for_role(c, ASNGN_ROLE_COMPRESSOR);
    int64_t deadline = t ? t->deadline_mono : 0;
    if (t && (t->cancel || (deadline > 0 && deadline <= asngn_clock_mono_ms(&c->clock)))) slot = -1;
    if (e == ASNGN_OK && slot >= 0) {
      char *gen = NULL;
      int tin = 0, tout = 0;
      asngn_buf up = {0};
      if (asngn_buf_printf(&up, "Tool output (%s):\n%s\n\nLength limit: about %d tokens.", label,
                           sample.text, c->cfg.digest_tokens) == ASNGN_OK &&
          asngn_models_generate(c, slot, ASNGN_TASK_COMPRESS, DIGEST_INSTRUCTION, up.data, NULL,
                                NULL, c->cfg.digest_tokens, deadline, NULL, NULL,
                                t ? &t->cancel : NULL, &gen, &tin, &tout) == ASNGN_OK &&
          gen && *gen) {
        digest = gen;
        compressor_used = true;
        if (aux_tokens) *aux_tokens += (size_t)(tout > 0 ? tout : 0);
      } else free(gen);
      asngn_buf_free(&up);
    }
    asngn_excerpt_free(&sample);
    if (!digest) {
      digest = view.text;
      view.text = NULL;
    } else if (view.matches) {
      /* A prose compressor cannot silently discard the diagnostic evidence. */
      asngn_buf combined = {0};
      e = asngn_buf_printf(&combined, "%s\n%s", digest, view.text);
      free(digest);
      digest = e == ASNGN_OK ? asngn_buf_detach(&combined) : NULL;
      asngn_buf_free(&combined);
    }
  }
  /* A compressor-only digest contains no extractive ranges. */
  asngn_excerpt no_ranges = {0};
  const asngn_excerpt *included = compressor_used && !view.matches ? &no_ranges : &view;
  char *selection = asngn_excerpt_trace(included, blob->object_ref, blob_len, 4096, compressor_used);
  if (selection) {
    asngn_tele_emit(c, "evidence_selection", NULL, NULL, s->slug, t ? t->led.turn : 0, selection);
    free(selection);
  }
  asngn_excerpt_free(&view);
  free(blob_text);
  if (digest == NULL) return ASNGN_ERR_NOMEM;

  /* "[B3 · fs.read · 41 KiB · digested] ..." */
  human_size(blob_len, size_str);
  asngn_buf_init(&line);
  e = asngn_buf_printf(&line,
                       "[B%zu \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 "
                       "digested; %s] %s",
                       s->blobs_n, label, size_str, blob->object_ref, digest);
  free(digest);
  if (e != ASNGN_OK) {
    asngn_buf_free(&line);
    return e;
  }
  *out = asngn_buf_detach(&line);
  asngn_buf_free(&line);
  if (*out == NULL) return ASNGN_ERR_NOMEM;

  if (saved_tokens != NULL) {
    int slot = t != NULL ? t->gen_slot : asngn_models_slot_for_role(c, ASNGN_ROLE_GENERATOR);
    int full = asngn_models_count_tokens(c, slot, text);
    int kept = asngn_models_count_tokens(c, slot, *out);
    if (full > kept) *saved_tokens += (size_t)(full - kept);
  }
  return ASNGN_OK;
}
