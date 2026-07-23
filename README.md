# Bennu

Bennu is a data-oriented language with scalar primitive lifting over rank-0
scalars and homogeneous rank-1 vectors. The public evaluator, REPL, file runner,
C11 emitter, and native builder all use the same rewrite grammar and typed
semantics.

The initial public primitives are exactly:

- `inc`: increment an `Int` or `Double` scalar or vector;
- `add`: add numeric arguments element by element;
- `equals`: compare `Bool`, `Int`, or `Double` arguments element by element;
- `not`: negate `Bool` arguments element by element; and
- `iota`: construct the `Int` vector `1` through `n`, or an empty `Int` vector
  when `n <= 0`.

General calls use brackets, such as `add[1 2.5]` and `inc[iota[3]]`. Unary
calls also support right-associative prefix syntax: `inc 5` and `inc inc 5`.
Scalars are `Bool`, signed 64-bit `Int`, or IEEE 754 binary64 `Double` values.
Vector literals are homogeneous, for example `(1 2 3)`, `(1.0 2.5)`, and
`(false true)`. Typed empty vectors are `Bool()`, `Int()`, and `Double()`.

Elementwise calls broadcast scalars over vectors and require all vector
arguments to have equal lengths. A singleton vector remains a vector and does
not broadcast. Exact overloads win; the only implicit conversion is
`Int -> Double`. Integer overflow is a structured domain error. Output is
canonical, including `true`/`false`, visible `.0` for integral-valued Doubles,
`-0.0`, `inf`, `-inf`, `nan`, and parenthesized vectors. Every complete program
is evaluated before runner output is published.

The CLI defaults to the `trusted-local-v1` execution profile with
`max_vector_bytes`, `max_live_evaluation_bytes`, and `max_work_units` omitted.
Mandatory representability, overflow-safe sizing, complete-allocation, and
all-or-nothing result checks still apply.

## Build and quick start

Building requires a C++20 compiler, CMake 3.20 or newer, and Ninja. Tests and
generated-program journeys also require a C11 compiler.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, use `build\bennu.exe`; native output names normally use `.exe`.

Start the REPL:

```sh
./build/bennu repl
```

```text
> add[1 2.5]
3.5
> inc iota 3
(2 3 4)
```

Run the canonical example:

```sh
./build/bennu run examples/rewrite.bennu
```

It prints:

```text
6
(8 -2 12 1)
3.5
(false true false true)
(true false)
(1 2 3 4 5)
```

Programs may declare ordered scalar parameters in a leading header and receive
their values from the runner after an explicit `--` boundary:

```bennu
parameters[count Int scale Double enabled Bool]
count
scale
enabled
```

```sh
./build/bennu run example.bennu -- -5 2.5 true
```

Runner arguments use Bennu's exact ASCII scalar spellings. `Bool` accepts only
`true` or `false`. `Int` accepts canonical signed decimal integers without `+`,
leading zeroes, or `-0`. `Double` accepts canonical decimal/exponent forms that
contain a decimal point or exponent, plus exactly `inf`, `-inf`, and `nan`;
finite overflow is rejected and finite underflow becomes signed zero. Conversion
is whole-token and locale-independent. A program with no parameters may be run
with no boundary or with a bare `--`; every token after the first boundary is
program data, including negative values and a second `--`.

Argument failures write one stable `bennu_argument_error` record to stderr. The
record contains the reason, required and supplied counts, one-based position,
declared parameter identity/type/span when applicable, and no raw argument text.
All arguments are decoded only after static source validation and before any
evaluation. The runner publishes the complete output batch only after decoding,
evaluation, and formatting all succeed, so failures leave stdout empty.
Formatting and stdout-device failures use the stable `bennu_formatting_error`
and `bennu_output_error` records defined by the language specification.

Emit deterministic, self-contained standard C11 and run it:

```sh
./build/bennu emit-c examples/rewrite.bennu -o rewrite.c
cc -std=c11 rewrite.c -o rewrite
./rewrite
```

Or build the native executable directly:

```sh
./build/bennu build examples/rewrite.bennu -o rewrite
./rewrite
```

Use `--cc <compiler>` after the output path, or set `CC`, to select a C
compiler. Otherwise Bennu searches for `cc` on Linux/macOS or `cl.exe` on
Windows. Compiler values are executable names or paths, not shell fragments or
flag strings. A selected compiler failure is final. Source validation,
generated-source publication, compilation, cleanup, and native replacement are
publish-last and preserve an existing destination on failure.

## Deliberate differences from Anka

Anka is language-design inspiration, not a compatibility target. Bennu keeps
readable bracket calls, parenthesized vector literals, and concise unary prefix
calls. Bennu deliberately uses `iota` as its only sequence-constructor spelling,
requires brackets for multi-argument calls, reserves parentheses for homogeneous
rank-1 literals, and requires an explicit type for empty vectors. The initial
language has no variables, user functions, trains, effects, reductions,
multidimensional arrays, `length`, or `divide`.

## Version and release provenance

The tracked root `VERSION` file is the sole product-version input. The current
source is the stable v0.2.0 release candidate and identifies itself as `0.2.0`;
`bennu --version` prints exactly `bennu 0.2.0` followed by one newline. CMake
rejects non-canonical or non-SemVer VERSION bytes and generates the private C++
version header, build identity, and Windows PE version resource from that value.
VERSION does not authorize a release or publication. At this stage, no v0.2.0
tag or release has been created.

The future-release workflow supports an unprivileged three-target dry run at an
explicit source ref. Production additionally requires all of the following:

- a deliberately selected stable VERSION without prerelease/build metadata;
- the exact annotated tag `v<VERSION>`, resolving to the exact source commit;
- a complete Linux x64, Windows x64, and macOS arm64 native test/package run;
- an absent release for that tag—the publisher refuses overwrite or clobber;
- GitHub artifact attestations for every exact archive and the final manifest;
  and
- draft creation, exact remote-byte download/comparison, and publication as the
  final mutation. Published releases and their tag/asset set are treated as
  immutable and are never republished by this flow.

The publisher addresses every draft operation through the GitHub REST API using
the numeric release ID returned by creation. It uploads exactly the three
archives and manifest, re-fetches the draft by that ID, resolves each asset ID,
and compares every downloaded byte before the final publication PATCH. A
failure before that PATCH leaves the draft unpublished for inspection; it is
never deleted or repaired by overwriting assets. After publication, all checks
are read-only.

Every target creates canonical, compact, newline-terminated JSON without a
timestamp or host path. `tools/release/provenance.py merge` validates the
target-native fragments against archive bytes and creates
`release-manifest.json`; `tools/release/provenance.py verify` independently
extracts and hashes the bytes instead of trusting fragment claims. Schema 1 has
`schema_version`, `version`, `source_commit`, `tag`, `trust_policy`, and sorted
`artifacts`. Each artifact records its target and fragment digest, archive name
and archive SHA-256, plus the contained executable path, version, and executable
SHA-256. Development manifests use a null tag. Production mismatch, duplicate,
unsafe-path, missing/extra-executable, or tamper cases fail closed.

In short, each record binds one archive SHA-256 and one executable SHA-256.

The enforced trust mechanism is `github-artifact-attestation`, using Sigstore
with GitHub OIDC and no repository secret. Verification binds the repository,
the `.github/workflows/future-release.yml` signer workflow, and the exact source
commit. This is not Authenticode or Apple Developer ID/notarization. In
particular, Issue #12 is **CLOSED NOT_PLANNED by the owner**: macOS Developer ID,
notarization, Gatekeeper verification, and a deployment floor were canceled and
do not exist. Windows PE identity metadata exists, but Windows Authenticode does
not. The same GitHub attestation policy therefore protects Windows and macOS
archives without invented signing credentials.

### Verify downloads without a checkout

Set `repository`, `tag`, `version`, and the exact 40-hex `commit` from the
release page. On Linux x64:

```sh
repository=OWNER/REPOSITORY tag=vX.Y.Z version=X.Y.Z commit=FULL_COMMIT
archive="bennu-v${version}-linux-x64.tar.gz"
gh release download "$tag" -R "$repository" -p "$archive" -p release-manifest.json
for file in "$archive" release-manifest.json; do
  gh attestation verify "$file" --repo "$repository" \
    --signer-workflow "$repository/.github/workflows/future-release.yml" \
    --source-digest "$commit" --deny-self-hosted-runners
done
python3 -c 'import hashlib,json,sys; m=json.load(open("release-manifest.json")); a=next(x for x in m["artifacts"] if x["target"]=="linux-x64"); assert m["source_commit"]==sys.argv[2] and hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest()==a["archive"]["sha256"]' "$archive" "$commit"
mkdir verified-linux && tar -xzf "$archive" -C verified-linux
test "$(sha256sum verified-linux/bennu | cut -d' ' -f1)" = "$(python3 -c 'import json; m=json.load(open("release-manifest.json")); print(next(x for x in m["artifacts"] if x["target"]=="linux-x64")["executable"]["sha256"])')"
test "$(verified-linux/bennu --version)" = "bennu $version"
```

On macOS arm64, use the same download and `gh attestation verify` loop with
`archive="bennu-v${version}-macos-arm64.tar.gz"`, then:

```sh
python3 -c 'import hashlib,json,sys; m=json.load(open("release-manifest.json")); a=next(x for x in m["artifacts"] if x["target"]=="macos-arm64"); assert m["source_commit"]==sys.argv[2] and hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest()==a["archive"]["sha256"]' "$archive" "$commit"
mkdir verified-macos && tar -xzf "$archive" -C verified-macos
test "$(shasum -a 256 verified-macos/bennu | awk '{print $1}')" = "$(python3 -c 'import json; m=json.load(open("release-manifest.json")); print(next(x for x in m["artifacts"] if x["target"]=="macos-arm64")["executable"]["sha256"])')"
test "$(verified-macos/bennu --version)" = "bennu $version"
```

On Windows x64 PowerShell:

```powershell
$repository="OWNER/REPOSITORY"; $tag="vX.Y.Z"; $version="X.Y.Z"; $commit="FULL_COMMIT"
$archive="bennu-v$version-windows-x64.zip"
gh release download $tag -R $repository -p $archive -p release-manifest.json
foreach ($file in @($archive, "release-manifest.json")) {
  gh attestation verify $file --repo $repository `
    --signer-workflow "$repository/.github/workflows/future-release.yml" `
    --source-digest $commit --deny-self-hosted-runners
  if ($LASTEXITCODE -ne 0) { throw "attestation failed: $file" }
}
$manifest=Get-Content release-manifest.json -Raw | ConvertFrom-Json
$artifact=$manifest.artifacts | Where-Object target -CEQ windows-x64
if ($manifest.source_commit -cne $commit -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $artifact.archive.sha256) { throw "archive provenance mismatch" }
Expand-Archive $archive verified-windows
$exe="verified-windows\bennu.exe"
if ((Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant() -cne $artifact.executable.sha256) { throw "executable digest mismatch" }
if ((& $exe --version) -cne "bennu $version") { throw "version mismatch" }
$info=(Get-Item $exe).VersionInfo
if ($info.ProductName -cne "Bennu" -or $info.ProductVersion -cne $version -or $info.FileVersion -cne $version -or $info.OriginalFilename -cne "bennu.exe") { throw "PE identity mismatch" }
```

## Platforms and historical release

The supported CI targets are Ubuntu 24.04 LTS x64, Windows 11 x64 or newer, and
macOS arm64. The Linux package compatibility floor is glibc 2.39 or newer on a
supported Linux 6.8 GA kernel (or supported Ubuntu HWE kernel), with ELF symbol
ceilings `GLIBC_2.34` and `GLIBCXX_3.4.32`; the executable dynamically resolves
`libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6`, and `libm.so.6`. Windows users
install the Microsoft Visual C++ 2017-2026 Redistributable (x64). On supported
Windows 11 systems with long paths enabled, `run`, `emit-c`, and `build` accept
ordinary paths beyond 260 characters; callers do not add a `\\?\` prefix. On
every platform, only `bennu build` requires an external C11 compiler.

The published `v0.1.0` archives, lightweight tag at commit
`0f31967e0a70b424f4201133a54ae7cd8aa5d659`, release metadata, historical
workflow, and three asset names/bytes are immutable historical artifacts.
v0.1.0 predates this contract, does not support the new version/provenance
contract, and is explicitly rejected by the generic future-release flow. It
remains available exactly as published; this document describes the current
source tree rather than retroactively changing or republishing that release.
