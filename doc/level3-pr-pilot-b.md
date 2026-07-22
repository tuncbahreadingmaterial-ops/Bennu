# Issue #41 Level 3 PR Pilot B

This document is an Issue #41 non-product control-plane pilot. It changes no
Bennu language, runtime, CLI, backend, release, or workflow behavior.

```text
pilot: issue-41-b
pilot_revision: 1
qa_expected: accept
baseline: 1c6c385744bf67c3cc0e838b6b82c50439723b97
```

## Concurrent isolation

Pilot B starts from the same accepted `main` commit as Pilot A, but the two
pilots have separate mutable state:

| Pilot | Worktree | Branch | Tracked document | Build/output scope |
| --- | --- | --- | --- | --- |
| A | `/home/tunc/workspaces/Bennu-issue41-pilot-a` | `issue/41-pilot-a` | `doc/level3-pr-pilot-a.md` | Pilot A worktree only |
| B | `/home/tunc/workspaces/Bennu-issue41-pilot-b` | `issue/41-pilot-b` | `doc/level3-pr-pilot-b.md` | Pilot B worktree only |

Neither pilot shares a branch, tracked mutable file, build directory, or
untracked-artifact set with the other.

## Intentional stale-base setup

This pull request intentionally remains open while Pilot A integrates. A later
dedicated Issue #41 stage will use the resulting stale base to prove that
integration is refused until the branch is refreshed and current CI and QA
evidence is produced. This pilot-generation task does not update the branch
after Pilot A merges.
