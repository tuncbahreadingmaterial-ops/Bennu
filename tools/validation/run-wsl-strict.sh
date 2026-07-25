#!/usr/bin/env bash
set -euo pipefail

source_directory="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bash "${source_directory}/tools/validation/bootstrap-wsl-tools.sh"

cache_home="${XDG_CACHE_HOME:-${HOME}/.cache}"
tool_root="${BENNU_VALIDATION_TOOL_ROOT:-${cache_home}/bennu/validation-tools}"
source_key="$(
  printf "%s" "$source_directory" | sha256sum | cut -c 1-12
)"
build_root="${BENNU_VALIDATION_BUILD_ROOT:-${cache_home}/bennu/validation-builds}"
log_root="${BENNU_VALIDATION_LOG_ROOT:-${cache_home}/bennu/validation-logs}"
build_directory="${build_root}/${source_key}/gnu-strict"
log_path="${log_root}/${source_key}/tier-strict.log"

export PATH="${tool_root}/cmake-3.30.5-linux-x86_64/bin:${tool_root}:/usr/local/bin:/usr/bin:/bin"

for tool in gcc g++ python3 cmake ctest ninja; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required strict-validation tool is unavailable: $tool" >&2
    exit 2
  fi
done

cmake --preset gnu-strict \
  -S "$source_directory" \
  -B "$build_directory"
cmake --build "$build_directory"

python3 "${source_directory}/tools/validation/run_ctest.py" \
  --ctest "$(command -v ctest)" \
  --build-dir "$build_directory" \
  --label '^tier[.]strict$' \
  --log "$log_path"
