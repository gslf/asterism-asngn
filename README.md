# ⁂ asngn
## Asterism Engine

A quality-per-token agentic harness for small LLMs. 


- **Zoned context** — system · memory (Asper) · tool catalog (astools) · rolling summary · verbatim turns · working zone, each under a hard token budget, assembled deterministically (golden-tested).
- **Folding** — old turns compress into the rolling summary via the light model, with an extractive fallback; oversized tool output is digested to a short summary plus an on-disk blob the model can reopen slice by slice (`OPEN B1`).
- **Two-pass turns** — cheap grammar-constrained decision passes (`CALL` / `RECALL` / `OPEN` / `THINK` / `CLARIFY` / `ANSWER`) choose the action; only the final answer runs on the expensive tier, under an explicit terse/normal/rich budget.
- **Semantic cache** — embedding-keyed reuse and light-tier adaptation of previous answers; tool-touched entries are never replayed, only surfaced as plan hints; a world-epoch counter ties cache validity to destructive tool activity. A separate exact-key cache short-circuits repeated read-only tool calls.
- **Safety** — input/plan/action/output gates, identical-call and oscillation guards, stall watchdog, step and tool caps, secret redaction, human confirmation for destructive tools, an optional judge pass — all measured in the ledger, never hidden.
- **Telemetry** — every token attributed to a zone, role, and tier; the per-turn ledger records spend, savings, and quality; rolling QpT (quality per kilotoken) in the header of the TUI.

## Building

The siblings are expected next to this repository:

```
asterism/
├── asterism-asngn     (this repo)
├── asterism-asper     (memory sibling; brings llama.cpp)
└── asterism-astools   (tools sibling; brings the patched xCDN-C)
```

```bash
cmake -S . -B build
cmake --build build -j 8
ctest --test-dir build
```

Options: `ASNGN_BUILD_TUI` (ON), `ASNGN_BUILD_MCP` (ON), `ASNGN_BUILD_TESTS` (ON), `ASNGN_NO_THREADS` (OFF), `ASNGN_SANITIZERS` (OFF), `ASNGN_WITH_LLAMA` (ON). Paths to the siblings can be overridden with `ASNGN_ASPER_DIR` / `ASNGN_ASTOOLS_DIR`.

Artifacts: `libasngn.a` / `libasngn.dylib`, the `asngn` terminal application, and the `asngn-mcp` MCP server.

The interactive TUI currently targets POSIX terminals (Linux and macOS). On Windows CMake disables it automatically; `libasngn` and `asngn-mcp` remain available.

## Running

```bash
# interactive TUI (dark theme, yellow accent)
./build/asngn --root ~/asngn

# one-shot headless turn for scripts and pipes
./build/asngn --root ~/asngn --once "explain the torn-tail rule"

# MCP server over stdio
./build/asngn-mcp --root ~/asngn
```

The engine root holds everything as human-readable xCDN text: `sessions/<slug>/{session,transcript,summary,ledger}.xcdn`, blobs, caches, and telemetry. Models are configured in `config.xcdn` (`models.pool`); the reference pool is Qwen2.5 0.5B/1.5B/7B Instruct plus multilingual-e5-small for embeddings, sharing the weights file with Asper. Without model weights the engine still runs degraded (and the whole test suite runs on scripted fake models).

## Layout

```
include/asngn.h   public C API
src/              libasngn: session store, context engine, caches,
                  orchestrator, control loop, safety, telemetry
tui/              in-house VT terminal application (no curses)
mcp/              asngn-mcp (JSON-RPC 2.0 over stdio)
tests/            unit / integration / golden tests, scripted fakes
docs/             SPECS.md, telemetry.md
```

MIT — see [LICENSE](LICENSE).
