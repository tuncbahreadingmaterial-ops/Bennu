#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: verify-linux-package.sh <archive> <source> <license> <language-surface>" >&2
  exit 2
fi

script_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
archive=$(realpath "$1")
source_file=$(realpath "$2")
license=$(realpath "$3")
language_surface=$4

case "$language_surface" in
  rewrite)
    expected_output=$'6\n(8 -2 12 1)\n3.5\n(false true false true)\n(true false)\n(1 2 3 4 5)\n'
    generated_name=rewrite.c
    invalid_source=$'iota[5]\nadd[1 true]\n'
    expected_error=TypeError
    ;;
  v0.1.0)
    expected_output=$'>>(1 2 3 4 5)\n>>6\n'
    generated_name=v0.1.0-level1.c
    historical_first_line=$(sed -n '1p' "$source_file")
    invalid_source=$(printf '%s\ninc %s\n' "$historical_first_line" "$historical_first_line")
    expected_error='type mismatch'
    ;;
  *)
    echo "error: unsupported language surface: $language_surface" >&2
    exit 2
    ;;
esac

for required_file in "$archive" "$source_file" "$license"; do
  if [[ ! -f "$required_file" ]]; then
    echo "error: Linux package verification input is missing: $required_file" >&2
    exit 1
  fi
done

work_root=$(mktemp -d)
trap 'rm -rf "$work_root"' EXIT
extracted="$work_root/extracted"
mkdir -p "$extracted"

actual_entries=$(tar -tzf "$archive" | LC_ALL=C sort)
expected_entries=$'LICENSE\nbennu'
if [[ "$actual_entries" != "$expected_entries" ]]; then
  echo "error: Linux archive layout mismatch: $actual_entries" >&2
  exit 1
fi

tar -xzf "$archive" -C "$extracted"
extracted_entries=$(find "$extracted" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
if [[ "$extracted_entries" != "$expected_entries" ]]; then
  echo "error: extracted Linux archive layout mismatch: $extracted_entries" >&2
  exit 1
fi
if [[ $(stat -c '%a' "$extracted/bennu") != "755" || ! -x "$extracted/bennu" ]]; then
  echo "error: extracted Bennu does not have executable mode 0755" >&2
  exit 1
fi
if [[ $(stat -c '%a' "$extracted/LICENSE") != "644" ]]; then
  echo "error: extracted LICENSE does not have mode 0644" >&2
  exit 1
fi
cmp --silent "$license" "$extracted/LICENSE"

bennu="$extracted/bennu"
python3 "$script_root/verify-linux-elf.py" "$bennu"

expected="$work_root/expected.out"
printf '%s' "$expected_output" >"$expected"
stdout="$work_root/stdout"
stderr="$work_root/stderr"

if ! "$bennu" --help >"$stdout" 2>"$stderr"; then
  echo "error: extracted Bennu --help failed" >&2
  exit 1
fi
if [[ -s "$stderr" ]] || ! grep -Fq 'Usage: bennu <command> [arguments]' "$stdout"; then
  echo "error: extracted Bennu --help output mismatch" >&2
  exit 1
fi

if ! "$bennu" run "$source_file" >"$stdout" 2>"$stderr"; then
  echo "error: extracted Bennu run failed" >&2
  exit 1
fi
if [[ -s "$stderr" ]] || ! cmp --silent "$expected" "$stdout"; then
  echo "error: extracted Bennu run output mismatch" >&2
  exit 1
fi

generated_c="$work_root/$generated_name"
generated="$work_root/emitted"
if ! "$bennu" emit-c "$source_file" -o "$generated_c" >"$stdout" 2>"$stderr"; then
  echo "error: extracted Bennu emit-c failed" >&2
  exit 1
fi
if [[ -s "$stdout" || -s "$stderr" ]]; then
  echo "error: extracted Bennu emit-c produced unexpected output" >&2
  exit 1
fi
cc -std=c11 -pedantic-errors -Wall -Wextra -Werror "$generated_c" -o "$generated"
if ! "$generated" >"$stdout" 2>"$stderr"; then
  echo "error: emitted C executable failed" >&2
  exit 1
fi
if [[ -s "$stderr" ]] || ! cmp --silent "$expected" "$stdout"; then
  echo "error: emitted C executable output mismatch" >&2
  exit 1
fi

native="$work_root/native"
if ! "$bennu" build "$source_file" -o "$native" --cc cc >"$stdout" 2>"$stderr"; then
  echo "error: extracted Bennu build with external C compiler failed" >&2
  exit 1
fi
if [[ -s "$stdout" || -s "$stderr" ]]; then
  echo "error: extracted Bennu build produced unexpected output" >&2
  exit 1
fi
if ! "$native" >"$stdout" 2>"$stderr"; then
  echo "error: native executable failed" >&2
  exit 1
fi
if [[ -s "$stderr" ]] || ! cmp --silent "$expected" "$stdout"; then
  echo "error: native executable output mismatch" >&2
  exit 1
fi

invalid="$work_root/invalid.bennu"
printf '%s' "$invalid_source" >"$invalid"
if "$bennu" run "$invalid" >"$stdout" 2>"$stderr"; then
  echo "error: invalid source unexpectedly ran successfully" >&2
  exit 1
fi
if [[ -s "$stdout" ]] || ! grep -Fq "$expected_error" "$stderr"; then
  echo "error: invalid run did not preserve atomic result output" >&2
  exit 1
fi

sentinel="$work_root/sentinel"
printf 'preserve-existing-output\n' >"$sentinel"
invalid_c="$work_root/invalid.c"
cp "$sentinel" "$invalid_c"
if "$bennu" emit-c "$invalid" -o "$invalid_c" >"$stdout" 2>"$stderr"; then
  echo "error: invalid emit-c unexpectedly succeeded" >&2
  exit 1
fi
if [[ -s "$stdout" ]] || ! cmp --silent "$sentinel" "$invalid_c"; then
  echo "error: invalid emit-c replaced its existing output" >&2
  exit 1
fi

invalid_native="$work_root/invalid-native"
cp "$sentinel" "$invalid_native"
chmod 755 "$invalid_native"
if "$bennu" build "$invalid" -o "$invalid_native" --cc cc >"$stdout" 2>"$stderr"; then
  echo "error: invalid build unexpectedly succeeded" >&2
  exit 1
fi
if [[ -s "$stdout" ]] || ! cmp --silent "$sentinel" "$invalid_native"; then
  echo "error: invalid build replaced its existing output" >&2
  exit 1
fi

echo "Linux package journeys passed: --help, run, emit-c, build, and atomic failures"
