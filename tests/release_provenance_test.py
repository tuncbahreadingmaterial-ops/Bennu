#!/usr/bin/env python3

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile


SOURCE_COMMIT = "0123456789abcdef0123456789abcdef01234567"
REPOSITORY = "example/Bennu"
WORKFLOW = ".github/workflows/future-release.yml"


def require(condition, message, result=None):
    if condition:
        return
    if result is not None:
        message += (
            f"\nstatus: {result.returncode}\nstdout:\n{result.stdout}"
            f"\nstderr:\n{result.stderr}"
        )
    raise AssertionError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_failure(command, description):
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    require(result.returncode != 0, f"{description} was accepted", result)
    require(result.stderr.startswith("error: "),
            f"{description} did not produce a concise error", result)
    require("Traceback" not in result.stderr, f"{description} caused an uncontrolled traceback", result)


def product_version(source_dir):
    contents = (Path(source_dir) / "VERSION").read_bytes()
    require(contents.endswith(b"\n") and contents.count(b"\n") == 1 and b"\r" not in contents,
            "root VERSION is not one canonical LF-terminated line")
    return contents[:-1].decode("ascii")


def create_archive(root, executable, executable_path, target, version):
    stage = root / "stage"
    stage.mkdir()
    staged_executable = stage / executable_path
    shutil.copyfile(executable, staged_executable)
    staged_executable.chmod(0o755)
    (stage / "LICENSE").write_text("test license\n", encoding="utf-8")
    if target.startswith("windows-"):
        archive = root / f"bennu-v{version}-windows-x64.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.write(staged_executable, executable_path)
            output.write(stage / "LICENSE", "LICENSE")
    else:
        archive = root / f"bennu-v{version}-{target}.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            output.add(staged_executable, executable_path)
            output.add(stage / "LICENSE", "LICENSE")
    return archive, staged_executable


def run_fragment(python, tool, source_dir, executable, target):
    executable_path = "bennu.exe" if target.startswith("windows-") else "bennu"
    version = product_version(source_dir)
    with tempfile.TemporaryDirectory(prefix="bennu-provenance-") as directory:
        root = Path(directory)
        archive, staged_executable = create_archive(
            root, Path(executable), executable_path, target, version
        )
        output = root / f"{target}.fragment.json"
        result = subprocess.run(
            [
                python,
                tool,
                "fragment",
                "--version-file",
                str(Path(source_dir) / "VERSION"),
                "--target",
                target,
                "--source-commit",
                SOURCE_COMMIT,
                "--archive",
                str(archive),
                "--executable-path",
                executable_path,
                "--repository",
                REPOSITORY,
                "--workflow",
                WORKFLOW,
                "--output",
                str(output),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        require(result.returncode == 0, "fragment creation failed", result)
        raw = output.read_bytes()
        require(raw.endswith(b"\n") and raw.count(b"\n") == 1,
                "fragment is not compact newline-terminated JSON")
        document = json.loads(raw)
        require(raw == (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode(),
                "fragment is not canonical JSON")
        require(document["schema_version"] == 1, "fragment schema mismatch")
        require(document["version"] == version, "fragment version mismatch")
        require(document["source_commit"] == SOURCE_COMMIT, "fragment commit mismatch")
        require(document["tag"] is None, "development fragment acquired a tag")
        require(document["target"] == target, "fragment target mismatch")
        require(document["archive"] == {
            "name": archive.name,
            "sha256": sha256(archive),
        }, "fragment archive identity mismatch")
        require(document["executable"] == {
            "path": executable_path,
            "sha256": sha256(staged_executable),
            "version": version,
        }, "fragment executable identity mismatch")
        trust = document["trust_policy"]
        require(trust["mechanism"] == "github-artifact-attestation",
                "fragment trust mechanism mismatch")
        require(trust["repository"] == REPOSITORY and trust["workflow"] == WORKFLOW,
                "fragment trust identity is not repository/workflow-bound")
        require(trust["source_commit"] == SOURCE_COMMIT,
                "fragment trust identity is not commit-bound")
        require(trust["windows"] == "not-authenticode",
                "Windows trust state is dishonest")
        require(trust["macos"] == "not-developer-id-or-notarized",
                "macOS native trust state is dishonest")
        require(trust["macos_issue_12"] == "CLOSED_NOT_PLANNED",
                "Issue #12 cancellation is missing")
        require(b"timestamp" not in raw and str(root).encode() not in raw,
                "fragment contains nondeterministic provenance")

        manifest = root / "release-manifest.json"
        merge = subprocess.run(
            [
                python,
                tool,
                "merge",
                "--fragment",
                str(output),
                "--asset-dir",
                str(root),
                "--output",
                str(manifest),
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        require(merge.returncode == 0, "manifest merge failed", merge)
        manifest_raw = manifest.read_bytes()
        merged = json.loads(manifest_raw)
        require(manifest_raw == (json.dumps(merged, sort_keys=True, separators=(",", ":")) + "\n").encode(),
                "manifest is not canonical JSON")
        require(merged["schema_version"] == 1, "manifest schema mismatch")
        require(merged["version"] == document["version"], "manifest version mismatch")
        require(merged["source_commit"] == SOURCE_COMMIT, "manifest commit mismatch")
        require(merged["tag"] is None, "development manifest acquired a tag")
        require(merged["trust_policy"] == trust, "manifest trust policy drifted")
        require(merged["artifacts"] == [{
            "archive": document["archive"],
            "executable": document["executable"],
            "fragment": {"name": output.name, "sha256": sha256(output)},
            "target": target,
        }], "manifest artifact mapping mismatch")

        verify = subprocess.run(
            [
                python,
                tool,
                "verify",
                "--manifest",
                str(manifest),
                "--asset-dir",
                str(root),
                "--fragment-dir",
                str(root),
                "--target",
                target,
                "--expected-repository",
                REPOSITORY,
                "--expected-workflow",
                WORKFLOW,
                "--expected-source-commit",
                SOURCE_COMMIT,
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        require(verify.returncode == 0, "manifest verification failed", verify)

        verify_command = [
            python, tool, "verify", "--manifest", str(manifest),
            "--asset-dir", str(root), "--fragment-dir", str(root),
            "--target", target, "--expected-repository", REPOSITORY,
            "--expected-workflow", WORKFLOW,
            "--expected-source-commit", SOURCE_COMMIT,
        ]
        archive_bytes = archive.read_bytes()
        archive.write_bytes(archive_bytes + b"tamper")
        expect_failure(verify_command, "tampered archive")
        archive.write_bytes(archive_bytes)

        fragment_bytes = output.read_bytes()
        changed_fragment = json.loads(fragment_bytes)
        changed_fragment["executable"]["sha256"] = "0" * 64
        output.write_text(json.dumps(changed_fragment, sort_keys=True, separators=(",", ":")) + "\n")
        expect_failure(verify_command, "tampered fragment")
        output.write_bytes(fragment_bytes)

        manifest_bytes = manifest.read_bytes()
        changed_manifest = json.loads(manifest_bytes)
        changed_manifest["artifacts"][0]["archive"]["sha256"] = "f" * 64
        manifest.write_text(json.dumps(changed_manifest, sort_keys=True, separators=(",", ":")) + "\n")
        expect_failure(verify_command, "tampered manifest")
        manifest.write_bytes(manifest_bytes)

        duplicate_merge = [
            python, tool, "merge", "--fragment", str(output),
            "--fragment", str(output), "--asset-dir", str(root),
            "--output", str(root / "duplicate-manifest.json"),
        ]
        expect_failure(duplicate_merge, "duplicate target fragments")

        mutations = {
            "version": lambda value: value.update({"version": "0.2.1"}),
            "commit": lambda value: value.update({"source_commit": "f" * 40}),
            "target": lambda value: value.update({"target": "unknown-target"}),
            "archive_name": lambda value: value["archive"].update({"name": "../archive.tar.gz"}),
            "executable_path": lambda value: value["executable"].update({"path": "../bennu"}),
            "archive_hash": lambda value: value["archive"].update({"sha256": "0" * 64}),
            "tag": lambda value: value.update({"tag": "v9.9.9"}),
            "extra_field": lambda value: value.update({"unexpected": True}),
            "trust_type": lambda value: value.update({"trust_policy": []}),
            "target_type": lambda value: value.update({"target": []}),
            "commit_type": lambda value: value.update({"source_commit": []}),
            "archive_type": lambda value: value.update({"archive": []}),
            "executable_type": lambda value: value.update({"executable": []}),
            "trust_repository_type": lambda value: value["trust_policy"].update({"repository": []}),
            "trust_workflow_type": lambda value: value["trust_policy"].update({"workflow": {}}),
        }
        for name, mutate in mutations.items():
            mutation_root = root / f"mutation-{name}"
            mutation_root.mkdir()
            candidate = json.loads(fragment_bytes)
            mutate(candidate)
            mutation_path = mutation_root / f"{target}.fragment.json"
            mutation_path.write_text(
                json.dumps(candidate, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            expect_failure(
                [python, tool, "merge", "--fragment", str(mutation_path),
                 "--asset-dir", str(root), "--output", str(mutation_root / "manifest.json")],
                f"fragment {name} mismatch",
            )

        duplicate_json = root / "duplicate-json" / f"{target}.fragment.json"
        duplicate_json.parent.mkdir()
        duplicate_json.write_text('{"schema_version":1,"schema_version":1}\n', encoding="utf-8")
        expect_failure(
            [python, tool, "merge", "--fragment", str(duplicate_json),
             "--asset-dir", str(root), "--output", str(duplicate_json.parent / "manifest.json")],
            "duplicate JSON keys",
        )

        non_object_fragment = root / "non-object.fragment.json"
        non_object_fragment.write_text("[]\n", encoding="utf-8")
        expect_failure(
            [python, tool, "merge", "--fragment", str(non_object_fragment),
             "--asset-dir", str(root), "--output", str(root / "non-object-manifest.json")],
            "non-object fragment",
        )

        manifest_mutations = {
            "artifacts_object": lambda value: value.update({"artifacts": {}}),
            "artifact_non_object": lambda value: value.update({"artifacts": [None]}),
            "artifact_target_type": lambda value: value["artifacts"][0].update({"target": []}),
            "tag_type": lambda value: value.update({"tag": {}}),
            "version_type": lambda value: value.update({"version": []}),
            "trust_type": lambda value: value.update({"trust_policy": []}),
            "archive_type": lambda value: value["artifacts"][0].update({"archive": []}),
            "fragment_type": lambda value: value["artifacts"][0].update({"fragment": []}),
        }
        for name, mutate in manifest_mutations.items():
            candidate = json.loads(manifest_bytes)
            mutate(candidate)
            candidate_path = root / f"manifest-{name}.json"
            candidate_path.write_text(
                json.dumps(candidate, sort_keys=True, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
            expect_failure(
                [python, tool, "verify", "--manifest", str(candidate_path),
                 "--asset-dir", str(root), "--fragment-dir", str(root),
                 "--target", target, "--expected-repository", REPOSITORY,
                 "--expected-workflow", WORKFLOW,
                 "--expected-source-commit", SOURCE_COMMIT],
                f"manifest {name} shape",
            )

        non_object_manifest = root / "manifest-non-object.json"
        non_object_manifest.write_text("[]\n", encoding="utf-8")
        expect_failure(
            [python, tool, "verify", "--manifest", str(non_object_manifest),
             "--asset-dir", str(root), "--fragment-dir", str(root),
             "--target", target, "--expected-repository", REPOSITORY,
             "--expected-workflow", WORKFLOW,
             "--expected-source-commit", SOURCE_COMMIT],
            "non-object manifest",
        )

        bad_archives = root / "bad-archives"
        bad_archives.mkdir()
        for case in ("unsafe", "missing", "extra"):
            case_root = bad_archives / case
            case_root.mkdir()
            bad_archive = case_root / archive.name
            if target.startswith("windows-"):
                with zipfile.ZipFile(bad_archive, "w") as package:
                    if case == "unsafe":
                        package.writestr(f"../{executable_path}", staged_executable.read_bytes())
                    else:
                        package.writestr("LICENSE", "test license\n")
                        if case == "extra":
                            package.writestr(executable_path, staged_executable.read_bytes())
                            package.writestr(f"other/{executable_path}", staged_executable.read_bytes())
            else:
                with tarfile.open(bad_archive, "w:gz") as package:
                    if case == "unsafe":
                        info = tarfile.TarInfo(f"../{executable_path}")
                        contents = staged_executable.read_bytes()
                        info.size = len(contents)
                        import io
                        package.addfile(info, io.BytesIO(contents))
                    else:
                        package.add(root / "stage" / "LICENSE", "LICENSE")
                        if case == "extra":
                            package.add(staged_executable, executable_path)
                            package.add(staged_executable, f"other/{executable_path}")
            expect_failure(
                [python, tool, "fragment", "--version-file", str(Path(source_dir) / "VERSION"),
                 "--target", target, "--source-commit", SOURCE_COMMIT,
                 "--archive", str(bad_archive), "--executable-path", executable_path,
                 "--repository", REPOSITORY, "--workflow", WORKFLOW,
                 "--output", str(case_root / f"{target}.fragment.json")],
                f"{case} executable archive",
            )


def run_gate(python, tool):
    with tempfile.TemporaryDirectory(prefix="bennu-production-gate-") as directory:
        repository = Path(directory)
        (repository / "VERSION").write_bytes(b"1.2.3\n\n")
        (repository / "tracked").write_text("source\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q", str(repository)], check=True)
        subprocess.run(
            ["git", "-C", str(repository), "add", "VERSION", "tracked"], check=True
        )
        subprocess.run(
            [
                "git", "-C", str(repository), "-c", "user.name=Bennu Test",
                "-c", "user.email=bennu@example.invalid", "commit", "-qm", "test",
            ],
            check=True,
        )
        mismatched_commit = subprocess.run(
            ["git", "-C", str(repository), "rev-parse", "HEAD"],
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
        (repository / "VERSION").write_bytes(b"1.2.3\n")
        subprocess.run(
            [
                "git", "-C", str(repository), "-c", "user.name=Bennu Test",
                "-c", "user.email=bennu@example.invalid", "tag", "-am", "release", "v1.2.3",
            ],
            check=True,
        )
        command = [
            python,
            tool,
            "gate",
            "--version-file",
            str(repository / "VERSION"),
            "--repository-dir",
            str(repository),
            "--source-commit",
            mismatched_commit,
            "--tag",
            "v1.2.3",
        ]
        expect_failure(command, "committed VERSION byte mismatch")

        subprocess.run(["git", "-C", str(repository), "tag", "-d", "v1.2.3"],
                       capture_output=True, check=True)
        subprocess.run(["git", "-C", str(repository), "add", "VERSION"], check=True)
        subprocess.run(
            [
                "git", "-C", str(repository), "-c", "user.name=Bennu Test",
                "-c", "user.email=bennu@example.invalid", "commit", "-qm", "canonical version",
            ],
            check=True,
        )
        commit = subprocess.run(
            ["git", "-C", str(repository), "rev-parse", "HEAD"],
            text=True, capture_output=True, check=True,
        ).stdout.strip()
        subprocess.run(
            [
                "git", "-C", str(repository), "-c", "user.name=Bennu Test",
                "-c", "user.email=bennu@example.invalid", "tag", "-am", "release", "v1.2.3",
            ],
            check=True,
        )
        command[command.index(mismatched_commit)] = commit
        accepted = subprocess.run(command, text=True, capture_output=True, check=False)
        require(accepted.returncode == 0, "annotated production tag was rejected", accepted)

        subprocess.run(["git", "-C", str(repository), "tag", "-d", "v1.2.3"],
                       capture_output=True, check=True)
        subprocess.run(["git", "-C", str(repository), "tag", "v1.2.3"], check=True)
        lightweight = subprocess.run(command, text=True, capture_output=True, check=False)
        require(lightweight.returncode != 0 and "annotated" in lightweight.stderr,
                "lightweight production tag was accepted", lightweight)

        (repository / "VERSION").write_text("0.2.0-dev\n", encoding="ascii")
        development = subprocess.run(command, text=True, capture_output=True, check=False)
        require(development.returncode != 0 and "stable" in development.stderr,
                "development VERSION passed the production gate", development)


def main():
    require(len(sys.argv) == 6,
            "usage: release_provenance_test.py <python> <tool> <source> <bennu> <target>")
    run_fragment(*sys.argv[1:])
    run_gate(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
