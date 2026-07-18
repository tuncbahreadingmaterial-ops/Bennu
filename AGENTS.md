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

### Issue branches, local-main integration, and two-stage verification

For Issue #6 and later implementation work, create an explicit two-card Hermes Kanban graph before dispatch:

```text
Implement #<number> (coder) -> Verify #<number> (chef)
```

The verification card must name the implementation card as a parent, so it remains dependency-gated until implementation is done. Do not create a second runnable implementation card for the same stage.

The Coder implementation card must use this sequence:

1. Start only after the issue's product dependencies and the shared-main serialization gate are satisfied. Confirm the working tree is clean and local `main` is current with `origin/main`.
2. Create one dedicated branch named `issue/<number>-<short-slug>` from `main`.
3. Keep every change for the issue on that branch. Make one or more scoped commits that reference the issue number.
4. On the issue branch, run the issue's required build, tests, runtime checks, and diff-hygiene checks.
5. After Coder's validation passes, merge the issue branch into local `main` and push the resulting `main` to `origin`.
6. Wait for all required CI on the integrated `main` SHA. Do not open a GitHub pull request and do not request or perform GitHub code review; Bennu uses neither.
7. Leave the GitHub issue open. Comment on it with the branch name, issue commit SHA or SHAs, resulting merged `main` SHA, exact changed files, push result, exact validation evidence, and required CI URLs and results.
8. Complete only the implementation card after the merge, push, CI, and durable GitHub handoff all exist. Implementation completion means ready for independent verification, not accepted.

If integration is unsafe because the worktree is dirty, the remote has unexpectedly diverged, the merge conflicts, validation or CI fails, or unrelated changes are present, stop and block the implementation card with evidence instead of forcing integration or marking the stage done.

After its parent completes, the Chef verification card automatically becomes ready. Chef must independently inspect the merged range and acceptance criteria, rerun relevant validation, exercise the required user or runtime behavior, probe adjacent and error cases, and confirm required CI before deciding acceptance. On success, Chef comments on the GitHub issue with exact evidence, closes the issue, and completes the verification card.

On verification failure, Chef keeps or reopens the GitHub issue and records the exact unmet criteria and evidence. Chef then creates the retry graph `failed verifier -> Coder correction -> Chef re-verification`: the correction card names the failed verifier as its parent, and the re-verification card names the correction card as its parent. After both downstream cards exist, Chef completes the failed verification-attempt card with structured metadata containing `accepted: false`, the correction task ID, and the re-verification task ID. This terminalizes the rejected attempt so its correction can become ready; it does not accept the product work. Only a later successful Chef verifier may record acceptance, close the GitHub issue, and complete the verification stage. The correction and re-verification stages follow the same integration and independent-evidence contracts.

Because Coder integrations share local `main`, allow only one active Coder integration at a time. Chef verification must not overlap an active Coder integration in the same checkout. Concurrent verification is allowed only from an isolated clean checkout that cannot modify or disturb the shared repository state.

Telegram terminal-event subscriptions and the existing three-hour stewardship cron remain notification and reconciliation safeguards. They do not replace the Kanban parent edge and are not the primary implementation-to-verification trigger.

Material decisions include syntax and semantics, data layout, ownership, dependencies, error representation, parsing or execution strategy, testing and benchmark policy, and the selection or rejection of alternative approaches. Routine keystrokes and mechanically implied edits do not need entries.
