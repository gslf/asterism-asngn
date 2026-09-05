#!/usr/bin/env python3
"""Outcome-gated real-model coding evaluation on temporary repositories."""
from __future__ import annotations
import argparse, hashlib, json, re, shutil, subprocess, tempfile, threading, time, uuid
from pathlib import Path
from oracle import hidden_checks, isolated
from metrics import interval, monitor_rss, percentile, terminate

TASKS = [
  {"name":"c_range_sum", "files":{
    "CMakeLists.txt":"cmake_minimum_required(VERSION 3.16)\nproject(range_sum C)\nenable_testing()\nadd_executable(test_range range.c test_range.c)\nadd_test(NAME range COMMAND test_range)\n",
    "range.h":"int range_sum(const int *values, int count);\n",
    "range.c":"#include \"range.h\"\nint range_sum(const int *values, int count) {\n  int total = 0;\n  for (int i = 0; i <= count; ++i) total += values[i];\n  return total;\n}\n",
    "test_range.c":"#include \"range.h\"\n#include <assert.h>\nint main(void) {\n  int xs[] = {2, 4, 8, 1000};\n  assert(range_sum(xs, 3) == 14);\n  assert(range_sum(xs, 0) == 0);\n  return 0;\n}\n"},
   "baseline":[["cmake","-S",".","-B","build"],["cmake","--build","build"],["ctest","--test-dir","build","--output-on-failure"]],
   "fail_index":2,
   "verify":[["cmake","-S",".","-B","build"],["cmake","--build","build"],["ctest","--test-dir","build","--output-on-failure"]],
   "prompt":"Diagnose the failing C test in this repository, fix the bug with a minimal patch, then build and run all the tests. Do not just explain: use the tools and modify the files."},
  {"name":"python_config_merge", "files":{
    "config_merge.py":"def merge_defaults(defaults, override):\n    result = defaults.copy()\n    for key, value in override.items():\n        if isinstance(value, dict) and isinstance(result.get(key), dict):\n            result[key] = value\n        else:\n            result[key] = value\n    return result\n",
    "test_config_merge.py":"import unittest\nfrom config_merge import merge_defaults\nclass MergeTests(unittest.TestCase):\n    def test_nested_override_preserves_defaults(self):\n        got = merge_defaults({'db': {'host': 'localhost', 'port': 5432}}, {'db': {'port': 6432}})\n        self.assertEqual(got, {'db': {'host': 'localhost', 'port': 6432}})\n    def test_inputs_are_not_mutated(self):\n        base = {'nested': {'keep': True}}\n        got = merge_defaults(base, {'nested': {'add': 1}})\n        got['nested']['add'] = 2\n        self.assertEqual(base, {'nested': {'keep': True}})\nif __name__ == '__main__': unittest.main()\n"},
   "baseline":[["python3","-m","unittest","-v"]], "fail_index":0,
   "verify":[["python3","-m","unittest","-v"]],
   "prompt":"Understand why the Python config-merge tests fail, implement a general fix without mutating the inputs, then run the whole suite. You must actually modify the repository using the tools."}
]

def run(cmd, cwd, timeout=180):
  return subprocess.run(cmd,cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout,check=False)

def init_repo(root, task):
  for rel,body in task["files"].items():
    p=root/rel; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(body,encoding="utf-8")
  for cmd in (["git","init","-q"],["git","config","user.email","quality@example.invalid"],["git","config","user.name","Quality Harness"],["git","add","."],["git","commit","-qm","broken baseline"]):
    cp=run(cmd,root)
    if cp.returncode: raise RuntimeError(f"setup failed: {cmd}\n{cp.stdout}")

def verify_patch(task, patch_path, *, sandbox=True):
  """Apply only implementation changes to a fresh, evaluator-owned fixture.

  Build files and original tests are acceptance inputs, never patch inputs.
  Added tests are retained in the patch artifact but are not oracle inputs.
  Production runs require the isolated evaluator; test-only callers can opt out.
  """
  implementation = {"c_range_sum": {"range.c", "range.h"},
                    "python_config_merge": {"config_merge.py"}}[task["name"]]
  with tempfile.TemporaryDirectory(prefix="asngn-oracle-") as td:
    root = Path(td)
    init_repo(root, task)
    applied = run(["git", "apply", "--index", str(patch_path.resolve())], root)
    if applied.returncode:
      return False, False, []
    changes = run(["git", "diff", "--name-only", "-z", "HEAD"], root)
    paths = set(filter(None, changes.stdout.split("\0")))
    additions = paths - implementation
    for name in additions:
      path = Path(name)
      if (name in task['files'] or path.suffix not in {'.py', '.c', '.h'} or
          not (name.startswith('tests/') or path.name.startswith('test_')) or
          (root / name).is_symlink() or not (root / name).is_file()):
        return False, True, []
    for name in additions:
      (root / name).unlink()
    # Reject symlink substitution before the verifier reads source files.
    if any((root / name).is_symlink() or not (root / name).is_file()
           for name in implementation):
      return False, True, []
    checks = []
    commands = task["verify"] + hidden_checks(task, root)
    (root / "build").mkdir(exist_ok=True)
    for command in commands:
      cp = isolated(command, root) if sandbox else run(command, root)
      checks.append({"command": command, "exit_code": cp.returncode,
                     "output": cp.stdout})
    return True, True, checks

def telemetry(path, session):
  text=path.read_text(errors="replace") if path.exists() else ""
  lines=[x for x in text.splitlines() if f'session: "{session}"' in x]
  joined="\n".join(lines)
  return {"tool_calls":sum('kind: "tool_call"' in x for x in lines),
          "guard_trips":sum('kind: "guard"' in x for x in lines),
          "invalid_tool_calls":len(re.findall(r"invalid-args|malformed call|protocol failure",joined,re.I))}

def evaluate(binary, source_engine, artifacts, task, timeout_s):
  started=time.monotonic(); session=f"quality-{task['name']}-{uuid.uuid4().hex[:8]}"
  with tempfile.TemporaryDirectory(prefix="asngn-quality-") as td:
    repo = Path(td) / 'workspace'
    engine = Path(td) / 'engine'
    repo.mkdir(); engine.mkdir()
    # Copy inputs only. Never import sessions, memories, cache or calibration.
    shutil.copyfile(source_engine / 'config.xcdn', engine / 'config.xcdn')
    if (source_engine / 'models').is_dir():
      (engine / 'models').symlink_to(source_engine / 'models', target_is_directory=True)
    init_repo(repo,task)
    baseline=[run(c,repo).returncode for c in task["baseline"]]
    baseline_broken=baseline[task["fail_index"]] != 0
    cmd=[str(binary),"--root",str(engine),"--workspace",str(repo),"--session",session,"--confirm","allow","--once",task["prompt"]]
    proc=subprocess.Popen(cmd,cwd=repo,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
    mem={}; mon=threading.Thread(target=monitor_rss,args=(proc,mem),daemon=True); mon.start()
    timed_out=False
    try: output,_=proc.communicate(timeout=timeout_s)
    except subprocess.TimeoutExpired:
      timed_out=True; terminate(proc); output,_=proc.communicate()
    mon.join(timeout=1)
    # Capture additions too, including legitimate regression tests, for audit.
    added = run(['git', 'ls-files', '--others', '--exclude-standard', '-z'], repo)
    for name in filter(None, added.stdout.split('\0')):
      if name.startswith('tests/') or Path(name).name.startswith('test_'):
        run(['git', 'add', '--intent-to-add', '--', name], repo)
    patch=run(["git","diff","--binary","HEAD"],repo).stdout
    patch_path=artifacts/(task["name"]+".patch"); patch_path.write_text(patch,encoding="utf-8")
    audit_ok, apply_ok, checks = verify_patch(task, patch_path)
    clean=run(["git","diff","--check"],repo).returncode == 0
    tm=telemetry(engine/"telemetry"/"telemetry.xcdn",session)
    tests_ok=bool(checks) and all(x["exit_code"]==0 for x in checks)
    success=(baseline_broken and not timed_out and proc.returncode==0 and bool(patch.strip()) and apply_ok and audit_ok and clean and tests_ok)
    return {"name":task["name"],"task_success":success,"baseline_failed_as_expected":baseline_broken,
      "agent_exit_code":proc.returncode,"timed_out":timed_out,"tests_passed":tests_ok,
      "patch_nonempty":bool(patch.strip()),"patch_applicable":apply_ok,"diff_clean":clean,"oracle_audit_passed":audit_ok,
      "latency_ms":round((time.monotonic()-started)*1000),"peak_tree_rss_kb":mem.get("peak_tree_rss_kb"),
      **tm,"checks":checks,"agent_output":output[-8000:]}

def main():
  p = argparse.ArgumentParser()
  p.add_argument('--asngn', required=True, type=Path)
  p.add_argument('--engine-root', required=True, type=Path)
  p.add_argument('--report', required=True, type=Path)
  p.add_argument('--artifacts', type=Path)
  p.add_argument('--timeout', type=int, default=1200)
  p.add_argument('--repeats', type=int, default=3)
  p.add_argument('--split', choices=['dev', 'holdout'], default='holdout')
  p.add_argument('--profile', required=True, help='Model/quantization/backend/hardware identifier')
  a = p.parse_args()
  if a.repeats < 1 or a.timeout < 1:
    p.error('repeats and timeout must be positive')
  config = a.engine_root.resolve() / 'config.xcdn'
  if not config.is_file():
    p.error('engine-root must contain config.xcdn; configure external inputs with absolute paths')
  artifacts = (a.artifacts or a.report.parent / 'quality-artifacts').resolve()
  artifacts.mkdir(parents=True, exist_ok=True)
  results = []
  for repeat in range(a.repeats):
    target = artifacts / str(repeat)
    target.mkdir(exist_ok=True)
    for task in TASKS:
      result = evaluate(a.asngn.resolve(), a.engine_root.resolve(), target, task, a.timeout)
      result['repeat'] = repeat
      results.append(result)
  solved = sum(r['task_success'] for r in results)
  latencies = [r['latency_ms'] for r in results]
  rss = [r['peak_tree_rss_kb'] for r in results if r['peak_tree_rss_kb'] is not None]
  report = {
    'schema_version': 2, 'suite': 'asterism-coding-smoke-v2',
    'split': a.split, 'profile': a.profile, 'repeats': a.repeats,
    'config_sha256': hashlib.sha256(config.read_bytes()).hexdigest(),
    'binary_sha256': hashlib.sha256(a.asngn.read_bytes()).hexdigest(),
    'tasks_sha256': hashlib.sha256(json.dumps(TASKS, sort_keys=True).encode()).hexdigest(),
    'timeout_s': a.timeout, 'state_isolation': 'new-engine-per-trial',
    'primary_metric': 'protected_task_success', 'passed': solved == len(results),
    'task_success_rate': solved / len(results),
    'task_success_wilson95': interval(solved, len(results)),
    'interval_caveat': 'Trials share two tasks; not a population estimate for repository coding.',
    'false_successes': None,  # Requires a typed task success claim, not prose guessing.
    'latency_p50_ms': percentile(latencies, .5),
    'latency_p95_ms': percentile(latencies, .95),
    'peak_tree_rss_kb': max(rss) if rss else None,
    'memory_measurement': 'Linux sampled tree RSS; shared pages repeat; VRAM unavailable',
    'tasks': results,
  }
  a.report.parent.mkdir(parents=True, exist_ok=True)
  a.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
  # Reports are immutable calibration inputs. Promotion is a separate offline step.
  print(json.dumps({k: v for k, v in report.items() if k != 'tasks'}, indent=2))
  return 0 if report['passed'] else 1


if __name__ == '__main__':
  raise SystemExit(main())
