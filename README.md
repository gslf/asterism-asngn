# ⁂ asngn - asterism engine

### A general-purpose agent harness for creating and completing real-world workflows and automations, with evidence-driven execution.

>⁂ asterism is a modular agent harness that turns language models into tools for creating and completing real-world workflows and automations. **SLM** and **local inference** friendly. Read the central [architecture decisions and system value](https://github.com/gslf/asterism-asngn/blob/main/docs/ARCHITECTURE.md).

| Project | Responsibility in the harness |
|---|---|
| [⁂ asngn](https://github.com/gslf/asterism-asngn) | Workflow orchestration, task state, execution policy and outcome checks |
| [⁂ asper](https://github.com/gslf/asterism-asper) | Durable memory, exact evidence, checkpoints and bounded context |
| [⁂ astools](https://github.com/gslf/asterism-astools) | Discoverable tool contracts, permissions and supervised actions |
| [⁂ asmodel](https://github.com/gslf/asterism-asmodel) | Shared inference resources and explicit provider capabilities |

The current distribution includes general system tools and specialized coding workflows. Other application domains require appropriate tool packages and host integrations. Persistent acceptance verification currently covers supported `project` workflows; broader domain-specific verifiers remain extension work.
S
ee [acceptance contracts](docs/acceptance.md).

Architecture and design: [docs/SPECS.md](docs/SPECS.md).


- **Lossless zoned context** — ⁂ asper owns exact scoped events, semantic memory, checkpoints and content-addressed objects. Each call materializes only the best bounded view with explicit exact/estimated token accounting.
- **Continuation instead of retry** — partial output returned at a token ceiling is preserved. Artifact drafts resume from a hashed exact prefix, while oversized tool output becomes a diagnostic view plus an ⁂ asper object reopenable at an explicit byte offset. [Evidence and context](docs/evidence-context.md).
- **Two-pass turns** — schema-constrained decision passes emit one action object per step (`{action: "call" | "discover" | "recall" | "open" | "think" | "clarify" | "answer", why, input, success, fallback}`, GBNF-enforced — the in-process analogue of llama.cpp-server JSON-schema output). Routine lookups may use the cheaper planner; complex workflows, including coding, are orchestrated by the generator tier. The final answer runs under an explicit terse/normal/rich budget that is stated in the prompt as well as enforced by the backend.
- **Semantic cache** — embedding-keyed reuse and light-tier adaptation of previous answers; tool-touched entries are never replayed, only surfaced as plan hints; a world-epoch counter ties cache validity to destructive tool activity. A separate exact-key cache short-circuits repeated read-only tool calls.
- **Safety** — input/plan/action/output gates, identical-call and oscillation guards, stall watchdog, step and tool caps, secret redaction, [durable action approvals](docs/approvals.md) with complete argument review, an optional judge pass — all measured in the ledger, never hidden.
- **Telemetry** — per-turn attribution and savings plus a separate, durable operation log for inference consumption, including failed or cancelled calls. Unknown usage retains its reservation. QpT remains diagnostic only: quality must be assessed against the requested outcome and observed evidence, alongside latency and memory. Coding checks include passing tests, applicable patches, valid tool calls and regressions.

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

⁂ asngn expects its three sibling repositories next to it (paths can be
overridden with `ASNGN_ASPER_DIR` / `ASNGN_ASTOOLS_DIR` /
`ASNGN_ASMODEL_DIR`):

```
asterism/
├── asterism-asngn     (this repo)
├── asterism-asper     (memory sibling; brings llama.cpp)
├── asterism-astools   (tools sibling; brings the pinned xCDN-C)
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

The build consumes ⁂ asper's pinned, unmodified llama.cpp submodule. CMake
configuration never patches a dependency working tree. Native calls go through
`src/llama_guard.cpp`, which translates escaping C++ exceptions into model
errors; it does not contain `abort`, segmentation faults or process termination.
Use a separate serving process when native crash isolation is required.

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
  (OFF), `ASNGN_WITH_LLAMA` (ON), `ASNGN_BUILD_ACP` (ON for threaded POSIX
  builds, OFF elsewhere).
- Artifacts: `libasngn.a` / `libasngn.dylib` (`asngn.lib` / `asngn.dll` on
  Windows), the ⁂ asngn terminal application, and the `asngn-mcp` MCP
  server. Threaded POSIX builds also include the limited `asngn-acp` editor host.

### 4. Choose model execution

For hosted models or a separate inference server, configure the model pool as
shown in [API providers](#shared-model-runtime-and-api-providers) and proceed to
tool installation. The weight-download instructions below apply to embedded
local inference. The reference SLM pool is a starting configuration; larger
models and mixed local/remote pools are also valid choices.

#### Download weights for embedded inference

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

For a local Linux runtime archive built from checked component pins, see
[distribution and relocation checks](docs/distribution.md). The current package
uses remote providers and is an unsigned development artifact.

POSIX (binaries in `build/`):

```bash
# interactive TUI 
./build/asngn
```

```bash
# one-shot headless turn for scripts and pipes
./build/asngn --once "Inspect the workspace and summarize its files by type"
```

```bash
# MCP server over stdio
./build/asngn-mcp
```

For an editor that accepts a custom ACP stdio agent, configure
`build/asngn-acp --workspace /absolute/repository --config /absolute/config.xcdn`.
The initial host supports text, resource links, streaming and bound permissions
for one operator-selected workspace. Read the [supported ACP profile](docs/acp.md)
before enabling it; supplied MCP servers and session loading are not yet supported.

Windows (binaries in `build\Release\`; the TUI needs a VT-capable console —
any Windows 10+ console works, Windows Terminal recommended):

```powershell
.\build\Release\asngn.exe
```

```powershell
.\build\Release\asngn.exe --once "Inspect the workspace and summarize its files by type"
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
one for embedded GGUF models, one for LM Studio, and a companion ⁂ astools
policy. Copy them to a separate engine root and replace the marked model/tool
paths. The repository itself is not an engine root and should never accumulate
sessions, memory, cache, telemetry, logs, models, or generated workspaces.

By default `integration.astools.workspace: "session"` gives every session an
isolated writable tree at `sessions/<slug>/workspace/`. Relative tool paths
are resolved only there. Operational metadata (`session.xcdn`, ledger) stays
beside the workspace; all conversation/checkpoint/object memory lives under
⁂ asper's `memory/` root and is never exposed as the tool working directory.
Set a concrete workspace path, or pass `--workspace`, only when a
session is deliberately meant to operate on an external checkout.

Sessions have a persistent mode and security profile. In the TUI, use
`/session new <slug>`, `/mode chat|coding|automate`, and `/profile <name>`.
Mode changes select a safe default profile; `/profile` can then choose one of
`chat`, `coding-readonly`, `coding-sandboxed`, or `automation-ci`. Denied or
missing authorization is reported in the workflow instead of being hidden;
switching profile or granting the tool permission enables the action.

## Host SDKs

[Python and JavaScript/TypeScript SDKs](sdk/README.md) expose asynchronous tasks,
cursor events, cancellation, acceptance criteria and read-only approval inspection
over the local MCP server. They ship without runtime dependencies and distinguish
committed turns from verified task success. Live task handles are process-local;
[durable task observations](docs/task-recovery.md) remain readable after release
or server restart through `agent_recover`.

## Shared model runtime and API providers

⁂ asngn and embedded ⁂ asper share one ⁂ asmodel runtime. ⁂ asper's curator
borrows the configured compressor slot and retrieval borrows the embedder
slot, so weights and reusable contexts/KV allocations are not loaded twice.
Standalone ⁂ asper/MCP creates its own manager and remains independent.

Pool entries can mix embedded GGUF models with compatible HTTP endpoints on
localhost, another machine or a hosted service. Local model downloads are not
required for roles served remotely. Model size is a deployment choice, not a
restriction of the harness. APIs outside the implemented profiles require a
provider adapter; required structured-output and reasoning controls still apply.

For example, the following configuration uses a local HTTP server:

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

`api_key_env` is the name of an environment variable, **not** the credential.

## GPU vs CPU at runtime

Compiling with `-DGGML_CUDA=ON` / `-DGGML_METAL=ON` only makes the GPU
*available*; each model in the pool decides its own placement through
`gpu_layers` in `config.xcdn`:

- `gpu_layers: -1` — every layer in VRAM (**the default**)
- `gpu_layers: 0` — CPU only
- `gpu_layers: N` — offload N layers, rest on CPU (for VRAM-tight setups)

The same knob exists for ⁂ asper's memory models (`curator.gpu_layers`,
`embedding.gpu_layers` in ⁂ asper's own config). On a CPU-only build the
value is ignored entirel. One config works everywhere.

The optional [native action protocol](docs/native-actions.md) sends selected tools
as native function schemas and retains correlated results. Enable it explicitly
for a verified provider configuration; the embedded backend currently uses the
constrained controller.

## Bigger context and longer answers

The defaults use a 32k profile, enough room for substantial
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
    astools: { tool_limit: 16, tool_schema_bytes: 24000 },
  },
  safety: {
    // Disabled: token budgets bound work; Esc/Ctrl+C cancels manually.
    turn_deadline: "PT0S",
    stall_timeout: "PT0S",
  },
}
```

Model paths are relative to the engine root.

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



## License
MIT [LICENSE](LICENSE).