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
- **Validation/evidence:** `SHARED-001` covers immutable scalar, vector,
  empty-vector, and nested tuple owners through final use and observes exact
  logical release. `SHARED-002` duplicates a flat argument index and proves C
  release is emitted only after its final call with no runtime use counter.
  `SHARED-003` covers the 47/48-byte live boundary, deterministic allocation
  ordinal 2 failure, retained completed-result cleanup, and exact releases.
  Existing rewrite, typed-lowering, and application focused suites remain
  unchanged apart from the liveness metadata snapshot.
