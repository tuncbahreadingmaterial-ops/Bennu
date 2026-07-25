#!/usr/bin/env python3
import json
import pathlib
import shutil
import subprocess
import sys


def fail(message: str) -> None:
    raise SystemExit(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def run_checked(command: list[str], working_directory: pathlib.Path) -> str:
    completed = subprocess.run(
        command,
        cwd=working_directory,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if completed.returncode != 0:
        fail(
            f"command failed ({completed.returncode}): {command}\n"
            f"{completed.stdout}"
        )
    return completed.stdout


def preset_by_name(
    presets: list[dict[str, object]], name: str
) -> dict[str, object]:
    for preset in presets:
        if preset.get("name") == name:
            return preset
    fail(f"CMakePresets.json is missing preset: {name}")


def test_labels(test: dict[str, object]) -> set[str]:
    labels: set[str] = set()
    for prop in test.get("properties", []):
        if prop.get("name") == "LABELS":
            labels.update(prop.get("value", []))
    return labels


def validate_powershell(path: pathlib.Path) -> None:
    powershell = shutil.which("pwsh") or shutil.which("powershell")
    if powershell is None:
        return
    quoted_path = str(path).replace("'", "''")
    command = (
        "$tokens=$null; $errors=$null; "
        "[System.Management.Automation.Language.Parser]::ParseFile("
        f"'{quoted_path}', [ref]$tokens, [ref]$errors) | Out-Null; "
        "if ($errors.Count -ne 0) { "
        "$errors | ForEach-Object { Write-Error $_ }; exit 1 }"
    )
    run_checked(
        [powershell, "-NoProfile", "-NonInteractive", "-Command", command],
        path.parent,
    )


def main() -> int:
    if len(sys.argv) != 4:
        fail(
            "usage: validation_workflow_contract.py "
            "<source-dir> <binary-dir> <cmake-command>"
        )

    source_directory = pathlib.Path(sys.argv[1]).resolve()
    binary_directory = pathlib.Path(sys.argv[2]).resolve()
    cmake_command = pathlib.Path(sys.argv[3]).resolve()
    ctest_name = "ctest.exe" if sys.platform == "win32" else "ctest"
    ctest_command = cmake_command.with_name(ctest_name)

    required_paths = [
        source_directory / "CMakePresets.json",
        source_directory
        / "tools"
        / "validation"
        / "Invoke-BennuWindowsValidation.ps1",
        source_directory / "tools" / "validation" / "bootstrap-wsl-tools.sh",
        source_directory / "tools" / "validation" / "run-wsl-sanitize.sh",
        source_directory / "tools" / "validation" / "run_ctest.py",
        source_directory / "tools" / "validation" / "run-wsl-strict.sh",
    ]
    for path in required_paths:
        require(path.is_file(), f"required validation entry point is missing: {path}")

    presets = json.loads(required_paths[0].read_text(encoding="utf-8"))
    require(presets.get("version") == 2, "CMake preset schema must remain version 2")
    configure_presets = presets.get("configurePresets", [])
    build_presets = presets.get("buildPresets", [])
    test_presets = presets.get("testPresets", [])
    required_configure = {
        "windows-release",
        "windows-strict",
        "gnu-strict",
        "linux-sanitize",
    }
    require(
        required_configure.issubset(
            {preset.get("name") for preset in configure_presets}
        ),
        "CMake configure presets are incomplete",
    )
    require(
        required_configure.issubset(
            {preset.get("name") for preset in build_presets}
        ),
        "CMake build presets are incomplete",
    )
    require(
        {"windows-release-full", "windows-release-qa", "windows-strict",
         "gnu-strict", "linux-sanitize"}.issubset(
            {preset.get("name") for preset in test_presets}
        ),
        "CTest presets are incomplete",
    )

    windows_strict = preset_by_name(configure_presets, "windows-strict")
    windows_strict_cache = windows_strict.get("cacheVariables", {})
    windows_cxx_flags = str(windows_strict_cache.get("CMAKE_CXX_FLAGS", ""))
    for required_flag in (
        "/W4",
        "/WX",
        "/permissive-",
        "/EHs-c-",
        "/D_HAS_EXCEPTIONS=0",
        "/wd4459",
    ):
        require(
            required_flag in windows_cxx_flags,
            f"Windows strict preset is missing {required_flag}",
        )

    sanitizer = preset_by_name(configure_presets, "linux-sanitize")
    sanitizer_cache = sanitizer.get("cacheVariables", {})
    sanitizer_flags = str(sanitizer_cache.get("CMAKE_CXX_FLAGS", ""))
    require(
        "-fsanitize=address,undefined" in sanitizer_flags
        and "-fno-exceptions" in sanitizer_flags,
        "Linux sanitizer preset lost sanitizer or no-exceptions flags",
    )

    run_checked(
        [str(cmake_command), "--list-presets"],
        source_directory,
    )

    gitattributes = (source_directory / ".gitattributes").read_text(
        encoding="utf-8"
    )
    require(
        "*.sh text eol=lf" in gitattributes.splitlines(),
        ".gitattributes must force every shell script to LF",
    )

    shell_scripts = [
        source_directory / "tools" / "validation" / "bootstrap-wsl-tools.sh",
        source_directory / "tools" / "validation" / "run-wsl-sanitize.sh",
        source_directory / "tools" / "validation" / "run-wsl-strict.sh",
    ]
    bash = None if sys.platform == "win32" else shutil.which("bash")
    for path in shell_scripts:
        require(b"\r\n" not in path.read_bytes(), f"shell script has CRLF: {path}")
        if bash is not None:
            run_checked([bash, "-n", str(path)], source_directory)

    bootstrap = shell_scripts[0].read_text(encoding="utf-8")
    for required_hash in (
        "f747d9b23e1a252a8beafb4ed2bc2ddf78cff7f04a8e4de19f4ff88e9b51dc9d",
        "6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255",
    ):
        require(required_hash in bootstrap, "WSL bootstrap lost a pinned SHA-256")
    require("sudo " not in bootstrap, "WSL bootstrap must not invoke sudo")
    require("apt-get" not in bootstrap, "WSL bootstrap must not invoke apt-get")

    windows_script = required_paths[1].read_text(encoding="utf-8")
    for required_text in (
        "vswhere.exe",
        "VsDevCmd.bat",
        "run_ctest.py",
        "cmake --preset",
        "cmake --build --preset",
        "PYTHONUTF8",
    ):
        require(
            required_text in windows_script,
            f"Windows validation entry point is missing: {required_text}",
        )
    validate_powershell(required_paths[1])

    run_checked(
        [sys.executable, str(required_paths[4]), "--help"],
        source_directory,
    )

    validation_ladder = (
        source_directory / "doc" / "validation-ladder.md"
    ).read_text(encoding="utf-8")
    for required_text in (
        "Invoke-BennuWindowsValidation.ps1",
        "run-wsl-strict.sh",
        "run-wsl-sanitize.sh",
        "run_ctest.py",
        "CMakePresets.json",
        "CTestCheckpoint.txt",
    ):
        require(
            required_text in validation_ladder,
            f"validation ladder is missing checked-in workflow text: {required_text}",
        )

    cmake_lists = (source_directory / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    for required_text in (
        "bennu_default_binary_only_tests",
        "bennu_strict_incompatible_tests",
        "bennu_sanitizer_incompatible_tests",
    ):
        require(
            required_text in cmake_lists,
            f"CTest compatibility policy is missing: {required_text}",
        )

    topology_text = run_checked(
        [
            str(ctest_command),
            "--test-dir",
            str(binary_directory),
            "--show-only=json-v1",
        ],
        source_directory,
    )
    topology = json.loads(topology_text)
    structural = [
        test
        for test in topology.get("tests", [])
        if test.get("name") == "unit.structural_host_allocation_refusal"
    ]
    if structural:
        labels = test_labels(structural[0])
        require("tier.full" in labels, "allocation-refusal test lost tier.full")
        require("tier.qa" in labels, "allocation-refusal test lost tier.qa")
        require("tier.strict" in labels, "allocation-refusal test lost tier.strict")
        require(
            "tier.sanitize" not in labels,
            "allocation-refusal test must be excluded from tier.sanitize",
        )

    print("validation workflow contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
