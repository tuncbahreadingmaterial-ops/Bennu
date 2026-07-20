# Bennu

Bennu is a data-oriented array language. Level 1 is a deliberately small,
complete toolchain for signed 64-bit integer expressions: evaluate them in a
REPL, run a source file, emit self-contained C11, or build a standalone native
executable with an installed C compiler.

Level 1 has two whole-word prefix primitives:

- `ioata n` produces `(1 2 ... n)` for positive `n` and `()` for `n <= 0`.
- `inc n` adds one to a scalar integer.

Each nonblank source line contains one expression. Calls can be nested, as in
`ioata inc 5`. Scalar values are printed as `6`, arrays as `(1 2 3 4 5)`, and
successful evaluated results have the exact `>>` prefix. Level 1 does not have
array literals, array-lifted `inc`, variables, or additional primitives.

See the [Level 1 language and toolchain](doc/level1.md) reference for the exact
grammar, limits, command behavior, compiler selection, Anka differences, and
release installation layout.

## Supported platforms and prerequisites

The v0.1.0 release targets are:

- Ubuntu 24.04 LTS x64
- Windows 11 x64 or newer
- macOS arm64

Building Bennu from source requires a C++20 compiler, CMake 3.20 or newer, and
Ninja. The default test configuration and generated programs also require a
C11 compiler. Bennu does not bundle, download, or install a C compiler.

The portable Linux archive's support floor is Ubuntu 24.04 LTS on x86-64 with
glibc 2.39 or newer and a supported Linux 6.8 GA kernel or supported Ubuntu HWE
kernel. Its enforced ELF symbol ceilings are `GLIBC_2.34` and
`GLIBCXX_3.4.32`. The executable dynamically resolves `libstdc++.so.6`,
`libgcc_s.so.1`, `libc.so.6`, and `libm.so.6` from the userland. Other
distribution/library combinations may work when they provide those interfaces,
but the release contract is tested on Ubuntu rather than on mixed userlands.

Before `bennu.exe` can launch, Windows users must install the
[Microsoft Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe),
version 14.51.36231.0 or newer. Microsoft requires the installed runtime to be
at least as recent as the build tools; see the
[official compatibility guidance](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist?view=msvc-170).
On every release target, only `bennu build` requires an external C11 compiler;
`--help`, `repl`, `run`, and `emit-c` do not require one.

## Level 1 quick start

From a clean checkout, configure, build, and test a Release build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The commands below use the Linux/macOS executable path. In PowerShell on
Windows, replace `./build/bennu` with `.\build\bennu.exe` and use `.exe` for
native output names.

Start the REPL:

```sh
./build/bennu repl
```

The canonical interaction is:

```text
> ioata 5
>>(1 2 3 4 5)
> inc 5
>>6
```

Run the committed example:

```sh
./build/bennu run examples/level1.bennu
```

It prints exactly:

```text
>>(1 2 3 4 5)
>>6
```

Emit self-contained standard C11, then compile and run it with a C compiler:

```sh
./build/bennu emit-c examples/level1.bennu -o level1.c
cc -std=c11 level1.c -o level1
./level1
```

Build and run a standalone native executable in one command:

```sh
./build/bennu build examples/level1.bennu -o level1
./level1
```

Use `--cc <compiler>` after the output to select an executable explicitly, or
set the `CC` environment variable. Otherwise Bennu searches for `cc` on Linux
and macOS or `cl.exe` on Windows. A selected compiler failure is final; Bennu
does not silently fall back to another compiler. Compiler values are executable
names or paths, not shell fragments or compiler-flag strings.

Successful commands return status 0. CLI, source, output, and compiler failures
return nonzero and write concise diagnostics to stderr. `run` validates the
complete file before printing any result. `emit-c` and `build` preserve an
existing output when validation, emission, compilation, or publication fails.

## v0.1.0 release installation

For the published v0.1.0 release, choose the asset matching the target:

- `bennu-v0.1.0-linux-x64.tar.gz`
- `bennu-v0.1.0-windows-x64.zip`
- `bennu-v0.1.0-macos-arm64.tar.gz`

Each archive contains only `bennu` (or `bennu.exe`) and `LICENSE` at the archive
root. Extract both files to a directory of your choice, keep `LICENSE` with the
executable, and invoke the executable by path. On Linux and macOS the archived
`bennu` file is executable; on Windows run `bennu.exe` from PowerShell.