#!/usr/bin/env python3
"""The crash reports (platform/crash_report.h) of a program the launcher runs, here cane-extractor.

SAFEHOUSE_CRASH_TEST=segv|abort|terminate|stack crashes it at the top of main. Checked: the output printed before the
crash reaches the pipe (stdout line buffered, so a crash loses nothing), then the "crash: <program> crashed: ..." line
with what happened, the context, the stack, and the exit is the crash's (a signal on Linux / macOS, the exception code
on Windows). Without the switch nothing of it appears.

  test-crash-report.py PROGRAM
"""
import os
import subprocess
import sys
import tempfile

EXPECT = {
    "segv": ["segmentation fault", "access violation"],
    "abort": ["abort"],
    "terminate": ["std::terminate: uncaught exception: the crash test's exception"],
    "stack": ["segmentation fault", "stack overflow"],
}


def run(program, test, home):
    env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home, XDG_DATA_HOME=home, XDG_CACHE_HOME=home, XDG_STATE_HOME=home,
               SAFEHOUSE_CRASH_DIR=home)
    env.pop("SAFEHOUSE_CRASH_TEST", None)
    if test:
        env["SAFEHOUSE_CRASH_TEST"] = test
    # stdout and stderr on one pipe, as the launcher runs it (platform/process.h)
    result = subprocess.run([program, "--help"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    return result.returncode, result.stdout.decode("utf-8", "replace").splitlines()


def main():
    program = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory(prefix="crash-report-") as home:
        code, lines = run(program, None, home)
        if any(line.startswith("crash: ") for line in lines):
            failures.append("a crash line without a crash")
        for test, words in EXPECT.items():
            code, lines = run(program, test, home)
            text = "\n".join(lines)
            before = [i for i, line in enumerate(lines) if line == f"crash test: {test} (stdout before the crash)"]
            report = [i for i, line in enumerate(lines) if line.startswith("crash: ") and " crashed: " in line]
            if code == 0:
                failures.append(f"{test}: exit 0")
            if not before:
                failures.append(f"{test}: the output before the crash is lost:\n{text}")
            if not report:
                failures.append(f"{test}: no crash line:\n{text}")
                continue
            line = lines[report[0]]
            if before and before[0] > report[0]:
                failures.append(f"{test}: the output before the crash comes after the report")
            if "cane-extractor" not in line or not any(w in line for w in words):
                failures.append(f"{test}: the crash line says {line!r}")
            if not any(l.startswith("crash:   stack:") for l in lines) or not any(l.startswith("crash:   #0 ") for l in lines):
                failures.append(f"{test}: no stack:\n{text}")
            if not any("doing: the crash test" in l for l in lines) or not any("event: crash test " + test in l for l in lines):
                failures.append(f"{test}: no context:\n{text}")
            print(f"{test}: exit {code}: {line}")
    for f in failures:
        print("FAILED:", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
