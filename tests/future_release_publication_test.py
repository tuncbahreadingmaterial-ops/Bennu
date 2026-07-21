#!/usr/bin/env python3

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


COMMIT = "0123456789abcdef0123456789abcdef01234567"
TAG = "v1.2.3"
NAMES = [
    "bennu-v1.2.3-linux-x64.tar.gz",
    "bennu-v1.2.3-macos-arm64.tar.gz",
    "bennu-v1.2.3-windows-x64.zip",
    "release-manifest.json",
]


def require(condition, message, result=None):
    if condition:
        return
    if result is not None:
        message += (
            f"\nstatus: {result.returncode}\nstdout:\n{result.stdout}"
            f"\nstderr:\n{result.stderr}"
        )
    raise AssertionError(message)


def load_state():
    path = Path(os.environ["FAKE_FUTURE_STATE"])
    return path, json.loads(path.read_text(encoding="utf-8"))


def save_state(path, state):
    path.write_text(json.dumps(state, sort_keys=True), encoding="utf-8")


def append_call(path, call):
    with path.with_suffix(".calls").open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(call, sort_keys=True) + "\n")


def release_object(state, apply_remote_override=True):
    assets = [
        {"id": 1001 + index, "name": name}
        for index, name in enumerate(sorted(state["assets"]))
    ]
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
        if state["draft"] and len(state["assets"]) == len(NAMES):
            response.update(state.get("remote_override_after_upload", {}))
        if state["published"]:
            response.update(state.get("published_override", {}))
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
    asset_match = re.fullmatch(
        r'\.assets\[\] \| select\(\.name == "([^"]+)"\) \| \.id', expression
    )
    if asset_match:
        for asset in response.get("assets", []):
            if asset["name"] == asset_match.group(1):
                print(asset["id"])
        return
    if expression == '[.id, (.id | type), .tag_name, .target_commitish, .draft, .prerelease][]':
        values = [
            response.get("id"),
            "number" if isinstance(response.get("id"), int) else "string",
            response.get("tag_name"),
            response.get("target_commitish"),
            response.get("draft"),
            response.get("prerelease"),
        ]
        for value in values:
            if isinstance(value, bool):
                print(str(value).lower())
            elif value is None:
                print("null")
            else:
                print(value)
        return
    if expression == '.object.sha + " " + .object.type':
        print(f"{response['object']['sha']} {response['object']['type']}")
        return
    print(f"fake gh does not support --jq {expression!r}", file=sys.stderr)
    raise SystemExit(90)


def parse_api(arguments):
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
            raise SystemExit(90)
    return method, endpoint, jq_expression, input_path, fields


def fake_gh(arguments):
    state_path, state = load_state()
    if arguments[:2] == ["attestation", "verify"]:
        append_call(state_path, {"command": "attestation", "arguments": arguments[2:]})
        state["attestation_count"] += 1
        save_state(state_path, state)
        if state["attestation_count"] == state.get("attestation_failure_number"):
            print("attestation verification failed", file=sys.stderr)
            return 1
        return 0
    if not arguments or arguments[0] != "api":
        print("fake gh only supports attestation verify and gh api", file=sys.stderr)
        return 90

    method, endpoint, jq_expression, input_path, fields = parse_api(arguments)
    append_call(
        state_path,
        {"method": method, "endpoint": endpoint, "fields": fields, "input": input_path},
    )
    repository = os.environ["GITHUB_REPOSITORY"]
    published_endpoint = f"repos/{repository}/releases/tags/{TAG}"
    release_endpoint = f"repos/{repository}/releases/{state['release_id']}"

    if method == "GET" and endpoint == published_endpoint:
        if not state["published"] or state.get("published_lookup_failure"):
            print("gh: Not Found (HTTP 404)", file=sys.stderr)
            return 1
        emit_jq(release_object(state), jq_expression)
        return 0

    if method == "GET" and endpoint == f"repos/{repository}/git/ref/tags/{TAG}":
        emit_jq(
            {"object": {"sha": state["tag_object"], "type": state["tag_type"]}},
            jq_expression,
        )
        return 0

    if method == "GET" and endpoint == f"repos/{repository}/git/tags/{state['tag_object']}":
        emit_jq(
            {"object": {"sha": state["tag_commit"], "type": "commit"}},
            jq_expression,
        )
        return 0

    if method == "GET" and endpoint == release_endpoint:
        if not state["exists"]:
            print("gh: Not Found (HTTP 404)", file=sys.stderr)
            return 1
        emit_jq(release_object(state), jq_expression)
        return 0

    asset_match = re.fullmatch(
        rf"repos/{re.escape(repository)}/releases/assets/(\d+)", endpoint or ""
    )
    if method == "GET" and asset_match:
        names = sorted(state["assets"])
        asset_index = int(asset_match.group(1)) - 1001
        if asset_index < 0 or asset_index >= len(names):
            print("gh: Not Found (HTTP 404)", file=sys.stderr)
            return 1
        contents = (state_path.with_suffix(".assets") / names[asset_index]).read_bytes()
        if state.get("tamper_download"):
            contents += b"tampered download"
        if state.get("tamper_download_after_publish") and state["published"]:
            contents += b"tampered published download"
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
            print("fake upload requires --input", file=sys.stderr)
            return 90
        if len(state["assets"]) + 1 == state.get("upload_failure_number"):
            print("gh: upload failed (HTTP 500)", file=sys.stderr)
            return 1
        contents = Path(input_path).read_bytes()
        if state.get("tamper_upload"):
            contents += b"tampered upload"
        (state_path.with_suffix(".assets") / name).write_bytes(contents)
        state["assets"].append(name)
        save_state(state_path, state)
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
        return 0

    print(f"fake gh does not support {method} {endpoint}", file=sys.stderr)
    return 90


def shell_quote(value):
    return "'" + value.replace("'", "'\"'\"'") + "'"


def write_wrapper(path):
    path.write_text(
        "#!/usr/bin/env bash\n"
        f"exec {shell_quote(sys.executable)} {shell_quote(str(Path(__file__).resolve()))} "
        "--fake-gh \"$@\"\n",
        encoding="utf-8",
    )
    path.chmod(0o755)


def read_calls(state_path):
    return [
        json.loads(line)
        for line in state_path.with_suffix(".calls").read_text(encoding="utf-8").splitlines()
    ]


def execute(bash, script, updates=None):
    with tempfile.TemporaryDirectory(prefix="bennu-future-publish-") as directory:
        root = Path(directory)
        candidates = root / "candidates"
        remote = root / "state.assets"
        fake_bin = root / "bin"
        candidates.mkdir()
        remote.mkdir()
        fake_bin.mkdir()
        for index, name in enumerate(NAMES[:-1]):
            (candidates / name).write_bytes(f"verified archive {index}\n".encode())
        manifest = {
            "version": "1.2.3",
            "tag": TAG,
            "source_commit": COMMIT,
            "artifacts": [{"archive": {"name": name}} for name in NAMES[:-1]],
        }
        (candidates / NAMES[-1]).write_text(json.dumps(manifest), encoding="utf-8")
        state = {
            "release_id": 424242,
            "exists": False,
            "published": False,
            "draft": False,
            "prerelease": False,
            "tag": None,
            "target": None,
            "assets": [],
            "tag_object": "tag-object",
            "tag_type": "tag",
            "tag_commit": COMMIT,
            "attestation_count": 0,
        }
        if updates:
            state.update(updates)
        if state["assets"]:
            for name in state["assets"]:
                shutil.copyfile(candidates / name, remote / name)
        state_path = root / "state.json"
        state_path.write_text(json.dumps(state), encoding="utf-8")
        state_path.with_suffix(".calls").write_text("", encoding="utf-8")
        write_wrapper(fake_bin / "gh")
        environment = os.environ.copy()
        environment.update(
            {
                "FAKE_FUTURE_STATE": str(state_path),
                "GITHUB_REPOSITORY": "example/Bennu",
                "RUNNER_TEMP": str(root),
                "PATH": str(fake_bin) + os.pathsep + environment["PATH"],
            }
        )
        result = subprocess.run(
            [bash, script, str(candidates), COMMIT, TAG],
            text=True,
            capture_output=True,
            check=False,
            env=environment,
        )
        return {
            "result": result,
            "state": json.loads(state_path.read_text(encoding="utf-8")),
            "calls": read_calls(state_path),
            "local": {name: (candidates / name).read_bytes() for name in NAMES},
            "remote": {path.name: path.read_bytes() for path in remote.iterdir()},
        }


def api_calls(case):
    return [call for call in case["calls"] if "method" in call]


def mutations(case):
    return [call for call in api_calls(case) if call["method"] in ("POST", "PATCH")]


def require_unpublished(case, description, draft_expected=True, patch_allowed=False):
    result = case["result"]
    require(result.returncode != 0, f"{description} unexpectedly succeeded", result)
    require(not case["state"]["published"], f"{description} published the release", result)
    if draft_expected:
        require(case["state"]["draft"], f"{description} did not preserve the draft", result)
    if not patch_allowed:
        require(
            not any(call["method"] == "PATCH" for call in api_calls(case)),
            f"{description} attempted publication",
            result,
        )


def run_success(bash, script):
    case = execute(bash, script)
    result = case["result"]
    require(result.returncode == 0, "fresh release path failed", result)
    require(result.stdout == "published verified non-prerelease v1.2.3 release\n",
            "success output changed", result)
    require(case["state"]["published"] and not case["state"]["draft"],
            "release was not published", result)
    require(sorted(case["state"]["assets"]) == NAMES, "remote names differ", result)
    require(case["remote"] == case["local"], "remote bytes differ", result)

    calls = case["calls"]
    api = api_calls(case)
    create = next(i for i, call in enumerate(api)
                  if call["method"] == "POST" and call["endpoint"].endswith("/releases"))
    patch = next(i for i, call in enumerate(api) if call["method"] == "PATCH")
    attestations = [i for i, call in enumerate(calls) if call.get("command") == "attestation"]
    first_creation_call = calls.index(api[create])
    require(len(attestations) == 4 and max(attestations) < first_creation_call,
            "all attestations must pass before draft creation", result)
    uploads = [call for call in api if call["method"] == "POST" and "/assets?name=" in call["endpoint"]]
    require([call["endpoint"].split("?name=", 1)[1] for call in uploads] == NAMES,
            "publisher did not upload exactly three archives plus manifest", result)
    require(any(call["endpoint"] == "repos/example/Bennu/releases/424242"
                for call in api[create + 1:patch]),
            "draft was not re-fetched by numeric ID", result)
    draft_downloads = [call for call in api[create + 1:patch]
                       if call["method"] == "GET" and "/releases/assets/" in call["endpoint"]]
    require(len(draft_downloads) == 4, "draft bytes were not all downloaded by asset ID", result)
    require(not any("/releases/tags/" in call["endpoint"] for call in api[create + 1:patch]),
            "draft lookup used the tag endpoint", result)
    require(api[patch]["endpoint"] == "repos/example/Bennu/releases/424242",
            "publication did not PATCH the numeric release ID", result)
    require(not any(call["method"] in ("POST", "PATCH") for call in api[patch + 1:]),
            "post-publication operation mutated remote state", result)
    require(any(call["endpoint"].endswith("/releases/tags/v1.2.3")
                for call in api[patch + 1:]),
            "published metadata was not verified read-only", result)
    published_downloads = [call for call in api[patch + 1:]
                           if call["method"] == "GET" and "/releases/assets/" in call["endpoint"]]
    require(len(published_downloads) == 4,
            "published bytes were not all re-downloaded read-only", result)


def run_precreation_failures(bash, script):
    published = {
        "exists": True,
        "published": True,
        "draft": False,
        "prerelease": False,
        "tag": TAG,
        "target": COMMIT,
        "assets": list(NAMES),
    }
    for updates, description in [
        (published, "existing published release"),
        ({"tag_type": "commit"}, "lightweight remote tag"),
        ({"tag_commit": "f" * 40}, "mismatched annotated tag"),
    ]:
        case = execute(bash, script, updates)
        require(case["result"].returncode != 0, f"{description} succeeded", case["result"])
        require(not mutations(case), f"{description} mutated a release", case["result"])

    for failure_number in range(1, 5):
        case = execute(bash, script, {"attestation_failure_number": failure_number})
        require(case["result"].returncode != 0,
                f"attestation failure {failure_number} succeeded", case["result"])
        require(not mutations(case),
                f"attestation failure {failure_number} mutated a release", case["result"])


def run_creation_failures(bash, script):
    malformed = [
        ("missing ID", {"id": None}),
        ("string ID", {"id": "424242"}),
        ("wrong tag", {"tag_name": "v9.9.9"}),
        ("wrong commit", {"target_commitish": "f" * 40}),
        ("published", {"draft": False}),
        ("prerelease", {"prerelease": True}),
    ]
    for description, override in malformed:
        case = execute(bash, script, {"creation_override": override})
        require_unpublished(case, f"malformed creation metadata ({description})")
        require(not any("/assets?name=" in call["endpoint"] for call in api_calls(case)),
                f"malformed creation metadata ({description}) uploaded assets", case["result"])

    case = execute(bash, script, {"creation_failure": True})
    require_unpublished(case, "creation API failure", draft_expected=False)
    require(not case["state"]["exists"], "creation failure produced a draft", case["result"])


def run_draft_failures(bash, script):
    case = execute(bash, script, {"upload_failure_number": 2})
    require_unpublished(case, "upload failure")
    require(case["state"]["assets"] == NAMES[:1], "upload continued after failure", case["result"])

    for updates, description in [
        ({"tamper_upload": True}, "uploaded-byte tamper"),
        ({"tamper_download": True}, "downloaded-byte tamper"),
        ({"asset_view": "missing"}, "missing asset"),
        ({"asset_view": "extra"}, "extra asset"),
        ({"remote_override": {"target_commitish": "f" * 40}}, "draft metadata mismatch"),
        ({"remote_override_after_upload": {"draft": False}}, "post-upload metadata mismatch"),
    ]:
        case = execute(bash, script, updates)
        require_unpublished(case, description)

    case = execute(bash, script, {"patch_failure": True})
    require_unpublished(case, "publication PATCH failure", patch_allowed=True)


def run_post_publish_failures(bash, script):
    for updates, description in [
        ({"published_lookup_failure": True}, "published metadata lookup failure"),
        ({"published_override": {"prerelease": True}}, "published metadata mismatch"),
        ({"tamper_download_after_publish": True}, "published byte mismatch"),
    ]:
        case = execute(bash, script, updates)
        require(case["result"].returncode != 0, f"{description} succeeded", case["result"])
        require(case["state"]["published"], f"{description} did not reach publication", case["result"])
        api = api_calls(case)
        patch = next(i for i, call in enumerate(api) if call["method"] == "PATCH")
        require(not any(call["method"] in ("POST", "PATCH") for call in api[patch + 1:]),
                f"{description} caused a post-publish mutation", case["result"])


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--fake-gh":
        return fake_gh(sys.argv[2:])
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <bash> <publish-future-release.sh>", file=sys.stderr)
        return 2
    run_success(sys.argv[1], sys.argv[2])
    run_precreation_failures(sys.argv[1], sys.argv[2])
    run_creation_failures(sys.argv[1], sys.argv[2])
    run_draft_failures(sys.argv[1], sys.argv[2])
    run_post_publish_failures(sys.argv[1], sys.argv[2])
    print("future release publication state-machine tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
