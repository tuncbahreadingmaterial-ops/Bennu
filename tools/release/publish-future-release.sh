#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: publish-future-release.sh <candidate-directory> <source-commit> <annotated-tag>" >&2
  exit 2
fi

candidate_directory=$(realpath "$1")
source_commit=$2
release_tag=$3
repository=${GITHUB_REPOSITORY:-}
if [[ -z "$repository" ]]; then
  echo "error: GITHUB_REPOSITORY is required" >&2
  exit 1
fi
if [[ ! "$source_commit" =~ ^[0-9a-f]{40}$ ]]; then
  echo "error: source commit must be an exact lowercase 40-hex object ID" >&2
  exit 1
fi
if [[ ! -f "$candidate_directory/release-manifest.json" ]]; then
  echo "error: verified release manifest is missing" >&2
  exit 1
fi

mapfile -t identity < <(python3 - "$candidate_directory/release-manifest.json" <<'PY'
import json
from pathlib import Path
import sys

try:
    document = json.loads(Path(sys.argv[1]).read_bytes())
    version = document["version"]
    source_commit = document["source_commit"]
    tag = document["tag"]
    artifacts = document["artifacts"]
    if (not isinstance(document, dict) or not isinstance(version, str)
            or not isinstance(source_commit, str) or not isinstance(tag, str)
            or not isinstance(artifacts, list)):
        raise ValueError("manifest identity has invalid types")
    archive_names = []
    for artifact in artifacts:
        if not isinstance(artifact, dict) or not isinstance(artifact.get("archive"), dict):
            raise ValueError("manifest artifact has invalid types")
        name = artifact["archive"].get("name")
        if not isinstance(name, str):
            raise ValueError("manifest archive name has invalid type")
        archive_names.append(name)
except (KeyError, OSError, TypeError, ValueError, json.JSONDecodeError) as error:
    raise SystemExit(f"error: unable to read release manifest identity: {error}")

print(version)
print(source_commit)
print(tag)
for name in sorted(archive_names):
    print(name)
PY
)
if [[ ${#identity[@]} -ne 6 ]]; then
  echo "error: manifest must describe exactly three release archives" >&2
  exit 1
fi
version=${identity[0]}
manifest_commit=${identity[1]}
manifest_tag=${identity[2]}
if [[ "$version" == "0.1.0" || "$release_tag" == "v0.1.0" ]]; then
  echo "error: v0.1.0 predates this contract and is immutable" >&2
  exit 1
fi
if [[ "$version" == *-* || "$version" == *+* || "$release_tag" != "v$version" ]]; then
  echo "error: publication requires stable VERSION and exact v<VERSION> tag" >&2
  exit 1
fi
if [[ "$manifest_commit" != "$source_commit" || "$manifest_tag" != "$release_tag" ]]; then
  echo "error: manifest version/tag/source identity mismatch" >&2
  exit 1
fi

expected_archives=(
  "bennu-v${version}-linux-x64.tar.gz"
  "bennu-v${version}-macos-arm64.tar.gz"
  "bennu-v${version}-windows-x64.zip"
)
for index in 0 1 2; do
  if [[ "${identity[$((index + 3))]}" != "${expected_archives[$index]}" ]]; then
    echo "error: manifest archive set is incomplete or unexpectedly named" >&2
    exit 1
  fi
  if [[ ! -f "$candidate_directory/${expected_archives[$index]}" ]]; then
    echo "error: candidate archive is missing: ${expected_archives[$index]}" >&2
    exit 1
  fi
done

expected_names=(
  "${expected_archives[0]}"
  "${expected_archives[1]}"
  "${expected_archives[2]}"
  release-manifest.json
)
release_files=(
  "$candidate_directory/${expected_names[0]}"
  "$candidate_directory/${expected_names[1]}"
  "$candidate_directory/${expected_names[2]}"
  "$candidate_directory/${expected_names[3]}"
)

published_release_endpoint="repos/${repository}/releases/tags/${release_tag}"
release_lookup=$(gh api "$published_release_endpoint" 2>&1) && {
  echo "error: refusing to overwrite existing published release $release_tag" >&2
  exit 1
}
if [[ "$release_lookup" != *"HTTP 404"* ]]; then
  echo "error: unable to prove published release $release_tag does not exist: $release_lookup" >&2
  exit 1
fi

tag_identity=$(gh api "repos/${repository}/git/ref/tags/${release_tag}" \
  --jq '.object.sha + " " + .object.type')
if [[ "$tag_identity" != *" tag" ]]; then
  echo "error: remote production tag is not annotated" >&2
  exit 1
fi
tag_object=${tag_identity% tag}
tag_target=$(gh api "repos/${repository}/git/tags/${tag_object}" \
  --jq '.object.sha + " " + .object.type')
if [[ "$tag_target" != "$source_commit commit" ]]; then
  echo "error: remote annotated tag does not resolve to the exact source commit" >&2
  exit 1
fi

for release_file in "${release_files[@]}"; do
  gh attestation verify "$release_file" \
    --repo "$repository" \
    --signer-workflow "$repository/.github/workflows/future-release.yml" \
    --source-digest "$source_commit" \
    --deny-self-hosted-runners
done

work_directory=$(mktemp -d "${RUNNER_TEMP:-/tmp}/bennu-future-publish.XXXXXX")
creation_fields_file="$work_directory/creation-fields.txt"
cleanup() {
  rm -rf -- "$work_directory"
}
trap cleanup EXIT

release_id=
release_api_endpoint=

remote_field() {
  local expression=$1
  gh api "$release_api_endpoint" --jq "$expression"
}

verify_remote_metadata() {
  local expected_draft=$1
  local observed_id

  observed_id=$(remote_field '.id')
  [[ "$observed_id" =~ ^[0-9]+$ ]] &&
    [[ "$observed_id" == "$release_id" ]] &&
    [[ "$(remote_field '.tag_name')" == "$release_tag" ]] &&
    [[ "$(remote_field '.target_commitish')" == "$source_commit" ]] &&
    [[ "$(remote_field '.draft')" == "$expected_draft" ]] &&
    [[ "$(remote_field '.prerelease')" == false ]]
}

verify_remote_assets() {
  local remote_names=()
  local name
  local asset_id
  local downloaded

  mapfile -t remote_names < <(
    gh api "$release_api_endpoint" --jq '.assets[].name' | LC_ALL=C sort
  )
  if [[ ${#remote_names[@]} -ne ${#expected_names[@]} ]]; then
    return 1
  fi
  for index in "${!expected_names[@]}"; do
    if [[ "${remote_names[$index]}" != "${expected_names[$index]}" ]]; then
      return 1
    fi
  done

  for name in "${expected_names[@]}"; do
    asset_id=$(gh api "$release_api_endpoint" \
      --jq ".assets[] | select(.name == \"$name\") | .id")
    if [[ ! "$asset_id" =~ ^[0-9]+$ ]]; then
      return 1
    fi
    downloaded="$work_directory/remote-$name"
    gh api -H 'Accept: application/octet-stream' \
      "repos/${repository}/releases/assets/${asset_id}" >"$downloaded"
    if ! cmp --silent "$candidate_directory/$name" "$downloaded"; then
      return 1
    fi
  done
}

gh api --method POST "repos/${repository}/releases" \
  -f tag_name="$release_tag" \
  -f target_commitish="$source_commit" \
  -f name="Bennu $version" \
  -f body='Target-native tested packages for Linux x64, Windows x64, and macOS arm64.' \
  -F draft=true \
  -F prerelease=false \
  --jq '[.id, (.id | type), .tag_name, .target_commitish, .draft, .prerelease][]' \
  >"$creation_fields_file"

mapfile -t creation_fields <"$creation_fields_file"
if [[ ${#creation_fields[@]} -ne 6 ]] || \
   [[ ! "${creation_fields[0]}" =~ ^[0-9]+$ ]] || \
   [[ "${creation_fields[1]}" != number ]] || \
   [[ "${creation_fields[2]}" != "$release_tag" ]] || \
   [[ "${creation_fields[3]}" != "$source_commit" ]] || \
   [[ "${creation_fields[4]}" != true ]] || \
   [[ "${creation_fields[5]}" != false ]]; then
  echo "error: created draft response metadata does not match; draft remains unpublished" >&2
  exit 1
fi

release_id=${creation_fields[0]}
release_api_endpoint="repos/${repository}/releases/${release_id}"
if ! verify_remote_metadata true; then
  echo "error: created draft metadata does not match; draft remains unpublished" >&2
  exit 1
fi

for name in "${expected_names[@]}"; do
  case "$name" in
    *.zip) content_type=application/zip ;;
    *.tar.gz) content_type=application/gzip ;;
    *.json) content_type=application/json ;;
    *) echo "error: unsupported release asset: $name" >&2; exit 1 ;;
  esac
  gh api --method POST \
    -H "Content-Type: $content_type" \
    "repos/${repository}/releases/${release_id}/assets?name=${name}" \
    --input "$candidate_directory/$name" >/dev/null
done

if ! verify_remote_metadata true || ! verify_remote_assets; then
  echo "error: uploaded draft metadata, assets, or bytes do not match; draft remains unpublished" >&2
  exit 1
fi

gh api --method PATCH "$release_api_endpoint" \
  -F draft=false \
  -F prerelease=false >/dev/null

release_api_endpoint=$published_release_endpoint
if ! verify_remote_metadata false || ! verify_remote_assets; then
  echo "error: published release verification failed" >&2
  exit 1
fi
echo "published verified non-prerelease $release_tag release"
