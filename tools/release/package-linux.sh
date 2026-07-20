#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: package-linux.sh <bennu> <license> <archive>" >&2
  exit 2
fi

script_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
bennu=$(realpath "$1")
license=$(realpath "$2")
archive_parent=$(realpath "$(dirname "$3")")
archive_name=$(basename "$3")
archive="${archive_parent}/${archive_name}"

if [[ ! -f "$bennu" || ! -x "$bennu" ]]; then
  echo "error: Linux package input is not an executable file: $bennu" >&2
  exit 1
fi
if [[ ! -f "$license" ]]; then
  echo "error: Linux package license is missing: $license" >&2
  exit 1
fi
if [[ -e "$archive" ]]; then
  echo "error: refusing to replace existing Linux archive: $archive" >&2
  exit 1
fi

work_root=$(mktemp -d)
archive_temp=$(mktemp --tmpdir="$archive_parent" ".${archive_name}.tmp.XXXXXX")
cleanup() {
  rm -f -- "$archive_temp"
  rm -rf -- "$work_root"
}
trap cleanup EXIT
stage="$work_root/stage"
extracted="$work_root/extracted"
mkdir -p "$stage" "$extracted"

cp -- "$bennu" "$stage/bennu"
cp -- "$license" "$stage/LICENSE"
chmod 755 "$stage/bennu"
chmod 644 "$stage/LICENSE"

python3 "$script_root/verify-linux-elf.py" "$stage/bennu"
tar -czf "$archive_temp" -C "$stage" bennu LICENSE

actual_entries=$(tar -tzf "$archive_temp" | LC_ALL=C sort)
expected_entries=$'LICENSE\nbennu'
if [[ "$actual_entries" != "$expected_entries" ]]; then
  echo "error: Linux archive layout mismatch: $actual_entries" >&2
  exit 1
fi

tar -xzf "$archive_temp" -C "$extracted"
extracted_entries=$(find "$extracted" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
if [[ "$extracted_entries" != "$expected_entries" ]]; then
  echo "error: extracted Linux archive layout mismatch: $extracted_entries" >&2
  exit 1
fi
if [[ $(stat -c '%a' "$extracted/bennu") != "755" ]]; then
  echo "error: Linux archive did not preserve executable mode 0755" >&2
  exit 1
fi
if [[ $(stat -c '%a' "$extracted/LICENSE") != "644" ]]; then
  echo "error: Linux archive did not preserve LICENSE mode 0644" >&2
  exit 1
fi
cmp --silent "$license" "$extracted/LICENSE"
python3 "$script_root/verify-linux-elf.py" "$extracted/bennu"

archive_hash=$(sha256sum "$archive_temp" | cut -d' ' -f1)
if ! ln -- "$archive_temp" "$archive"; then
  echo "error: unable to publish verified Linux archive atomically: $archive" >&2
  exit 1
fi
rm -f -- "$archive_temp"
echo "Verified Linux archive: $archive"
echo "SHA-256: $archive_hash"
