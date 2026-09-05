# ⁂ asngn
## Asterism Engine

An outcome-gated agentic coding engine for small local LLMs.

Architecture and design: [docs/SPECS.md](docs/SPECS.md).


- **Lossless zoned context** — Asper owns exact scoped events, semantic memory, checkpoints and content-addressed objects; each call materializes only the best bounded view with the consuming model's tokenizer.
- **Continuation instead of retry** — partial output returned at a token ceiling is preserved. Artifact drafts resume from a hashed exact prefix, while oversized tool output becomes a short view plus an Asper object reopenable via `OPEN B1`.
- **Two-pass turns** — schema-constrained decision passes emit one action object per step (`{action: "call" | "recall" | "open" | "think" | "clarify" | "answer", why, input, success, fallback}`, GBNF-enforced — the in-process analogue of llama.cpp-server JSON-schema output). Routine lookups may use the cheaper planner; coding and complex work is orchestrated by the generator tier. The final answer runs under an explicit terse/normal/rich budget that is stated in the prompt as well as enforced by the backend.
- **Semantic cache** — embedding-keyed reuse and light-tier adaptation of previous answers; tool-touched entries are never replayed, only surfaced as plan hints; a world-epoch counter ties cache validity to destructive tool activity. A separate exact-key cache short-circuits repeated read-only tool calls.
- **Safety** — input/plan/action/output gates, identical-call and oscillation guards, stall watchdog, step and tool caps, secret redaction, human confirmation for destructive tools, an optional judge pass — all measured in the ledger, never hidden.
- **Telemetry** — every token attributed to a zone, role, and tier; the per-turn ledger records spend and savings. QpT remains diagnostic only: coding quality is gated by task success, passing tests, applicable patches, valid tool calls, regressions, latency, and memory.

---

## Building from zero

Five steps on every platform: **prerequisites → clone → build → models →
tools**. Each is spelled out below for Linux, macOS and Windows, with and
without a GPU. If something goes wrong, see
[Troubleshooting](#troubleshooting).

### 1. Prerequisites

| Platform | Required | For the GPU build (optional) |
|---|---|---|
| **Linux** | git, CMake ≥ 3.16, GCC or Clang, make/ninja | NVIDIA driver + [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads) ≥ 12 |
| **macOS** | git, CMake ≥ 3.16, Xcode Command Line Tools (`xcode-select --install`) | nothing extra — Metal ships with macOS |
| **Windows** | git, CMake ≥ 3.16, Visual Studio 2022+ with the **Desktop development with C++** workload | NVIDIA driver + [CUDA Toolkit](https://developer.nvidia.com/cuda-downloads) ≥ 12 (install *after* Visual Studio so its VS integration lands) |

The model-download script additionally needs `curl` and Python 3; install
both explicitly when they are not present. On Windows, `curl.exe` ships
with current OS releases and Python is available from
[python.org](https://www.python.org/downloads/) or the Microsoft Store. The engine itself has no runtime
dependency on either.

On Windows run everything below from a **Developer PowerShell for VS** (or
any shell with CMake and git on PATH).

### 2. Clone the workspace

asngn expects its three sibling repositories next to it (paths can be
overridden with `ASNGN_ASPER_DIR` / `ASNGN_ASTOOLS_DIR` /
`ASNGN_ASMODEL_DIR`):

```
asterism/
├── asterism-asngn     (this repo)
├── asterism-asper     (memory sibling; brings llama.cpp)
├── asterism-astools   (tools sibling; brings the patched xCDN-C)
└── asterism-asmodel   (shared model runtime)
```

Same commands on every platform — `--recurse-submodules` matters, the
siblings pin llama.cpp and xCDN-C as submodules:

```bash
mkdir asterism && cd asterism
git clone --recurse-submodules https://github.com/gslf/asterism-asngn
git clone --recurse-submodules https://github.com/gslf/asterism-asper
git clone --recurse-submodules https://github.com/gslf/asterism-astools
git clone https://github.com/gslf/asterism-asmodel
```

### 3. Configure and build

All commands run from inside `asterism-asngn`. Pick **one** configure line
(CPU or GPU), then build.

No manual step is needed for the vendored llama.cpp: asper ships local
patches under `asterism-asper/deps/patches/llama/` (currently one that
stops a grammar-sampler edge case from aborting the whole process) and its
CMake applies them to the `deps/llama.cpp` submodule at configure time,
idempotently — a fresh clone or a submodule re-checkout just needs a
re-run of cmake, which happens on the next build anyway. `git` must be on
PATH (it already is if you cloned). The submodule will show up as
"modified content" in `git status`; that is the applied patch, not local
noise. If a future submodule bump makes a patch stop applying, the
configure fails loudly with instructions instead of silently building an
unpatched llama.

The engine is model-agnostic by construction: every llama.cpp call that
model-controlled data reaches (tokenize, chat template, decode/encode,
sampler) goes through `src/llama_guard.cpp`, a C++ shim that converts any
exception llama.cpp lets escape into a normal model error. A model whose
tokenizer, template, or grammar interaction misbehaves degrades that one
turn; it can no longer kill the process.

#### Linux

```bash
# CPU only
cmake -S . -B build
```

```bash
# NVIDIA GPU (CUDA)
cmake -S . -B build -DGGML_CUDA=ON
```

```bash
cmake --build build -j 8
ctest --test-dir build
```

#### macOS

```bash
# CPU only
cmake -S . -B build
```

```bash
# Apple GPU (Metal)
cmake -S . -B build -DGGML_METAL=ON
```

```bash
cmake --build build -j 8
ctest --test-dir build
```

#### Windows

Same shape, but MSVC is a multi-config generator: pass `--config Release`
to every build and `-C Release` to ctest, and the binaries land in
`build\Release\` instead of `build\`.

```bash
# CPU only
cmake -S . -B build
```

```bash
# NVIDIA GPU (CUDA)
cmake -S . -B build -DGGML_CUDA=ON
```

```bash
cmake --build build --config Release -j 8
ctest --test-dir build -C Release
```

Notes that apply to every platform:

- The GPU flag builds llama.cpp's GPU backend into the binaries; whether a
  given model actually runs on the GPU is decided at runtime by
  `gpu_layers` in the config (default: everything on the GPU — see
  [GPU vs CPU at runtime](#gpu-vs-cpu-at-runtime)). A GPU build still runs
  fine on machines without the GPU libraries loaded.
- The full test suite runs on scripted fake models — it needs **no model
  weights and no GPU**, and must pass identically on the CPU and GPU
  builds.
- Build options: `ASNGN_BUILD_TUI` (ON), `ASNGN_BUILD_MCP` (ON),
  `ASNGN_BUILD_TESTS` (ON), `ASNGN_NO_THREADS` (OFF), `ASNGN_SANITIZERS`
  (OFF), `ASNGN_WITH_LLAMA` (ON).
- Artifacts: `libasngn.a` / `libasngn.dylib` (`asngn.lib` / `asngn.dll` on
  Windows), the `asngn` terminal application, and the `asngn-mcp` MCP
  server.

### 4. Download the model weights

The general profile runs without weights (degraded: no model calls, tools and
sessions still work), but for real use you want the reference pool —
Qwen2.5 0.5B / 1.5B / 7B Instruct plus multilingual-e5-small for
embeddings, ~5.9 GB total, placed in `<engine-root>/models/` under the
exact filenames the default config expects.

One command; the engine root is created if missing. Every source URL is
pinned to an immutable revision and checked against the size and SHA-256 in
`scripts/models.manifest.tsv`. Re-running verifies existing files; use
`--repair` (`-Repair` on PowerShell) to replace a mismatch.

```bash
# Linux / macOS                     (default root: ~/asngn)
scripts/fetch-models.sh ~/asngn
```

```bash
# Windows                           (default root: %USERPROFILE%\asngn)
powershell -ExecutionPolicy Bypass -File scripts\fetch-models.ps1 $env:USERPROFILE\asngn
```

Downloading by hand instead: fetch these four files into
`<engine-root>/models/` with the names on the left.

| File in `models/` | Source |
|---|---|
| `qwen2.5-0.5b-instruct-q4_k_m.gguf` | [Qwen/Qwen2.5-0.5B-Instruct-GGUF](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF) |
| `qwen2.5-1.5b-instruct-q4_k_m.gguf` | [Qwen/Qwen2.5-1.5B-Instruct-GGUF](https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF) |
| `qwen2.5-7b-instruct-q4_k_m.gguf` | [bartowski/Qwen2.5-7B-Instruct-GGUF](https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF) — `Qwen2.5-7B-Instruct-Q4_K_M.gguf`, renamed (the official Qwen repo only ships this quant split in two parts) |
| `multilingual-e5-small-q8_0.gguf` | [cstr/multilingual-e5-small-GGUF](https://huggingface.co/cstr/multilingual-e5-small-GGUF) — then run `python3 scripts/gguf_add_kv.py <file> tokenizer.ggml.token_type_count 2` (that conversion predates a metadata key llama.cpp now requires for BERT-style models; the fetch scripts do this for you) |

### 5. Install the tool registry

The engine loads its tools from `<engine-root>/tools/` (the
`integration.astools.root` config key, default `tools`), which starts out
empty. The build already produced a ready registry in `build/packages/` —
the seven standard packages (`edit`, `env`, `fs`, `git`, `grep`, `proc`,
`sys`), each a directory with a `manifest.xcdn` and a
`bin/<os-arch>/astools-std[.exe]` binary. Copy them across:

```bash
# Linux / macOS
mkdir -p ~/asngn/tools
cp -R build/packages/* ~/asngn/tools/
```

```bash
# Windows
New-Item -ItemType Directory -Force $env:USERPROFILE\asngn\tools | Out-Null
Copy-Item -Recurse -Force build\packages\* $env:USERPROFILE\asngn\tools\
```

Alternatively ask the fetch script to install the registry explicitly:

```bash
scripts/fetch-models.sh ~/asngn --install-tools
powershell -File scripts\fetch-models.ps1 $env:USERPROFILE\asngn -InstallTools
```

Without this flag the model downloader never changes the tool registry.

Without the registry the engine still runs, but the model can never act:
the tool catalog is empty, the decision grammar drops the `call`
action, and every request is answered chat-only.

## Running

POSIX (binaries in `build/`):

```bash
# interactive TUI (dark theme, yellow accent)
./build/asngn
```

```bash
# one-shot headless turn for scripts and pipes
./build/asngn --once "create a small C++ program"
```

```bash
# MCP server over stdio
./build/asngn-mcp
```

Windows (binaries in `build\Release\`; the TUI needs a VT-capable console —
any Windows 10+ console works, Windows Terminal recommended):

```powershell
.\build\Release\asngn.exe
```

```powershell
.\build\Release\asngn.exe --once "create a small C++ program"
```

```powershell
.\build\Release\asngn-mcp.exe
```

With no arguments the engine root is `~/asngn` (`%USERPROFILE%\asngn` on
Windows), and `<engine-root>/config.xcdn` is discovered automatically.
`engine.root` and `integration.astools.workspace` belong in that file;
`--root`, `--config`, and `--workspace` are explicit overrides, not required
startup ceremony.

Reusable starter configurations live together under [`examples/`](examples/README.md):
one for embedded GGUF models, one for LM Studio, and a companion astools
policy. Copy them to a separate engine root and replace the marked model/tool
paths. The repository itself is not an engine root and should never accumulate
sessions, memory, cache, telemetry, logs, models, or generated workspaces.

By default `integration.astools.workspace: "session"` gives every session an
isolated writable tree at `sessions/<slug>/workspace/`. Relative tool paths
are resolved only there. Operational metadata (`session.xcdn`, ledger) stays
beside the workspace; all conversation/checkpoint/object memory lives under
Asper's `memory/` root and is never exposed as the tool working directory.
Set a concrete workspace path, or pass `--workspace`, only when a
session is deliberately meant to operate on an external checkout.

Sessions have a persistent mode and security profile. In the TUI, use
`/session new <slug>`, `/mode chat|coding|automate`, and `/profile <name>`.
Mode changes select a safe default profile; `/profile` can then choose one of
`chat`, `coding-readonly`, `coding-sandboxed`, or `automation-ci`. Denied or
missing authorization is reported in the workflow instead of being hidden;
switching profile or granting the tool permission enables the action.

## Shared model runtime and API providers

asngn and embedded Asper share one `asmodel` runtime. Asper's curator
borrows the configured compressor slot and retrieval borrows the embedder
slot, so weights and reusable contexts/KV allocations are not loaded twice.
Standalone Asper/MCP creates its own manager and remains independent.

Every pool entry may be an embedded GGUF or an OpenAI-compatible endpoint:

```xcdn
#asngn_config {
  models: {
    max_resident: 3,
    max_ram_mb: 16000,
    max_vram_mb: 12000,
    pool: [
      {
        id: "remote-chat", backend: "openai",
        base_url: "http://127.0.0.1:1234/v1",
        model: "qwen3-8b",
        api_key_env: "LOCAL_LLM_API_KEY",
        provider: "lmstudio", // "llama-server" | "lmstudio" | "vllm"
        ctx: 32768, warm: true, kv_cache: true,
      },
      {
        id: "remote-embed", backend: "openai",
        base_url: "http://127.0.0.1:1234/v1",
        model: "text-embedding-nomic-embed-text-v1.5",
        embedding: true, dim: 768, ctx: 512,
      },
    ],
    roles: {
      router: "remote-chat", planner: "remote-chat",
      generator: "remote-chat", compressor: "remote-chat",
      adapter: "remote-chat", judge: "remote-chat",
      embedder: "remote-embed",
    },
  },
}
```

`api_key_env` is the name of an environment variable, not the credential.
`provider` selects an asmodel protocol profile; OpenAI-shaped endpoints are
not assumed to have interchangeable extensions. Decision, classifier, and
judge calls require constrained output and reasoning-off per request. Draft
and answer calls keep the model's normal reasoning behavior. An unsupported
combination fails closed instead of dropping a control. Provider selection is
explicit; reasoning policy is exclusively per request.
Remote long-form calls use Chat Completions SSE on LM Studio, llama.cpp server,
and vLLM. Token budgets, not elapsed time, bound inference by default:
`safety.turn_deadline` and `safety.stall_timeout` are both `PT0S` (disabled).
`Esc`, `Ctrl+C`, or `asngn_task_cancel()` immediately aborts the in-flight
provider request. A client may still opt into a per-turn `deadline_ms`, and an
operator may configure a nonzero stall timeout; neither path retries.
When an endpoint reports `finish_reason=length`, asmodel returns the decoded
partial bytes with the limit status. ASNGN consumes them only in resumable
phases; it never pays again for the same completed prefix.
`ram_mb`/`vram_mb` may be declared per entry when automatic estimates
are not appropriate; the manager evicts the least-recently-used idle model
to stay within resident, RAM and VRAM budgets. `warm: false` leaves a slot
lazy.

## GPU vs CPU at runtime

Compiling with `-DGGML_CUDA=ON` / `-DGGML_METAL=ON` only makes the GPU
*available*; each model in the pool decides its own placement through
`gpu_layers` in `config.xcdn`:

- `gpu_layers: -1` — every layer in VRAM (**the default**)
- `gpu_layers: 0` — CPU only
- `gpu_layers: N` — offload N layers, rest on CPU (for VRAM-tight setups)

The same knob exists for Asper's memory models (`curator.gpu_layers`,
`embedding.gpu_layers` in Asper's own config). On a CPU-only build the
value is ignored entirely — one config works everywhere.

To confirm the GPU is actually in use, watch VRAM while a turn runs
(`nvidia-smi` on NVIDIA, Activity Monitor's GPU history on macOS): loading
the 7B model should claim several GB.

## Bigger context and longer answers

The defaults use a professional 32k profile: enough room for substantial
tool traces, source drafts, and long-form answers without starving the
response pass. On machines with less memory, reduce the model contexts and
zone budgets together rather than shrinking only the answer cap. The core
profile is equivalent to:

```
#asngn_config {
  models: {
    pool: [
      { id: "nano",  path: "models/qwen2.5-0.5b-instruct-q4_k_m.gguf",
        ctx: 8192,  threads: 4, gpu_layers: -1 },
      { id: "light", path: "models/qwen2.5-1.5b-instruct-q4_k_m.gguf",
        ctx: 32768, threads: 6, gpu_layers: -1 },
      { id: "std",   path: "models/qwen2.5-7b-instruct-q4_k_m.gguf",
        ctx: 32768, threads: 8, gpu_layers: -1 },
      { id: "embed", path: "models/multilingual-e5-small-q8_0.gguf",
        ctx: 512, threads: 4, embedding: true, dim: 384 },
    ],
    sampling: {
      classify: { max_tokens: 64 },
      decide:   { max_tokens: 1024 },
      // Optional ceiling. Omit it to use all context remaining after the
      // prompt; artifact drafts never inherit the answer detail cap.
      draft:    { temp: 0.2, top_p: 0.9 },
      judge:    { max_tokens: 128 },
    },
  },
  detail: {                    // per-answer output budgets (tokens)
    terse_tokens:  1024,
    normal_tokens: 4096,
    rich_tokens:   10240,      // /detail rich, or --detail rich
  },
  context: {                   // zone budgets inside the 32k window
    memory_checkpoint_tokens: 3072,
    memory_history_tokens:    8192,
    working_tokens:  6144,
    safety_margin:   512,
    digest_threshold_chars: 32768,
    digest_tokens:   2048,
    pinned_max:      32,
  },
  integration: {
    astools: { catalog_chars: 24000 },
  },
  safety: {
    // Disabled: token budgets bound work; Esc/Ctrl+C cancels manually.
    turn_deadline: "PT0S",
    stall_timeout: "PT0S",
  },
}
```

Model paths are relative to the engine root. Rough VRAM guide for this
profile with everything on the GPU: the three Qwen models plus 32k KV
caches settle around 11–12 GB; halve `ctx` (or set `gpu_layers: 0` on
`light`) to fit smaller cards.

## Interactive TUI controls

The input prompt bar is always at least three rows high and expands up to
six rows. Longer prompts remain editable in a scrolling viewport that follows
the text insertion point during editing; arrows at the right edge indicate
hidden rows. `Alt+↑` / `Alt+↓` scroll this viewport one line without
moving the insertion point, and `Alt+PgUp` / `Alt+PgDn` scroll it one page.
Typing or moving the insertion point resumes automatic following. Pressing
`Enter` sends the prompt and clears the prompt bar immediately.

Chat and input history use separate controls:

- `↑` / `↓` scroll the chat one line; `PgUp` / `PgDn` scroll one page.
- `Alt+↑` / `Alt+↓` scroll the prompt one line; `Alt+PgUp` / `Alt+PgDn`
  scroll it one page without moving the insertion point.
- `Ctrl+P` / `Ctrl+N` select the previous or next prompt from input history.
- `Home` / `End` jump to the beginning or end of the prompt and move its
  viewport accordingly.
- `Alt+Enter` or `Ctrl+J` inserts a newline without sending.
- `Ctrl+U` / `Ctrl+K` / `Ctrl+W` delete to the start, end, or previous word;
  `Ctrl+Y` restores the last deleted text.
- `F1` opens the complete in-app key reference.
- Operational rationales appear live in blue; final assistant output keeps the
  normal foreground color. Rationales are short and redacted.

## Troubleshooting

- **`ASNGN_ERR_MODEL: model 'std': failed to load models/...`** — the
  weights are missing from `<engine-root>/models/`: run step 4. Paths in
  the config are engine-root-relative.
- **The model never calls tools — it answers with CLI instructions
  ("run `git status` yourself") instead of acting** — the tool registry is
  empty: `<engine-root>/tools/` has no packages, so the decision grammar
  has no `call` action and the model literally cannot emit one. Run
  step 5 (copy `build/packages/*` into `<engine-root>/tools/`).
- **`bert model needs to define token type count`** — the embedder GGUF
  lacks the `tokenizer.ggml.token_type_count` metadata key (common in
  older community conversions): `python3 scripts/gguf_add_kv.py <file>
  tokenizer.ggml.token_type_count 2`, or just use the fetch script.
- **“the interactive TUI needs a terminal”** — stdin/stdout is a pipe, or
  `TERM=dumb`, or (Windows) the console has no VT support. Use a real
  terminal (Windows 10+ console / Windows Terminal), or `--once` for
  scripted use.
- **Windows: `LNK1104: cannot open ... asngn.exe`** — the TUI is still
  running and holds the file; close it and rebuild.
- **Windows sandbox note** — tool sandboxing rides on Job Objects (cpu /
  memory / process-count caps are kernel-enforced); the `strict` level
  (fs confinement, network deny) is not wired on Windows yet and degrades
  to `basic` with a warning.
- **First turn after changing the embedder file** — a one-time
  `embedding cache invalid ... vectors rebuild lazily` warning is
  expected; the cache rebuilds itself.

## Layout

```
include/asngn.h   public C API
src/              libasngn: session store, context engine, caches,
                  orchestrator, control loop, safety, telemetry
tui/              in-house VT terminal application (no curses)
mcp/              asngn-mcp (JSON-RPC 2.0 over stdio)
scripts/          model-pool fetch + tool-registry install helpers
                  (sh / ps1), GGUF kv patcher
tests/            unit / integration / golden tests, scripted fakes
docs/             SPECS.md, telemetry.md
```

MIT — see [LICENSE](LICENSE).

Design and operational details for hybrid retrieval, evidence metadata, the turn WAL and concurrent session scheduling: [retrieval-memory-transactions](docs/retrieval-memory-transactions.md).
