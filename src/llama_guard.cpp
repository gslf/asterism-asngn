/*
 * llama_guard.cpp — exception-guarded wrappers around llama.cpp C entry
 * points. See llama_guard.h for the rationale. This is deliberately the
 * only C++ translation unit in libasngn.
 *
 * MIT License — per aspera ad astra.
 */
#include "llama_guard.h"

#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <exception>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace {

void llg_report(const char *where, const char *what) {
  /* stderr keeps the message visible in headless runs even when the host
   * redirects the engine logger; the caller's own error path logs too. */
  std::fprintf(stderr, "asngn: llama guard: %s: caught C++ exception: %s\n",
               where, what != nullptr ? what : "(unknown)");
  std::fflush(stderr);
}

/* ---- last-words reporter -------------------------------------------------
 * The guards above cover the API seam on the calling thread, but abort()
 * (GGML_ABORT, CRT fail-fast) and std::terminate on llama's internal
 * threads bypass them and kill the process with 0xC0000409 and no output.
 * These handlers make that death loud: pending exception message plus a
 * symbolized backtrace on stderr, then a recognizable exit code. */

void llg_backtrace(void) {
#if defined(_WIN32)
  void *frames[62];
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(proc, nullptr, TRUE);
  USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
  for (USHORT i = 0; i < n; i++) {
    DWORD64 addr = (DWORD64)frames[i];
    char sym_buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)sym_buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    if (SymFromAddr(proc, addr, &disp, sym)) {
      if (SymGetLineFromAddr64(proc, addr, &ldisp, &line)) {
        std::fprintf(stderr, "  #%02u %s+0x%llx  [%s:%lu]\n", i, sym->Name,
                     (unsigned long long)disp, line.FileName, line.LineNumber);
      } else {
        std::fprintf(stderr, "  #%02u %s+0x%llx\n", i, sym->Name,
                     (unsigned long long)disp);
      }
    } else {
      std::fprintf(stderr, "  #%02u 0x%llx\n", i, (unsigned long long)addr);
    }
  }
  std::fflush(stderr);
#endif
}

void llg_terminate_handler(void) {
  std::fprintf(stderr,
               "asngn: llama guard: std::terminate on thread %lu — ",
#if defined(_WIN32)
               (unsigned long)GetCurrentThreadId()
#else
               0ul
#endif
  );
  std::exception_ptr p = std::current_exception();
  if (p) {
    try {
      std::rethrow_exception(p);
    } catch (const std::exception &e) {
      std::fprintf(stderr, "pending exception: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "pending exception of non-std type\n");
    }
  } else {
    std::fprintf(stderr,
                 "no pending exception (noexcept violation or direct call)\n");
  }
  std::fflush(stderr);
  llg_backtrace();
  _exit(134);
}

void llg_abort_handler(int) {
  std::fprintf(stderr,
               "asngn: llama guard: abort() raised on thread %lu\n",
#if defined(_WIN32)
               (unsigned long)GetCurrentThreadId()
#else
               0ul
#endif
  );
  std::fflush(stderr);
  llg_backtrace();
  _exit(134);
}

struct llg_last_words_installer {
  llg_last_words_installer() {
    std::set_terminate(llg_terminate_handler);
    std::signal(SIGABRT, llg_abort_handler);
  }
};
llg_last_words_installer llg_installer;

}  // namespace

extern "C" int32_t asngn_llg_tokenize(const struct llama_vocab *vocab,
                                      const char *text, int32_t text_len,
                                      llama_token *tokens,
                                      int32_t n_tokens_max, bool add_special,
                                      bool parse_special) {
  try {
    return llama_tokenize(vocab, text, text_len, tokens, n_tokens_max,
                          add_special, parse_special);
  } catch (const std::exception &e) {
    llg_report("tokenize", e.what());
  } catch (...) {
    llg_report("tokenize", nullptr);
  }
  return INT32_MIN;
}

extern "C" int32_t asngn_llg_chat_apply_template(
    const char *tmpl, const struct llama_chat_message *chat, size_t n_msg,
    bool add_ass, char *buf, int32_t length) {
  try {
    return llama_chat_apply_template(tmpl, chat, n_msg, add_ass, buf, length);
  } catch (const std::exception &e) {
    llg_report("chat_apply_template", e.what());
  } catch (...) {
    llg_report("chat_apply_template", nullptr);
  }
  return -1;
}

extern "C" int asngn_llg_sampler_sample(struct llama_sampler *smpl,
                                        struct llama_context *ctx,
                                        int32_t idx, llama_token *out) {
  try {
    *out = llama_sampler_sample(smpl, ctx, idx);
    return 0;
  } catch (const std::exception &e) {
    llg_report("sampler_sample", e.what());
  } catch (...) {
    llg_report("sampler_sample", nullptr);
  }
  return -1;
}

extern "C" int32_t asngn_llg_decode(struct llama_context *ctx,
                                    struct llama_batch batch) {
  try {
    return llama_decode(ctx, batch);
  } catch (const std::exception &e) {
    llg_report("decode", e.what());
  } catch (...) {
    llg_report("decode", nullptr);
  }
  return INT32_MIN;
}

extern "C" int32_t asngn_llg_encode(struct llama_context *ctx,
                                    struct llama_batch batch) {
  try {
    return llama_encode(ctx, batch);
  } catch (const std::exception &e) {
    llg_report("encode", e.what());
  } catch (...) {
    llg_report("encode", nullptr);
  }
  return INT32_MIN;
}

extern "C" void asngn_llg_memory_clear(llama_memory_t mem, bool data) {
  try {
    llama_memory_clear(mem, data);
  } catch (const std::exception &e) {
    llg_report("memory_clear", e.what());
  } catch (...) {
    llg_report("memory_clear", nullptr);
  }
}
