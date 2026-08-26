#!/usr/bin/env python3
"""Outcome-gated real-model coding evaluation on temporary repositories."""
from __future__ import annotations
import argparse, json, re, subprocess, tempfile, threading, time, uuid
from pathlib import Path

TASKS = [
  {"name":"c_range_sum", "files":{
    "CMakeLists.txt":"cmake_minimum_required(VERSION 3.16)\nproject(range_sum C)\nenable_testing()\nadd_executable(test_range range.c test_range.c)\nadd_test(NAME range COMMAND test_range)\n",
    "range.h":"int range_sum(const int *values, int count);\n",
    "range.c":"#include \"range.h\"\nint range_sum(const int *values, int count) {\n  int total = 0;\n  for (int i = 0; i <= count; ++i) total += values[i];\n  return total;\n}\n",
    "test_range.c":"#include \"range.h\"\n#include <assert.h>\nint main(void) {\n  int xs[] = {2, 4, 8, 1000};\n  assert(range_sum(xs, 3) == 14);\n  assert(range_sum(xs, 0) == 0);\n  return 0;\n}\n"},
   "baseline":[["cmake","-S",".","-B","build"],["cmake","--build","build"],["ctest","--test-dir","build","--output-on-failure"]],
   "fail_index":2,
   "verify":[["cmake","-S",".","-B","build"],["cmake","--build","build"],["ctest","--test-dir","build","--output-on-failure"]],
   "prompt":"Diagnostica il test C fallito in questo repository, correggi il bug con una patch minima, compila ed esegui tutti i test. Non limitarti a spiegare: usa gli strumenti e modifica i file."},
  {"name":"python_config_merge", "files":{
    "config_merge.py":"def merge_defaults(defaults, override):\n    result = defaults.copy()\n    for key, value in override.items():\n        if isinstance(value, dict) and isinstance(result.get(key), dict):\n            result[key] = value\n        else:\n            result[key] = value\n    return result\n",
    "test_config_merge.py":"import unittest\nfrom config_merge import merge_defaults\nclass MergeTests(unittest.TestCase):\n    def test_nested_override_preserves_defaults(self):\n        got = merge_defaults({'db': {'host': 'localhost', 'port': 5432}}, {'db': {'port': 6432}})\n        self.assertEqual(got, {'db': {'host': 'localhost', 'port': 6432}})\n    def test_inputs_are_not_mutated(self):\n        base = {'nested': {'keep': True}}\n        got = merge_defaults(base, {'nested': {'add': 1}})\n        got['nested']['add'] = 2\n        self.assertEqual(base, {'nested': {'keep': True}})\nif __name__ == '__main__': unittest.main()\n"},
   "baseline":[["python3","-m","unittest","-v"]], "fail_index":0,
   "verify":[["python3","-m","unittest","-v"]],
   "prompt":"Comprendi perché i test Python di merge configurazione falliscono, implementa una correzione generale senza mutare gli input, poi esegui l'intera suite. Devi modificare davvero il repository usando gli strumenti."}
]

def run(cmd, cwd, timeout=180):
  return subprocess.run(cmd,cwd=cwd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout,check=False)

def init_repo(root, task):
  for rel,body in task["files"].items():
    p=root/rel; p.parent.mkdir(parents=True,exist_ok=True); p.write_text(body,encoding="utf-8")
  for cmd in (["git","init","-q"],["git","config","user.email","quality@example.invalid"],["git","config","user.name","Quality Harness"],["git","add","."],["git","commit","-qm","broken baseline"]):
    cp=run(cmd,root)
    if cp.returncode: raise RuntimeError(f"setup failed: {cmd}\n{cp.stdout}")

def monitor_rss(proc, result):
  peak=0; status=Path(f"/proc/{proc.pid}/status")
  while proc.poll() is None:
    try:
      m=re.search(r"^VmRSS:\s+(\d+)\s+kB",status.read_text(errors="replace"),re.M)
      if m: peak=max(peak,int(m.group(1)))
    except OSError: pass
    time.sleep(.1)
  result["peak_rss_kb"]=peak

def telemetry(path, session):
  text=path.read_text(errors="replace") if path.exists() else ""
  lines=[x for x in text.splitlines() if f'session: "{session}"' in x]
  joined="\n".join(lines)
  return {"tool_calls":sum('kind: "tool_call"' in x for x in lines),
          "guard_trips":sum('kind: "guard"' in x for x in lines),
          "invalid_tool_calls":len(re.findall(r"invalid-args|malformed call|protocol failure",joined,re.I))}

def evaluate(binary, engine, artifacts, task, timeout_s):
  started=time.monotonic(); session=f"quality-{task['name']}-{uuid.uuid4().hex[:8]}"
  with tempfile.TemporaryDirectory(prefix="asngn-quality-") as td:
    repo=Path(td); init_repo(repo,task)
    baseline=[run(c,repo).returncode for c in task["baseline"]]
    baseline_broken=baseline[task["fail_index"]] != 0
    cmd=[str(binary),"--root",str(engine),"--workspace",str(repo),"--session",session,"--confirm","allow","--once",task["prompt"]]
    proc=subprocess.Popen(cmd,cwd=repo,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    mem={}; mon=threading.Thread(target=monitor_rss,args=(proc,mem),daemon=True); mon.start()
    timed_out=False
    try: output,_=proc.communicate(timeout=timeout_s)
    except subprocess.TimeoutExpired:
      timed_out=True; proc.kill(); output,_=proc.communicate()
    mon.join(timeout=1)
    checks=[]
    for check in task["verify"]:
      cp=run(check,repo); checks.append({"command":check,"exit_code":cp.returncode,"output":cp.stdout[-4000:]})
    patch=run(["git","diff","--binary","HEAD"],repo).stdout
    patch_path=artifacts/(task["name"]+".patch"); patch_path.write_text(patch,encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="asngn-patch-") as bd:
      base=Path(bd); init_repo(base,task)
      apply=run(["git","apply","--check",str(patch_path)],base)
    clean=run(["git","diff","--check"],repo).returncode == 0
    tm=telemetry(engine/"telemetry"/"telemetry.xcdn",session)
    tests_ok=all(x["exit_code"]==0 for x in checks)
    success=(baseline_broken and not timed_out and proc.returncode==0 and bool(patch.strip()) and apply.returncode==0 and clean and tests_ok and 0<tm["tool_calls"]<=12 and tm["guard_trips"]<=3 and tm["invalid_tool_calls"]==0)
    return {"name":task["name"],"task_success":success,"baseline_failed_as_expected":baseline_broken,
      "agent_exit_code":proc.returncode,"timed_out":timed_out,"tests_passed":tests_ok,
      "patch_nonempty":bool(patch.strip()),"patch_applicable":apply.returncode==0,"diff_clean":clean,
      "latency_ms":round((time.monotonic()-started)*1000),"peak_rss_kb":mem.get("peak_rss_kb",0),
      **tm,"checks":checks,"agent_output":output[-8000:]}

def main():
  p=argparse.ArgumentParser(); p.add_argument("--asngn",required=True,type=Path); p.add_argument("--engine-root",required=True,type=Path); p.add_argument("--report",required=True,type=Path); p.add_argument("--artifacts",type=Path); p.add_argument("--timeout",type=int,default=1200); a=p.parse_args()
  artifacts=(a.artifacts or a.report.parent/"quality-artifacts").resolve(); artifacts.mkdir(parents=True,exist_ok=True)
  results=[evaluate(a.asngn.resolve(),a.engine_root.resolve(),artifacts,t,a.timeout) for t in TASKS]
  report={"schema_version":1,"primary_metric":"all_tasks_successful","qpt_role":"diagnostic_only","passed":all(r["task_success"] for r in results),"task_success_rate":sum(r["task_success"] for r in results)/len(results),"tests_passed":sum(r["tests_passed"] for r in results),"patches_applicable":sum(r["patch_applicable"] for r in results),"tool_calls_valid":all(r["invalid_tool_calls"]==0 for r in results),"useless_attempts":sum(r["guard_trips"] for r in results),"regressions":sum(not r["tests_passed"] for r in results),"latency_ms":sum(r["latency_ms"] for r in results),"peak_rss_kb":max(r["peak_rss_kb"] for r in results),"tasks":results}
  a.report.parent.mkdir(parents=True,exist_ok=True); a.report.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
  print(json.dumps({k:v for k,v in report.items() if k!="tasks"},indent=2))
  for r in results: print(f"{r['name']}: {'PASS' if r['task_success'] else 'FAIL'}")
  return 0 if report["passed"] else 1
if __name__=="__main__": raise SystemExit(main())
