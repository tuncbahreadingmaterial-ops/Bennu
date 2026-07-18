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
- Record material design and implementation decisions as they are made in [`doc/decision-diary.md`](doc/decision-diary.md). Each entry must include the date, related GitHub issue, context, decision, alternatives considered, rationale, consequences or follow-up, and validation or evidence.
- Keep the decision diary append-only. Correct an earlier entry with a new entry rather than silently rewriting history.

### Issue branches and local-main integration

Coder must use this sequence for every GitHub issue:

1. Before starting, confirm the working tree is clean and local `main` is current with `origin/main`.
2. Create one dedicated branch named `issue/<number>-<short-slug>` from `main`.
3. Keep every change for the issue on that branch. Make one or more scoped commits that reference the issue number.
4. On the issue branch, run the issue's required build, tests, runtime checks, and diff-hygiene checks.
5. After the worker's validation passes, merge the issue branch into local `main`.
6. Push the resulting `main` to `origin`.
7. Do not open a GitHub pull request and do not request or perform GitHub code review. Bennu does not use GitHub pull requests or GitHub code reviews.
8. Leave the GitHub issue open for Chef. Comment on it with the branch name, issue commit SHA or SHAs, resulting merged `main` SHA, exact changed files, push result, and exact validation evidence. Then block the matching Hermes Kanban card as `review-required` for Chef's independent inspection and product verification.

If integration is unsafe because the worktree is dirty, the remote has unexpectedly diverged, the merge conflicts, validation fails, or unrelated changes are present, stop and block the card with evidence instead of forcing the merge. Because issue branches integrate through one shared `main`, normal implementation is serialized to one active Coder issue at a time unless Chef explicitly establishes that independent integration is safe.

Local merge is integration, not acceptance. Chef's post-merge verification remains required. If acceptance fails, Chef keeps or reopens the same GitHub issue and returns the same Kanban card with the unmet criteria.

Material decisions include syntax and semantics, data layout, ownership, dependencies, error representation, parsing or execution strategy, testing and benchmark policy, and the selection or rejection of alternative approaches. Routine keystrokes and mechanically implied edits do not need entries.
