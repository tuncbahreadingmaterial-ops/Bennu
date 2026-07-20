# Issue #33 C-backend strategy prototype

This directory is reproducible evidence tooling for Issue #33 and WP8. It is
not a production backend. The executable is excluded from CMake's default
build, includes `src/rewrite.cpp` only in its benchmark translation unit to
reach the accepted internal evaluator, and has no route from the `bennu` CLI.

## Clean Release commands

Configure an isolated Release build with the opt-in switch, then run the fast
equivalence/failure smoke check:

```sh
cmake -S . -B build/issue33 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBENNU_BUILD_ISSUE33_BENCHMARK=ON
cmake --build build/issue33 --target issue33_verify -j2
```

Run the repeated measurement separately:

```sh
cmake --build build/issue33 --target issue33_measure -j2
```

The measurement target writes untracked evidence beneath
`build/issue33/issue33-evidence/`. It performs one discarded full warmup per
candidate/workload pair, then records five samples and writes `raw.csv`,
`summary.csv`, `metadata.json`, and `complexity.csv`. `summary.csv` is the
median of every available numeric metric; no trimmed or selected samples are
used. Change the sample count only by invoking `benchmark.py measure` directly
with the same arguments shown by the generated CMake target and a different
`--samples` value of at least two.

The configured C compiler is used for every candidate with exactly:

```text
-std=c11 -O2 -Wall -Wextra -Werror -pedantic-errors
```

The opt-in targets require a Unix host and a GNU- or Clang-compatible compiler
driver; unsupported hosts fail during opt-in configuration while ordinary
default builds remain unaffected. The driver refuses to record a non-Release
build. It uses Python's monotonic nanosecond timer around processes. On Linux
with `/usr/bin/time -q`, maximum resident memory is captured consistently as KiB
for emission, C compilation, and execution. On other supported Unix hosts the
three peak-memory fields are empty and `metadata.json` records that memory
measurement is unsupported. The smoke verifier itself remains independent of
the Linux memory facility; on hosts with `/dev/full` it also proves that a
generated stdout failure returns nonzero.

For the required no-regression check, use a separate configuration without the
opt-in switch:

```sh
cmake -S . -B build/release-default -DCMAKE_BUILD_TYPE=Release
cmake --build build/release-default -j2
ctest --test-dir build/release-default --output-on-failure
git diff --check
```

## Candidates and exact hybrid boundary

- `constants` embeds the canonical result bytes produced only after the
  internal rewrite evaluator has accepted and formatted the complete program.
  Large output is split into strictly portable C string objects of at most
  4,000 bytes.
- `flat` is a fixed-corpus typed flat-IR/runtime-loop prototype. It emits typed
  `int64_t` payloads for arbitrary vector input and typed loops for `inc` and
  nested `iota` lifting, plus checked C-side formatting/output helpers. It does
  not infer payload values from a vector count. Scalar and typed-empty roots
  take the corresponding fixed-corpus literal path.
- `hybrid` uses constants when the sum of materialized vector elements across
  all successful roots is **less than or equal to 1,000**, and uses the flat
  runtime loop when it is greater than 1,000. Scalars contribute zero vector
  elements. This is the one and only threshold.

Every candidate first runs the same complete internal rewrite evaluation.
Parse, resolution, primitive, profile/resource, or formatting failure returns
before a C-source string exists. Generated C is held in memory and written
only after successful emission. These prototypes deliberately implement only
the corpus below; there is no generic or public backend surface.

## Workload corpus

| Identity | Size | Rewrite source | Purpose |
| --- | ---: | --- | --- |
| `scalar_bool` | 1 | `true` | Bool scalar root |
| `scalar_int` | 1 | `-9223372036854775808` | Int scalar root |
| `scalar_double` | 1 | `-0.0` | Double scalar root/canonical signed zero |
| `empty_bool` | 0 | `Bool()` | typed empty Bool vector |
| `empty_int` | 0 | `Int()` | typed empty Int vector |
| `empty_double` | 0 | `Double()` | typed empty Double vector |
| `arbitrary_lifted` | 4 | `inc[(7 -3 11 0)]` | arbitrary actual payload, not count-derived |
| `nested_iota_1` | 1 | `inc[inc[iota[1]]]` | nested iota/lifting, singleton |
| `nested_iota_1000` | 1,000 | `inc[inc[iota[1000]]]` | nested iota/lifting, threshold boundary |
| `nested_iota_100000` | 100,000 | `inc[inc[iota[100000]]]` | safe larger nested workload |
| `invalid_source` | 0 | `add[1, 2]` | deterministic parse rejection |
| `resource_refusal` | 1,000 | `inc[iota[1000]]` | `bounded-v1`, `max_vector_bytes=64` refusal |

The driver obtains oracle bytes from the evaluator separately. It compiles and
runs a candidate only after successful emission, and accepts a measurement row
only after the executable's stdout is byte-for-byte identical to those oracle
bytes. Invalid and resource cases must return nonzero with empty stdout from
both oracle and candidate emission commands; their compile/execution fields
remain empty.

## Evidence schemas

Each `raw.csv` row records revision and dirty state; build type and CMake
version; host, OS, and CPU; configured C and C++ compiler paths, identities,
versions, and flags;
workload identity, size, expected outcome, strategy, and one-based sample
index; equivalence and failure-atomicity status; emitted C bytes and emission
nanoseconds; C compile nanoseconds; executable bytes; execution nanoseconds;
and emission/compile/execution peak KiB where supported. Rejection rows retain
emission cost and zero source bytes but leave inapplicable compile/execution
fields empty.

The exact `raw.csv` column order is:

```text
revision,dirty,build_type,cmake_version,host,os,cpu,
c_compiler,c_compiler_id,c_compiler_version,c_flags,
cxx_compiler,cxx_compiler_id,cxx_compiler_version,cxx_flags,
workload_id,workload_size,expected_outcome,strategy,sample_index,
equivalence_status,failure_atomic,emitted_source_bytes,emission_time_ns,
c_compile_time_ns,executable_bytes,execution_time_ns,
emission_peak_kib,compile_peak_kib,execution_peak_kib
```

Emission time is wall time for the complete prototype `emit` process: required
evaluator validation and canonical formatting, candidate source construction,
and source transfer to the driver. Compile and execution time each surround
only their corresponding child process.

`metadata.json` uses the run-level provenance keys from the first 15 raw
columns, plus `peak_memory_method`, `aggregation`, `warmup`,
`hybrid_threshold`, and `samples`. `summary.csv` groups by workload and
strategy and contains `samples`, equivalence status, and one `median_...`
column for each of the eight raw cost metrics.

`complexity.csv` is regenerated from the plainly marked
`ISSUE33_COMPLEXITY_BEGIN/END` regions in `prototype.cpp`. It reports inclusive
source locations, physical lines, nonblank/non-comment substantive lines, and
an explicit list of duplicated semantic responsibilities for shared
validation, constants, flat runtime, and hybrid selection. The regions include
candidate-owned helpers and emitted runtime source; command parsing, manifest
serialization, and generic process glue are intentionally outside them and
remain inspectable in the same file. These counts are implementation-surface
evidence, not a quality or performance score.

The reviewed clean-Release run and selected flat-lowering decision are retained
under `doc/evidence/issue-33/` and in `doc/decision-diary.md`. Regeneration
writes only beneath the build tree; replacing durable evidence requires a new
reviewed run rather than silently overwriting the committed files.
