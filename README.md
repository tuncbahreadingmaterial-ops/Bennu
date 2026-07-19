# Bennu

Bennu is a data-oriented programming language. The repository provides a portable build, a reusable Level 1 expression-evaluation core, an interactive REPL, and a source-file runner.

## Prerequisites

- A C++20 compiler
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

On Windows, run `./build/bennu.exe --help` from PowerShell. The `emit-c` and `build` subcommands remain reserved and report that they are not implemented with a nonzero exit status.

Start the Level 1 REPL:

```sh
./build/bennu repl
```

Run a complete Level 1 source file:

```sh
./build/bennu run path/to/program.bennu
```

Successful values use the stable `>>` prefix. REPL prompts and values go to stdout; source diagnostics go to stderr and include the source name, line, column, category, and message. A file is evaluated completely before any values are written.

## Level 1 evaluation core

`include/bennu/evaluator.hpp` exposes plain data structures and free functions for tokenizing, parsing, evaluating, and formatting signed 64-bit integer expressions using `inc` and `ioata`. `bennu_core` is a standard-library-only CMake library target. Command-line, prompt, and filesystem behavior stays in the `bennu` executable and reuses this core.
