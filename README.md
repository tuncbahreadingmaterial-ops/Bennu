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

## Platforms and historical release

The supported CI targets are Ubuntu 24.04 LTS x64, Windows 11 x64 or newer, and
macOS arm64. The Linux package compatibility floor is glibc 2.39 or newer on a
supported Linux 6.8 GA kernel (or supported Ubuntu HWE kernel), with ELF symbol
ceilings `GLIBC_2.34` and `GLIBCXX_3.4.32`; the executable dynamically resolves
`libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6`, and `libm.so.6`. Windows users
install the Microsoft Visual C++ 2017-2026 Redistributable (x64). On every
platform, only `bennu build` requires an external C11 compiler.

The published `v0.1.0` archives and their release workflow are immutable
historical artifacts. They predate this incompatible rewrite and remain
available under their original asset names; this document describes the current
source tree rather than retroactively changing that release.
