# Product direction

- Bennu is a data-oriented programming language written in C++.
- Use [Anka](https://github.com/tuncb/anka) for language-design cues and experiments. Anka is inspiration, not a requirement for architectural or behavioral compatibility; document where Bennu deliberately differs.
- Optimize first for correctness, data integrity, a small coherent language, predictable costs, and measurable performance.

# C++ and architecture constraints

- Do not use object-oriented design.
- Keep data and behavior separate: represent data with plain structs and transform it with free functions.
- Do not add `private` or `protected` members. Structs expose their data directly.
- Do not add operator overloads.
- Do not add default function arguments.
- Do not use C++ exceptions for normal or recoverable errors. Do not throw exceptions from Bennu-owned code.
- Return errors explicitly. `tl::optional` and `tl::expected` may be used when optional- or expected-style results are appropriate.
- Prefer direct data flow, contiguous and simple data layouts, visible ownership and allocation, and shallow abstractions. Complexity must buy a concrete language or product capability.

# Validation ladder

Use the repository's CTest validation ladder for implementation work. The exact
commands and label taxonomy are documented in `doc/validation-ladder.md`.

1. During implementation and fixes, run the `tier.focused` invariant baseline,
   the affected `area.*` labels, and focused `bennu_tests` cases.
2. Before implementation handoff, run `tier.review` plus the affected-area
   tests. Do not repeatedly run complete validation when the diff is still
   changing.
3. After review findings are resolved, run `tier.full` in the ordinary Release
   build and the applicable `tier.strict` and `tier.sanitize` configurations.
4. Final isolated QA runs `tier.qa` and every acceptance-criterion-specific
   scenario. Focused or review validation never substitutes for final QA.
5. Report the exact build directory, configuration, commands, selected labels,
   and results so later agents do not repeat equivalent validation without a
   concrete reason.
6. Use the checked-in validation entry points instead of reconstructing host
   setup ad hoc:
   - on Windows, use `tools/validation/Invoke-BennuWindowsValidation.ps1` so
     MSVC, SDK, Unicode process launch, presets, and logs share one environment;
   - for WSL sanitizer QA, use
     `tools/validation/run-wsl-sanitize.sh`, which bootstraps pinned user-local
     CMake/Ninja tools without `sudo` or system package mutation; and
   - for WSL strict validation, use `tools/validation/run-wsl-strict.sh` so
     permission-sensitive fixtures run on the Linux filesystem instead of
     DrvFS; and
   - run long CTest selections through `tools/validation/run_ctest.py` so output
     is durable and an interrupted run can resume from CTest failover state.

# Issue workflow

If the user asks for issue workflow:

1. Inspect the issue, the current repository state, and the relevant code and documentation. Define concrete acceptance criteria before implementation. If an ambiguity would materially change the solution, ask the user for clarification.
2. Classify the issue:
   - Use the full workflow below for code changes, language changes, bug fixes, performance work, and other behavior-affecting changes.
   - For trivial documentation, comment, spelling, or formatting-only changes, use a lightweight workflow: create the branch, make the change, obtain one independent review, run the relevant focused checks, then push and create the PR. Use the full workflow whenever risk or scope is uncertain.
3. Create a branch for the issue. For a GitHub issue, name it `GH-<issue-id>-<brief-name>`.
4. Create an implementation agent. Give it the issue context, acceptance criteria, relevant Bennu constraints, and responsibility for running focused tests before handing off.
5. Create a separate review agent to inspect the complete diff. The reviewer must check:
   - correctness and coverage of the acceptance criteria;
   - tests, regressions, edge cases, and data integrity;
   - compliance with Bennu's C++ and data-oriented architecture constraints;
   - unnecessary complexity, hidden costs, ownership, and allocation behavior;
   - documentation of intentional differences from Anka when relevant.
6. Treat only concrete, actionable review findings as blocking. If findings exist, create a fix agent to address them, run the relevant tests, and then have a review agent inspect the updated diff. Repeat until there are no actionable findings. If agents repeatedly disagree about the same finding or progress stalls, summarize the disagreement and ask the user instead of looping indefinitely.
7. Update the issue branch with the latest `main` before final QA and resolve any integration conflicts on the issue branch.
8. Create a separate QA agent. The QA agent must:
   - create an isolated Git worktree from the latest `main`;
   - create a temporary QA branch in that worktree and merge the issue branch into it;
   - never modify or merge into the real `main` branch;
   - verify every acceptance criterion and run the relevant test suites, builds, static checks, benchmarks, or manual scenarios;
   - report the exact commands, environment assumptions, results, and any reproducible failures.
9. If QA finds an issue, fix it on the issue branch, rerun focused tests, obtain another review of the resulting diff, and repeat isolated QA. Continue until QA passes or a genuine blocker requires user input.
10. Confirm that the final diff contains only intended changes and that all required checks pass.
11. Push the issue branch and create a ready-for-review PR. Include the issue link, acceptance criteria, implementation summary, validation evidence, performance results when relevant, and any intentional Bennu/Anka differences.
