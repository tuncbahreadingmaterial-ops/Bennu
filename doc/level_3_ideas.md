# Level 3 Ideas

**Status:** Exploratory and non-normative

This document records a possible design for passing command-line arguments to
Bennu scripts and generated native programs. It does not accept syntax,
semantics, implementation scope, or a Level 3 roadmap. Any implementation must
first turn the selected parts into a focused specification and observable
acceptance criteria.

The parameter and runtime-argument proposal in sections 1 through 13 is now
resolved normatively by
[BENNU-SPEC-0005](../spec/bennu-spec-0005-program-parameters.md). This document
remains a non-normative design record; its open-decision wording does not
override that specification. The tuple proposal in section 14 is now resolved
normatively by
[BENNU-SPEC-0006](../spec/bennu-spec-0006-structural-tuples-and-profile-v2.md);
its exploratory alternatives and open-decision wording likewise do not override
the accepted contract.

## 1. Goal

Allow one Bennu program to operate on values supplied when it is run, while
preserving the same behavior across:

- the public evaluator;
- `bennu run`;
- emitted C11; and
- executables produced by `bennu build`.

The initial design should keep argument types known before execution. That lets
the evaluator and C emitter select primitive overloads without adding untyped
runtime dispatch.

## 2. Proposed initial boundary

Start with required positional scalar parameters of these existing Bennu
types:

- `Bool`;
- `Int`; and
- `Double`.

Do not include strings, vectors, named command-line options, optional
parameters, default values, variadic parameters, environment variables, or
standard-input decoding in the first version.

This boundary is intentionally small:

- the types already exist;
- scalar shapes are known statically;
- `bennu_apply` can keep receiving a preselected primitive implementation;
- the generated runtime already checks value-dependent failures such as
  integer overflow; and
- Bool, Int, and Double argument spellings can be restricted to portable ASCII,
  avoiding an immediate cross-platform Unicode `argv` contract.

## 3. Provisional source shape

A script could optionally begin with one parameter declaration. The spelling
below is illustrative rather than accepted syntax:

```bennu
parameters[n Int delta Double enabled Bool]
add[iota[n] delta]
not[enabled]
```

The declaration is interpreted as ordered name/type pairs:

```text
parameter 1: n       Int
parameter 2: delta   Double
parameter 3: enabled Bool
```

Possible declaration rules:

- the declaration may occur at most once and only before executable roots;
- each parameter name must be a valid identifier and unique in the declaration;
- each parameter type must be an accepted scalar type name;
- a parameter reference is an expression whose value is supplied at runtime;
- parameter references are immutable;
- a parameter name may not silently shadow a primitive name; and
- a program without a declaration retains its current zero-argument behavior.

A future syntax decision should compare this declaration with alternatives such
as a distinct header form. It should not introduce special primitives such as
`arg-int[0]` merely to avoid defining parameter references.

## 4. Command-line behavior

The runner should use an explicit `--` boundary:

```sh
bennu run program.bennu -- 5 2.5 true
```

The separator makes negative values inert script data and leaves room for
future runner options. It is not included in the script's argument list.

Emission and building do not require concrete runtime values:

```sh
bennu emit-c program.bennu -o program.c
bennu build program.bennu -o program
```

The resulting native program receives its arguments directly:

```sh
./program 5 2.5 true
```

Initial CLI rules should be exact:

- a zero-parameter script accepts no script arguments;
- a parameterized script requires exactly one argument per declaration entry;
- missing and extra arguments fail before evaluation;
- arguments are parsed from left to right and the lowest failing position wins;
- Bool accepts only `true` or `false`;
- Int uses the accepted signed decimal Int literal grammar and must fit Int64;
- Double uses the accepted Double literal grammar, including the chosen
  spellings for `inf`, `-inf`, `nan`, and signed zero; and
- argument text is passed as data and is never interpreted by a shell.

Whether a declared Double should accept an integer-looking token such as `2`
is an explicit syntax decision. Requiring `2.0` initially would keep source and
command-line literal spelling aligned.

The REPL has no implicit script argument list. Parameter declarations should be
rejected there until a separate interactive binding design exists.

## 5. Frontend and evaluation model

The current rewrite path evaluates concrete values while constructing its typed
lowering. Runtime parameters require a clearer separation between analysis and
execution:

```text
source
  -> parse and resolve declarations and calls
  -> determine parameter and expression types
  -> select primitive signatures and implementations
  -> produce typed flat lowering
  -> bind concrete argument values
  -> execute
```

The flat lowering could gain a parameter-reference node containing:

- the parameter index;
- its declared scalar type;
- its declaration and reference source spans; and
- scalar container metadata.

Primitive overload selection then uses declared types rather than runtime
values. For example, if `delta` is declared as Double, `add[n delta]` can select
the Double implementation and insert the existing `Int -> Double` conversion
without knowing either value.

Value-dependent failures remain runtime failures. For example, `inc[n]` is a
valid Int expression even though supplying `9223372036854775807` must produce
`integer_overflow` when the script executes.

The public API should continue to use plain data and free functions. A future
API design could provide an overload that accepts a span of typed scalar
values. Text decoding belongs at a CLI or embedding boundary rather than inside
primitive application.

## 6. Generated C11

For a parameterized script, the emitted entry point becomes:

```c
int main(int argc, char **argv) {
  /* Validate argument count. */
  /* Decode each argument into a typed BennuValue slot. */
  /* Execute the existing lowered operation sequence. */
}
```

Generated argument helpers should:

- validate `argc` before indexing `argv`;
- parse without locale-dependent numeric meaning;
- reject partial parses and out-of-range values;
- create scalar `BennuValue` records without dynamic allocation;
- report deterministic failures to stderr;
- return nonzero on argument, evaluation, or output failure; and
- preserve the existing cleanup paths.

Once parameter slots are initialized, ordinary calls can continue to look like:

```c
if (!bennu_apply(&bennu_resources,
                 BENNU_IMPL_ADD_DOUBLE,
                 &bennu_values[result_index],
                 &bennu_values[n_index],
                 &bennu_values[delta_index],
                 2U,
                 "add",
                 bennu_source_location(offset, line, column))) {
  goto bennu_failure;
}
```

For typed scalar parameters, `bennu_apply` does not need runtime overload or
shape selection. It already:

- projects scalar operands;
- performs `Int -> Double` conversion for a selected Double implementation;
- charges scalar work;
- checks integer domain failures; and
- returns a typed scalar result.

The emitter must no longer require successful concrete evaluation in order to
emit a program. It must instead prove that the typed lowering is valid while
leaving value-dependent execution to the evaluator or generated runtime.

## 7. Diagnostics and atomic output

Argument failures should be distinct from source syntax, type, shape, domain,
resource, and output failures. A future structured argument error should be
able to carry:

- supplied argument count and required count;
- one-based parameter position;
- parameter name;
- expected scalar type; and
- a stable reason such as `missing`, `extra`, `invalid_literal`, or
  `out_of_range`.

Raw argument text should either be omitted from diagnostics or escaped through
one defined routine. It must never be treated as terminal formatting or a
format string.

Evaluation remains transactional with respect to stdout:

1. validate argument count;
2. decode every argument;
3. evaluate every program root;
4. only then publish formatted root values.

Thus a malformed argument or a later integer overflow produces no partial
program output.

## 8. Resource accounting and ownership

Initial scalar parameter binding requires no vector allocation. Parameter
values are copied into ordinary scalar value slots, and `argv` remains owned by
the host process.

Argument decoding should occur before the evaluation resource counter begins,
matching source parsing and validation. Operations involving parameters are
charged normally when executed. Any later vector-argument design must define:

- how vectors are encoded on the command line;
- who owns decoded storage;
- when vector bytes become live;
- whether input construction consumes work units; and
- how input storage participates in maximum-live-byte limits.

## 9. Portability boundary

Restricting the first version to ASCII Bool and numeric tokens avoids claiming
portable Unicode command-line behavior that standard C11 does not provide.

The Bennu Windows CLI receives a wide command line and converts it to UTF-8,
whereas a strictly portable generated C program normally receives narrow
`char **argv` through the C runtime. A future String parameter therefore needs
a separate encoding and Windows entry-point decision. String support should not
be implied by the initial scalar-argument feature.

## 10. Alternatives considered

### Raw string `argv`

Expose every argument as a String vector. This is familiar, but Bennu has no
String type and portable Windows decoding is unresolved. It would make the
first argument feature depend on a much larger value-domain change.

### Special argument primitives

Add forms such as `arg-int[0]` and `arg-double[1]`. This is mechanically small,
but it embeds command-line positions into the primitive system, repeats type
information at each use, provides no single script interface, and scales poorly
to embeddings that do not use a process command line.

### Embed values while emitting or building

Pass values to `emit-c` or `build` and compile them into the artifact. That is
configuration or specialization, not runtime script arguments, and would
require rebuilding for every input.

### Untyped runtime overload selection

Let every argument choose its type from its text and select primitive overloads
while executing. This makes program types input-dependent and would require a
new dynamic dispatch and error model in both evaluators.

### Environment variables or standard input

Both may be useful future input channels, but neither replaces a declared,
typed script interface. They should have separate semantics and resource
policies.

## 11. Suggested staged implementation

1. Specify parameter declaration and reference syntax.
2. Specify argument decoding, error precedence, and CLI boundaries.
3. Separate typed lowering from concrete evaluation.
4. Add scalar parameter-reference nodes and typed public evaluator input.
5. Update `bennu run` with the explicit `--` boundary.
6. Emit `main(int argc, char **argv)` and portable scalar decoders.
7. Verify direct, runner, emitted-C, and native differential behavior.
8. Consider String, vector, named, optional, or variadic arguments only as
   separately justified follow-up work.

## 12. Initial acceptance ideas

A future implementation issue should at minimum prove:

- existing zero-argument programs remain byte-for-byte behaviorally unchanged;
- valid Bool, Int, and Double parameters agree across every execution surface;
- missing, extra, malformed, and out-of-range arguments fail deterministically;
- negative numeric arguments after `--` are treated as data;
- parameter-driven overload promotion agrees with literal-driven promotion;
- parameter-driven integer overflow is detected at execution time;
- no argument or evaluation failure publishes partial stdout;
- emitted C is deterministic and independent of concrete runtime values;
- generated C compiles as strict C11 on supported toolchains;
- allocation and work-limit behavior remains aligned across backends; and
- Linux, Windows, and macOS native argument journeys agree for the initial
  ASCII scalar domain.

## 13. Open decisions

- What is the final declaration syntax?
- Can parameter names equal primitive names?
- Must Double arguments use visibly Double syntax?
- What exact structured error and presentation represent argument failures?
- Should public embeddings supply typed values, argument text, or both through
  separate boundaries?
- Does `evaluate_expression` accept parameterized source, or are parameters a
  program-only concept?
- Is there a configured maximum number of parameters beyond source-size and
  representability limits?
- Should a later String design remain UTF-8 everywhere or expose platform
  decoding explicitly?

## 14. Tuple values and automatic argument spreading

This is a separate, exploratory Level 3 idea. It does not depend on accepting
the typed-script-argument proposal above.

### Goal and source shape

Add fixed-length tuple values while making a tuple convenient to use as the
argument sequence of a prefix call. The motivating equivalence is:

```bennu
add[1 2]
add [1 2]
```

Both forms would call `add` with the two Int arguments `1` and `2`. Whitespace
is meaningful here:

- `add[1 2]` is the existing direct call with an explicit argument list;
- `[1 2]` in expression position constructs a tuple; and
- `add [1 2]` is prefix application to one tuple expression, whose outer
  elements are automatically spread into the call.

This deliberately gives useful meaning to a form that the current parser
rejects as `whitespace_before_bracket`. It also keeps `()` available for the
existing homogeneous vector syntax and uses Anka's bracketed tuple spelling as
a language-design cue without requiring compatible implementation details.

Tuple literals should support zero, one, or many heterogeneous expressions:

```bennu
[]
[1]
[1 2.5 true]
[1 [2 3]]
```

They are immutable, fixed-arity values. Construction evaluates elements once,
from left to right. Canonical formatting uses the same bracketed spelling and
preserves nesting.

### Proposed call rule

Automatic spreading occurs only when a call is written as prefix application
to one expression and that expression evaluates to a tuple:

```text
apply_prefix(function, value):
  tuple value  -> call function with the tuple's outer elements
  other value  -> call function with one argument
```

Spreading is exactly one level, never recursive. For example, applying a
function to `[1 [2 3]]` supplies two arguments: `1` and `[2 3]`. The empty tuple
supplies zero arguments and a singleton tuple supplies one.

An explicit bracketed argument list does not implicitly spread tuple-valued
arguments. It is the escape hatch for a future function that deliberately
accepts a tuple:

```bennu
consume[[1 2]]  # one tuple argument
consume [1 2]   # two automatically spread arguments
```

The language should expand the argument sequence before arity checking,
overload selection, type conversion, shape checking, and primitive execution.
Expansion must not depend on which overloads happen to exist; otherwise adding
an overload could silently reinterpret an existing program.

General prefix application needs its own precedence and associativity rules.
A focused first implementation could accept only `name <tuple-expression>`
until those rules are specified, but it should still use the same semantic
boundary so tuple results can later be spread without special-casing literals.

### Fit with the current frontend

The lexer already recognizes square brackets. The rewrite parser currently
distinguishes adjacent call brackets and explicitly diagnoses whitespace before
a bracket. That point can become the initial grammar split:

```text
name[expressions...]  -> direct argument-list call
name [expressions...] -> prefix call whose operand is a tuple literal
[expressions...]      -> tuple literal expression
```

The parsed representation needs a tuple-literal node plus a contiguous element
range in an arena, matching the current flat/data-oriented approach. A prefix
application node should retain both the whole operand span and the individual
tuple-element spans. After evaluating the operand once, call preparation can
present either a one-value span or the tuple's element span to the existing
primitive application boundary.

`apply_primitive` and scalar primitive kernels should not implement spreading.
They should continue to receive the final semantic argument span. This keeps
direct API calls explicit and prevents tuple syntax from leaking into primitive
behavior.

Diagnostics should describe the expanded call. Thus `add []` is an arity error
with zero supplied arguments, while `add [1 true]` is a type error on the second
expanded argument. The primary call location can remain the function name; an
argument-specific error should point to the responsible tuple element rather
than only to the complete tuple.

### Value, type, and ownership model

The public value domain currently distinguishes scalar and homogeneous vector
containers. First-class heterogeneous tuples require a third container kind
and a representation for a fixed sequence of complete Bennu values, not merely
a new scalar element type.

A data-oriented representation should use an owned contiguous element buffer
or a flat value arena with an offset and count. Construction, copying,
validation, destruction, and formatting must operate through free functions.
Nested tuples make release and validation recursive in meaning; an iterative
arena walk may be preferable if the language permits deep nesting.

The type representation also needs to describe ordered heterogeneous element
types. The current `ValueType` pair of container kind and scalar element type is
not sufficient for `[Int Double]`. Before tuple-accepting functions are added,
primitive signatures can remain scalar/vector-only, but diagnostics and typed
lowering still need a stable tuple type descriptor or a structural type arena.

Tuple elements retain their existing value semantics. In particular:

- vectors inside a tuple still own their vector storage;
- moving elements into a tuple transfers that ownership exactly once;
- spreading borrows the tuple's elements for the duration of the call;
- spreading does not copy or reallocate contained vectors; and
- destroying a tuple releases every transitive owned value exactly once.

### Resources and emitted C11

Tuple construction must participate in the same resource model on every
execution surface. The design must decide whether the tuple's element table,
transitive payloads, or both count toward maximum live bytes, and whether each
constructed element consumes work. Limits for total elements and nesting depth
should be explicit so validation, formatting, and cleanup cannot exhaust the
host stack or overflow representable sizes.

The first implementation should construct the tuple and then borrow its outer
element span for spreading. An optimizer may later eliminate a tuple used only
by an immediate prefix call, but only if evaluator and emitted-C work,
allocation, failure precedence, and observable resource limits remain
equivalent.

Generated C11 needs the corresponding tuple container tag, element storage,
validation, formatting, and cleanup paths. Prefix-call lowering expands the
outer elements before invoking the existing `bennu_apply` boundary. Successful
calls such as `add [1 2]` can therefore continue to use the existing selected
`add` implementation and scalar kernels.

### Alternatives considered

Treat whitespace before `[` as an alternate spelling of the existing direct
call. This makes the motivating example work without tuples, but `[1 2]` is not
a first-class value and tuple-producing expressions cannot be composed.

Spread tuples in every call position. This is superficially uniform, but it
removes a clear way to pass one tuple value to a tuple-aware function and makes
nesting harder to read.

Add an explicit spread marker such as `add[...[1 2]]`. This is unambiguous and
may remain useful for spreading one value among other explicit arguments, but
it loses the concise prefix form requested here.

Recursively flatten nested tuples. This makes argument count depend on the
complete nested structure and prevents nested tuples from remaining meaningful
values. One-level spreading is more predictable.

### Suggested staged implementation

1. Specify tuple syntax, canonical formatting, nesting, and size limits.
2. Specify the minimal prefix-application grammar and its whitespace rule.
3. Add tuple parse/lowering nodes and structural tuple type descriptions.
4. Add resource-accounted tuple value construction, validation, and cleanup.
5. Expand one tuple operand into a borrowed semantic argument span before
   primitive validation.
6. Add equivalent tuple storage, spreading, formatting, and cleanup to emitted
   C11.
7. Add general prefix-call precedence or explicit mixed-argument spreading only
   through separate, justified follow-up work.

### Initial acceptance ideas

A future implementation issue should at minimum prove:

- `add [1 2]` agrees with `add[1 2]` across the evaluator, CLI, emitted C, and
  native execution;
- `[]`, `[1]`, heterogeneous tuples, and nested tuples format canonically;
- spreading is one level and preserves left-to-right evaluation;
- direct argument lists preserve a tuple as one argument;
- arity and type diagnostics report expanded arguments and precise element
  spans;
- tuple construction, failed calls, formatting failures, and cleanup neither
  leak nor double-release nested vector or tuple storage;
- configured work and live-byte limits fail at the same semantic point across
  backends;
- allocation failure at every tuple allocation boundary remains transactional;
- existing adjacent calls such as `add[1 2]` retain their current behavior; and
- the formerly invalid whitespace-before-bracket form changes only where the
  new prefix-call grammar accepts it.

### Open decisions

- Is general `function expression` application introduced immediately, or is
  the first grammar restricted to a tuple operand?
- Can prefix application target only primitive names initially, or any future
  callable value?
- How are tuple types represented in public diagnostics and typed lowering?
- What exact depth, element-count, work, and live-byte rules apply?
- Should a later explicit spread marker support mixing tuple elements with
  other direct arguments?
- Are tuple destructuring and tuple-accepting primitive signatures part of the
  same language level or separate follow-up features?
