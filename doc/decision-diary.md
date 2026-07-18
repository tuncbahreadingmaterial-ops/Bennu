# Bennu Decision Diary

Record material design and implementation decisions here as they are made. Keep this diary append-only: add a new entry to correct or supersede an earlier decision instead of silently rewriting history.

## Entry format

### YYYY-MM-DD — Short decision title

- **Related issue/PR:** Link or identifier
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
