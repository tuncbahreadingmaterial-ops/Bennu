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

## 4. Strict

Use a separate build directory and the platform's strict warnings,
conversions, warnings-as-errors, and no-exceptions flags. On GNU-compatible
toolchains:

```sh
cmake -S . -B build-strict -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror -fno-exceptions" \
  -DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
cmake --build build-strict
ctest --test-dir build-strict -L "^tier[.]strict$" --output-on-failure
```

The label excludes only tests whose contract requires the default binary's
exact dynamic-dependency set.

## 5. Sanitizer

Use a separate Debug build on a GNU- or Clang-compatible toolchain:

```sh
cmake -S . -B build-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-exceptions" \
  -DCMAKE_C_FLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-sanitize
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
ctest --test-dir build-sanitize -L "^tier[.]sanitize$" --output-on-failure
```

Platform sanitizer availability and environment syntax vary. Record the exact
compiler, flags, environment, exclusions, and results.

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

`tier.qa` contains every test registered on the current host. It is a label,
not permission to reuse a non-isolated development build as final QA evidence.
