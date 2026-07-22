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
- Record material design and implementation decisions in the active GitHub issue's one stable file under [`doc/decisions/`](doc/decisions/README.md). Use lowercase ASCII `issue-<number>-<stable-kebab-slug>.md`; the issue number is authoritative, and the slug and path never change after creation.
- Each decision section must include the date, context, decision, alternatives considered, rationale, consequences or follow-up, validation or evidence, and any superseded file and heading.
- Keep each issue record append-only. Multiple decisions and corrections for the same issue append new sections to its existing file. Cross-issue supersession appends only to the later issue's file and does not modify the superseded record.
- Routine issue work must not edit `doc/decisions/README.md`, `doc/decisions/TEMPLATE.md`, the `doc/decision-diary.md` compatibility pointer, `doc/decisions/legacy-decision-diary.md`, or another issue's record.

### Historical Level 1 and Level 2 integration workflow

Level 1 and Level 2 used dedicated issue branches followed by serialized local merges into `main`, push-to-`main` CI, and independent verification after integration. Pull requests and GitHub review were deliberately excluded during those levels. That history remains recorded in this agreement and the [legacy decision diary](doc/decisions/legacy-decision-diary.md), but Issue #41 supersedes those integration instructions for Level 3; they are not an active alternative to the protected workflow below.

### Level 3 protected pull-request workflow

Use this success path for every independently executable Level 3 issue:

```text
Implement #N (Coder, isolated worktree)
  -> PR QA #N (Chef, independent clean worktree)
    -> Integrate and post-merge verify #N (Chef, protected main)
      -> Accept-route #N (success-only dependency gate)
```

Independent implementation roots and PR-QA stages may run concurrently. Integration into `main` is serialized. Work that genuinely depends on another issue waits for its predecessor's successful accept-route, not merely for an implementation card, open pull request, green CI, or merge.

#### Implementation stage — Coder

1. Start only after the issue's product dependencies are accepted. Re-read the live issue and comments, fetch the remote, and fail closed unless the dedicated worktree is clean and its `issue/<number>-<short-slug>` branch starts from the current accepted `origin/main`.
2. Use one isolated worktree, branch, build directory, and untracked-artifact set per issue. Never implement in another worker's worktree or share mutable build output across stages.
3. Keep the change within one issue and one pull request. Make scoped commits that reference the issue, append material decisions only to that issue's record under `doc/decisions/`, and avoid unrelated cleanup.
4. Run the required clean Release build, full CTest suite, issue-specific user journeys, strict/configuration checks, and diff hygiene before handoff.
5. Push the issue branch and open or update one pull request against `main`. Link it with `Refs #N`, never `Closes #N`, `Fixes #N`, or an equivalent auto-close keyword. Map every acceptance criterion to exact tests or evidence and record the changed files, material decisions, risks, validation commands/results, and exact head SHA.
6. Wait for all required pull-request checks on the exact current revision, including the Linux x64, Windows x64, macOS arm64 matrix and aggregate `PR Gate`. Complete only the implementation-stage card with the pull request URL and exact evidence. Coder does not approve, merge, close the issue, or claim product acceptance.

#### PR-QA stage — Chef / Verity Meridian

1. Work from a separate clean worktree. Read the live issue, pull request metadata and discussion, commits, complete diff, changed-file scope, and that issue's decision record; verify its append-only history and never modify Coder's worktree.
2. Check out and record the exact pull-request head and merge-ref SHAs. Independently run the required Release build, full CTest suite, acceptance journeys, and relevant adjacent, error, resource, generated, and native paths.
3. Confirm every required CI result belongs to the exact current revision and verify the source of the App-owned `Verity Meridian / QA accepted` check. Record durable findings and structured `accepted: true` or `accepted: false` evidence tied to that SHA.
4. On rejection, leave the pull request and issue open, emit a failed QA signal, create a focused Coder correction stage on the same pull-request branch, and parent-gate a fresh Chef re-verification stage on that correction. Terminalize the rejected QA attempt only with `accepted: false` and both retry task IDs. A later successful verifier alone may release integration.

#### Latest-base tree-identity verification after `main` moves — Chef / Verity Meridian

An old-base branch result is never acceptance. The review of an unchanged issue patch may be reused, but runtime and integration evidence from an old base may not. Use this path only when the previously reviewed patch head remains exact and unchanged:

1. Record the unchanged, previously reviewed pull-request head as `H` and the current accepted live `main` as `M`. In a unique clean verifier checkout, without mutating the pull-request branch, construct `S = merge(M, H)` with `M` as the latest base. Record `H`, `M`, the synthetic merge commit and tree, the construction commands, and the clean-checkout state.
2. If construction of `S` conflicts, stop. Conflict absence, file disjointness, old branch CI, and manual conflict resolution are not acceptance. Route a focused correction or refresh on the pull-request branch followed by fresh full independent QA; reuse no product evidence across the conflict.
3. If `S` is clean, run the full latest-base QA on `S`: the required build and tests, issue acceptance journeys, complete diff and scope checks, and relevant adjacent, error, resource, generated, and native paths. Record the exact commands and results against `S` and its tree.
4. Only after `S` passes, mechanically update the pull-request branch to integrate exactly `M` and record the resulting head as `H2`. The update may contain no product edits beyond integrating `M`. Independently prove the issue patch is unchanged, the update delta is integration-only, and `tree(H2) == tree(S)` exactly.
5. Require the Linux x64, Windows x64, macOS arm64, and source-bound `PR Gate` checks on exact `H2`. Verity Meridian may emit its App-owned acceptance check on `H2` only after independently proving the unchanged-patch, integration-only, and exact-tree identities. The full product QA need not be repeated on the byte-identical `H2` tree.

Strict up-to-date branch protection remains enabled throughout Level 3. Do not disable `required_status_checks.strict`, remove or weaken source-bound required checks, relax conversation or administrator enforcement, or substitute conflict absence for latest-base evidence. A native merge queue is deferred for Level 4.

#### Integration, post-merge verification, and acceptance — Chef

1. Immediately before integration, re-read the live pull request, branch, and `main`. Require the pull-request head to equal its exact accepted QA head, all exact-head source-bound checks to be current and green, all blocking discussion to be resolved, and the accepted integration tree to remain current. When the latest-base path was used, additionally require exact head `H2`, live `main == M`, and `tree(H2) == tree(S)`. Otherwise require the accepted head to remain up to date with `main` and its current merge-ref tree to equal the tree independently tested by PR QA. Refuse integration and route a new latest-base verification or correction if the live base moved, the head changed, or any patch, tree, check, or authority identity is absent, stale, failed, or mismatched.
2. Squash merge only through an exact-head guard such as `gh pr merge --match-head-commit <accepted-head> --squash`. Direct pushes, merge commits into `main`, stale acceptance, and unguarded merges are not valid Level 3 integration paths.
3. Independently prove the resulting `main` tree exactly equals the accepted pre-merge integration tree (`tree(S)` when the latest-base path was used). Wait for the complete push-to-`main` Main CI matrix and `PR Gate` on the exact resulting `main` SHA, then rerun focused acceptance from a clean checkout of that SHA. Pre-merge CI and tree identity do not replace post-merge verification.
4. Keep the GitHub issue open through merge. Only after merged-`main` tree identity, exact-SHA CI, and independent focused acceptance succeed may Chef comment the exact evidence, close the issue, record `accepted: true`, and complete the success-only accept-route that releases dependent work.

If any stage sees a dirty or unexpected worktree, remote divergence, conflict, unrelated diff, failed validation, missing authority, or mismatched SHA, stop and record the evidence instead of stashing, resetting, bypassing protection, or claiming completion.

Material decisions include syntax and semantics, data layout, ownership, dependencies, error representation, parsing or execution strategy, testing and benchmark policy, and the selection or rejection of alternative approaches. Routine keystrokes and mechanically implied edits do not need entries.
