#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys

ALLOWED_GLIBC = "GLIBC_2.34"
ALLOWED_GLIBCXX = "GLIBCXX_3.4.32"
EXPECTED_LIBRARIES = {
    "libc.so.6",
    "libgcc_s.so.1",
    "libm.so.6",
    "libstdc++.so.6",
}
EXPECTED_NEEDED = {
    "libc.so.6",
    "libgcc_s.so.1",
    "libstdc++.so.6",
}


def version_key(version):
    return tuple(int(part) for part in version.rsplit("_", 1)[1].split("."))


def highest_version(text, prefix):
    pattern = re.compile(rf"\b{re.escape(prefix)}_(\d+(?:\.\d+)+)\b")
    versions = {f"{prefix}_{match}" for match in pattern.findall(text)}
    if not versions:
        return ""
    return max(versions, key=version_key)


def parse_libraries(text):
    libraries = set()
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if "=>" not in line:
            continue
        name, location = (part.strip() for part in line.split("=>", 1))
        if location.startswith("not found"):
            continue
        if re.fullmatch(r"lib[^/\s]+\.so(?:\.\d+)*", name):
            libraries.add(name)
    return libraries


def parse_needed(text):
    return set(re.findall(r"\(NEEDED\).*?Shared library: \[([^\]]+)\]", text))


def format_set(values):
    return ", ".join(sorted(values))


def verify_inspection(inspection):
    errors = []
    file_text = inspection.get("file", "")
    ldd_text = inspection.get("ldd", "")
    version_text = inspection.get("version_info", "")
    dynamic_text = inspection.get("dynamic", "")

    for required in (
        "ELF 64-bit LSB pie executable",
        "x86-64",
        "dynamically linked",
        "interpreter /lib64/ld-linux-x86-64.so.2",
    ):
        if required not in file_text:
            errors.append(f"ELF identity mismatch: file output is missing '{required}'")

    if "not found" in ldd_text:
        missing = [
            line.strip() for line in ldd_text.splitlines() if "not found" in line
        ]
        errors.append(f"unresolved dynamic libraries: {'; '.join(missing)}")

    libraries = parse_libraries(ldd_text)
    if libraries != EXPECTED_LIBRARIES:
        errors.append(
            "dynamic library set mismatch: "
            f"expected [{format_set(EXPECTED_LIBRARIES)}], "
            f"observed [{format_set(libraries)}]"
        )

    highest_glibc = highest_version(version_text, "GLIBC")
    highest_glibcxx = highest_version(version_text, "GLIBCXX")
    if not highest_glibc:
        errors.append("GLIBC requirements are missing from readelf version info")
    elif version_key(highest_glibc) > version_key(ALLOWED_GLIBC):
        errors.append(
            f"GLIBC ceiling exceeded: required {highest_glibc}, "
            f"allowed {ALLOWED_GLIBC}"
        )
    if not highest_glibcxx:
        errors.append("GLIBCXX requirements are missing from readelf version info")
    elif version_key(highest_glibcxx) > version_key(ALLOWED_GLIBCXX):
        errors.append(
            f"GLIBCXX ceiling exceeded: required {highest_glibcxx}, "
            f"allowed {ALLOWED_GLIBCXX}"
        )

    needed = parse_needed(dynamic_text)
    if needed != EXPECTED_NEEDED:
        errors.append(
            "direct NEEDED set mismatch: "
            f"expected [{format_set(EXPECTED_NEEDED)}], "
            f"observed [{format_set(needed)}]"
        )

    return {
        "errors": errors,
        "highest_glibc": highest_glibc,
        "highest_glibcxx": highest_glibcxx,
        "libraries": libraries,
        "needed": needed,
    }


def run_command(arguments):
    process = subprocess.run(
        arguments,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        detail = process.stderr.strip() or process.stdout.strip()
        raise RuntimeError(
            f"{' '.join(arguments)} failed with status {process.returncode}: {detail}"
        )
    return process.stdout.strip()


def inspect_elf(path):
    text_path = str(path)
    return {
        "file": run_command(["file", text_path]),
        "ldd": run_command(["ldd", text_path]),
        "version_info": run_command(["readelf", "--version-info", text_path]),
        "dynamic": run_command(["readelf", "-d", text_path]),
    }


def main():
    if len(sys.argv) != 2:
        print("usage: verify-linux-elf.py <bennu-elf>", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    if not path.is_file():
        print(f"error: Linux ELF does not exist: {path}", file=sys.stderr)
        return 1

    try:
        inspection = inspect_elf(path)
    except (OSError, RuntimeError) as error:
        print(f"error: Linux ELF inspection failed: {error}", file=sys.stderr)
        return 1

    report = verify_inspection(inspection)
    print(f"file: {inspection['file']}")
    print(f"ldd:\n{inspection['ldd']}")
    print(f"readelf --version-info:\n{inspection['version_info']}")
    print(f"readelf -d:\n{inspection['dynamic']}")
    print(f"highest GLIBC: {report['highest_glibc']} (allowed <= {ALLOWED_GLIBC})")
    print(
        f"highest GLIBCXX: {report['highest_glibcxx']} "
        f"(allowed <= {ALLOWED_GLIBCXX})"
    )
    print(f"dynamic libraries: {format_set(report['libraries'])}")
    print(f"direct NEEDED: {format_set(report['needed'])}")

    if report["errors"]:
        for error in report["errors"]:
            print(f"error: Linux ELF compatibility: {error}", file=sys.stderr)
        return 1

    print("Linux ELF compatibility policy passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
