# Asterism Python SDK

Async host client for the local `asngn-mcp` server, Asterism wire contract 1.
Requires Python 3.11+. No third-party runtime dependencies.

```python
from asterism import Client

async def run():
    async with await Client.start(["asngn-mcp", "--root", "/absolute/store"]) as client:
        task = await client.session("example").submit("Hello")
        result = await task.wait(timeout=120)
        print(result["outcome"], result["task_state"], result["answer"])
        await task.release()
```

Durations are seconds. `done` and `ASNGN_OK` do not certify task success;
inspect `task_state`. A local timeout/coroutine cancellation does not cancel the
engine task; call `task.cancel()`, poll to completion, then release.
`task.updates()` yields cursor pages including event gaps/truncation. Handles
belong to the live server and do not survive its restart. Approval inspection is
read-only. Context exit closes the owned server; always close it.

See `sdk/README.md` in the source repository for the full lifecycle and limits.
