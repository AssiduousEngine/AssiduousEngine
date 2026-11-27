# Engine systems as plugins

> APPROVED 2026-11-26 (user), built on branch `systems-plugins`. Nothing here is built yet. Read on master at `6d58c84d9`. Particles, terrain
> with vegetation, and navigation move out of the engine into official plugins, after the
> groundwork every move shares. The script languages already have their own approved spec,
> [script-backends-as-plugins.md](script-backends-as-plugins.md), which needs the refresh listed
> here before it is built. Physics and audio stay in the engine (user, 2026-11-26).

## Goal

A system a project does not use is not part of it: no components in its inspector's add menu, no
asset types in its New menu, no pages, no script surface, and no code in a player built for the
project. A project enables the systems it uses, the way it enables Dialogue today.

What this does not change: official plugins are compiled into the export templates, dormant unless
a project enables them ([plugins.md](plugins.md), Decided 2). A template player keeps carrying
every system; only a player built for one project gets smaller. The gain is a cleaner engine with
a smaller core, and projects that show only what they use.

## Decided (user, 2026-11-26)

1. **Physics and audio stay in the engine.** Seven of the nine sample projects use each, the
   templates would carry them anyway, and what other systems need from physics already goes
   through scene capabilities (`ISceneCharacterMotion`, `ISceneRayQuery`, `IStaticGeometrySource`,
   `IStaticColliderSource`). Their unused imports and links are cleaned up regardless (G6).
2. **Candidates:** particles, terrain and vegetation (one plugin), navigation, and the script
   languages (their own spec).

## Is it feasible

Yes, for each candidate. None is drawn by code hard-wired in the renderer, none is called by name
from the frame loop, and each reaches the engine through registries a plugin part already has.
What is missing is mostly the other half of those registries: a plugin loads in the editor when a
project opens and unloads when it closes (plugins.md, X3), and several registries a system uses
have no way to remove what it added.

**How each draws today** (checked):
- Particles: `ParticleSubsystem` registers a `ParticleRenderer` through
  `RenderSubsystem::RegisterRenderer` and each scene's manager through `RegisterProvider`
  (`Engine.Render/RenderSubsystem.cppm:131,140`). The manager stamps the Transparent category, so
  particles sort with other transparents in the existing blended pass. Simulation is on the CPU.
- Terrain: the same two calls; shadows and picking go through the `Renderer` interface's own
  methods (`Foundation/Render/Pipeline.cppm`). Vegetation draws through the shared mesh renderer
  as an `IRenderDataProvider`.
- The one hard-wired piece is shaders: `particle*.hlsl` and `terrain*.hlsl` live in
  `Data/Engine/Shaders` and load by name; `ShaderSystemHost` reads only `Engine/Shaders/` or its
  pack (`Shaders.System/ShaderSystemHost.cppm:11-19`). Imgui, already a plugin, keeps its shaders
  there too.

**How they register today** (checked): each system's domain is in `kAllDomains`
(`Engine.Composition/CompositionImpl.cpp`), its subsystem is added by hand in
`DefaultApplicationImpl.cpp`, its asset types in `Pipeline.Registration/RegistrationImpl.cpp`, its
pages in `Tools.Editor/Main.cpp`. Each has a plugin equivalent (`EnginePluginServices`,
`context.Subsystems()` as Imgui's part does, `PipelinePluginServices`, the editor's page
factories), except one: each of these domains sets `installScene`, and
`EnginePluginServices::AddDomain` refuses such a domain (`PluginServices.cppm:42-43`,
`CompositionImpl.cpp:113`). Its scene managers have to come through `AddSceneManager` instead.

## Groundwork (shared by every move)

**G1. Removal for what a plugin registers.** Each registry below gets the remove half, used by
the part's unload, with a test that loads, unloads and loads again:
- `RenderSubsystem::RegisterRenderer` and `RegisterProvider` (only register today; providers go
  only with their scene) and `RendererRegistry::Register` (`Foundation/Render/Pipeline.cppm`).
- `ExtendScene` (`Script.Facades/ScriptFacades.cppm:765`), which adds `scene.particles` and the
  like through `core::ExtendProperty` and has no removal.
- `ThumbnailService::RegisterGenerator` and `RegisterSceneGenerator`
  (`ThumbnailService.cppm:203,208`).
- The viewport tool provider and tool panel registries (`ViewportTools.cppm`, `ToolPanel.cppm`),
  used by terrain's sculpt and paint tools and vegetation's scatter.
- The inspector extras registry and the component gizmo registry, used by navigation, which are
  also not offered to plugins today (`InspectorExtras.cppm`, `ComponentGizmosImpl.cpp`).

A static player never unloads, so this is for the editor; without it, closing a project leaves
the editor holding pointers into a closed plugin.

**G2. Domains split for plugins.** Each candidate's domain keeps its types, factories and script
facade; its `installScene` becomes one `AddSceneManager` contribution per manager. Update order
comes from each system's `UpdateOrder()`, not install order (`SceneSystem.cppm`), so contributing
at the tail keeps the order (to be confirmed by the tests that run a frame).

**G3. C# sees plugin facades.** `Tools.DotNetBindings` generates `Engine.g.cs` from
`RegisterAllScriptFacades` and `RegisterAllSceneComponentReflection` (`Main.cpp:26-28`), the
engine's domains alone: Dialogue's facade is already missing from C#. The generator loads the
plugins the build carries (`UTIL_HOST_PLUGINS`) before it reads the registry, so moving a system
out does not take it from C#. Holdout (C#) uses `NavAgentComponent` and would break without this,
so G3 comes before navigation moves; particles and terrain have no C# user. Its test builds C#
that calls a plugin's facade and runs it with the plugin loaded, not a search of the generated
file. Bindings cover the plugins the engine ships; a project's own packages need bindings built
per project, which belongs to the script backends spec's C# phase. C# support for Dialogue itself
(a C# game running a conversation) comes after this spec (user, 2026-11-26).

**G4. A prefab override of an unknown component survives.** Scenes keep the records of a
component whose plugin is absent and write them back (`Scene.Resource/SceneResource.cppm:454`),
but an override of such a component's fields on a prefab instance is dropped with a warning
(`:736-741`). Opening a project with a plugin missing would then lose edits on save. It must be
kept, as the whole records are.

**G5. Plugins load before scenes and the script surface.** Checked on the branch: it holds
already. The player loads the project's plugins after the base startup and before the initial
scene (`Engine.Player/PlayerApplication.h`, the `CreatePluginHost` call in its configure), so a
part finds the engine's subsystems, and a subsystem a part adds to the running manager is brought
up at once (`SubsystemManager.cppm`). The editor loads them at project open, before any page reads
their types, and reloads them by capturing and restoring every live scene
(`Editor.App/EditorSession.cppm`, `LoadPlugins`, `ReloadPlugins`). A part's domain registers its
facade after the engine's (`RegisterAllScriptFacades` runs in the app's configure), before any
script compiles; `AddSceneManager` reaches live scenes and later ones. Integration.Plugins covers
each: NativeSample's component turning a player's scene, AngelScript and Luau reaching a plugin's
component and Dialogue's facade, a package's contributions arriving and leaving in the editor and
the cook. What no plugin has done yet is register a renderer from a subsystem's `OnReady`, as
particles and terrain do; their moves test it in the player and the editor.

**G6. Dead links and imports go.** Imports and links that use nothing, found in the study:
particles in `Engine.Player/PlayerMain.cpp`, `WebMain.cpp` and `Tools.Export`; terrain and
vegetation resources in `Editor.Scene` and `Engine.DefaultApp`; physics likewise (PlayerMain,
WebMain, `Editor.Play/GamePage`, Tools.Export) and the uncalled `DefaultApplication::Physics()`;
audio in `Editor.Scene` and Tools.Export. The player's audio imports stay: `PlayerApplication.h`,
included after them, applies the bus layout and the user's volumes. Each is checked by building
without it.

**G8. A plugin's own Foundation library.** Each candidate has Foundation code: its types, its
resources and their factories, and the wrapper over its third-party library. It fits none of the
parts, which follow the hosts that load them (plugins.md, decision 2): the Pipeline and Editor parts
need it as much as the Engine part does. A package therefore also holds `<Name>.Foundation`
(`Plugins::<Name>.Foundation`), a library that is not a part:
- No host loads it and it has no entry, so the manifest's `parts` does not list it.
- It links Foundation libraries and third-party code only, as the engine's own Foundation layer
  does.
- The parts link by what they use, not in a chain (today each part links the previous one
  PUBLIC, `cmake/Plugins.cmake`, `util_add_plugin_part`, so a Pipeline part reaches the plugin's
  runtime code). Decided (user, 2026-11-26): the Engine and Pipeline parts each link the
  Foundation library alone; Tooling links Pipeline; Editor links Tooling and Engine. What a host
  loads is unchanged: the cook, export and MCP roles still load the Engine parts
  (`UTIL_PLUGIN_ROLE_cook_KINDS`), since a level read into a cook's scratch scene needs the
  plugin's scene managers. The gain is the build graph, not a lighter cook.
- A new helper, `util_add_plugin_library(Foundation)`, builds it the way `util_add_engine_library`
  builds an engine library: static, or shared in a shared build, so the parts in every host share
  one copy of its registrations.
- In a shared build it ships beside the parts: the plugin library list the build writes
  (`plugin-libraries.txt`) and export's staging carry it.
- Its tests are `<Name>.Foundation.Tests`.

plugins.md gains the library kind and the links with it. Dialogue changes with them: its Pipeline
part uses the Engine part's types today (`DialogueSettings`, `DialogueConversation`,
`DialogueStep`). Those partitions import Foundation alone, so they become `Dialogue.Foundation`:
`:settings`, `:conversation`, `:cast` and `:resources`. The Engine part keeps what plays a
conversation and needs the engine: `:runner`, `:run`, `:stockbox`, `:facade` and `:linetrack`
(whose implementation shows lines through the run's runner).

**G7 (optional). Plugin shaders.** A plugin's own shader folder, mounted beside `Engine/Shaders`
in development and cooked into the pack at export. Until it exists, a plugin's shaders stay in
`Data/Engine/Shaders`, as Imgui's do; that costs only pack size.

## The plugins

Each is an official plugin under `Code/Plugins/<Name>/`, with a `Plugin.xml` for Linux64, Win64,
Web and Android64, added to `UTIL_HOST_PLUGINS` (`CMakeLists.txt:677`) so every template carries
it. Each manifest lists its types' data versions.

**Particles** (`particles`), size M.
- `Particles.Foundation`: `Foundation/Particles` and `Particles.Resource`.
- With the two in one library, a system references its texture, mesh and materials directly.
  Today `Foundation/Particles` links Core alone, since `Texture` and `Materials` bring the RHI, so a
  `ParticleSystem` holds bare GUIDs (`textureRef`, `meshRef`, `materialRefs`,
  `ParticleEffect.cppm:159-174`) and `ParticleEffectResource` keeps parallel arrays of proxies the
  factory binds from them (`ParticleEffectResource.cppm:276-323`). The system holds
  `Ref<Texture>`, `Ref<StaticMesh>` and `Array<Ref<Material>>` instead, bound as a component's
  references are; the parallel arrays and the factory's binding go, and the effect page and the
  pipeline set references rather than GUIDs. Its own commit after the move, with the effect's
  serializer version bumped if the stored form changes.
- Engine part: `Engine.Particles`. Its load adds the subsystem, the scene manager and the domain
  with the `scene.particles` facade.
- Pipeline part: `Particles.Pipeline` (the effect asset, its builder and creators).
- Editor part: `Editor.Particles` (the effect page) and the particle thumbnail generator, which
  moves out of `Editor.Scene` (`AssetThumbnailsImpl.cpp`).
- Projects: Sky Hopper, PaperKid, Snowline, Lamplight.

**Terrain** (`terrain`: terrain and vegetation together; one plugin, named for what a project adds, since vegetation links terrain), size M to L, about 13k lines moved.
- `Terrain.Foundation`: `Foundation/Terrain`, `Terrain.Resource`, `Vegetation`,
  `Vegetation.Resource`.
- Engine part: `Engine.Terrain`, `Engine.Vegetation`.
- Pipeline part: `Terrain.Pipeline`, `Vegetation.Pipeline`. Editor part: `Editor.Terrain`,
  `Editor.Vegetation` with their viewport tools, panels and the splatmap thumbnail.
- The heightfield stays in the engine: physics uses it. Its resource module lives in
  `Foundation/Heightfield.Resource`, but only `TerrainDomain` registered it
  (`Engine.Terrain/TerrainComponentsImpl.cpp:147`); physics' domain registers it instead.
- Vegetation's scatter tool reaches `PhysicsSceneSystem` directly; it moves to a scene capability
  for overlap queries, so the plugin does not link physics' internals.
- Projects: Snowline only.

**Navigation** (`navigation`), size M.
- `Navigation.Foundation`: `Foundation/Navigation` and `Navigation.Resource`, and Recast and Detour
  with them.
- Engine part: `Engine.Navigation`. It needs no physics: the bake asks every scene system through
  `IStaticGeometrySource`.
- Pipeline part: `Navigation.Pipeline`. Editor part: `Editor.Navigation`, its inspector extras,
  gizmos and preferences (preferences register before the app is built today, plugins load at
  project open: they register at load instead).
- Tooling part: the `navigation_bake` MCP tool, which lives in `Editor.Scene`
  (`SceneMcpToolsImpl.cpp`) and needs a scene page; the plugin's MCP services gain that access.
- Projects: Lamplight, PaperKid, Masquerade, Holdout.

## Script languages: what the spec needs before it is built

[script-backends-as-plugins.md](script-backends-as-plugins.md) still holds; these change it:
- C# export always builds a player for the project, with the game's code compiled in
  (`Editor.Project/ExportImpl.cpp`, `ChoosePlayer`), and links it through
  `dotnet_link_compiled_runtime`, a path outside `util_link_plugins`. The C# plugin's build or
  Tooling part has to own that link; the spec's "is it in the template" question does not apply
  to C#.
- Places that name a language and that the spec does not list: an `"angelscript"` default in
  `Engine.Behavior/BehaviorSubsystem.cppm`, `Editor.Project/ExportImpl.cpp` and
  `Script.Pipeline/ScriptAsset.cppm`, and `Editor.Mcp/ScriptValidate.cppm`'s own language list,
  which already leaves out C#.
- Its first phase is partly done: a run with only native behaviors never starts a VM
  (`ScriptBehaviorClasses`). The build still requires AngelScript or Luau (`CMakeLists.txt`).
- Line numbers have moved; it is re-read at the current master.

## Data

Each move bumps the project data version (13 before particles) with a migration that adds the
plugin to the `plugins` list of every project written before the move, the shape Millbrook's
`dialogue` entry has (decided, user, 2026-11-26): whatever a project did with the system, a script
adding a component included, keeps working; a project that does not want it removes it. New projects
start without it. `ProjectSettings::kFormerlyBuiltIn` lists each system with the version it left at
(particles at 14). Type names and hashes stay as they are, so existing files read unchanged. Every
sample project is migrated by it and committed.

Each plugin's modules are named as Dialogue's are (`<name>.foundation`, `<name>.engine`,
`<name>.pipeline`, `<name>.editor`, decided, user, 2026-11-26), renamed in a commit of its own after
the move.

## Order of work

Each step green on the clang lane on its own; the other lanes when a plugin is done.

1. G6 (dead links and imports), G4 (prefab overrides), and the heightfield module registered by
   physics' domain instead of terrain's.
2. G1 and G2, with load, unload and reload tests through a test plugin.
3. G5, then G8.
4. Particles: the smallest, and it proves the render path and the editor unload.
5. Terrain.
6. G3, then navigation.
7. The script backends spec refreshed and brought back for approval.
8. G7 when a plugin's shaders are worth moving.

## Risks

- **Editor unload with a live scene.** Terrain's sculpt tool edits an asset live and its GPU
  caches go at `OnPrepareShutdown`; unloading a plugin while its scene is open must tear down in
  that order.
- **Unknown sequence tracks** (not checked): a sequence with a particle track opened without the
  plugin must keep it, as scenes keep components.
- **The script surface hash** differs once a project's set of plugins decides which facades exist;
  cooked scripts recook (plugins.md, X7). `scene.particles` is a property, not a global, so the
  per-platform check for hidden facades has to see it.
- **Tests and samples** that link these modules directly (Engine.Composition.Tests,
  Engine.Behavior.Tests, Editor.Scene.Tests, Integration.ShaderBlocks, Integration.Mcp, the
  playground samples) relink through `util_link_plugins`.
