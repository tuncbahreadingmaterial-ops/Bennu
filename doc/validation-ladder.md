# Validation ladder

Bennu uses CTest labels to make validation cost explicit. The ladder reduces
duplicate complete-suite runs while preserving full Release, strict,
sanitizer, and isolated-QA evidence before handoff.

Focused and review validation are development feedback. They do not replace
the complete configurations required after a diff is stable.

## Labels

Every registered CTest has:

- `tier.full` and `tier.qa`;
- either `kind.unit` or `kind.contract`;
- one or more functional labels such as `area.cli`, `area.core`, `area.docs`,
  `area.public`, `area.release`, `area.spec`, or `area.workflow`; and
- `tier.strict` and `tier.sanitize` when the test is compatible with those
  configurations.

Architecture and tuple-specific contracts additionally use
`area.architecture` and `area.tuple`. Host-specific tests use
`scope.platform`.

The Linux dynamic-dependency and package probes require the ordinary binary's
exact dependency set. They use `capability.default_binary` instead of
`tier.strict` or `tier.sanitize`.

List the configured topology and labels with:

```sh
ctest --test-dir build --show-only=json-v1
```

## Checked-in entry points

`CMakePresets.json` owns the Release, strict, and sanitizer configuration flags.
Use the platform entry points below instead of copying flags or reconstructing
compiler environments in an ad hoc shell.

On Windows, run the complete configure/build/test flow from PowerShell:

```powershell
pwsh -File tools/validation/Invoke-BennuWindowsValidation.ps1 `
  -Preset windows-release -Tier full
pwsh -File tools/validation/Invoke-BennuWindowsValidation.ps1 `
  -Preset windows-strict -Tier strict
```

The script locates Visual Studio with `vswhere`, imports `VsDevCmd.bat` into the
current PowerShell environment, verifies the compiler/SDK/CMake/Ninja tools,
and launches CTest from PowerShell so Unicode and long-path tests do not pass
through a legacy command-prompt code page. Use `-Tier focused`, `review`, or
`qa` with `-Preset windows-release` when that ladder phase is required.

On WSL2 Ubuntu x86_64, install only the small bootstrap prerequisites once:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential git python3 ca-certificates curl tar
```

Then use the repository sanitizer entry point:

```sh
bash tools/validation/run-wsl-sanitize.sh
```

It does not use `sudo` or mutate WSL packages. The bootstrap downloads pinned
CMake 3.30.5 and Ninja 1.12.1 archives, verifies their checked-in SHA-256
values, and reuses them from `${XDG_CACHE_HOME:-$HOME/.cache}`. The sanitizer
build and logs also live in that persistent cache rather than `/tmp`.

For any long labeled selection, use the cross-platform logged runner:

```sh
python3 tools/validation/run_ctest.py \
  --ctest "$(command -v ctest)" \
  --build-dir build \
  --label '^tier[.]review$' \
  --log build/validation-logs/tier-review.log
```

The runner streams output, flushes a durable log, and enables CTest failover
mode. If an interrupted CTest leaves
`Testing/Temporary/CTestCheckpoint.txt`, rerun the same command to continue
from that checkpoint. Interruptions that occur before CTest records failover
state restart the selection while preserving the prior log.

## 1. Focused

Run this tier during implementation and fixes:

```sh
ctest --test-dir build -L "^tier[.]focused$" --output-on-failure
```

It is a small invariant baseline covering architecture, documentation,
positive specification traceability, and stdout behavior. Pair it with every
affected area. For example:

```sh
ctest --test-dir build -L "^area[.](public|tuple)$" --output-on-failure
ctest --test-dir build -L "^area[.]workflow$" --output-on-failure
```

The current doctest executable is one CTest, so use its case filter for focused
unit work:

```sh
./build/bennu_tests --test-case="TUP-*" --no-skip
```

On a multi-config Windows build, use the configuration-specific executable
path, such as `build\Release\bennu_tests.exe`.

## 2. Review

Before implementation handoff, run the review baseline and the affected-area
selection:

```sh
ctest --test-dir build -L "^tier[.]review$" --output-on-failure
ctest --test-dir build -L "^area[.]tuple$" --output-on-failure
```

The review baseline adds the complete unit executable, negative traceability,
and main/checkout workflow contracts. Reviewers may add tests for concrete
risks, but should not rerun an equivalent complete matrix without a reason.

## 3. Full

After review findings are resolved, build Release and run every registered
test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build -L "^tier[.]full$" --output-on-failure
```

An unfiltered CTest invocation remains equivalent and is used by Main CI:

```sh
ctest --test-dir build --output-on-failure
```

The corresponding checked-in Windows commands are:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release-full -F
```

Use `Invoke-BennuWindowsValidation.ps1` for normal agent work because it also
establishes and verifies the Visual Studio environment.

## 4. Strict

Use a separate build directory and the platform's strict warnings,
conversions, warnings-as-errors, and no-exceptions flags. On GNU-compatible
toolchains, the checked-in preset is authoritative:

```sh
cmake --preset gnu-strict
cmake --build --preset gnu-strict
ctest --preset gnu-strict -F
```

On Windows, the preset records the repository-supported MSVC warnings,
warnings-as-errors, conformance, no-exceptions, CRT, and name-hiding policy:

```powershell
pwsh -File tools/validation/Invoke-BennuWindowsValidation.ps1 `
  -Preset windows-strict -Tier strict
```

For a checkout mounted into WSL from Windows, use the WSL entry point:

```sh
bash tools/validation/run-wsl-strict.sh
```

It overrides the preset build directory into the persistent Linux cache.
Keeping the build and test fixtures off DrvFS is required for permission
contracts such as `cli.run_unreadable_file`.

The label excludes only tests whose contract requires the default binary's
exact dynamic-dependency set.

## 5. Sanitizer

Use a separate Debug build on a GNU- or Clang-compatible toolchain:

```sh
cmake --preset linux-sanitize
cmake --build --preset linux-sanitize
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
ctest --preset linux-sanitize -F
```

Platform sanitizer availability and environment syntax vary. Record the exact
compiler, flags, environment, exclusions, and results.

`unit.structural_host_allocation_refusal` deliberately constrains `RLIMIT_AS`
and is excluded from `tier.sanitize` because that contract conflicts with
ASan's address-space reservation. It remains required by ordinary Release,
`tier.qa`, `tier.review`, and `tier.strict`. The Linux dynamic-dependency and
package probes remain excluded from both instrumented configurations.

On WSL, prefer `tools/validation/run-wsl-sanitize.sh`; it applies the same preset
with persistent, pinned tools and the logged CTest runner.

## 6. Isolated QA

Final QA uses the isolated worktree and temporary integration branch required
by `AGENTS.md`. In that worktree:

1. merge the issue branch into the latest `main`;
2. run `tier.qa` in an ordinary Release build;
3. run the applicable strict and sanitizer tiers;
4. verify every acceptance criterion and platform-specific scenario; and
5. report exact commands, environment assumptions, and reproducible failures.

Run the complete QA selection with:

```sh
ctest --test-dir build-qa -L "^tier[.]qa$" --output-on-failure
```

On Windows, the isolated worktree should run:

```powershell
pwsh -File tools/validation/Invoke-BennuWindowsValidation.ps1 `
  -Preset windows-release -Tier qa
```

`tier.qa` contains every test registered on the current host. It is a label,
not permission to reuse a non-isolated development build as final QA evidence.
