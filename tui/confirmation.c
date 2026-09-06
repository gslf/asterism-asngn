/* Scrollable review of the complete redacted request, bound to its durable ID. */
#include "tui.h"
#include <stdio.h>
#include <string.h>

void tui_confirm_clear(tui_app *a) {
  asngn_approval_free(a->confirm.review);
  memset(&a->confirm, 0, sizeof a->confirm);
}

void tui_confirm_open(tui_app *a, const char *id, int destructive) {
  tui_confirm_clear(a);
  asngn_approval *review = NULL;
  asngn_err e = asngn_approval_get(a->ses, &review);
  if (e == ASNGN_OK && (strcmp(review->id, id) || review->status != ASNGN_APPROVAL_PENDING)) {
    asngn_approval_free(review);
    return; /* A queued event can outlive its request. */
  }
  a->confirm.review = review;
  snprintf(a->confirm.id, sizeof a->confirm.id, "%s", id);
  a->confirm.active = *id != 0;
  a->confirm.destructive = destructive;
}

static void resolve(tui_app *a, int allow, int wide) {
  if (allow && !a->confirm.review) {
    chat_systemf(&a->chat, "Cannot approve: complete request details are unavailable.");
    return;
  }
  asngn_err e = asngn_confirm(a->ctx, a->confirm.id, allow, wide);
  if (e != ASNGN_OK && e != ASNGN_ERR_NOT_FOUND)
    chat_systemf(&a->chat, "Confirmation failed: %s", asngn_err_name(e));
  else tui_confirm_clear(a);
  a->dirty = a->structural = 1;
}

void tui_confirm_key(tui_app *a, const tui_key *k) {
  if (k->kind == TK_CHAR) {
    char c = k->utf8[0];
    if (c == 'y' || c == 'Y') resolve(a, 1, 0);
    else if (c == 'n' || c == 'N') resolve(a, 0, 0);
    else if (c == 'a' || c == 'A') resolve(a, 1, 1);
  } else if (k->kind == TK_ESC || k->kind == TK_CTRL_C || k->kind == TK_CTRL_D) resolve(a, 0, 0);
  else if (k->kind == TK_UP) a->confirm.top--;
  else if (k->kind == TK_DOWN) a->confirm.top++;
  else if (k->kind == TK_PGUP) a->confirm.top -= 8;
  else if (k->kind == TK_PGDN) a->confirm.top += 8;
  else if (k->kind == TK_HOME) a->confirm.top = 0;
  else if (k->kind == TK_END) a->confirm.top = a->confirm.rows - 1;
  if (a->confirm.top < 0) a->confirm.top = 0;
  if (a->confirm.rows && a->confirm.top >= a->confirm.rows) a->confirm.top = a->confirm.rows - 1;
}

static int wrap(const char *s, int width, int first, int count, const char **starts, int *lengths) {
  size_t i = 0, len = strlen(s);
  int row = 0;
  while (i < len) {
    size_t end = i;
    int cells = 0;
    while (end < len && s[end] != '\n' && cells++ < width)
      end = tui_u8_next(s, end);
    if (row >= first && row < first + count) {
      starts[row - first] = s + i;
      lengths[row - first] = (int)(end - i);
    }
    row++;
    i = end < len && s[end] == '\n' ? end + 1 : end;
  }
  return row;
}

void modal_draw_confirm(tui_app *a, tui_frame *f) {
  const tui_theme *th = &a->theme;
  asngn_approval *review = a->confirm.review;
  const char *text =
      review ? review->arguments : "Complete request unavailable. Approval is disabled; n denies.";
  const char *starts[16];
  int lens[16];
  int bw = f->w - 8, visible = f->h - 10;
  if (bw > 86) bw = 86;
  if (bw < 20 || visible < 1) return;
  if (visible > 16) visible = 16;
  a->confirm.rows = wrap(text, bw - 4, 0, 0, starts, lens);
  int max_top = a->confirm.rows > visible ? a->confirm.rows - visible : 0;
  if (a->confirm.top > max_top) a->confirm.top = max_top;
  (void)wrap(text, bw - 4, a->confirm.top, visible, starts, lens);
  int lines = a->confirm.rows - a->confirm.top;
  if (lines > visible) lines = visible;
  int bh = 8 + lines, bx = (f->w - bw) / 2, by = (f->h - bh) / 2, row = by + 2;
  d_dim_rect(f, 0, 0, f->w, f->h);
  for (int y = by; y < by + bh; y++)
    for (int x = bx; x < bx + bw; x++)
      d_cell(f, x, y, " ", TFG_DEFAULT, TBG_DEFAULT, 0);
  d_box(f, bx, by, bw, bh, th, 1, TFG_ACCENT, 0);
  d_put(f, bx + 2, by, " confirm ", TFG_ACCENT, TBG_DEFAULT, TA_BOLD);
  char label[256];
  snprintf(label, sizeof label, "%s.%s%s", review ? review->tool_ref : "tool",
           review ? review->command : "?", a->confirm.destructive ? " [destructive]" : "");
  d_putn(f, bx + 2, row++, label, bw - 4, TFG_BRIGHT, TBG_DEFAULT, TA_BOLD);
  snprintf(label, sizeof label, "Arguments: rows %d-%d of %d (arrows/PgUp/PgDn)",
           a->confirm.top + 1, a->confirm.top + lines, a->confirm.rows);
  d_putn(f, bx + 2, row++, label, bw - 4, TFG_DIM, TBG_DEFAULT, 0);
  for (int i = 0; i < lines; i++) {
    /* 82 codepoints need at most 328 UTF-8 bytes. */
    char line[344];
    size_t n = (size_t)lens[i];
    if (n >= sizeof line) n = sizeof line - 1;
    memcpy(line, starts[i], n);
    line[n] = 0;
    d_putn(f, bx + 2, row++, line, bw - 4, TFG_DIM, TBG_DEFAULT, 0);
  }
  d_putn(f, bx + 2, by + bh - 3, "y allow once | n deny | a allow package for session", bw - 4,
         TFG_DIM, TBG_DEFAULT, 0);
  d_putn(f, bx + 2, by + bh - 2, "Session grant also requires the same workspace and profile.",
         bw - 4, TFG_DIM, TBG_DEFAULT, 0);
}
