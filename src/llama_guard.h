/*
 * llama_guard.h — exception-guarded wrappers around llama.cpp C entry
 * points.
 *
 * llama.cpp is C++ behind a C API, and several of its paths can throw
 * (grammar accept on a piece that overruns the root, tokenizers on invalid
 * UTF-8, chat-template detection on unknown templates, allocation
 * failures). asngn's callers are C translation units: an exception that
 * escapes the C API has no handler anywhere on the stack and terminates
 * the whole process (observed on Windows as the silent fast-fail
 * 0xC0000409, exit code 9 in POSIX shells). These wrappers are the single
 * C++ seam where such exceptions are caught and converted into the error
 * returns the C callers already handle. Model-agnostic by design:
 * whatever a future model's tokenizer or template does, the process
 * survives and the turn degrades to a model error.
 *
 * MIT License — per aspera ad astra.
 */
#ifndef ASNGN_LLAMA_GUARD_H
#define ASNGN_LLAMA_GUARD_H

/* llama.h/ggml.h are not pedantic-C99-clean (anonymous unions, typedef
 * redefinitions); include them with those diagnostics off. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc11-extensions"
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4201) /* nameless struct/union */
#endif
#include "llama.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as llama_tokenize; returns INT32_MIN if an exception was
 * caught (llama's own negative returns stay meaningful). */
int32_t asngn_llg_tokenize(const struct llama_vocab *vocab, const char *text,
                           int32_t text_len, llama_token *tokens,
                           int32_t n_tokens_max, bool add_special,
                           bool parse_special);

/* Same contract as llama_chat_apply_template; returns -1 if an exception
 * was caught, which callers already treat as "template unsupported". */
int32_t asngn_llg_chat_apply_template(const char *tmpl,
                                      const struct llama_chat_message *chat,
                                      size_t n_msg, bool add_ass, char *buf,
                                      int32_t length);

/* Wraps llama_sampler_sample (which also accepts the token into the
 * sampler chain). Returns 0 and stores the token in *out on success, -1 if
 * an exception was caught (treat as end of generation / model error). */
int asngn_llg_sampler_sample(struct llama_sampler *smpl,
                             struct llama_context *ctx, int32_t idx,
                             llama_token *out);

/* Same contract as llama_decode / llama_encode; returns INT32_MIN if an
 * exception was caught (any nonzero return is already an error there). */
int32_t asngn_llg_decode(struct llama_context *ctx, struct llama_batch batch);
int32_t asngn_llg_encode(struct llama_context *ctx, struct llama_batch batch);

/* Same contract as llama_memory_clear; exceptions are swallowed (a failed
 * clear surfaces on the next decode at worst). */
void asngn_llg_memory_clear(llama_memory_t mem, bool data);

#ifdef __cplusplus
}
#endif

#endif /* ASNGN_LLAMA_GUARD_H */
