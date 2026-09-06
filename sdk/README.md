# Local host SDKs

Python and JavaScript/TypeScript clients launch `asngn-mcp` with an explicit argv,
initialize MCP stdio revision `2025-06-18`, and require Asterism contract version 1.
They have no runtime dependencies beyond Python 3.11+ or Node.js 22+.
This is the host API for this engine, not a general MCP server importer or ACP adapter.

The server advertises `capabilities.experimental["dev.asterism/asngn"].contractVersion`.
A missing or different version is rejected; the SDK does not guess an older contract.
`tools/list` continues to expose the complete JSON Schemas of the underlying tools.

## Install from this checkout

```sh
python3 -m pip install ./sdk/python
npm install ./sdk/javascript
```

No registry publication is implied. The Python distribution is `asterism-sdk`
(import `asterism`); the Node package is `@asterism/sdk`. Both are version 0.1.0.
Node ships ESM source and TypeScript declarations, with no transpilation at runtime.
Python uses setuptools only to build its wheel. Each package includes its license.

## Python

```python
import asyncio
from asterism import Client

async def main():
    async with await Client.start(["asngn-mcp", "--root", "/absolute/engine/store",
                                  "--config", "/absolute/engine.xcdn"]) as client:
        session = client.session("example")
        task = await session.submit("Inspect the repository and report missing tests")
        async for page in task.updates(timeout=120):
            if page["cursor_gap"]:
                print("Some events are no longer retained")
            for event in page["events"]:
                print(event["text"], end="")
            if page["done"]:
                print(page["outcome"], page["task_state"], page["answer"])
        await task.release()

asyncio.run(main())
```

Python durations are **seconds**. Task cancellation uses `await task.cancel()`;
cancelling the Python coroutine stops the local wait only.

## JavaScript / TypeScript

```typescript
import { Client } from '@asterism/sdk';

const client = await Client.start(['asngn-mcp', '--root', '/absolute/engine/store',
                                 '--config', '/absolute/engine.xcdn']);
try {
  const task = await client.session('example').submit('Inspect the repository and report missing tests');
  for await (const page of task.updates({ timeout: 120_000 })) {
    if (page.cursor_gap) console.error('Some events are no longer retained');
    for (const event of page.events) process.stdout.write(event.text);
    if (page.done) console.log(page.outcome, page.task_state, page.answer);
  }
  await task.release();
} finally {
  await client.close();
}
```

JavaScript durations are **milliseconds**, positive and at most 2,147,483,647. Request/watch options accept
`AbortSignal`; abort stops the local wait only. `await task.cancel()` requests
engine cancellation. `Client` also implements `Symbol.asyncDispose`.

## Lifecycle and results

- `session(slug)` selects a store session; its first operation opens it on the server.
  `submit` returns immediately after admission. `client.task(id)` attaches another
  observer on the **same live client/server**; it does not recover a crashed task.
- `poll(cursor)` returns retained events and `next_cursor`. Pass that cursor into
  the next poll. `updates` does this automatically and yields even empty pages.
  Stop iteration to stop polling; neither that nor a local deadline cancels the task.
- `wait` discards intermediate pages and returns the last terminal packet. Use
  `updates` to observe events, gaps and truncation throughout a task. `poll(0)` can
  reread the currently retained window after completion.
- `done` means the engine task ended. `outcome` can still be cancellation or failure.
  `ASNGN_OK` is not proof of repository correctness. `task_state: succeeded` requires
  the host's current acceptance contract and runtime evidence. `unconfirmed`,
  `incomplete`, `superseded` and `unavailable` remain distinct.
- Event kinds are 0 = output, 1 = reasoning, 2 = notice. A `truncated` event is an
  incomplete view; the terminal `answer` is separate. The server retains at most
  256 events per task and 32 task handles. Event text can be untrusted tool/model data.
- `release` frees a completed handle. For a running task it returns `done: false`
  and retains the handle. After cancellation, keep polling until `done`, then release.
  Releasing in a different observer invalidates that handle for everyone.
- `work`, `define_work` / `defineWork`, and `invalidate_work` / `invalidateWork`
  expose revision-checked host acceptance. A criterion's `depends_on` is a bit mask
  of earlier entries. See [acceptance contracts](../docs/acceptance.md).
- `approval()` returns the latest durable approval, including complete **redacted**
  arguments when present. It never grants permission. There is no approve method;
  trusted embedding hosts decide via the C API. See [approvals](../docs/approvals.md).
- `call_tool` / `callTool` exposes the remaining server tools using ordinary JSON.
  It does not elevate authority, retry effects or interpret tool output as instructions.

## Failures and resource bounds

`RpcError` retains JSON-RPC code/data. `ToolError` retains the engine error code and
payload. A failed *task* is instead an ordinary terminal packet with its outcome.
`ProtocolError` denotes malformed wire data; `TransportError` includes disconnection
and exhausted request budgets. `RequestTimeout` means only that the local wait
expired. A submit whose reply was lost might already have executed: **do not retry
it automatically**. The current server has no task listing/reconciliation endpoint.

Requests and incoming frames are limited to 8 MiB. At most 64 requests and 8 MiB of
serialized pending request data are admitted per client. Timed-out/aborted requests
retain a slot until their late replies arrive; they cannot grow an unbounded ID set.
IDs are never reused. JSON duplicate keys, invalid UTF-8, mismatched IDs and incomplete
response frames fail closed. The server also closes a request exceeding 8 MiB.
Runtime parsing overhead is additional to these payload limits.

Stderr is drained continuously; `client.stderr` exposes only its last 64 KiB.
Diagnostics are not copied automatically into exceptions and may contain sensitive
information. `env`, when supplied, is the complete child environment; otherwise it
inherits the caller's. No shell command is constructed.

Closing rejects pending calls, closes stdin and gives the server two seconds to
shut down, then sends termination, waits one second, and kills it if needed. POSIX
clients also terminate remaining members of the spawned process group. Descendants
that deliberately create new sessions can escape that group; on Windows the SDK
only terminates the child. These clients do not provide a sandbox. Forced shutdown
can leave uncertain external effects; it does not manufacture a task success.

Protocol basis: [MCP stdio transport](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports),
[Python subprocess lifecycle](https://docs.python.org/3/library/asyncio-subprocess.html),
[Node child processes](https://nodejs.org/api/child_process.html).

## Verification

CMake registers the Python and Node contract tests when those interpreters are
available, and real MCP/HTTP integrations when the remote adapter is built.
Missing optional runtimes are reported at configuration time. `npm ci --prefix sdk
--ignore-scripts` installs only the locked TypeScript check tool; it is not needed
by an SDK consumer. `ctest -R test_sdk --output-on-failure` runs the registered checks.

`python sdk/tests/package_smoke.py /absolute/asngn-mcp /absolute/node
/absolute/typescript/bin/tsc` checks both archives and installed imports. Its Python
interpreter needs pip and setuptools 77+ (CI pins 84.0.0). It builds in a temporary
copy, installs with no index or runtime dependencies, and uses the actual server.
The HTTP fixture is scripted: these checks establish contracts, not model quality.
