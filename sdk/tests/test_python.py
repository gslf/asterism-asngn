"""Transport faults use a hostile peer; lifecycle integration uses asngn-mcp."""
import asyncio
import errno
import json
import os
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from asterism import Client, ProtocolError, RequestTimeout, RpcError, ToolError, TransportError
from asterism.contract import poll, task_record
from asterism.consumption import consumption

PEER = [sys.executable, "-B", str(Path(__file__).with_name("peer.py"))]


class TransportTests(unittest.IsolatedAsyncioTestCase):
    def test_consumption_retains_full_counters_and_checks_invariants(self):
        value = json.loads(Path(__file__).with_name("consumption.json").read_text())
        result = consumption(value)
        self.assertEqual(result["lifetime"]["charged_tokens"], 9007199254741115)
        self.assertEqual(result["lifetime"]["unsettled_tokens"], 9007199254740993)
        for invalid in (None, 1, True, "", "01", "-1", "1.0", "1e2", "1\n", "1\r", "1\u2028",
                        " 1", "1\0", "١", "9223372036854775808", "9" * 100):
            bad = {**value, "lifetime": {**value["lifetime"], "calls": invalid}}
            with self.subTest(invalid=invalid), self.assertRaises(ProtocolError):
                consumption(bad)
        for change in ({"charged_tokens": "0"}, {"unsettled_calls": "0"}, {"unknown_calls": "0"},
                       {"calls": "1"}, {"failed_calls": "4"}, {"extra": "0"}):
            with self.subTest(change=change), self.assertRaises(ProtocolError):
                consumption({**value, "lifetime": {**value["lifetime"], **change}})
        for change in ({"scope": "session"}, {"schema": True}, {"utc_day": 1.5},
                       {"utc_day": 106751991167301}, {"lifetime": []}, {"extra": False},
                       {"lifetime": value["today"], "today": value["lifetime"]}):
            with self.subTest(change=change), self.assertRaises(ProtocolError):
                consumption({**value, **change})

    def test_durable_outcomes_do_not_imply_replay_or_success(self):
        cases = json.loads(Path(__file__).with_name("task_records.json").read_text())
        record = cases["record"]
        self.assertEqual(task_record(record, record["task_id"]), record)
        for change in cases["invalid_changes"]:
            with self.subTest(change=change), self.assertRaises(ProtocolError):
                task_record({**record, **change}, record["task_id"])
        for state in ("interrupted", "turn_committed"):
            value = {**record, "state": state, "turn_committed": state == "turn_committed"}
            del value["outcome"]
            self.assertEqual(task_record(value, value["task_id"]), value)

    async def test_correlated_parallel_errors_and_unicode(self):
        async with await Client.start(PEER) as client:
            values = await asyncio.gather(*(client.call_tool("echo", {"i": i, "text": "α\n日本語"}) for i in range(32)))
            self.assertEqual([v["i"] for v in values], list(range(32)))
            self.assertEqual(values[0]["text"], "α\n日本語")
            with self.assertRaises(RpcError) as rpc:
                await client.call_tool("rpc_error", {})
            self.assertEqual(rpc.exception.data, {"field": "x"})
            with self.assertRaises(ToolError) as tool:
                await client.call_tool("denied", {})
            self.assertEqual(tool.exception.code, "ASNGN_ERR_DENIED")
            self.assertEqual(await client.call_tool("echo", {}), {})

    async def test_local_deadlines_cancellation_and_late_replies(self):
        async with await Client.start(PEER) as client:
            with self.assertRaises(RequestTimeout):
                await client.call_tool("late", {}, timeout=0.02)
            waiting = asyncio.create_task(client.call_tool("late", {}))
            await asyncio.sleep(0.02)
            waiting.cancel()
            with self.assertRaises(asyncio.CancelledError):
                await waiting
            self.assertEqual(await client.call_tool("echo", {"alive": True}), {"alive": True})
            await asyncio.sleep(0.3)
            self.assertEqual(await client.call_tool("echo", {}), {})
            self.assertEqual(len(client._rpc._pending), 0)

    async def test_bounded_queue_and_close_pending(self):
        client = await Client.start(PEER)
        calls = [asyncio.create_task(client.call_tool("hold", {})) for _ in range(64)]
        await asyncio.sleep(0.05)
        with self.assertRaisesRegex(TransportError, "budget"):
            await client.call_tool("echo", {})
        await asyncio.gather(client.close(), client.close())
        results = await asyncio.gather(*calls, return_exceptions=True)
        self.assertTrue(all(isinstance(e, TransportError) for e in results))
        self.assertIsNotNone(client._rpc.process.returncode)

    async def test_reject_oversized_outgoing_before_write(self):
        async with await Client.start(PEER) as client:
            with self.assertRaises(TransportError):
                await client.call_tool("echo", {"text": "x" * (8 * 1024 * 1024)})
            self.assertEqual(await client.call_tool("echo", {}), {})

    async def test_invalid_peers_close_and_reap(self):
        for mode in ("malformed", "duplicate", "badutf8", "oversize", "wrong_id", "unterminated", "exit"):
            with self.subTest(mode=mode):
                async with await Client.start(PEER) as client:
                    with self.assertRaises(TransportError):
                        await client.call_tool(mode, {})
                self.assertIsNotNone(client._rpc.process.returncode)

    async def test_start_failure(self):
        with self.assertRaises(OSError):
            await Client.start([str(Path(__file__).with_name("missing-server"))])

    async def test_stderr_is_drained_and_bounded(self):
        async with await Client.start(PEER) as client:
            await client.call_tool("stderr", {})
            await asyncio.sleep(0.05)
            self.assertTrue(client.stderr.endswith("α-end"))
            self.assertLessEqual(len(client._rpc._stderr), 65536)

    @unittest.skipUnless(os.name == "posix", "POSIX process group test")
    async def test_descendant_does_not_keep_pipes_open(self):
        async with await Client.start(PEER) as client:
            pid = (await client.call_tool("orphan", {}))["pid"]
        # Linux can briefly retain a reparented zombie; it cannot run or hold pipes.
        for _ in range(100):
            try:
                os.kill(pid, 0)
                if sys.platform == "linux" and Path(f"/proc/{pid}/stat").read_text().split()[2] in ("Z", "X"):
                    return
            except OSError as error:
                if error.errno in (errno.ENOENT, errno.ESRCH):
                    return
                raise
            await asyncio.sleep(0.01)
        self.fail("descendant remained alive after close")

    def test_cursor_gaps_and_terminal_truth(self):
        packet = {"task_id": "task", "done": True, "cursor_gap": True, "next_cursor": 301,
                  "events": [{"cursor": 300, "kind": 0, "text": "α", "truncated": True}],
                  "outcome": "ASNGN_OK", "answer": "partial", "task_state": "incomplete"}
        self.assertEqual(poll(packet, "task", 0)["task_state"], "incomplete")
        for change in ({"cursor_gap": False}, {"task_state": "complete"}, {"next_cursor": 302}, {"done": 1}):
            with self.assertRaises(ProtocolError):
                poll({**packet, **change}, "task", 0)


if __name__ == "__main__":
    unittest.main()
