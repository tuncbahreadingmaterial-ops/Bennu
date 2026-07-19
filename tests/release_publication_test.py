#!/usr/bin/env python3

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECTED_COMMIT = "0123456789abcdef0123456789abcdef01234567"
EXPECTED_TAG = "v0.1.0"
EXPECTED_NAMES = [
    "bennu-v0.1.0-linux-x64.tar.gz",
    "bennu-v0.1.0-macos-arm64.tar.gz",
    "bennu-v0.1.0-windows-x64.zip",
]


def load_state():
    state_path = Path(os.environ["FAKE_RELEASE_STATE"])
    return state_path, json.loads(state_path.read_text(encoding="utf-8"))


def save_state(state_path, state):
    state_path.write_text(json.dumps(state, sort_keys=True), encoding="utf-8")


def append_call(state_path, call):
    with state_path.with_suffix(".calls").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(call) + "\n")


def release_object(state, apply_remote_override=True):
    assets = []
    for index, name in enumerate(sorted(state["assets"]), start=1):
        assets.append({"id": 1000 + index, "name": name})
    if state.get("asset_view") == "missing" and assets:
        assets.pop()
    elif state.get("asset_view") == "extra":
        assets.append({"id": 1999, "name": "unexpected.bin"})
    response = {
        "id": state["release_id"],
        "tag_name": state["tag"],
        "target_commitish": state["target"],
        "draft": state["draft"],
        "prerelease": state["prerelease"],
        "assets": assets,
    }
    if apply_remote_override:
        response.update(state.get("remote_override", {}))
        if state["draft"] and len(state["assets"]) == len(EXPECTED_NAMES):
            response.update(state.get("remote_override_after_upload", {}))
    return response


def emit_jq(response, expression):
    if expression is None:
        sys.stdout.write(json.dumps(response))
        return
    scalar_fields = {
        ".id": "id",
        ".tag_name": "tag_name",
        ".target_commitish": "target_commitish",
        ".draft": "draft",
        ".prerelease": "prerelease",
    }
    if expression in scalar_fields:
        value = response.get(scalar_fields[expression])
        if isinstance(value, bool):
            print(str(value).lower())
        elif value is not None:
            print(value)
        return
    if expression == ".assets[].name":
        for asset in response.get("assets", []):
            print(asset["name"])
        return
    match = re.fullmatch(r'\.assets\[\] \| select\(\.name == "([^"]+)"\) \| \.id', expression)
    if match:
        for asset in response.get("assets", []):
            if asset["name"] == match.group(1):
                print(asset["id"])
        return
    creation_expressions = {
        "[.id, .tag_name, .target_commitish, .draft, .prerelease][]": (
            "id",
            "tag_name",
            "target_commitish",
            "draft",
            "prerelease",
        ),
        "[.id, (.id | type), .tag_name, .target_commitish, .draft, .prerelease][]": (
            "id",
            "id_type",
            "tag_name",
            "target_commitish",
            "draft",
            "prerelease",
        ),
    }
    if expression in creation_expressions:
        for key in creation_expressions[expression]:
            value = response.get(key)
            if key == "id_type":
                value = "number" if isinstance(response.get("id"), int) else "string"
            if isinstance(value, bool):
                print(str(value).lower())
            elif value is None:
                print("null")
            else:
                print(value)
        return
    print(f"fake gh does not support --jq {expression!r}", file=sys.stderr)
    raise SystemExit(90)


def fake_gh(arguments):
    if not arguments or arguments[0] != "api":
        print("fake gh only supports gh api", file=sys.stderr)
        return 90

    method = "GET"
    endpoint = None
    jq_expression = None
    input_path = None
    fields = {}
    index = 1
    while index < len(arguments):
        argument = arguments[index]
        if argument == "--method":
            method = arguments[index + 1]
            index += 2
        elif argument in ("-H", "--header"):
            index += 2
        elif argument in ("-f", "-F"):
            key, value = arguments[index + 1].split("=", 1)
            fields[key] = value
            index += 2
        elif argument == "--input":
            input_path = arguments[index + 1]
            index += 2
        elif argument == "--jq":
            jq_expression = arguments[index + 1]
            index += 2
        elif endpoint is None:
            endpoint = argument
            index += 1
        else:
            print(f"fake gh received unexpected argument {argument!r}", file=sys.stderr)
            return 90

    state_path, state = load_state()
    append_call(
        state_path,
        {"method": method, "endpoint": endpoint, "fields": fields, "input": input_path},
    )
    repository = os.environ["GITHUB_REPOSITORY"]
    tag_endpoint = f"repos/{repository}/releases/tags/{EXPECTED_TAG}"
    release_endpoint = f"repos/{repository}/releases/{state['release_id']}"

    if method == "GET" and endpoint == tag_endpoint:
        if not state["published"] or state.get("published_lookup_failure"):
            print("gh: Not Found (HTTP 404)", file=sys.stderr)
            return 1
        emit_jq(release_object(state), jq_expression)
        return 0

    if method == "GET" and endpoint == release_endpoint:
        if not state["exists"]:
            print("gh: Not Found (HTTP 404)", file=sys.stderr)
            return 1
        emit_jq(release_object(state), jq_expression)
        return 0

    asset_match = re.fullmatch(rf"repos/{re.escape(repository)}/releases/assets/(\d+)", endpoint or "")
    if method == "GET" and asset_match:
        asset_id = int(asset_match.group(1))
        names = sorted(state["assets"])
        if asset_id < 1001 or asset_id >= 1001 + len(names):
            print("gh: Not Found (HTTP 404)", file=sys.stderr)
            return 1
        asset_path = state_path.with_suffix(".assets") / names[asset_id - 1001]
        contents = asset_path.read_bytes()
        if state.get("corrupt_download"):
            contents += b"corrupt"
        sys.stdout.buffer.write(contents)
        return 0

    if method == "POST" and endpoint == f"repos/{repository}/releases":
        if state.get("creation_failure"):
            print("gh: Server Error (HTTP 500)", file=sys.stderr)
            return 1
        if state["exists"]:
            print("gh: Validation Failed (HTTP 422)", file=sys.stderr)
            return 1
        state.update(
            {
                "exists": True,
                "published": False,
                "draft": True,
                "prerelease": False,
                "tag": fields.get("tag_name"),
                "target": fields.get("target_commitish"),
            }
        )
        save_state(state_path, state)
        response = release_object(state, apply_remote_override=False)
        response.update(state.get("creation_override", {}))
        emit_jq(response, jq_expression)
        return 0

    upload_match = re.fullmatch(
        rf"repos/{re.escape(repository)}/releases/{state['release_id']}/assets\?name=(.+)",
        endpoint or "",
    )
    if method == "POST" and upload_match:
        name = upload_match.group(1)
        if input_path is None:
            print("fake gh asset upload requires --input", file=sys.stderr)
            return 90
        if len(state["assets"]) + 1 == state.get("upload_failure_number"):
            print("gh: upload failed (HTTP 500)", file=sys.stderr)
            return 1
        source = Path(input_path)
        destination = state_path.with_suffix(".assets") / name
        shutil.copyfile(source, destination)
        state["assets"].append(name)
        save_state(state_path, state)
        emit_jq({"id": 1000 + len(state["assets"]), "name": name}, jq_expression)
        return 0

    if method == "PATCH" and endpoint == release_endpoint:
        if fields.get("draft") != "false" or fields.get("prerelease") != "false":
            print("fake gh rejected unexpected publication fields", file=sys.stderr)
            return 90
        if state.get("patch_failure"):
            print("gh: publish failed (HTTP 500)", file=sys.stderr)
            return 1
        state["draft"] = False
        state["published"] = True
        save_state(state_path, state)
        emit_jq(release_object(state), jq_expression)
        return 0

    print(f"fake gh does not support {method} {endpoint}", file=sys.stderr)
    return 90


def fake_git(arguments):
    state_path, state = load_state()
    append_call(state_path, {"git": arguments})
    if arguments == ["rev-parse", "refs/tags/v0.1.0^{commit}"]:
        print(state["tag_commit"])
        return 0
    if arguments[:1] == ["fetch"]:
        return 0
    if arguments == ["merge-base", "--is-ancestor", EXPECTED_COMMIT, "refs/remotes/origin/main"]:
        return 0 if state["reachable"] else 1
    print(f"fake git does not support {arguments!r}", file=sys.stderr)
    return 90


def write_wrapper(path, python, driver, mode):
    path.write_text(
        "#!/usr/bin/env bash\n"
        f"exec {shlex_quote(python)} {shlex_quote(driver)} {mode} \"$@\"\n",
        encoding="utf-8",
    )
    path.chmod(0o755)


def shlex_quote(value):
    return "'" + value.replace("'", "'\"'\"'") + "'"


def read_calls(state_path):
    return [json.loads(line) for line in state_path.with_suffix(".calls").read_text(encoding="utf-8").splitlines()]


def require(condition, message, result=None):
    if condition:
        return
    if result is not None:
        message += f"\nstatus: {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    raise AssertionError(message)


def execute_case(bash, publish_script, state_updates=None):
    with tempfile.TemporaryDirectory(prefix="bennu-release-publication-") as directory:
        root = Path(directory)
        fake_bin = root / "bin"
        assets = root / "local-assets"
        remote_assets = root / "state.assets"
        fake_bin.mkdir()
        assets.mkdir()
        remote_assets.mkdir()
        for index, name in enumerate(EXPECTED_NAMES):
            (assets / name).write_bytes(f"verified asset {index}\n".encode())

        state = {
            "release_id": 424242,
            "exists": False,
            "published": False,
            "draft": False,
            "prerelease": False,
            "tag": None,
            "target": None,
            "assets": [],
            "tag_commit": EXPECTED_COMMIT,
            "reachable": True,
        }
        if state_updates:
            state.update(state_updates)
        if state["assets"]:
            for name in state["assets"]:
                shutil.copyfile(assets / name, remote_assets / name)

        state_path = root / "state.json"
        state_path.write_text(json.dumps(state), encoding="utf-8")
        state_path.with_suffix(".calls").write_text("", encoding="utf-8")
        write_wrapper(fake_bin / "gh", sys.executable, str(Path(__file__).resolve()), "--fake-gh")
        write_wrapper(fake_bin / "git", sys.executable, str(Path(__file__).resolve()), "--fake-git")

        environment = os.environ.copy()
        environment.update(
            {
                "FAKE_RELEASE_STATE": str(state_path),
                "GITHUB_REPOSITORY": "example/Bennu",
                "RUNNER_TEMP": str(root),
                "PATH": str(fake_bin) + os.pathsep + environment["PATH"],
            }
        )
        result = subprocess.run(
            [bash, publish_script, str(assets), EXPECTED_COMMIT],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )
        remote_bytes = {
            path.name: path.read_bytes() for path in remote_assets.iterdir() if path.is_file()
        }
        return {
            "result": result,
            "state": json.loads(state_path.read_text(encoding="utf-8")),
            "calls": read_calls(state_path),
            "local_bytes": {name: (assets / name).read_bytes() for name in EXPECTED_NAMES},
            "remote_bytes": remote_bytes,
        }


def api_calls(case):
    return [call for call in case["calls"] if "method" in call]


def mutation_calls(case):
    return [call for call in api_calls(case) if call["method"] in ("POST", "PATCH")]


def require_failed_unpublished(case, description, draft_exists=True, allow_patch_attempt=False):
    result = case["result"]
    require(result.returncode != 0, f"{description} unexpectedly succeeded", result)
    require(not case["state"]["published"], f"{description} published the release", result)
    if draft_exists:
        require(case["state"]["draft"], f"{description} did not preserve the draft", result)
    if not allow_patch_attempt:
        require(
            not any(call["method"] == "PATCH" for call in api_calls(case)),
            f"{description} attempted publication",
            result,
        )


def run_fresh_release_case(bash, publish_script):
    case = execute_case(bash, publish_script)
    result = case["result"]
    require(result.returncode == 0, "fresh release path failed", result)
    require(
        result.stdout == "published verified non-prerelease v0.1.0 release\n",
        "fresh release success output changed",
        result,
    )

    state = case["state"]
    require(state["published"] and not state["draft"], "release was not published last", result)
    require(sorted(state["assets"]) == EXPECTED_NAMES, "remote asset names are not exact", result)
    require(case["remote_bytes"] == case["local_bytes"], "remote asset bytes differ", result)

    calls = api_calls(case)
    create_index = next(
        index
        for index, call in enumerate(calls)
        if call["method"] == "POST" and call["endpoint"] == "repos/example/Bennu/releases"
    )
    patch_index = next(index for index, call in enumerate(calls) if call["method"] == "PATCH")
    uploads = [call for call in calls if call["method"] == "POST" and "/assets?name=" in call["endpoint"]]
    draft_downloads = [
        call
        for call in calls[create_index + 1 : patch_index]
        if call["method"] == "GET" and "/releases/assets/" in call["endpoint"]
    ]
    require(len(uploads) == 3, "fresh release did not upload exactly three assets", result)
    require(
        [call["endpoint"].split("?name=", 1)[1] for call in uploads] == EXPECTED_NAMES,
        "fresh release upload order or names changed",
        result,
    )
    require(
        len(draft_downloads) == 3,
        "fresh release did not download all three draft assets before publication",
        result,
    )
    require(
        not any(
            call["method"] == "GET" and "/releases/tags/" in call["endpoint"]
            for call in calls[create_index + 1 : patch_index]
        ),
        "draft path queried the published-tag endpoint after creation",
        result,
    )
    require(
        any(
            call["method"] == "GET" and call["endpoint"] == "repos/example/Bennu/releases/424242"
            for call in calls[create_index + 1 : patch_index]
        ),
        "draft was not addressed by release ID before publication",
        result,
    )
    require(
        any(
            call["method"] == "GET" and call["endpoint"] == "repos/example/Bennu/releases/tags/v0.1.0"
            for call in calls[patch_index + 1 :]
        ),
        "published release was not verified through the tag endpoint",
        result,
    )
    require(
        not any(call["method"] in ("POST", "PATCH") for call in calls[patch_index + 1 :]),
        "fresh release mutated remote state after publication",
        result,
    )


def run_creation_response_failure_cases(bash, publish_script):
    malformed = [
        ("missing ID", {"id": None}),
        ("nonnumeric ID", {"id": "not-a-number"}),
        ("numeric string ID", {"id": "424242"}),
        ("wrong tag", {"tag_name": "v9.9.9"}),
        ("wrong target", {"target_commitish": "f" * 40}),
        ("published response", {"draft": False}),
        ("prerelease response", {"prerelease": True}),
    ]
    for description, override in malformed:
        case = execute_case(bash, publish_script, {"creation_override": override})
        require_failed_unpublished(case, f"malformed creation response ({description})")
        require(
            not any("/assets?name=" in call["endpoint"] for call in api_calls(case)),
            f"malformed creation response ({description}) uploaded an asset",
            case["result"],
        )

    case = execute_case(bash, publish_script, {"creation_failure": True})
    require_failed_unpublished(case, "failed draft creation", draft_exists=False)
    require(not case["state"]["exists"], "failed creation produced a draft", case["result"])


def run_draft_failure_cases(bash, publish_script):
    case = execute_case(bash, publish_script, {"upload_failure_number": 2})
    require_failed_unpublished(case, "asset upload failure")
    require(case["state"]["assets"] == EXPECTED_NAMES[:1], "upload failure did not stop immediately", case["result"])

    for asset_view in ("missing", "extra"):
        case = execute_case(bash, publish_script, {"asset_view": asset_view})
        require_failed_unpublished(case, f"{asset_view} remote asset set")

    case = execute_case(bash, publish_script, {"corrupt_download": True})
    require_failed_unpublished(case, "draft asset byte mismatch")

    case = execute_case(
        bash,
        publish_script,
        {"remote_override": {"target_commitish": "f" * 40}},
    )
    require_failed_unpublished(case, "draft metadata mismatch")
    require(
        not any("/assets?name=" in call["endpoint"] for call in api_calls(case)),
        "draft metadata mismatch uploaded assets",
        case["result"],
    )

    case = execute_case(
        bash,
        publish_script,
        {"remote_override_after_upload": {"draft": False}},
    )
    require_failed_unpublished(case, "draft metadata change after upload")

    case = execute_case(bash, publish_script, {"patch_failure": True})
    require_failed_unpublished(case, "publish API failure", allow_patch_attempt=True)


def published_state(**updates):
    state = {
        "exists": True,
        "published": True,
        "draft": False,
        "prerelease": False,
        "tag": EXPECTED_TAG,
        "target": EXPECTED_COMMIT,
        "assets": list(EXPECTED_NAMES),
    }
    state.update(updates)
    return state


def run_existing_release_cases(bash, publish_script):
    case = execute_case(bash, publish_script, published_state())
    result = case["result"]
    require(result.returncode == 0, "matching published release was not a no-op", result)
    require(
        result.stdout == "matching v0.1.0 release is already published; no changes made\n",
        "matching published release output changed",
        result,
    )
    require(not mutation_calls(case), "matching published release was mutated", result)

    case = execute_case(
        bash,
        publish_script,
        published_state(remote_override={"target_commitish": "f" * 40}),
    )
    require(case["result"].returncode != 0, "mismatched published metadata succeeded", case["result"])
    require(not mutation_calls(case), "mismatched published metadata was mutated", case["result"])

    case = execute_case(bash, publish_script, published_state(corrupt_download=True))
    require(case["result"].returncode != 0, "mismatched published bytes succeeded", case["result"])
    require(not mutation_calls(case), "mismatched published bytes were mutated", case["result"])


def run_provenance_cases(bash, publish_script):
    cases = [
        ("tag mismatch", {"tag_commit": "f" * 40}),
        ("tag not on live main", {"reachable": False}),
    ]
    for description, state in cases:
        case = execute_case(bash, publish_script, state)
        require(case["result"].returncode != 0, f"{description} succeeded", case["result"])
        require(not api_calls(case), f"{description} reached the release API", case["result"])


def run_post_publish_verification_case(bash, publish_script):
    case = execute_case(bash, publish_script, {"published_lookup_failure": True})
    require(case["result"].returncode != 0, "missing public release verification succeeded", case["result"])
    calls = api_calls(case)
    patch_index = next(index for index, call in enumerate(calls) if call["method"] == "PATCH")
    require(
        not any(call["method"] in ("POST", "PATCH") for call in calls[patch_index + 1 :]),
        "public verification failure caused another mutation",
        case["result"],
    )


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--fake-gh":
        return fake_gh(sys.argv[2:])
    if len(sys.argv) >= 2 and sys.argv[1] == "--fake-git":
        return fake_git(sys.argv[2:])
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <bash> <publish-release.sh>", file=sys.stderr)
        return 2
    bash = sys.argv[1]
    publish_script = sys.argv[2]
    run_fresh_release_case(bash, publish_script)
    run_creation_response_failure_cases(bash, publish_script)
    run_draft_failure_cases(bash, publish_script)
    run_existing_release_cases(bash, publish_script)
    run_provenance_cases(bash, publish_script)
    run_post_publish_verification_case(bash, publish_script)
    print("release publication state-machine tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
