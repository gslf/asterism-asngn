/* Exercise the pinned backend grammar, without loading model weights. */
#include "llama-grammar.h"

extern "C" int asngn_test_gbnf_accepts(const char *gbnf, const char *ascii) {
  llama_grammar *grammar =
      llama_grammar_init_impl(nullptr, gbnf, "root", false, nullptr, 0, nullptr, 0);
  if (!grammar) return -1;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(ascii);
       *p && !grammar->stacks.empty(); p++)
    llama_grammar_accept(grammar, *p);
  int accepted = 0;
  for (const auto &stack : grammar->stacks)
    if (stack.empty()) accepted = 1;
  llama_grammar_free_impl(grammar);
  return accepted;
}
