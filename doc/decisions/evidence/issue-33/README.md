# Issue #33 C-backend strategy evidence

This directory records the clean-Release measurement used to select Bennu's
rewrite C-backend strategy. It is evidence for Issue #33, not a production
backend or a cross-machine performance claim.

## Provenance and method

- Measured revision: `0d5b1e68e3694ebec38f8fc25835a0518942489d`
- Working tree at measurement start: clean (`dirty=false`)
- Host: `NucBoxM6Ultra`, Linux 6.18.33.2 under WSL2, x86_64
- CPU: AMD Ryzen 5 7640HS, 6 cores / 12 logical CPUs
- Build: CMake 4.4.0, Release, GNU C++ 15.2.0 (`-O3 -DNDEBUG`)
- Generated C: GNU C 15.2.0, C11, `-O2 -Wall -Wextra -Werror -pedantic-errors`
- Samples: one discarded warmup followed by five recorded samples for every
  workload/strategy pair
- Aggregation: independent median of every numeric metric; no sample was
  trimmed or selected
- Peak memory: GNU `/usr/bin/time -q -f %M`, maximum resident KiB

The exact environment strings and flags are in `metadata.json`. The exact
command from a clean checkout is:

```sh
cmake -S . -B build/issue33 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBENNU_BUILD_ISSUE33_BENCHMARK=ON
cmake --build build/issue33 --target issue33_verify -j2
cmake --build build/issue33 --target issue33_measure -j2
```

The corpus and candidate definitions are documented in
`tools/issue33/README.md`. The run covered 12 workloads and all three
strategies. Every one of the 30 successful candidate/workload combinations
compiled as strict C11 and produced bytes identical to the internal rewrite
evaluator. All six invalid-source/resource-refusal combinations rejected
before emitting C and produced zero partial source bytes.

## Durable artifacts

- `raw.csv`: all 180 recorded rows (12 workloads x 3 strategies x 5 samples)
- `summary.csv`: medians grouped by workload and strategy
- `metadata.json`: run-level provenance and method
- `complexity.csv`: reproducible prototype surface counts and duplicated
  semantic responsibilities

These files are copied verbatim from
`build/issue33/issue33-evidence/`. Their SHA-256 values at handoff are:

```text
daf1cfa728afc33ac0e672d82e2bdbf935dab07af7da0b1cd257d3cab927bdb4  complexity.csv
c22d2289ef40eb55793e895d0ec42096b8237d762416a5f4d26eeba5209c3062  metadata.json
d136bfbacc655c857fc25261b09c9c5a3288d68732abc82a071e9c2aa855af34  raw.csv
c83ac358ba85ebc3af0195d681e97c5cd7772fd4a8c1e910205d0a9b682b8113  summary.csv
```

## Observed scaling

The table below reports medians from the nested `iota` plus two lifted `inc`
operations. Times are raw nanoseconds, not normalized throughput.

| Length | Strategy | C bytes | Emission ns | C compile ns | Executable bytes | Execution ns | Compile peak KiB |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | constants | 239 | 1,587,457 | 216,765,589 | 16,088 | 1,197,644 | 24,944 |
| 1 | flat | 1,082 | 1,568,282 | 225,253,397 | 16,264 | 1,232,939 | 27,240 |
| 1,000 | constants | 4,298 | 1,733,488 | 220,128,021 | 20,192 | 1,216,779 | 24,924 |
| 1,000 | flat | 1,085 | 1,675,741 | 225,005,336 | 16,264 | 1,273,154 | 27,240 |
| 100,000 | constants | 633,875 | 12,732,797 | 254,405,632 | 619,152 | 1,706,930 | 31,712 |
| 100,000 | flat | 1,087 | 11,281,351 | 225,992,053 | 16,264 | 6,238,255 | 27,168 |

At 100,000 elements, the constants candidate generated 583.14 times as many
C bytes and a 38.07-times-larger executable. Its median C compile took
28,413,579 ns longer and 4,544 KiB more peak memory, while its generated
program ran 4,531,325 ns faster. Median emission + compile + one execution was
268.845359 ms for constants and 243.511659 ms for flat on this host. These are
same-host observations for strategy selection, not a general Bennu performance
claim.

For small roots, constants remained smaller and compiled about 4% faster. The
1,000-element constants source was already 3.96 times the flat source but still
compiled slightly faster. The measured hybrid therefore behaved like constants
through exactly 1,000 materialized vector elements and like flat above that
boundary.

The reproducible substantive-line counts were 16 for shared validation, 59 for
constants, 57 for flat runtime lowering, and 24 for hybrid selection. Thus the
single-path constants and flat prototypes required 75 and 73 lines including
shared validation, while the hybrid required all paths and selector logic (156
lines). These counts describe the prototype surface; they are not a quality
score.

## Selected strategy

Select **validated typed flat-IR lowering to C11 loops and runtime value/
formatting helpers** for WP9. Do not adopt the result-constant or hybrid paths.

The decision prioritizes bounded artifact scaling and one coherent backend path
over the constants candidate's lower execution time in this output-heavy
prototype. The flat candidate kept emitted source and executable size nearly
constant from 1 to 100,000 generated `iota` elements, reduced the measured
large-case compile and memory costs, and directly represented arbitrary input
payload values. Its small-case compile cost and large-case formatting-loop cost
are accepted. The hybrid's small-case savings do not justify permanently
shipping both semantic representations or treating the measured 1,000-element
crossover as a language/backend threshold.

## Required WP9 boundary

The selected strategy has this explicit resource and error boundary:

1. Before any destination publication, parse and resolve the complete rewrite
   source, validate the primitive table and exact execution-profile
   configuration, and run the accepted evaluator/resource semantics. Any
   syntax, type, shape, deterministic domain, profile, or injected allocation
   failure returns no C bytes and preserves the destination.
2. Lower only the accepted typed flat program. Emit actual Bool/Int/Double
   scalar and literal payload data plus typed operation loops. Vector length may
   control a loop, but it must never stand in for arbitrary vector contents;
   `iota` is the only initial primitive whose values are defined by its scalar
   count.
3. The generated runtime replays postorder value lifetimes and canonical
   profile charges before allocation or scalar-kernel work. Admission is
   transactional and uses the accepted limit precedence. It keeps all roots
   until the complete program succeeds so deterministic execution failures
   cannot produce partial stdout.
4. Generated allocation and output operations return explicit nonzero status;
   natural host-allocation availability remains the profile-specified
   cross-backend exception. Formatting uses one canonical typed helper surface,
   including exact Double behavior.
5. No backend-specific result-size cap or hybrid threshold is introduced. The
   only limits are the selected execution profile's declared limits and
   representability checks.

The benchmark-only prototypes remain opt-in and unreachable from `bennu`. WP9
must implement the selected path deliberately rather than promote this fixed
corpus prototype into production.
