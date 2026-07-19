# Bennu Level 1 language and toolchain

Level 1 is Bennu's first complete source-to-native slice. It evaluates signed
64-bit decimal integer expressions and one-dimensional integer arrays through
two prefix primitives. The same accepted semantics drive the REPL, source-file
runner, deterministic C11 emitter, and external-compiler native build.

## Supported targets and prerequisites

The supported v0.1.0 release targets are Linux x64, Windows x64, and macOS
arm64. Other systems may build from source but are not Level 1 release targets.

A source build requires:

- a C++20 compiler;
- CMake 3.20 or newer;
- Ninja; and
- a C11 compiler for the default tests, emitted programs, and `bennu build`.

Bennu does not bundle, download, or install a C compiler. Install a platform C
toolchain separately and ensure its driver is available by name on `PATH`, or
pass its executable path explicitly.

From a clean checkout:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Examples below use `./build/bennu` and native output `./level1` on Linux and
macOS. In PowerShell on Windows, use `.\build\bennu.exe` and `level1.exe`.

## Language

### Grammar and program lines

Level 1 uses recursive unary prefix expressions:

```text
program     := zero or more blank lines or expression lines
expression  := integer
             | "inc" horizontal-space expression
             | "ioata" horizontal-space expression
integer     := "-"? decimal-digit+
```

Each nonblank line contains exactly one complete expression. A primitive name
and its argument require at least one space or tab. Leading and trailing spaces
or tabs are allowed. Calls can be nested on the same line:

```text
ioata inc 5
```

This evaluates `inc 5` first and then produces `(1 2 3 4 5 6)`.

LF and CRLF terminate lines. Blank or horizontal-whitespace-only lines are
ignored, and the final expression does not require a trailing newline. A lone
carriage return is invalid. A file containing only blank lines is a valid empty
program and produces no values.

### Integers, arrays, and limits

Integer literals are decimal signed 64-bit values from
`-9223372036854775808` through `9223372036854775807`, inclusive. A leading plus
sign, digit separators, nondecimal bases, and values outside that range are not
Level 1 syntax.

Level 1 values are either scalar integers or one-dimensional integer arrays.
Arrays have no literal syntax; `ioata` is the only way source code creates one.

- `inc n` requires a scalar integer and returns `n + 1`. Incrementing
  `9223372036854775807` is an overflow error.
- `ioata n` requires a scalar integer. For positive `n`, it returns the array
  from 1 through `n`, inclusive. For `n <= 0`, it returns the empty array `()`.
- One `ioata` result may contain at most 1,000,000 elements. The sum of all
  positive `ioata` results retained while evaluating one complete program may
  also contain at most 1,000,000 elements.

Passing an array where either primitive requires a scalar is a wrong type error.
In particular, Level 1 `inc` does not lift over arrays, so `inc ioata 3` fails
instead of producing an incremented array.

### Exact formatting

Formatting is deterministic:

- scalar integer: `6`
- nonempty array: `(1 2 3 4 5)`
- empty array: `()`

Array elements are separated by one ASCII space with no commas. Every evaluated
result line starts with `>>` and has no space between the prefix and the value:

```text
>>(1 2 3 4 5)
>>6
```

## Commands

The committed `examples/level1.bennu` file contains exactly:

```text
ioata 5
inc 5
```

All batch journeys below produce the two exact result lines shown above.

### REPL

```sh
./build/bennu repl
```

The REPL writes `> ` prompts to stdout, reads one expression per line, and
writes a successful value with the `>>` prefix. A blank line is ignored. A
source error is written to stderr and the next prompt is shown, so one bad line
does not end the session. End-of-input exits successfully.

### Run a source file

```sh
./build/bennu run examples/level1.bennu
```

`run` reads and evaluates the complete file before writing values. Any file,
syntax, range, allocation, or type error returns nonzero and produces no partial
result stream.

### Emit portable C

```sh
./build/bennu emit-c examples/level1.bennu -o level1.c
cc -std=c11 level1.c -o level1
./level1
```

For MSVC from a configured developer environment, the compile step can be:

```powershell
cl /nologo /std:c11 level1.c /Fe:level1.exe
.\level1.exe
```

`emit-c` first validates and evaluates the complete source, then atomically
replaces the requested output. The deterministic generated source is
self-contained standard C11, uses only the standard C library, and does not
embed the source path or require Bennu or the `.bennu` file at runtime. Success
produces no command output. A source or output failure returns nonzero and does
not replace an existing C file.

### Build a native executable

Use the platform fallback compiler:

```sh
./build/bennu build examples/level1.bennu -o level1
./level1
```

Select a compiler explicitly:

```sh
./build/bennu build examples/level1.bennu -o level1 --cc clang
```

Or use the standard environment variable:

```sh
CC=gcc ./build/bennu build examples/level1.bennu -o level1
```

In PowerShell, the equivalent environment form is:

```powershell
$env:CC = "cl.exe"
.\build\bennu.exe build examples\level1.bennu -o level1.exe
```

Compiler selection precedence is exact:

1. nonempty `--cc <compiler>`;
2. nonempty `CC`; then
3. platform fallback: `cc` on Linux and macOS, `cl.exe` on Windows.

An explicit `--cc` or `CC` selection is final. If that executable is missing,
cannot start, or reports failure, Bennu returns nonzero and does not try the next
source or fallback. Empty `CC` is treated as absent. A compiler value is one
executable name or path, such as `cc`, `clang`, `gcc`, `cl.exe`, or an absolute
path. It is not a shell fragment and cannot contain an extra compiler-flag
string. On Windows, a selected driver named `cl` or `cl.exe` receives MSVC C11
arguments; other selected drivers receive GCC/Clang-style C11 arguments.

`build` invokes the compiler directly without a command shell. It validates the
Bennu source before invocation, builds in an isolated temporary directory next
to the requested output, and publishes the native output only after compilation
and cleanup succeed. Missing fallback discovery, process-start failure, compiler
termination or nonzero status, success without a usable executable, and output
lifecycle failures all return nonzero with compiler-selection context. Failed
builds preserve an existing output and do not leave a misleading new executable.

## Failure and exit behavior

Successful commands return status 0. Command-line, file, source, emission,
compiler, and output failures return nonzero, except that the interactive REPL
reports a bad line and continues until end-of-input. Diagnostics go to stderr;
source diagnostics include source name, one-based line and column, error
category, and message. Prompts and successful values go to stdout.

Batch commands do not print partial successful values before a later source
error. Generated C and native outputs are replaced only after their complete
operation succeeds.

## Deliberate differences from Anka

Anka provides language-design cues, not a compatibility target. Bennu preserves
Anka's whole-word spelling `ioata` and the familiar `ioata 5` / `inc 5` examples.
Level 1 deliberately excludes Anka array literals, array-lifted `inc`, broader
Anka syntax and semantics, and architectural compatibility. Do not assume an
Anka program beyond this documented surface is a Bennu Level 1 program.

## v0.1.0 release assets and layout

Issue #10 will publish these exact target archives after the release pipeline is
independently accepted and the v0.1.0 release gate is authorized:

- `bennu-v0.1.0-linux-x64.tar.gz`
- `bennu-v0.1.0-windows-x64.zip`
- `bennu-v0.1.0-macos-arm64.tar.gz`

Each archive has exactly two files at the archive root:

```text
bennu        # Linux and macOS; executable
LICENSE      # MIT license
```

or on Windows:

```text
bennu.exe
LICENSE
```

Download the archive matching the operating system and CPU architecture,
extract both files into a directory of your choice, and run `bennu` or
`bennu.exe` from that directory. The release binary still requires an external
C11 compiler only when using `bennu build`; REPL, run, and emit-C do not bundle
or install one.