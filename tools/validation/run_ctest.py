#!/usr/bin/env python3
import argparse
import pathlib
import shlex
import subprocess
import sys
import time


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run a labeled CTest selection with failover checkpoints and a "
            "live durable log."
        )
    )
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--log", required=True)
    return parser.parse_args()


def display_command(command: list[str]) -> str:
    if sys.platform == "win32":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def run() -> int:
    arguments = parse_arguments()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    log_path = pathlib.Path(arguments.log).resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)

    command = [
        arguments.ctest,
        "--test-dir",
        str(pathlib.Path(arguments.build_dir).resolve()),
        "-F",
        "-L",
        arguments.label,
        "--output-on-failure",
    ]
    started = time.monotonic()
    print(f"CTest command: {display_command(command)}", flush=True)
    print(f"CTest log: {log_path}", flush=True)

    with log_path.open("a", encoding="utf-8", newline="") as log:
        log.write("\n--- CTest invocation ---\n")
        log.write(f"command={display_command(command)}\n")
        log.flush()
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        assert process.stdout is not None
        try:
            for line in process.stdout:
                sys.stdout.write(line)
                sys.stdout.flush()
                log.write(line)
                log.flush()
        except KeyboardInterrupt:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            print(
                "CTest interrupted; rerun the same command to resume from "
                "CTestCheckpoint.txt.",
                file=sys.stderr,
                flush=True,
            )
            return 130

        exit_code = process.wait()
        elapsed = time.monotonic() - started
        summary = f"exit_code={exit_code} elapsed_seconds={elapsed:.3f}\n"
        log.write(summary)
        print(summary.rstrip(), flush=True)
        return exit_code


if __name__ == "__main__":
    raise SystemExit(run())
