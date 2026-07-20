#!/usr/bin/env python3

import importlib.util
import json
import pathlib
import subprocess
import sys

sys.dont_write_bytecode = True


def fail(message):
    print(message, file=sys.stderr)
    return 1


def load_module(path):
    spec = importlib.util.spec_from_file_location("verify_linux_elf", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"unable to load verifier: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    if len(sys.argv) != 4:
        return fail("usage: linux_compatibility_test.py <verifier> <fixtures> <bennu>")

    verifier_path = pathlib.Path(sys.argv[1])
    fixtures = pathlib.Path(sys.argv[2])
    bennu = pathlib.Path(sys.argv[3])
    if not verifier_path.is_file():
        return fail(f"verifier is missing: {verifier_path}")

    verifier = load_module(verifier_path)

    compatible = json.loads((fixtures / "linux-abi-compatible.json").read_text())
    report = verifier.verify_inspection(compatible)
    if report["errors"]:
        return fail(f"compatible fixture was rejected: {report['errors']}")
    if report["highest_glibc"] != "GLIBC_2.34":
        return fail(f"wrong GLIBC result: {report['highest_glibc']}")
    if report["highest_glibcxx"] != "GLIBCXX_3.4.32":
        return fail(f"wrong GLIBCXX result: {report['highest_glibcxx']}")

    below_ceilings = dict(compatible)
    below_ceilings["version_info"] = (
        below_ceilings["version_info"]
        .replace("GLIBC_2.34", "GLIBC_2.33")
        .replace("GLIBCXX_3.4.32", "GLIBCXX_3.4.31")
    )
    report = verifier.verify_inspection(below_ceilings)
    if report["errors"]:
        return fail(f"below-ceiling fixture was rejected: {report['errors']}")

    incompatible = json.loads((fixtures / "linux-abi-incompatible.json").read_text())
    report = verifier.verify_inspection(incompatible)
    expected_error = (
        "GLIBC ceiling exceeded: required GLIBC_2.36, allowed GLIBC_2.34"
    )
    if report["errors"] != [expected_error]:
        return fail(f"controlled incompatible fixture did not fail clearly: {report['errors']}")

    future_standard_library = dict(compatible)
    future_standard_library["version_info"] = future_standard_library[
        "version_info"
    ].replace("GLIBCXX_3.4.32", "GLIBCXX_3.4.33")
    report = verifier.verify_inspection(future_standard_library)
    expected_error = (
        "GLIBCXX ceiling exceeded: required GLIBCXX_3.4.33, "
        "allowed GLIBCXX_3.4.32"
    )
    if report["errors"] != [expected_error]:
        return fail(f"future GLIBCXX fixture did not fail clearly: {report['errors']}")

    unexpected_library = dict(compatible)
    unexpected_library["ldd"] += (
        "\nlibz.so.1 => /lib/x86_64-linux-gnu/libz.so.1 (0x0006)"
    )
    report = verifier.verify_inspection(unexpected_library)
    if not any("dynamic library set mismatch" in error for error in report["errors"]):
        return fail(f"unexpected library fixture was not rejected: {report['errors']}")

    process = subprocess.run(
        [sys.executable, str(verifier_path), str(bennu)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        return fail(
            "real ELF verification failed:\n"
            f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}"
        )

    try:
        real_report = verifier.verify_inspection(verifier.inspect_elf(bennu))
    except (OSError, RuntimeError) as error:
        return fail(f"real ELF evidence could not be measured: {error}")
    if real_report["errors"]:
        return fail(f"real ELF was outside compatibility policy: {real_report['errors']}")
    for expected in (
        f"highest GLIBC: {real_report['highest_glibc']} (allowed <= {verifier.ALLOWED_GLIBC})",
        f"highest GLIBCXX: {real_report['highest_glibcxx']} (allowed <= {verifier.ALLOWED_GLIBCXX})",
        "dynamic libraries: libc.so.6, libgcc_s.so.1, libm.so.6, libstdc++.so.6",
    ):
        if expected not in process.stdout:
            return fail(f"real ELF report is missing '{expected}':\n{process.stdout}")

    print("Linux ABI ceiling fixtures and real ELF passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
