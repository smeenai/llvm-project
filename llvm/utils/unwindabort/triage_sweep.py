#!/usr/bin/env python3
"""Summarize stress_sweep.py results: reports bucketed by pass and callee."""

import collections
import json
import re
import sys

REPORT_RE = re.compile(
    r"unwindabort-check: pass '([^']*)' made '([^']*)' "
    r"able to unwind to its caller via:\s*(.*)$"
)
CALLEE_RE = re.compile(r"@([\w.$]+)\(")


def main():
    data = json.load(open(sys.argv[1]))
    print("status counts:", data["status_counts"])
    by_pass = collections.defaultdict(lambda: collections.defaultdict(list))
    crashes = [r for r in data["results"] if r["status"] == "crash"]
    for r in data["results"]:
        for rep in r["reports"]:
            m = REPORT_RE.match(rep)
            if not m:
                continue
            pass_name, _, inst = m.groups()
            kind = inst.split()[0] if inst.startswith("resume") else "call"
            callee = CALLEE_RE.search(inst)
            key = f"{kind} {callee.group(1) if callee else '<indirect>'}"
            by_pass[pass_name][key].append((r["file"], r["passes"]))
    for pass_name, keys in sorted(
        by_pass.items(), key=lambda kv: -sum(map(len, kv[1].values()))
    ):
        total = sum(map(len, keys.values()))
        print(f"\n== {pass_name}: {total} reports")
        for key, hits in sorted(keys.items(), key=lambda kv: -len(kv[1]))[:15]:
            f, p = hits[0]
            print(
                f"  {len(hits):5d}  {key:50s}  e.g. {f.split('llvm/test/')[-1]} [{p}]"
            )
    print(f"\n{len(crashes)} crashing runs")
    for r in crashes[:20]:
        print(f"  {r['file'].split('llvm/test/')[-1]} [{r['passes']}]")


if __name__ == "__main__":
    main()
