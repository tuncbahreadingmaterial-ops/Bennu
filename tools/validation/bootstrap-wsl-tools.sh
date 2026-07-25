#!/usr/bin/env bash
set -euo pipefail

cmake_version="3.30.5"
cmake_archive="cmake-${cmake_version}-linux-x86_64.tar.gz"
cmake_sha256="f747d9b23e1a252a8beafb4ed2bc2ddf78cff7f04a8e4de19f4ff88e9b51dc9d"
ninja_version="1.12.1"
ninja_archive="ninja-linux.zip"
ninja_sha256="6f98805688d19672bd699fbbfa2c2cf0fc054ac3df1f0e6a47664d963d530255"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "error: the pinned WSL validation tools require Linux x86_64" >&2
  exit 2
fi

for tool in curl tar python3 sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required bootstrap tool is unavailable: $tool" >&2
    exit 2
  fi
done

cache_home="${XDG_CACHE_HOME:-${HOME}/.cache}"
tool_root="${BENNU_VALIDATION_TOOL_ROOT:-${cache_home}/bennu/validation-tools}"
mkdir -p "$tool_root"

download_verified() {
  local url="$1"
  local output="$2"
  local expected_sha256="$3"

  if [[ -f "$output" ]] &&
     printf "%s  %s\n" "$expected_sha256" "$output" | sha256sum --check --status; then
    return
  fi

  local temporary="${output}.download"
  rm -f "$temporary"
  curl --fail --location --retry 3 --output "$temporary" "$url"
  printf "%s  %s\n" "$expected_sha256" "$temporary" |
    sha256sum --check --status
  mv -f "$temporary" "$output"
}

cmake_path="${tool_root}/cmake-${cmake_version}-linux-x86_64/bin/cmake"
if [[ ! -x "$cmake_path" ]]; then
  download_verified \
    "https://github.com/Kitware/CMake/releases/download/v${cmake_version}/${cmake_archive}" \
    "${tool_root}/${cmake_archive}" \
    "$cmake_sha256"
  tar -xzf "${tool_root}/${cmake_archive}" -C "$tool_root"
fi

ninja_path="${tool_root}/ninja"
if [[ ! -x "$ninja_path" ]]; then
  download_verified \
    "https://github.com/ninja-build/ninja/releases/download/v${ninja_version}/${ninja_archive}" \
    "${tool_root}/${ninja_archive}" \
    "$ninja_sha256"
  python3 -c \
    'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "${tool_root}/${ninja_archive}" \
    "$tool_root"
  chmod +x "$ninja_path"
fi

"$cmake_path" --version | head -n 1
"$ninja_path" --version
printf "BENNU_VALIDATION_TOOL_ROOT=%s\n" "$tool_root"
