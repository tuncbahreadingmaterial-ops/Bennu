# Bennu

Bennu is a data-oriented programming language. This repository currently provides the portable build, test, and command-line foundation; language execution is not implemented yet.

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
