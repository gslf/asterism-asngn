# Quality harness (not CI-gating)

Scenario transcripts with expected routes, folds, and cache outcomes, run against real candidate models to confirm or amend the reference pool before v1. This harness needs actual GGUF weights; the CI suite never does (it runs on scripted fakes).

## Setup

Download the reference pool into `<engine-root>/models/`:

- `qwen2.5-0.5b-instruct-q4_k_m.gguf` (nano)
- `qwen2.5-1.5b-instruct-q4_k_m.gguf` (light)
- `qwen2.5-7b-instruct-q4_k_m.gguf` (std)
- `multilingual-e5-small-q8_0.gguf` (embed — the same file Asper uses)

## Running a scenario

Each `*.scenario.xcdn` file lists turns with expectations:

```
#scenario {
  name: "routing-basics",
  turns: [
    { say: "ciao!", expect: { class: "simple", mode: "direct" } },
    { say: "leggi src/main.c e dimmi cosa fa",
      expect: { mode: "plan", tools: ["fs.read"] } },
    { say: "ciao!", expect: { cache: "hit" } },
  ],
}
```

Run them with the headless CLI and compare the ledger:

```bash
./build/asngn --root /tmp/quality --once "ciao!"
# then inspect /tmp/quality/sessions/main/ledger.xcdn route fields
```

Measured axes: routing accuracy (expected vs ledgered class/mode), compression fidelity (QA over folded conversations), and paraphrase-set cache correctness (hit/adapt/miss vs expectation). Results amend the default pool, never the engine logic.
