/*
 * test_grammar.c — classify/judge micro-grammars, the per-turn
 * action-object step grammar (minimal, blob handles), and astools
 * grafting.
 *
 * MIT License — per aspera ad astra.
 */

#include "asngn_test.h"

#include <string.h>

#include "asngn_internal.h"

static size_t count_substr(const char *hay, const char *needle) {
  size_t n = 0, nl = strlen(needle);
  const char *p = hay;
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += nl;
  }
  return n;
}

/* Shared productions, in emission order. */
#define G_THINK \
  "think     ::= \"{action: \\\"think\\\", input: \\\"\" text \"\\\"}\"\n"
#define G_CLARIFY \
  "clarify   ::= \"{action: \\\"clarify\\\", why: \\\"\" meta \"\\\", " \
  "input: \\\"\" text \"\\\"}\"\n"
#define G_ANSWER "answer    ::= \"{action: \\\"answer\\\"}\"\n"
#define G_TAIL \
  "text      ::= tchar{1,2048}\n" \
  "meta      ::= tchar{1,512}\n" \
  "tchar     ::= [^\\\"\\\\\\x0A\\x0D]\n"

TEST(classify_grammar_exact) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_classify(&g));
  ASSERT_EQ_STR(g,
      "root ::= \"CLASS \" (\"SIMPLE\" | \"MODERATE\" | \"COMPLEX\")"
      " \" | DETAIL \" (\"TERSE\" | \"NORMAL\" | \"RICH\")"
      " \" | MODE \" (\"DIRECT\" | \"PLAN\")"
      " \" | TASK \" (\"CHAT\" | \"LOOKUP\" | \"EXPLAIN\" | \"EDIT\" | "
      "\"BUILD\" | \"GENERATE\" | \"REFACTOR\" | \"DEBUG\") \"\\n\"\n");
  free(g);
}

TEST(judge_grammar_exact) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_judge(&g));
  ASSERT_EQ_STR(g,
      "root ::= \"SCORE \" (\"10\" | [0-9]) \" | \" jtext \"\\n\"\n"
      "jtext ::= jchar (jchar)*\n"
      "jchar ::= [^|\\x0A\\x0D]\n");
  free(g);
}

TEST(steps_minimal_grammar) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, false, false, true, 0, NULL, &g));
  ASSERT_EQ_STR(g,
      "root      ::= step \"\\n\"\n"
      "step      ::= think | clarify | answer\n"
      G_THINK G_CLARIFY G_ANSWER G_TAIL);
  ASSERT_TRUE(strstr(g, "recall") == NULL);
  ASSERT_TRUE(strstr(g, "open") == NULL);
  ASSERT_TRUE(strstr(g, "call") == NULL);
  free(g);
}

TEST(steps_recall_alternative) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, false, true, true, 0, NULL, &g));
  ASSERT_EQ_STR(g,
      "root      ::= step \"\\n\"\n"
      "step      ::= recall | think | clarify | answer\n"
      "recall    ::= \"{action: \\\"recall\\\", why: \\\"\" meta \"\\\", "
      "input: \\\"\" text \"\\\", success: \\\"\" meta \"\\\", fallback: "
      "\\\"\" meta \"\\\"}\"\n"
      G_THINK G_CLARIFY G_ANSWER G_TAIL);
  free(g);
}

TEST(steps_blob_handles) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, false, false, true, 3, NULL, &g));
  ASSERT_EQ_STR(g,
      "root      ::= step \"\\n\"\n"
      "step      ::= open | think | clarify | answer\n"
      "open      ::= \"{action: \\\"open\\\", why: \\\"\" meta \"\\\", "
      "input: \\\"\" handle \"\\\"}\"\n"
      G_THINK G_CLARIFY G_ANSWER
      "handle    ::= \"B1\" | \"B2\" | \"B3\"\n"
      G_TAIL);
  ASSERT_TRUE(strstr(g, "handle    ::= \"B1\" | \"B2\" | \"B3\"\n") != NULL);
  free(g);
}

TEST(steps_think_can_be_withheld) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, false, false, false, 0, NULL, &g));
  ASSERT_EQ_STR(g,
      "root      ::= step \"\\n\"\n"
      "step      ::= clarify | answer\n"
      G_CLARIFY G_ANSWER G_TAIL);
  ASSERT_TRUE(strstr(g, "think") == NULL);
  free(g);
}

/* Small hand-written astools-style export for the graft tests. */
static const char k_export[] =
    "root ::= \"CALL \" call \"\\n\"\n"
    "call ::= t-x-c-y\n"
    "t-x-c-y ::= \"x.y {}\"\n"
    "str ::= \"\\\"\" \"\\\"\"\n";

static const char k_grafted[] =
    "root      ::= step \"\\n\"\n"
    "step      ::= call | think | clarify | answer\n"
    "call      ::= \"{action: \\\"call\\\", why: \\\"\" meta \"\\\", "
    "input: \" astools-call \", success: \\\"\" meta \"\\\", fallback: "
    "\\\"\" meta \"\\\"}\"\n"
    G_THINK G_CLARIFY G_ANSWER G_TAIL
    "astools-call ::= t-x-c-y\n"
    "t-x-c-y ::= \"x.y {}\"\n"
    "str ::= \"\\\"\" \"\\\"\"\n";

TEST(steps_graft_astools) {
  char *g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, true, false, true, 0, k_export, &g));
  ASSERT_EQ_STR(g, k_grafted);
  /* astools' own root line is dropped: exactly one root rule remains */
  ASSERT_EQ_INT((long long)count_substr(g, "root"), 1);
  ASSERT_TRUE(strncmp(g, "root      ::=", 13) == 0);
  /* the bare astools "call" rule was renamed to astools-call */
  ASSERT_TRUE(strstr(g, "\ncall ::=") == NULL);
  ASSERT_TRUE(strstr(g, "astools-call ::= t-x-c-y\n") != NULL);
  ASSERT_TRUE(strstr(g, "t-x-c-y ::= \"x.y {}\"\n") != NULL);
  ASSERT_TRUE(strstr(g, "str ::= \"\\\"\" \"\\\"\"\n") != NULL);
  free(g);
}

TEST(steps_graft_deterministic) {
  char *g1 = NULL, *g2 = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, true, true, true, 2, k_export, &g1));
  ASSERT_OK(asngn_grammar_steps(NULL, true, true, true, 2, k_export, &g2));
  ASSERT_EQ_STR(g1, g2); /* byte-identical */
  ASSERT_EQ_INT((long long)count_substr(g1, "root"), 1);
  free(g1);
  free(g2);
}

TEST(steps_call_dropped_without_export) {
  /* with_call without a usable astools grammar: CALL is dropped, not
   * emitted with an undefined rule. */
  char *g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, true, false, true, 0, NULL, &g));
  ASSERT_TRUE(strstr(g, "call") == NULL);
  free(g);
  g = NULL;
  ASSERT_OK(asngn_grammar_steps(NULL, true, false, true, 0,
                                "root ::= \"x\"\nfoo ::= \"y\"\n", &g));
  ASSERT_TRUE(strstr(g, "call") == NULL);
  ASSERT_TRUE(strstr(g, "foo") == NULL); /* nothing grafted either */
  free(g);
  g = NULL;
  ASSERT_OK(asngn_grammar_steps(
      NULL, true, false, true, 0,
      "root ::= \"CALL \" call \"\\n\"\ncall ::= \"\"\n", &g));
  ASSERT_TRUE(strstr(g, "call") == NULL); /* empty registry */
  free(g);
}

TEST_LIST = {
  TEST_ENTRY(classify_grammar_exact),
  TEST_ENTRY(judge_grammar_exact),
  TEST_ENTRY(steps_minimal_grammar),
  TEST_ENTRY(steps_recall_alternative),
  TEST_ENTRY(steps_blob_handles),
  TEST_ENTRY(steps_think_can_be_withheld),
  TEST_ENTRY(steps_graft_astools),
  TEST_ENTRY(steps_graft_deterministic),
  TEST_ENTRY(steps_call_dropped_without_export),
};

RUN_ALL_TESTS()
