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

On Windows, run `./build/bennu.exe --help` from PowerShell.

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

Emission validates and evaluates the complete source before atomically replacing the output. Generated C uses only the standard C library, contains no source or host paths, and produces the same batch output without Bennu or the original `.bennu` file at runtime.

Build a standalone native executable through an installed external C compiler:

```sh
./build/bennu build examples/level1.bennu -o level1
./level1
```

Compiler selection is, in order: `--cc <compiler>`, a nonempty `CC` environment variable, then the platform fallback. The fallback searches `cc` on Linux and macOS and `cl.exe` on Windows. A selected `--cc` or `CC` value is final if it cannot start or compile; Bennu does not silently try another compiler. Compiler values are executable names or paths, not shell fragments or flag strings. Bennu invokes the compiler directly, creates C and compiler artifacts in an isolated temporary directory beside the requested output, and replaces the requested output only after successful compilation and cleanup. It does not bundle, download, or install a compiler.

## Level 1 evaluation core

`include/bennu/evaluator.hpp` exposes plain data structures and free functions for tokenizing, parsing, evaluating, and formatting signed 64-bit integer expressions using `inc` and `ioata`. `include/bennu/c_emitter.hpp` exposes deterministic C11 translation through the same evaluation path. `include/bennu/native_builder.hpp` exposes plain compiler-selection, command-construction, and native-build data transformations. `bennu_core` is a standard-library-only CMake library target. Command-line and prompt behavior stays in the `bennu` executable and reuses this core.
