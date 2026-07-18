# Bennu Decision Diary

Record material design and implementation decisions here as they are made. Keep this diary append-only: add a new entry to correct or supersede an earlier decision instead of silently rewriting history.

## Entry format

### YYYY-MM-DD — Short decision title

- **Related issue:** Link or identifier
- **Context:** The problem, constraints, and relevant facts.
- **Decision:** What was selected, including its scope.
- **Alternatives considered:** Other credible options and why they were not selected.
- **Rationale:** Why this choice best fits the current constraints.
- **Consequences or follow-up:** Expected effects, risks, and any later work.
- **Validation/evidence:** Tests, measurements, review evidence, or other checks supporting the decision.

## Entries

### 2026-07-18 — Establish a minimal, normative contributor contract

- **Related issue/PR:** [Issue #1](https://github.com/tuncbahreadingmaterial-ops/Bennu/issues/1)
- **Context:** Bennu had no tracked files or implementation baseline. Before implementation begins, contributors need one repository-root contract that preserves the product direction, C++ architecture constraints, explicit error handling, scoped issue workflow, and ongoing decision record required by Issue #1. The issue explicitly excludes choices about syntax, execution model, C++ standard, build system, supported platforms, dependencies, and compiler or interpreter implementation.
- **Decision:** Bootstrap the repository with only root-level `AGENTS.md` and this diary. State the issue's contributor rules normatively and group them by product direction, C++ and architecture constraints, and engineering workflow. Treat Anka as linked design inspiration rather than a compatibility target. Use one append-only diary with a compact, reusable field-based entry format, and deliberately defer every listed non-goal.
- **Alternatives considered:** Copy Anka's architecture or behavior; add a build system or implementation skeleton; replace explicit constraints with generic style guidance; or create separate ADR files for each decision. These options were rejected because they would impose compatibility not requested by the issue, introduce speculative scaffolding, weaken discoverability and enforcement, or add unnecessary process for the project's current size.
- **Rationale:** Two focused Markdown files are the smallest complete bootstrap. A root `AGENTS.md` is automatically discoverable by contributors and agents, while a single diary makes decisions chronological and easy to inspect without selecting product or implementation details prematurely.
- **Consequences or follow-up:** Future contributions must follow `AGENTS.md`, document intentional differences from Anka, and append diary entries for material choices. Syntax, execution strategy, toolchain, platforms, dependencies, and implementation remain undecided until issue-driven work selects them. If a recorded decision changes, the correction must be a new entry.
- **Validation/evidence:** Both files were read back from the repository root and checked directly against every acceptance criterion in Issue #1. The complete untracked-file list contained only `AGENTS.md` and `doc/decision-diary.md`; inspection confirmed there is no compiler, interpreter, or unrelated scaffolding.

### 2026-07-18 — Adopt issue branches and local-main integration without pull requests

- **Related issue:** [Issue #3](https://github.com/tuncbahreadingmaterial-ops/Bennu/issues/3)
- **Context:** Bennu's contributor contract required issue-driven, scoped work but did not define how Coder isolates, integrates, or hands off an issue. The project needs a predictable path that gives Chef an already-integrated local result to verify while avoiding a GitHub pull-request review loop. A shared `main` also makes unconstrained concurrent integration unsafe.
- **Decision:** For every issue, Coder starts from a clean, current local `main`; creates one `issue/<number>-<short-slug>` branch; keeps scoped, issue-referencing commits on that branch; and runs all required validation there. After validation passes, Coder merges the branch into local `main`, pushes `main` to `origin`, leaves the GitHub issue open with exact branch, commit, merged-main, changed-file, push, and validation evidence, and blocks the matching Kanban card as `review-required`. Bennu does not use GitHub pull requests or GitHub code review. Unsafe integration stops and blocks with evidence. One active Coder issue is the default unless Chef explicitly establishes independent integration as safe.
- **Alternatives considered:** Use GitHub pull requests and reviews; commit issue work directly to `main`; leave validated issue branches unmerged for Chef; or allow multiple Coders to merge concurrently by default. Pull requests add a review mechanism the project deliberately does not want, direct commits remove issue-level isolation, unmerged branches shift integration work to verification, and default concurrency risks conflicts or remote divergence on the shared integration branch.
- **Rationale:** A short-lived issue branch provides isolation and auditable commits, while local merge plus a pushed `main` gives Chef one stable integrated range to inspect. Explicit evidence and stop conditions preserve independent verification without disguising integration as acceptance or forcing unsafe Git operations.
- **Consequences or follow-up:** Coder work is normally serialized. Local merge does not close the issue or accept the result; Chef still inspects the merged range, reruns tests, exercises the product, and either accepts the work or returns the same issue and card with exact unmet criteria. A dirty worktree, unexpected remote divergence, conflict, failed validation, or unrelated diff blocks integration until resolved.
- **Validation/evidence:** Issue #3 exercises this policy itself on `issue/3-local-main-workflow`. Validation reads both changed files from the repository root, checks the acceptance criteria and normative text, confirms only `AGENTS.md` and `doc/decision-diary.md` changed, inspects issue-referencing history and matching branch/main/origin SHAs, verifies fresh Hermes context discovery of root `AGENTS.md`, and confirms no pull request exists for the issue.
