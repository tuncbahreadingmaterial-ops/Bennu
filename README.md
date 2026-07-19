# Bennu

Bennu is a data-oriented programming language. The repository provides a portable build, a reusable Level 1 expression-evaluation core, an interactive REPL, a source-file runner, and deterministic portable C emission.

## Prerequisites

- A C++20 compiler
- A C11 compiler for generated-program tests and emitted artifacts
- CMake 3.20 or newer
- Ninja

## Build and test

From a clean checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Show the command-line surface on Linux or macOS:

```sh
./build/bennu --help
```

On Windows, run `./build/bennu.exe --help` from PowerShell. The `build` subcommand remains reserved and reports that it is not implemented with a nonzero exit status.

Start the Level 1 REPL:

```sh
./build/bennu repl
```

Run a complete Level 1 source file:

```sh
./build/bennu run path/to/program.bennu
```

Successful values use the stable `>>` prefix. REPL prompts and values go to stdout; source diagnostics go to stderr and include the source name, line, column, category, and message. A file is evaluated completely before any values are written.

Emit self-contained standard C11 for a complete Level 1 source file:

```sh
./build/bennu emit-c examples/level1.bennu -o level1.c
cc -std=c11 level1.c -o level1
./level1
```

Emission validates and evaluates the complete source before atomically replacing the output. Generated C uses only the standard C library, contains no source or host paths, and produces the same batch output without Bennu or the original `.bennu` file at runtime. Bennu does not discover or invoke the C compiler.

## Level 1 evaluation core

`include/bennu/evaluator.hpp` exposes plain data structures and free functions for tokenizing, parsing, evaluating, and formatting signed 64-bit integer expressions using `inc` and `ioata`. `include/bennu/c_emitter.hpp` exposes deterministic C11 translation through the same evaluation path. `bennu_core` is a standard-library-only CMake library target. Command-line, prompt, and filesystem behavior stays in the `bennu` executable and reuses this core.
