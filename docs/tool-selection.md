# Tool selection

Each action turn creates a command snapshot using the user's request as a bounded
lexical query. The model can replace it with:

```xcdn
{action: "discover", why: "find the test workflow", input: "project test"}
```

Discovery consumes one action step but performs no inference or tool execution.
The resulting catalog reports selected/omitted counts, descriptions and argument
schemas. GBNF, JSON output constraints and invocation use this same snapshot.
Commands outside it receive `asngn/not-selected`; discovery remains available even
when the selection is empty. `no_tools` disables both discovery and execution.

Configuration:

```xcdn
integration: { astools: { tool_limit: 16, tool_schema_bytes: 24000 } }
```

The maximum is 64 commands and 1 MiB of schema/metadata reserves. These replace
`catalog_level` and `catalog_chars`. Context admission still measures the assembled
prompt, including formatting and instructions; this byte budget is not an exact
token count. Ranking is a bounded metadata heuristic, with no measured multilingual
or task-success guarantee. A specific name/query can recover an omitted command.

The optional [native action protocol](native-actions.md) exports this same
selection as function schemas, with at most 59 tools plus runtime controls.

The host filters disabled runtimes, statically impossible permissions and commands
excluded by the read-only profile. Argument-specific path checks remain mandatory.
Before confirmation or a cached result, Astools validates the selected package,
arguments and current policy. Cache keys include package bytes and workspace state.
After queue admission, invocation checks identity and availability again. Changes
require a fresh discovery. Invocation propagates cancellation and waits for the
tool to settle before the turn can release its state.

Package identity covers manifests and package-relative entry artifacts; external
toolchains and interpreter dependencies require independent version policies.
The check and OS process creation are not atomic against an external writer.
Metadata annotations narrow host policy but cannot grant OS capabilities. Existing
confirmation decisions are not yet durable across restart.

Contract tests cover consistent schemas/grammar/catalog, explicit version lookup,
read-only filtering, revocation, actual invocation and a full turn that discovers
and executes a command missing from its initial selection.
