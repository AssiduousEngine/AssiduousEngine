# Build specs

Specs for work not yet built: each is self-contained (context, scope, the files it touches, a
phased plan, acceptance and known gotchas). [CONVENTIONS.md](../Process/CONVENTIONS.md) is binding
for every one.

## Index

| Spec | Size | State |
|---|---|---|
| [stability.md](stability.md) | L | APPROVED 2026-11-13: the audit's re-checked findings, data loss first, S0 to S8 |
| [2d-games.md](2d-games.md) | L | PROPOSED: 2D inside the 3D world, L0 to L7 |
| [plugins.md](plugins.md) | L | APPROVED 2026-11-03: source plugins for every host and platform, X0 to X6 |
| [whiteboxing.md](whiteboxing.md) | M | PROPOSED and reviewed 2026-09-23: parametric blockout pieces, P0 to P3 |
| [script-run-modules.md](script-run-modules.md) | M (L with P2) | PROPOSED 2026-09-26: one script module per run, hot reload that migrates |
| [code-editing.md](code-editing.md) | L | APPROVED 2026-11-17, built 2026-11-18 (C# on dotnet; no entity handler yet): a project's Code folder, outside code editors, log references, E0 to E6 |
| [scripting-typed-calls.md](scripting-typed-calls.md) | L | APPROVED 2026-11-18: script speed measured against native code, then a typed call path and names bound once |
| [script-backends-as-plugins.md](script-backends-as-plugins.md) | L | APPROVED 2026-11-19: each script language a plugin; a gameplay layer that runs native behaviors with no VM, their handlers reflected methods found by name |
| [run-and-behavior-naming.md](run-and-behavior-naming.md) | M | APPROVED 2026-11-19: ApplicationRun, the Facade suffix, RunBehavior/SceneBehavior/EntityBehavior as base types |
| [entity-events.md](entity-events.md) | M | APPROVED and built 2026-11-19: domains post entity events to the scene; Engine.Integration and its bridges gone |
| [terrain-improvements.md](terrain-improvements.md) | L | PROPOSED 2026-11-14: detiling, steep faces, distance scale, geomorphing, ground rules, authoring, T0 to T6 |
| [sequencer.md](sequencer.md) | L | APPROVED 2026-11-20; Q0 to Q4 built (model, asset, scene player, component, clips, sounds, camera cuts, fades, the editor's panel, MCP tools), Q5 next: cutscene timelines, track kinds from each domain, holds for dialogue, Q0 to Q5 |

A spec moves to `../Archive/<name>-history.md` once it is built (or made obsolete), with a banner
saying so, and leaves this index; whatever it still owes goes to the backlogs in `../Plans/`
(`weekly_backlog.md`, or `backlog-0.1.md` / `backlog-0.2.md` for a release). The sweep of
2026-11-02 archived every built spec of the time and put their leftovers under "Left over from the
specs archived 2026-11-02" in `weekly_backlog.md`.

Sizes: S = a session or less, M = a few sessions, L = a multi-session track landing in phases,
green after each.
