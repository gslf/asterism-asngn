/* Streaming file hashes and language census share the authorized tree walk. */
#include "workspace_tree.h"
#include <string.h>

/* Code-language census by file extension, tallied during the fingerprint
 * walk. .h counts toward C — indistinguishable from C++ headers without
 * parsing, and close enough for a routing signal. */
static const struct {
  const char *ext;
  const char *lang;
} ws_langs[] = {{"c", "c"},         {"h", "c"},       {"cpp", "cpp"}, {"cc", "cpp"},
                {"cxx", "cpp"},     {"hpp", "cpp"},   {"hh", "cpp"},  {"py", "python"},
                {"rs", "rust"},     {"go", "go"},     {"js", "js"},   {"jsx", "js"},
                {"mjs", "js"},      {"ts", "ts"},     {"tsx", "ts"},  {"java", "java"},
                {"cs", "csharp"},   {"rb", "ruby"},   {"php", "php"}, {"sh", "shell"},
                {"swift", "swift"}, {"kt", "kotlin"}, {"zig", "zig"}, {NULL, NULL}};

#define WS_LANG_N (sizeof ws_langs / sizeof ws_langs[0] - 1)

typedef struct {
  size_t files, bytes;
  asngn_sha256_ctx *hash;
  size_t ext_count[WS_LANG_N];
} ws_scan;

static void ws_scan_file(ws_scan *scan, const char *name, size_t len) {
  const char *dot = strrchr(name, '.');
  size_t i;
  scan->files++;
  scan->bytes += len;
  if (dot == NULL || dot[1] == '\0') return;
  for (i = 0; i < WS_LANG_N; i++) {
    const char *e = ws_langs[i].ext, *p = dot + 1;
    while (*e != '\0' && *p != '\0' && (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p) == *e) {
      e++;
      p++;
    }
    if (*e == '\0' && *p == '\0') {
      scan->ext_count[i]++;
      return;
    }
  }
}

static void ws_scan_finish(const ws_scan *scan, asngn_repo_stats *out) {
  /* dominant language = argmax of per-language counts (extensions that
   * map to one language pool their tallies) */
  size_t lang_count[WS_LANG_N];
  size_t i, j, best = 0, best_count = 0;
  memset(lang_count, 0, sizeof lang_count);
  for (i = 0; i < WS_LANG_N; i++) {
    for (j = 0; j <= i; j++)
      if (strcmp(ws_langs[j].lang, ws_langs[i].lang) == 0) break;
    lang_count[j] += scan->ext_count[i];
  }
  out->files = scan->files;
  out->bytes = scan->bytes;
  out->language[0] = '\0';
  for (i = 0; i < WS_LANG_N; i++) {
    if (lang_count[i] > best_count) {
      best = i;
      best_count = lang_count[i];
    }
  }
  if (best_count > 0) snprintf(out->language, sizeof out->language, "%s", ws_langs[best].lang);
  out->loaded = true;
}

static asngn_err hash_file(const char *path, FILE *file, size_t bytes, void *userdata) {
  ws_scan *scan = userdata;
  asngn_sha256_ctx item;
  asngn_sha256_init(&item);
  unsigned char buffer[8192], hash[32], size[8];
  for (size_t left = bytes; left;) {
    size_t n = left > sizeof buffer ? sizeof buffer : left;
    if (fread(buffer, 1, n, file) != n || ferror(file)) return ASNGN_ERR_IO;
    asngn_sha256_update(&item, buffer, n);
    left -= n;
  }
  asngn_sha256_final(&item, hash);
  /* Fixed-width little-endian lengths avoid host-size and endian ambiguity. */
  for (size_t i = 0; i < sizeof size; i++)
    size[i] = (unsigned char)((uint64_t)bytes >> (i * 8));
  asngn_sha256_update(scan->hash, path, strlen(path) + 1);
  asngn_sha256_update(scan->hash, size, sizeof size);
  asngn_sha256_update(scan->hash, hash, sizeof hash);
  ws_scan_file(scan, path, bytes);
  return ASNGN_OK;
}
asngn_err asngn_workspace_tree_hash(const char *root, asngn_sha256_ctx *hash,
                                    asngn_repo_stats *stats) {
  if (stats) memset(stats, 0, sizeof *stats);
  ws_scan scan = {.hash = hash};
  asngn_tree_stats walked = {0};
  asngn_err e = asngn_tree_walk(root, NULL, hash_file, NULL, &scan, &walked);
  /* A skipped alias/special file cannot silently certify the whole workspace. */
  if (e == ASNGN_OK && walked.excluded) e = ASNGN_ERR_DENIED;
  if (e == ASNGN_OK && stats) ws_scan_finish(&scan, stats);
  return e;
}
