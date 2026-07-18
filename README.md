# Bennu

Bennu is a data-oriented programming language. The repository provides a portable build and command-line foundation plus a reusable Level 1 expression-evaluation core. The core is not wired to the reserved command-line flows yet.

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

Show the reserved command-line surface on Linux or macOS:

```sh
./build/bennu --help
```

On Windows, run `./build/bennu.exe --help` from PowerShell. The `repl`, `run`, `emit-c`, and `build` subcommands are reserved for later issues and currently report that they are not implemented with a nonzero exit status.

## Level 1 evaluation core

`include/bennu/evaluator.hpp` exposes plain data structures and free functions for tokenizing, parsing, evaluating, and formatting signed 64-bit integer expressions using `inc` and `ioata`. `bennu_core` is a standard-library-only CMake library target. It has no command-line, prompt, or filesystem behavior.
