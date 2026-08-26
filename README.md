# ⁂ asngn
## Asterism Engine

An outcome-gated agentic coding engine for small local LLMs.


- **Zoned context** — system · memory (Asper) · tool catalog (astools) · rolling summary · verbatim turns · working zone, each under a hard token budget, assembled deterministically (golden-tested).
- **Folding** — old turns compress into the rolling summary via the light model, with an extractive fallback; oversized tool output is digested to a short summary plus an on-disk blob the model can reopen slice by slice (`OPEN B1`).
- **Two-pass turns** — cheap grammar-constrained decision passes (`CALL` / `RECALL` / `OPEN` / `THINK` / `CLARIFY` / `ANSWER`) choose the action; only the final answer runs on the expensive tier, under an explicit terse/normal/rich budget.
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

asngn expects its two sibling repositories next to it (paths can be
overridden with `ASNGN_ASPER_DIR` / `ASNGN_ASTOOLS_DIR`):

```
asterism/
├── asterism-asngn     (this repo)
├── asterism-asper     (memory sibling; brings llama.cpp)
└── asterism-astools   (tools sibling; brings the patched xCDN-C)
```

Same commands on every platform — `--recurse-submodules` matters, the
siblings pin llama.cpp and xCDN-C as submodules:

```bash
mkdir asterism && cd asterism
git clone --recurse-submodules https://github.com/gslf/asterism-asngn
git clone --recurse-submodules https://github.com/gslf/asterism-asper
git clone --recurse-submodules https://github.com/gslf/asterism-astools
```

### 3. Configure and build

All commands run from inside `asterism-asngn`. Pick **one** configure line
(CPU or GPU), then build.

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
the tool catalog is empty, the decision grammar drops the `CALL`
alternative, and every request is answered chat-only.

## Running

POSIX (binaries in `build/`):

```bash
# interactive TUI (dark theme, yellow accent)
./build/asngn --root ~/asngn --workspace /path/to/repository
```

```bash
# one-shot headless turn for scripts and pipes
./build/asngn --root ~/asngn --workspace /path/to/repository --once "fix the failing tests"
```

```bash
# MCP server over stdio
./build/asngn-mcp --root ~/asngn --workspace /path/to/repository
```

Windows (binaries in `build\Release\`; the TUI needs a VT-capable console —
any Windows 10+ console works, Windows Terminal recommended):

```bash
.\build\Release\asngn.exe --root $env:USERPROFILE\asngn
```

```bash
.\build\Release\asngn.exe --root $env:USERPROFILE\asngn --once "explain the torn-tail rule"
```

```bash
.\build\Release\asngn-mcp.exe --root $env:USERPROFILE\asngn
```

The engine root holds everything as human-readable xCDN text:
`sessions/<slug>/{session,transcript,summary,ledger}.xcdn`, blobs, caches,
and telemetry. Configuration lives in `<engine-root>/config.xcdn`
(discovered automatically; `--config` overrides). Without a config file
the built-in defaults apply.

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

The defaults are sized for small machines (8k context, answers capped at
1024 tokens). The Qwen2.5 GGUFs natively support **32k context**; a
machine with ≥16 GB VRAM (or plenty of RAM on CPU) runs comfortably with
all budgets raised. Drop this into `<engine-root>/config.xcdn` as a
writing / knowledge-base profile:

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
  },
  detail: {                    // per-answer output budgets (tokens)
    terse_tokens:  512,
    normal_tokens: 2048,
    rich_tokens:   8192,       // /detail rich, or --detail rich
  },
  context: {                   // zone budgets inside the 32k window
    summary_tokens:  2000,
    verbatim_tokens: 10000,
    working_tokens:  4000,
    fold_tokens:     400,
    digest_threshold_chars: 8192,
    digest_tokens:   512,
  },
  safety: {
    turn_deadline: "PT10M",    // long rich answers need the headroom
  },
}
```

Model paths are relative to the engine root. Rough VRAM guide for this
profile with everything on the GPU: the three Qwen models plus 32k KV
caches settle around 11–12 GB; halve `ctx` (or set `gpu_layers: 0` on
`light`) to fit smaller cards.

## Troubleshooting

- **`ASNGN_ERR_MODEL: model 'std': failed to load models/...`** — the
  weights are missing from `<engine-root>/models/`: run step 4. Paths in
  the config are engine-root-relative.
- **The model never calls tools — it answers with CLI instructions
  ("run `git status` yourself") instead of acting** — the tool registry is
  empty: `<engine-root>/tools/` has no packages, so the decision grammar
  has no `call` rule and the model literally cannot emit `CALL`. Run
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
