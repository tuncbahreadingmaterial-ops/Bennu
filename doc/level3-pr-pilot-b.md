# Issue #41 Level 3 PR Pilot B

This document is an Issue #41 non-product control-plane pilot. It changes no
Bennu language, runtime, CLI, backend, release, or workflow behavior.

```text
pilot: issue-41-b
pilot_revision: 2
qa_expected: accept
baseline: dd4389a827f762fd02076df9a3d5285cb0dbf2a6
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

## Stale-base refresh

Pilot B remained open while Pilot A integrated, and the dedicated Issue #41
stage refused integration against the resulting stale base. Revision 2 merges
the integrated Pilot A `main` commit recorded above. Fresh current-head CI and
QA evidence are required before Pilot B can integrate.
