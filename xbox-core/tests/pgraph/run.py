#!/usr/bin/env python3
"""Runs nxdk_pgraph_tests (https://github.com/abaire/nxdk_pgraph_tests) in xbox-core, in parallel
jobs, and compares the results with the hardware captures
(https://github.com/abaire/nxdk_pgraph_tests_golden_results).

    run.py <nxdk_pgraph_tests_xiso.iso> <golden results dir> [--jobs N] [--suite NAME ...]
           [--out DIR] [--core PATH] [--seconds S]

Each job gets its own folder (its E: drive) with a configuration that runs its share of the suites
and then shuts down; the images end up in <out>/<suite>/<test>.png.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("iso")
    p.add_argument("golden")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    p.add_argument("--suite", action="append", default=[], help="suite name as the menu shows it")
    p.add_argument("--out", default=os.path.abspath("pgraph-results"))
    p.add_argument("--work", default=os.path.abspath("pgraph-work"))
    p.add_argument("--core", default=os.path.expanduser("~/xc-host/xbox-core"))
    p.add_argument("--seconds", type=int, default=3600)
    args = p.parse_args()

    suites = args.suite or sorted(d.replace("_", " ") for d in os.listdir(args.golden)
                                  if os.path.isdir(os.path.join(args.golden, d)))
    shares = [suites[i::args.jobs] for i in range(args.jobs)]
    processes = []
    for i, share in enumerate(shares):
        if not share:
            continue
        work = os.path.join(args.work, "job%d" % i)
        shutil.rmtree(work, ignore_errors=True)
        out = os.path.join(work, "xbox-data", "e", "nxdk_pgraph_tests")
        os.makedirs(out)
        config = {
            "settings": {
                "enable_progress_log": True,
                "disable_autorun": False,
                "enable_autorun_immediately": True,
                "enable_shutdown_on_completion": True,
                "output_directory_path": "e:/nxdk_pgraph_tests",
                "skip_tests_by_default": True,
                "delay_milliseconds_before_exit": 0,
                "delay_milliseconds_between_tests": 0,
            },
            "test_suites": {name: {"skipped": False} for name in share},
        }
        with open(os.path.join(out, "nxdk_pgraph_tests_config.json"), "w") as f:
            json.dump(config, f, indent=2)
        log = open(os.path.join(work, "run.log"), "w")
        processes.append((work, subprocess.Popen(
            [args.core, os.path.abspath(args.iso), "--seconds", str(args.seconds)],
            cwd=work, stdout=log, stderr=subprocess.STDOUT)))
        print("job %d: %d suites" % (i, len(share)), flush=True)
    for work, process in processes:
        process.wait()

    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(args.out)
    for work, _ in processes:
        results = os.path.join(work, "xbox-data", "e", "nxdk_pgraph_tests")
        for suite in os.listdir(results):
            source = os.path.join(results, suite)
            if os.path.isdir(source):
                shutil.copytree(source, os.path.join(args.out, suite), dirs_exist_ok=True)
    return subprocess.call([sys.executable, os.path.join(HERE, "compare.py"), args.out, args.golden])


if __name__ == "__main__":
    sys.exit(main())
