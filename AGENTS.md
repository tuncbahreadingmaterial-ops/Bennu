# Bennu Contributor Agreement

These rules apply to every human and agent contributor working in this repository.

## Product direction

- Bennu is a data-oriented programming language written in C++.
- Use [Anka](https://github.com/tuncb/anka) for language-design cues and experiments. Anka is inspiration, not a requirement for architectural or behavioral compatibility; document where Bennu deliberately differs.
- Optimize first for correctness, data integrity, a small coherent language, predictable costs, and measurable performance.

## C++ and architecture constraints

- Do not use object-oriented design.
- Keep data and behavior separate: represent data with plain structs and transform it with free functions.
- Do not add `private` or `protected` members. Structs expose their data directly.
- Do not add operator overloads.
- Do not add default function arguments.
- Do not use C++ exceptions for normal or recoverable errors. Do not throw exceptions from Bennu-owned code.
- Return errors explicitly. `tl::optional` and `tl::expected` may be used when optional- or expected-style results are appropriate.
- Prefer direct data flow, contiguous and simple data layouts, visible ownership and allocation, and shallow abstractions. Complexity must buy a concrete language or product capability.

## Engineering workflow

- Drive work from a GitHub issue with observable acceptance criteria.
- Keep changes inside the active issue's scope. Create a linked follow-up issue for genuinely new work.
- Add or update automated tests for behavior changes, and run the relevant build and tests before declaring work complete.
- Do not claim performance improvements without comparable before-and-after measurements.
- Record material design and implementation decisions as they are made in [`doc/decision-diary.md`](doc/decision-diary.md). Each entry must include the date, related issue or pull request, context, decision, alternatives considered, rationale, consequences or follow-up, and validation or evidence.
- Keep the decision diary append-only. Correct an earlier entry with a new entry rather than silently rewriting history.

Material decisions include syntax and semantics, data layout, ownership, dependencies, error representation, parsing or execution strategy, testing and benchmark policy, and the selection or rejection of alternative approaches. Routine keystrokes and mechanically implied edits do not need entries.
