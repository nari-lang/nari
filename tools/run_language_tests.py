#!/usr/bin/env python3
"""Run Nari tests"""

import argparse
import math
import os
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--interpreter", required=True)
    parser.add_argument("--timeout", type=float, default=os.environ.get("TEST_TIMEOUT", "30"))
    parser.add_argument("--skip-network", action="store_true", default=os.environ.get("SKIP_NETWORK_TESTS") == "1")
    args = parser.parse_args()
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a positive finite number")

    root = Path(__file__).resolve().parent.parent
    interpreter = Path(args.interpreter).resolve()
    if not interpreter.is_file():
        parser.error(f"interpreter not found: {interpreter}")

    network_tests = {"test_spawn_methods.nari", "test_stdlib_net.nari", "test_io_multiplexing.nari"}
    cases = []
    for expected_success, directory in ((True, "expect_pass"), (False, "expect_fail")):
        for path in sorted((root / "tests" / directory).rglob("*.nari")):
            # jank todo
            if expected_success and "npkg" in path.name:
                continue
            if args.skip_network and path.name in network_tests:
                continue
            cases.append((expected_success, path))

    failed = []
    for expected_success, path in cases:
        relative = path.relative_to(root)
        try:
            result = subprocess.run(
                [str(interpreter), str(relative)], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                encoding="utf-8", errors="replace", timeout=args.timeout,
            )
        except subprocess.TimeoutExpired:
            print(f"[timeout] {relative} ({args.timeout:g}s)", flush=True)
            failed.append(relative)
            continue
        except OSError as error:
            print(f"[fail] {relative}: {error}", flush=True)
            failed.append(relative)
            continue

        valid_failure = 0 < result.returncode < 128 and "execution continued after" not in result.stdout
        passed = result.returncode == 0 if expected_success else valid_failure
        print(f"[{'pass' if passed else 'fail'}] {relative}", flush=True)
        if not passed:
            print(f"exit status: {result.returncode}\n{result.stdout}", flush=True)
            failed.append(relative)

    print(f"Passed tests: {len(cases) - len(failed)} / {len(cases)}", flush=True)
    if failed:
        print("Failed tests:\n" + "\n".join(f" - {path}" for path in failed), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
