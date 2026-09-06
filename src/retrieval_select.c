/* Keep a bounded heap across the full scan and a small shortlist per file. */
#include "retrieval_select.h"
#include <stdlib.h>
#include <string.h>

static void release(chunk *v) {
  free(v->path);
  free(v->text);
  free(v->vec);
}
static bool better(const chunk *a, const chunk *b) {
  if (a->admission_score != b->admission_score)
    return a->admission_score > b->admission_score;
  int order = strcmp(a->path, b->path);
  return order ? order < 0 : a->offset < b->offset;
}
static void swap(chunk *a, chunk *b) {
  chunk temp = *a;
  *a = *b;
  *b = temp;
}
static asngn_err retain(code_selection *s, const chunk *view, size_t bytes, bool active) {
  code_index *ix = s->index;
  chunk *heap = ix->v + s->pinned;
  size_t count = ix->n - s->pinned;
  if (ix->n == CODE_MAX && (active || !count || !better(view, heap)))
    return ASNGN_OK;
  chunk owned = *view;
  owned.path = asngn_strdup(view->path);
  owned.text = asngn_strndup(view->text, bytes);
  if (!owned.path || !owned.text) {
    release(&owned);
    return ASNGN_ERR_NOMEM;
  }
  asngn_sha256(owned.text, bytes, owned.hash);
  if (active) {
    ix->v[ix->n++] = owned;
    s->pinned++;
    return ASNGN_OK;
  }
  if (ix->n < CODE_MAX) {
    size_t pos = count;
    heap[pos] = owned;
    ix->n++;
    while (pos && better(&heap[(pos - 1) / 2], &heap[pos])) {
      size_t parent = (pos - 1) / 2;
      swap(&heap[parent], &heap[pos]);
      pos = parent;
    }
  } else {
    release(heap);
    heap[0] = owned;
    for (size_t pos = 0;;) {
      size_t child = pos * 2 + 1;
      if (child >= count)
        break;
      if (child + 1 < count && better(&heap[child], &heap[child + 1]))
        child++;
      if (!better(&heap[pos], &heap[child]))
        break;
      swap(&heap[pos], &heap[child]);
      pos = child;
    }
  }
  return ASNGN_OK;
}
asngn_err asngn_code_select_file(code_selection *s, const char *path, const char *data, size_t len,
                                 bool active) {
  if (asngn_code_stopped(s->c, s->turn))
    return s->turn->cancel ? ASNGN_ERR_CANCELLED : ASNGN_ERR_TIMEOUT;
  uint8_t file_hash[32];
  asngn_sha256(data, len, file_hash);
  if (!active && s->active_read && !strcmp(path, s->turn->s->active_file))
    return memcmp(file_hash, s->active_hash, 32) ? ASNGN_ERR_BUSY : ASNGN_OK;
  if (active) {
    s->active_read = true;
    memcpy(s->active_hash, file_hash, 32);
  }
  if (memchr(data, 0, len) || !asngn_utf8_valid(data, len)) {
    s->skipped_binary++;
    return ASNGN_OK;
  }
  chunk best[CODE_FILE_CHUNKS] = {0};
  size_t sizes[CODE_FILE_CHUNKS] = {0}, kept = 0, off = 0, line = 1;
  while (off < len) {
    if (asngn_code_stopped(s->c, s->turn))
      return s->turn->cancel ? ASNGN_ERR_CANCELLED : ASNGN_ERR_TIMEOUT;
    size_t end = len - off > CODE_CHUNK ? off + CODE_CHUNK : len;
    if (end < len) {
      while (end > off && data[end - 1] != '\n')
        end--;
      if (end == off)
        end = off + CODE_CHUNK;
    }
    while (end < len && ((unsigned char)data[end] & 0xc0) == 0x80)
      end++;
    chunk view = {.path = (char *)path, .text = (char *)data + off, .line = line, .offset = off};
    view.admission_score = asngn_code_rank(s, path, data + off, end - off);
    for (size_t i = off; i < end; i++)
      if (data[i] == '\n')
        line++;
    view.end_line = data[end - 1] == '\n' ? line - 1 : line;
    memcpy(view.file_hash, file_hash, 32);
    s->candidates++;
    if (active) {
      asngn_err e = retain(s, &view, end - off, true);
      if (e != ASNGN_OK)
        return e;
    } else {
      size_t worst = 0;
      for (size_t i = 1; i < kept; i++)
        if (better(&best[worst], &best[i]))
          worst = i;
      if (kept < CODE_FILE_CHUNKS)
        worst = kept++;
      else if (!better(&view, &best[worst])) {
        off = end;
        continue;
      }
      best[worst] = view;
      sizes[worst] = end - off;
    }
    off = end;
  }
  for (size_t i = 0; i < kept; i++) {
    asngn_err e = retain(s, &best[i], sizes[i], false);
    if (e != ASNGN_OK)
      return e;
  }
  return ASNGN_OK;
}
static int by_location(const void *a, const void *b) {
  const chunk *x = a, *y = b;
  int order = strcmp(x->path, y->path);
  return order ? order : (x->offset > y->offset) - (x->offset < y->offset);
}
void asngn_code_selection_sort(code_selection *s) {
  qsort(s->index->v + s->pinned, s->index->n - s->pinned, sizeof(chunk), by_location);
}
