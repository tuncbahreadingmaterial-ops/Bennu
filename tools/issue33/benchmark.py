#!/usr/bin/env python3
"""Reproducible verification and measurement driver for Bennu Issue #33."""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


STRATEGIES = ("constants", "flat", "hybrid")
RAW_FIELDS = (
    "revision",
    "dirty",
    "build_type",
    "cmake_version",
    "host",
    "os",
    "cpu",
    "c_compiler",
    "c_compiler_id",
    "c_compiler_version",
    "c_flags",
    "cxx_compiler",
    "cxx_compiler_id",
    "cxx_compiler_version",
    "cxx_flags",
    "workload_id",
    "workload_size",
    "expected_outcome",
    "strategy",
    "sample_index",
    "equivalence_status",
    "failure_atomic",
    "emitted_source_bytes",
    "emission_time_ns",
    "c_compile_time_ns",
    "executable_bytes",
    "execution_time_ns",
    "emission_peak_kib",
    "compile_peak_kib",
    "execution_peak_kib",
)
METRIC_FIELDS = (
    "emitted_source_bytes",
    "emission_time_ns",
    "c_compile_time_ns",
    "executable_bytes",
    "execution_time_ns",
    "emission_peak_kib",
    "compile_peak_kib",
    "execution_peak_kib",
)


def fail(message: str) -> None:
    print(f"issue33 benchmark: {message}", file=sys.stderr)
    raise SystemExit(1)


def run(command: list[str], *, input_bytes: bytes | None = None) -> subprocess.CompletedProcess[bytes]:
    try:
        return subprocess.run(command, input=input_bytes, capture_output=True, check=False)
    except OSError as error:
        fail(f"cannot run {command[0]}: {error}")


def timed_run(command: list[str], peak_memory: bool) -> tuple[subprocess.CompletedProcess[bytes], int, str]:
    peak_file: Path | None = None
    invoked = command
    if peak_memory:
        descriptor, peak_name = tempfile.mkstemp(prefix="bennu-issue33-peak-")
        os.close(descriptor)
        peak_file = Path(peak_name)
        invoked = [
            "/usr/bin/time",
            "-q",
            "-f",
            "%M",
            "-o",
            str(peak_file),
            *command,
        ]
    started = time.perf_counter_ns()
    completed = run(invoked)
    elapsed = time.perf_counter_ns() - started
    peak = ""
    if peak_file is not None:
        try:
            peak = peak_file.read_text(encoding="ascii").strip()
        finally:
            peak_file.unlink(missing_ok=True)
        if peak and not peak.isdecimal():
            fail(f"unexpected /usr/bin/time peak-memory output: {peak!r}")
    return completed, elapsed, peak


def peak_memory_supported() -> bool:
    if platform.system() != "Linux" or not Path("/usr/bin/time").is_file():
        return False
    descriptor, peak_name = tempfile.mkstemp(prefix="bennu-issue33-probe-")
    os.close(descriptor)
    peak_file = Path(peak_name)
    try:
        completed = run(
            [
                "/usr/bin/time",
                "-q",
                "-f",
                "%M",
                "-o",
                str(peak_file),
                sys.executable,
                "-c",
                "pass",
            ]
        )
        measured = peak_file.read_text(encoding="ascii").strip()
        return completed.returncode == 0 and measured.isdecimal()
    finally:
        peak_file.unlink(missing_ok=True)


def parse_manifest(prototype: Path) -> list[dict[str, str]]:
    completed = run([str(prototype), "manifest"])
    if completed.returncode != 0:
        fail(f"prototype manifest failed: {completed.stderr.decode(errors='replace').strip()}")
    try:
        rows = list(csv.DictReader(completed.stdout.decode("utf-8").splitlines()))
    except (UnicodeDecodeError, csv.Error) as error:
        fail(f"invalid prototype manifest: {error}")
    required = {"workload_id", "workload_size", "expected_outcome"}
    if not rows or set(rows[0]) != required:
        fail("prototype manifest has an unexpected schema")
    identifiers = [row["workload_id"] for row in rows]
    if len(identifiers) != len(set(identifiers)):
        fail("prototype manifest contains duplicate workload identifiers")
    return rows


def oracle(prototype: Path, workload: str, expected: str) -> bytes:
    completed = run([str(prototype), "oracle", workload])
    if expected == "success":
        if completed.returncode != 0:
            fail(f"oracle rejected successful workload {workload}: {completed.stderr.decode(errors='replace').strip()}")
        return completed.stdout
    if completed.returncode == 0 or completed.stdout:
        fail(f"oracle failure for {workload} was not atomic")
    return b""


def compile_candidate(cc: str, c_flags: list[str], source: Path, executable: Path,
                      peak_memory: bool) -> tuple[int, str]:
    completed, elapsed, peak = timed_run(
        [cc, *c_flags, str(source), "-o", str(executable)], peak_memory
    )
    if completed.returncode != 0:
        fail(
            "C compilation failed:\n"
            + completed.stdout.decode(errors="replace")
            + completed.stderr.decode(errors="replace")
        )
    return elapsed, peak


def verify_pair(prototype: Path, cc: str, c_flags: list[str], workload: str,
                expected: str, strategy: str, directory: Path,
                peak_memory: bool, check_stdout_failure: bool) -> dict[str, str | int]:
    expected_bytes = oracle(prototype, workload, expected)
    emitted, emission_ns, emission_peak = timed_run(
        [str(prototype), "emit", strategy, workload], peak_memory
    )
    if expected != "success":
        if emitted.returncode == 0:
            fail(f"{strategy}/{workload} unexpectedly emitted C")
        if emitted.stdout:
            fail(f"{strategy}/{workload} emitted partial C while rejecting")
        return {
            "equivalence_status": "expected_rejection",
            "failure_atomic": "true",
            "emitted_source_bytes": 0,
            "emission_time_ns": emission_ns,
            "c_compile_time_ns": "",
            "executable_bytes": "",
            "execution_time_ns": "",
            "emission_peak_kib": emission_peak,
            "compile_peak_kib": "",
            "execution_peak_kib": "",
        }
    if emitted.returncode != 0:
        fail(f"{strategy}/{workload} emission failed: {emitted.stderr.decode(errors='replace').strip()}")
    source = directory / f"{strategy}-{workload}.c"
    executable = directory / f"{strategy}-{workload}"
    source.write_bytes(emitted.stdout)
    compile_ns, compile_peak = compile_candidate(
        cc, c_flags, source, executable, peak_memory
    )
    executed, execution_ns, execution_peak = timed_run([str(executable)], peak_memory)
    if executed.returncode != 0:
        fail(
            f"{strategy}/{workload} generated executable failed with "
            f"status {executed.returncode}: {executed.stderr.decode(errors='replace').strip()}"
        )
    if executed.stdout != expected_bytes:
        fail(f"{strategy}/{workload} output is not byte-equivalent to the rewrite evaluator")
    if check_stdout_failure and Path("/dev/full").exists():
        with Path("/dev/full").open("wb") as failing_stdout:
            failed_write = subprocess.run(
                [str(executable)],
                stdout=failing_stdout,
                stderr=subprocess.PIPE,
                check=False,
            )
        if failed_write.returncode == 0:
            fail(f"{strategy}/{workload} did not report a generated stdout failure")
    return {
        "equivalence_status": "byte_equivalent",
        "failure_atomic": "true",
        "emitted_source_bytes": len(emitted.stdout),
        "emission_time_ns": emission_ns,
        "c_compile_time_ns": compile_ns,
        "executable_bytes": executable.stat().st_size,
        "execution_time_ns": execution_ns,
        "emission_peak_kib": emission_peak,
        "compile_peak_kib": compile_peak,
        "execution_peak_kib": execution_peak,
    }


def complexity_rows(prototype_source: Path) -> list[dict[str, str | int]]:
    lines = prototype_source.read_text(encoding="utf-8").splitlines()
    open_region: tuple[str, str, int] | None = None
    rows: list[dict[str, str | int]] = []
    for number, line in enumerate(lines, start=1):
        marker = "ISSUE33_COMPLEXITY_BEGIN "
        if marker in line:
            if open_region is not None:
                fail("nested complexity regions in prototype source")
            payload = line.split(marker, 1)[1].strip()
            name, responsibilities = payload.split(" | ", 1)
            open_region = (name, responsibilities, number + 1)
        elif "ISSUE33_COMPLEXITY_END" in line:
            if open_region is None:
                fail("unmatched complexity region end in prototype source")
            name, responsibilities, begin = open_region
            physical = number - begin
            substantive = sum(
                1
                for candidate in lines[begin - 1 : number - 1]
                if candidate.strip() and not candidate.lstrip().startswith("//")
            )
            rows.append(
                {
                    "component": name,
                    "begin_line": begin,
                    "end_line": number - 1,
                    "physical_lines": physical,
                    "substantive_lines": substantive,
                    "duplicated_semantic_responsibilities": responsibilities,
                }
            )
            open_region = None
    if open_region is not None or {row["component"] for row in rows} != {
        "shared_validation",
        "constants",
        "flat_runtime",
        "hybrid",
    }:
        fail("prototype complexity regions are incomplete")
    return rows


def git_text(source_dir: Path, arguments: list[str]) -> str:
    completed = run(["git", "-C", str(source_dir), *arguments])
    if completed.returncode != 0:
        fail(f"git {' '.join(arguments)} failed")
    return completed.stdout.decode("utf-8").strip()


def cpu_identity() -> str:
    if platform.system() == "Linux":
        try:
            for line in Path("/proc/cpuinfo").read_text(
                encoding="utf-8", errors="replace"
            ).splitlines():
                key, separator, value = line.partition(":")
                if separator and key.strip() == "model name" and value.strip():
                    return value.strip()
        except OSError:
            pass
    return platform.processor() or platform.machine()


def base_metadata(args: argparse.Namespace, peak_memory: bool) -> dict[str, str]:
    dirty = bool(git_text(args.source_dir, ["status", "--porcelain", "--untracked-files=all"]))
    return {
        "revision": git_text(args.source_dir, ["rev-parse", "HEAD"]),
        "dirty": str(dirty).lower(),
        "build_type": args.build_type,
        "cmake_version": args.cmake_version,
        "host": platform.node(),
        "os": platform.platform(),
        "cpu": cpu_identity(),
        "c_compiler": str(Path(args.cc).resolve()),
        "c_compiler_id": args.c_compiler_id,
        "c_compiler_version": args.c_compiler_version,
        "c_flags": " ".join(args.c_flag),
        "cxx_compiler": args.cxx_compiler,
        "cxx_compiler_id": args.cxx_compiler_id,
        "cxx_compiler_version": args.cxx_compiler_version,
        "cxx_flags": args.cxx_flags,
        "peak_memory_method": "/usr/bin/time -q -f %M (maximum resident KiB)"
        if peak_memory
        else "unsupported on this host; CSV fields are empty",
        "aggregation": "median of each numeric metric over recorded samples; warmup excluded",
        "warmup": "one full emit/compile/execute verification per successful candidate/workload; one rejection check per failing pair",
        "hybrid_threshold": "constants when total materialized vector elements <= 1000; flat runtime loops above 1000",
    }


def write_csv(path: Path, fieldnames: tuple[str, ...] | list[str], rows: list[dict[str, object]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def command_verify(args: argparse.Namespace) -> None:
    manifest = parse_manifest(args.prototype)
    complexity = complexity_rows(args.prototype_source)
    with tempfile.TemporaryDirectory(prefix="bennu-issue33-verify-") as temporary:
        directory = Path(temporary)
        for workload in manifest:
            for strategy in STRATEGIES:
                verify_pair(
                    args.prototype,
                    args.cc,
                    args.c_flag,
                    workload["workload_id"],
                    workload["expected_outcome"],
                    strategy,
                    directory,
                    False,
                    workload["workload_id"] == "scalar_bool",
                )
    counts = ", ".join(
        f"{row['component']}={row['substantive_lines']} substantive lines"
        for row in complexity
    )
    print(
        f"Issue #33 smoke verification passed: {len(manifest)} workloads x "
        f"{len(STRATEGIES)} strategies; {counts}"
    )


def command_measure(args: argparse.Namespace) -> None:
    if args.samples < 2:
        fail("measurement requires at least two recorded samples")
    if args.build_type != "Release":
        fail(f"measurement requires a Release build, got {args.build_type!r}")
    manifest = parse_manifest(args.prototype)
    peak_memory = peak_memory_supported()
    metadata = base_metadata(args, peak_memory)
    output_dir = args.output.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="bennu-issue33-measure-") as temporary:
        directory = Path(temporary)
        for workload in manifest:
            for strategy in STRATEGIES:
                verify_pair(
                    args.prototype,
                    args.cc,
                    args.c_flag,
                    workload["workload_id"],
                    workload["expected_outcome"],
                    strategy,
                    directory,
                    peak_memory,
                    False,
                )
        raw_rows: list[dict[str, object]] = []
        common = {field: metadata[field] for field in RAW_FIELDS if field in metadata}
        for workload in manifest:
            for strategy in STRATEGIES:
                for sample in range(1, args.samples + 1):
                    measured = verify_pair(
                        args.prototype,
                        args.cc,
                        args.c_flag,
                        workload["workload_id"],
                        workload["expected_outcome"],
                        strategy,
                        directory,
                        peak_memory,
                        False,
                    )
                    raw_rows.append(
                        {
                            **common,
                            "workload_id": workload["workload_id"],
                            "workload_size": workload["workload_size"],
                            "expected_outcome": workload["expected_outcome"],
                            "strategy": strategy,
                            "sample_index": sample,
                            **measured,
                        }
                    )
    summary_rows: list[dict[str, object]] = []
    for workload in manifest:
        for strategy in STRATEGIES:
            selected = [
                row
                for row in raw_rows
                if row["workload_id"] == workload["workload_id"]
                and row["strategy"] == strategy
            ]
            summary: dict[str, object] = {
                "workload_id": workload["workload_id"],
                "workload_size": workload["workload_size"],
                "expected_outcome": workload["expected_outcome"],
                "strategy": strategy,
                "samples": args.samples,
                "equivalence_status": selected[0]["equivalence_status"],
            }
            for metric in METRIC_FIELDS:
                values = [int(row[metric]) for row in selected if str(row[metric])]
                summary[f"median_{metric}"] = statistics.median(values) if values else ""
            summary_rows.append(summary)
    complexity = complexity_rows(args.prototype_source)
    write_csv(output_dir / "raw.csv", list(RAW_FIELDS), raw_rows)
    write_csv(output_dir / "summary.csv", list(summary_rows[0]), summary_rows)
    write_csv(output_dir / "complexity.csv", list(complexity[0]), complexity)
    (output_dir / "metadata.json").write_text(
        json.dumps({**metadata, "samples": args.samples}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"Issue #33 raw evidence written to {output_dir}")


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    subparsers = result.add_subparsers(dest="command", required=True)
    for name in ("verify", "measure"):
        sub = subparsers.add_parser(name)
        sub.add_argument("--prototype", type=Path, required=True)
        sub.add_argument("--prototype-source", type=Path, required=True)
        sub.add_argument("--cc", required=True)
        sub.add_argument("--c-flag", action="append", default=[])
        if name == "measure":
            sub.add_argument("--source-dir", type=Path, required=True)
            sub.add_argument("--output", type=Path, required=True)
            sub.add_argument("--samples", type=int, default=5)
            sub.add_argument("--build-type", required=True)
            sub.add_argument("--cmake-version", required=True)
            sub.add_argument("--c-compiler-id", required=True)
            sub.add_argument("--c-compiler-version", required=True)
            sub.add_argument("--cxx-compiler", required=True)
            sub.add_argument("--cxx-compiler-id", required=True)
            sub.add_argument("--cxx-compiler-version", required=True)
            sub.add_argument("--cxx-flags", required=True)
    return result


def main() -> None:
    args = parser().parse_args()
    args.prototype = args.prototype.resolve()
    args.prototype_source = args.prototype_source.resolve()
    if args.command == "verify":
        command_verify(args)
    else:
        command_measure(args)


if __name__ == "__main__":
    main()
