# Build specs

Specs for work not yet built: each is self-contained (context, scope, the files it touches, a
phased plan, acceptance and known gotchas). [CONVENTIONS.md](../Process/CONVENTIONS.md) is binding
for every one.

## Index

| Spec | Size | State |
|---|---|---|
| [online.md](online.md) | L | APPROVED 2026-11-22, not started: online services (achievements, stats, leaderboards, cloud saves, presence) behind a backend plugin, Steam's first, O0 to O7 |
| [scene-storage.md](scene-storage.md) | L | APPROVED 2026-11-27, not started: the scene's storage and stepping (paged component index, a list of dirty transform tops, entity data in columns, change lists a consumer clears with `Get` const and `Edit`, concurrent systems in AsyncUpdate), measured first, steps 0 to 5 |
| [terrain-improvements.md](terrain-improvements.md) | L | PROPOSED 2026-11-14: detiling, steep faces, distance scale, geomorphing, ground rules, authoring, T0 to T6 |
| [2d-games.md](2d-games.md) | L | PROPOSED: 2D inside the 3D world, L0 to L7 |
| [whiteboxing.md](whiteboxing.md) | M | PROPOSED and reviewed 2026-09-23: parametric blockout pieces, P0 to P3 |
| [script-run-modules.md](script-run-modules.md) | M (L with P2) | PROPOSED 2026-09-26: one script module per run, hot reload that migrates |

A spec moves to `../Archive/<name>-history.md` once it is built (or made obsolete), with a banner
saying so, and leaves this index; whatever it still owes goes to the backlogs in `../Plans/`
(`weekly_backlog.md`, or `backlog-0.1.md` / `backlog-0.2.md` for a release). The sweeps of
2026-11-02 and 2026-11-27 archived every built spec of their time and put their leftovers under
"Left over from the specs archived ..." in `weekly_backlog.md` (the large ones of 2026-11-27 in
`backlog-0.2.md`).

Sizes: S = a session or less, M = a few sessions, L = a multi-session track landing in phases,
green after each.
