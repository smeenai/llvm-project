#!/usr/bin/env python3
"""Run opt with -unwindabort-stress -verify-unwindabort-preserved over lit tests.

For each .ll file, runs every `-passes=` pipeline found in its RUN lines plus
some default pipelines, and collects `unwindabort-check:` reports. Opt failures
are recorded but otherwise ignored: many tests are expected to fail, or need
flags this script doesn't reproduce.
"""

import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import tempfile

PASSES_RE = re.compile(r"--?passes(?:=|\s+)(?:'([^']*)'|\"([^\"]*)\"|(\S+))")
# Flags that change how the IR is interpreted, worth carrying over from RUN lines.
KEEP_FLAG_RE = re.compile(r"(--?(?:mtriple|mattr|mcpu|data-layout)=\S+)")
DEFAULT_PIPELINES = ["default<O2>", "lto<O2>"]


def run_lines(path):
    lines, cur = [], ""
    with open(path, errors="replace") as f:
        for line in f:
            m = re.match(r"\s*;\s*RUN:\s?(.*)$", line)
            if not m:
                continue
            text = m.group(1).rstrip()
            if text.endswith("\\"):
                cur += text[:-1] + " "
                continue
            lines.append(cur + text)
            cur = ""
    return lines


def jobs_for(path):
    jobs = set()
    for line in run_lines(path):
        for seg in re.split(r"\||&&", line):
            if not re.search(r"(^|\s)opt(\s|$)", seg):
                continue
            m = PASSES_RE.search(seg)
            if not m:
                continue
            passes = next(g for g in m.groups() if g is not None)
            if "%" in passes:
                continue
            flags = tuple(KEEP_FLAG_RE.findall(seg))
            jobs.add((passes, flags))
    for p in DEFAULT_PIPELINES:
        jobs.add((p, ()))
    return [(path, passes, flags) for passes, flags in sorted(jobs)]


def run(opt, job, timeout, cwd):
    path, passes, flags = job
    cmd = [
        opt,
        "-disable-output",
        "-unwindabort-stress",
        "-verify-unwindabort-preserved",
        f"-passes={passes}",
        *flags,
        path,
    ]
    try:
        p = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            errors="replace",
            cwd=cwd,
        )
    except subprocess.TimeoutExpired:
        return job, "timeout", []
    reports = [
        line for line in p.stderr.splitlines() if line.startswith("unwindabort-check:")
    ]
    if p.returncode < 0 or "PLEASE submit a bug report" in p.stderr:
        status = "crash"
    elif p.returncode != 0:
        status = "error"
    else:
        status = "ok"
    return job, status, reports


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--opt", required=True)
    ap.add_argument("--out", required=True, help="JSON results file")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("dirs", nargs="+")
    args = ap.parse_args()

    files = sorted(
        os.path.abspath(os.path.join(root, f))
        for d in args.dirs
        for root, _, fs in os.walk(d)
        for f in fs
        if f.endswith(".ll")
    )
    jobs = [j for f in files for j in jobs_for(f)]
    print(f"{len(files)} files, {len(jobs)} opt runs", file=sys.stderr)

    results = []
    status_counts = collections.Counter()
    # Some RUN-line pipelines write files (e.g. .dot graphs) to the cwd.
    scratch = tempfile.mkdtemp(prefix="ua-sweep-")
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(run, args.opt, j, args.timeout, scratch) for j in jobs]
        for i, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            (path, passes, flags), status, reports = fut.result()
            status_counts[status] += 1
            if reports or status in ("crash", "timeout"):
                results.append(
                    {
                        "file": path,
                        "passes": passes,
                        "flags": list(flags),
                        "status": status,
                        "reports": reports,
                    }
                )
            if i % 2000 == 0:
                print(f"{i}/{len(jobs)} {dict(status_counts)}", file=sys.stderr)

    with open(args.out, "w") as f:
        json.dump({"status_counts": status_counts, "results": results}, f, indent=1)
    print(
        f"done: {dict(status_counts)}; "
        f"{sum(1 for r in results if r['reports'])} runs with reports",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
