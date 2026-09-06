/* Read-only helper for comparisons with a real Git executable. */
#include "asngn_internal.h"
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  char *root = os_realpath(argv[1]);
  if (!root || strlen(root) >= ASNGN_WORKSPACE_PATH_MAX) { free(root); return 2; }
  asngn_workspace_info w = {0};
  strcpy(w.canonical_root, root);
  strcpy(w.repository_root, root);
  free(root);
  asngn_err e = asngn_workspace_snapshot(&w, NULL);
  printf("%s\n%s\n%s\n%s\n", asngn_err_name(e), w.head, w.branch, w.fingerprint);
  return e != ASNGN_OK;
}
