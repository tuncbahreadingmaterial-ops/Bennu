# Issue #52 — Borrowed multi-consumer liveness

## 2026-07-25 — Derive ownership release from static uses

- **Context:** The flat typed program already stores call arguments as indices
  into one contiguous node arena, but direct evaluation moved each argument into
  a temporary owner and required its `remaining_uses` count to be exactly one.
  Generated C likewise released every argument after every call. Those rules
  made a repeated argument index invalid even though primitive application is
  immutable and does not retain its arguments.
- **Decision:** Lowering records one deterministic use count on every node,
  including root retention, while keeping the existing contiguous node and
  argument-index arenas. Typed application receives a bounded span of immutable
  pointers into the evaluator's owned node-value arena. A consumer attempt
  decrements all of its static argument uses only after shape/application work
  completes; a non-root owner is released when its count reaches zero. Roots
  retain their owner until result transfer or generated-output cleanup.
  Root indices remain unique because the evaluator's result is a vector of
  uniquely owned values; a repeated root would require a copy or shared owner,
  so malformed repeated-root lowering is rejected before either backend. The
  ordinary lowering path performs that check directly over the root arena
  without allocating a node-sized seen-root sidecar. Full node/use/root
  invariant validation is reserved for prepared flat inputs: each lowering
  node must preserve its source-node kind and each call node must exactly
  preserve the source call's checked argument range and count before any
  prepared owner can move.
  Generated C consumes the same counts while emitting code and writes a direct
  `bennu_release` only at a non-root argument's last successful use. Runtime
  values contain no reference count, borrowed result, or shared owner.
- **Alternatives considered:** Move or clone one payload per consumer; introduce
  reference-counted values; retain all intermediates to program completion; or
  have the runtime search future calls for aliases. Moving invalidates later
  consumers, cloning changes charges and allocation/failure order, reference
  counting adds mutation and runtime policy, whole-program retention inflates
  live-byte behavior, and runtime searches hide a cost already knowable from
  flat lowering.
- **Rationale:** Static counts make ownership and costs visible, preserve
  primitive const-borrowing, add no payload copy or runtime allocation, and
  apply uniformly to scalars, vectors, empty vectors, and structural tuple
  owners. The same sidecar drives evaluator and C release placement.
- **Anka difference:** Anka's executor experiments are a language-design cue,
  but Bennu deliberately does not adopt executor-owned graphs, dynamic alias
  management, mutation, or runtime reference counting. Bennu's ordered flat
  lowering fixes sharing and last use before execution.
- **Validation/evidence:** `SHARED-001` covers immutable scalar, vector, and
  empty-vector owners, plus a valid prepared tuple producer feeding two
  successful immutable-borrow consumers through the evaluator's fail-fast
  execution loop. It also covers tuple consumer failure, cleanup, and rejection
  of an invalid static use count before ownership moves. `SHARED-002` duplicates
  a flat argument index in an invariant-valid program and drives the complete
  production C translation-unit emitter, proving C release is emitted only
  after its final call with no runtime use counter. `SHARED-ROOT` proves repeated
  owned roots are rejected before evaluator/backend behavior can diverge.
  `SHARED-003` covers the 47/48-byte live boundary, deterministic allocation
  ordinal 2 failure, retained completed-result cleanup, and exact releases.
  `SHARED-004` runs that graph through the production prepared evaluator seam
  and checks successful roots, maximum-live precedence, allocation ordinals,
  first failure, and zero-live cleanup. Prepared mutation probes cover node-kind,
  call-range, first-argument, argument-count, and use-count mismatches, checking
  evaluator and emitter rejection while the prepared owner remains intact.
  Generated C from `SHARED-002` compiles under strict C11 and runs natively with
  the same successful output.
- **Current tuple C dependency:** Issue #49 supplies direct `Value` tuple
  ownership, but the generated-C `BennuValue` still has only scalar and vector
  representations. Prepared tuple emission therefore returns an explicit
  profile error naming Issue #50 rather than claiming cross-backend tuple
  parity. Completing the issue's generated-C tuple criterion requires #50's
  tuple runtime/lowering work; #52 does not duplicate that concurrent change.
