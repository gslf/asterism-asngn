/*
 * theme.c — palette and glyph sets of the asngn TUI.
 *
 * Theme "asterism" (default): dark, starry, precision instrument — the
 * background stays the terminal default, chrome is dim gray, and the
 * single contrast color is a warm star-yellow (#FFC64D). A soft amber
 * marks warnings; red appears only for errors and over-budget. Theme
 * "plain": monochrome ASCII, chosen by config or automatically when the
 * locale is not UTF-8.
 *
 * MIT License — per aspera ad astra.
 */

#include "tui.h"

#include <stdlib.h>
#include <string.h>

/* ── palette → SGR fragments ──────────────────────────────────────────── */

/* index: [mode][palette]; "" = leave the terminal default */
static const char *const FG[3][TFG_COUNT] = {
    /* TUI_COLOR_16 */
    {"", "90", "33;1", "33", "91", "97", "36"},
    /* TUI_COLOR_256 */
    {"", "38;5;245", "38;5;221", "38;5;179", "38;5;203", "38;5;255",
     "38;5;117"},
    /* TUI_COLOR_TRUE — accent #FFC64D, amber (214,158,46) */
    {"", "38;2;148;148;148", "38;2;255;198;77", "38;2;214;158;46",
     "38;2;255;95;95", "38;2;240;240;240", "38;2;120;190;255"},
};

static const char *const BG[3][TBG_COUNT] = {
    {"", ""},              /* 16-color: no shade, callers fall back dim */
    {"", "48;5;236"},      /* 256-color fenced-code shade              */
    {"", "48;2;38;38;38"}, /* truecolor shade                          */
};

const char *tui_fg_seq(tui_colormode m, uint8_t fg) {
  if (fg >= TFG_COUNT) return "";
  return FG[m][fg];
}

const char *tui_bg_seq(tui_colormode m, uint8_t bg) {
  if (bg >= TBG_COUNT) return "";
  return BG[m][bg];
}

/* ── locale probe ─────────────────────────────────────────────────────── */

static int env_is_utf8(const char *name) {
  const char *v = getenv(name);
  if (v == NULL || v[0] == '\0') return -1; /* unset */
  if (strstr(v, "UTF-8") != NULL || strstr(v, "utf-8") != NULL ||
      strstr(v, "UTF8") != NULL || strstr(v, "utf8") != NULL)
    return 1;
  return 0;
}

int tui_locale_utf8(void) {
#ifdef _WIN32
  /* term.c switches the console output code page to UTF-8; only an
   * explicit non-UTF-8 locale env opts back into the ASCII glyph set. */
  int r = env_is_utf8("LC_ALL");
  if (r < 0) r = env_is_utf8("LC_CTYPE");
  if (r < 0) r = env_is_utf8("LANG");
  return r != 0;
#else
  int r = env_is_utf8("LC_ALL");
  if (r < 0) r = env_is_utf8("LC_CTYPE");
  if (r < 0) r = env_is_utf8("LANG");
  return r == 1;
#endif
}

/* ── glyph sets ───────────────────────────────────────────────────────── */

void tui_theme_init(tui_theme *t, int plain) {
  memset(t, 0, sizeof *t);
  t->ascii = plain;
  if (!plain) {
    static const char *const SPIN[10] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                         "⠴", "⠦", "⠧", "⠇", "⠏"};
    int i;
    t->star = "✦";
    t->bullet = "·";
    t->marker = "▸";
    t->prompt = "›";
    t->bar_full = "▓";
    t->bar_empty = "░";
    t->newmark = "▼";
    t->check = "✓";
    t->cross = "✗";
    t->dot_on = "●";
    t->dot_off = "○";
    t->ellipsis = "…";
    for (i = 0; i < 10; i++) t->spinner[i] = SPIN[i];
    t->spinner_n = 10;
    t->tl = "┌"; t->tr = "┐"; t->bl = "└"; t->br = "┘";
    t->hline = "─"; t->vline = "│";
    t->dtl = "╔"; t->dtr = "╗"; t->dbl = "╚"; t->dbr = "╝";
    t->dh = "═"; t->dv = "║";
    t->sep_l = "├"; t->sep_r = "┤";
    t->spark[0] = "⣀"; t->spark[1] = "⣤";
    t->spark[2] = "⣶"; t->spark[3] = "⣿";
  } else {
    static const char *const SPIN[4] = {"|", "/", "-", "\\"};
    int i;
    t->star = "*";
    t->bullet = "-";
    t->marker = ">";
    t->prompt = ">";
    t->bar_full = "#";
    t->bar_empty = "-";
    t->newmark = "v";
    t->check = "+";
    t->cross = "x";
    t->dot_on = "*";
    t->dot_off = ".";
    t->ellipsis = "..";
    for (i = 0; i < 4; i++) t->spinner[i] = SPIN[i];
    t->spinner_n = 4;
    t->tl = "+"; t->tr = "+"; t->bl = "+"; t->br = "+";
    t->hline = "-"; t->vline = "|";
    t->dtl = "+"; t->dtr = "+"; t->dbl = "+"; t->dbr = "+";
    t->dh = "="; t->dv = "|";
    t->sep_l = "+"; t->sep_r = "+";
    t->spark[0] = "."; t->spark[1] = ":";
    t->spark[2] = "+"; t->spark[3] = "#";
  }
}
