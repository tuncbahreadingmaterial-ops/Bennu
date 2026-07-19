#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  printf 'usage: %s <verified-asset-directory> <expected-commit>\n' "$0" >&2
  exit 2
fi

asset_dir=$1
expected_commit=$2
tag=v0.1.0
repository=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}
published_release_endpoint="repos/${repository}/releases/tags/${tag}"
expected_names=(
  bennu-v0.1.0-linux-x64.tar.gz
  bennu-v0.1.0-macos-arm64.tar.gz
  bennu-v0.1.0-windows-x64.zip
)

if [[ ! -d "$asset_dir" ]]; then
  printf 'verified asset directory does not exist: %s\n' "$asset_dir" >&2
  exit 1
fi

mapfile -t actual_names < <(
  for path in "$asset_dir"/*; do
    [[ -f "$path" ]] && basename "$path"
  done | LC_ALL=C sort
)
if [[ ${#actual_names[@]} -ne ${#expected_names[@]} ]]; then
  printf 'local asset set has %d files; expected %d\n' "${#actual_names[@]}" "${#expected_names[@]}" >&2
  exit 1
fi
for index in "${!expected_names[@]}"; do
  if [[ "${actual_names[$index]}" != "${expected_names[$index]}" ]]; then
    printf 'local asset set mismatch: expected %s, observed %s\n' \
      "${expected_names[*]}" "${actual_names[*]}" >&2
    exit 1
  fi
done

resolved_tag=$(git rev-parse 'refs/tags/v0.1.0^{commit}')
if [[ "$resolved_tag" != "$expected_commit" ]]; then
  printf 'tag commit mismatch: expected %s, observed %s\n' "$expected_commit" "$resolved_tag" >&2
  exit 1
fi
git fetch --force --no-tags origin '+refs/heads/main:refs/remotes/origin/main'
if ! git merge-base --is-ancestor "$expected_commit" refs/remotes/origin/main; then
  printf 'v0.1.0 commit %s is not reachable from live origin/main\n' "$expected_commit" >&2
  exit 1
fi

work_dir=$(mktemp -d "${RUNNER_TEMP:-/tmp}/bennu-publish.XXXXXX")
release_json="$work_dir/release.json"
creation_fields_file="$work_dir/creation-fields.txt"
api_error="$work_dir/api-error.txt"
cleanup() {
  rm -rf "$work_dir"
}
trap cleanup EXIT

load_existing_release() {
  if gh api "$published_release_endpoint" >"$release_json" 2>"$api_error"; then
    return 0
  fi
  if grep -q 'HTTP 404' "$api_error"; then
    return 1
  fi
  printf 'failed to inspect existing release:\n' >&2
  command cat "$api_error" >&2
  exit 1
}

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
    [[ "$(remote_field '.tag_name')" == "$tag" ]] &&
    [[ "$(remote_field '.target_commitish')" == "$expected_commit" ]] &&
    [[ "$(remote_field '.draft')" == "$expected_draft" ]] &&
    [[ "$(remote_field '.prerelease')" == false ]]
}

verify_remote_assets() {
  local remote_names=()
  local name
  local asset_id
  local downloaded

  mapfile -t remote_names < <(gh api "$release_api_endpoint" --jq '.assets[].name' | LC_ALL=C sort)
  if [[ ${#remote_names[@]} -ne ${#expected_names[@]} ]]; then
    return 1
  fi
  for index in "${!expected_names[@]}"; do
    if [[ "${remote_names[$index]}" != "${expected_names[$index]}" ]]; then
      return 1
    fi
  done

  for name in "${expected_names[@]}"; do
    asset_id=$(gh api "$release_api_endpoint" --jq ".assets[] | select(.name == \"$name\") | .id")
    if [[ -z "$asset_id" ]]; then
      return 1
    fi
    downloaded="$work_dir/remote-$name"
    gh api -H 'Accept: application/octet-stream' \
      "repos/${repository}/releases/assets/${asset_id}" >"$downloaded"
    if ! cmp --silent "$asset_dir/$name" "$downloaded"; then
      return 1
    fi
  done
}

release_id=
release_api_endpoint=$published_release_endpoint
if load_existing_release; then
  release_id=$(remote_field '.id')
  if [[ ! "$release_id" =~ ^[0-9]+$ ]] || ! verify_remote_metadata false; then
    printf 'existing release metadata does not match the eligible v0.1.0 release\n' >&2
    exit 1
  fi
  if ! verify_remote_assets; then
    printf 'existing release assets do not exactly match the verified archives; refusing overwrite\n' >&2
    exit 1
  fi
  printf 'matching v0.1.0 release is already published; no changes made\n'
  exit 0
else
  gh api --method POST "repos/${repository}/releases" \
    -f tag_name="$tag" \
    -f target_commitish="$expected_commit" \
    -f name='Bennu v0.1.0' \
    -f body='Bennu Level 1: target-native tested packages for Linux x64, Windows x64, and macOS arm64.' \
    -F draft=true \
    -F prerelease=false \
    --jq '[.id, (.id | type), .tag_name, .target_commitish, .draft, .prerelease][]' >"$creation_fields_file"

  mapfile -t creation_fields <"$creation_fields_file"
  if [[ ${#creation_fields[@]} -ne 6 ]] || \
     [[ ! "${creation_fields[0]}" =~ ^[0-9]+$ ]] || \
     [[ "${creation_fields[1]}" != number ]] || \
     [[ "${creation_fields[2]}" != "$tag" ]] || \
     [[ "${creation_fields[3]}" != "$expected_commit" ]] || \
     [[ "${creation_fields[4]}" != true ]] || \
     [[ "${creation_fields[5]}" != false ]]; then
    printf 'created draft response metadata does not match the eligible v0.1.0 release\n' >&2
    exit 1
  fi
  release_id=${creation_fields[0]}
  release_api_endpoint="repos/${repository}/releases/${release_id}"
  if ! verify_remote_metadata true; then
    printf 'created draft metadata does not match the eligible v0.1.0 release; draft remains unpublished\n' >&2
    exit 1
  fi

  for name in "${expected_names[@]}"; do
    case "$name" in
      *.zip) content_type=application/zip ;;
      *.tar.gz) content_type=application/gzip ;;
      *) printf 'unsupported release asset: %s\n' "$name" >&2; exit 1 ;;
    esac
    gh api --method POST \
      -H "Content-Type: $content_type" \
      "repos/${repository}/releases/${release_id}/assets?name=${name}" \
      --input "$asset_dir/$name" >/dev/null
  done

  if ! verify_remote_metadata true || ! verify_remote_assets; then
    printf 'uploaded draft assets do not exactly match the verified archives; draft remains unpublished\n' >&2
    exit 1
  fi
fi

release_by_id_endpoint="repos/${repository}/releases/${release_id}"
gh api --method PATCH "$release_by_id_endpoint" -F draft=false -F prerelease=false >/dev/null
release_api_endpoint=$published_release_endpoint
if ! verify_remote_metadata false || ! verify_remote_assets; then
  printf 'published release verification failed\n' >&2
  exit 1
fi
printf 'published verified non-prerelease v0.1.0 release\n'
