# Online

> STATUS: APPROVED 2026-11-22 (proposed 2026-11-04, refreshed 2026-11-22 against the code and the
> user's rulings of that day, below). Sized L, in phases O0 to O7, green after each.
> Origin: the 0.1 release's last system (user, 2026-11-03): "Survey the reference engines", and
> "the point here is not to copy one reference or the other. It is to come up with a suitable API
> that fits our engine". Its first platform backend, Steam, is a plugin, which is why the plugin
> API (`Specs/plugins.md`) came first. Read CONVENTIONS.md first.

## Goal

A game reaches its platform's online services through one engine API, the same on every platform:
achievements, stats, leaderboards, cloud saves, rich presence and DLC. Which service answers is
the platform's: Steam on desktop when the project enables it, a Local backend everywhere else (the
web included) and in play in editor. A game written against the API runs, and behaves sensibly,
on a platform with none of those services.

Not goals: lobbies, matchmaking, voice and friends lists (later; the user ruled them out of 0.1),
purchases and stores, the platform's overlay UI, platforms other than Linux, Windows and the web
(the design leaves room for consoles and mobile; nothing for them is built here).

## Decided (user, 2026-11-03)

1. **0.1 scope**: achievements, stats, leaderboards, cloud saves, rich presence, DLC. Lobbies and
   voice later.
2. **The asynchronous shape is the `Http` facade's**: a call that can wait returns a request id and
   takes a handler; the handler runs on the game's thread; `cancel` drops it.
3. **Not a copy of a reference**: an API that fits this engine.
4. **Steam is a plugin**, the plugin API's first customer, on Linux64 and Win64.

## Decided (user, 2026-11-22)

5. **The Steam backend is a plugin whose dependency lives outside the tree.** The Steamworks SDK is
   never in the repository; when a project enables the plugin, the SDK must be provided, and the
   engine says so when it is not (Decision 8). The official export templates are built without it,
   so they do not carry the plugin, and a project that enables it exports natively (Decision 7).
6. **Achievement, stat, leaderboard and DLC ids are the game's, the same on every backend.** A
   backend whose platform assigns its own ids may map them in its plugin settings (Decision 4).
   Steam needs no map: its API names are chosen on Steam's side to match.
7. **Play in editor uses the enabled backend plugin**, as it uses every other enabled plugin; Local
   only when no plugin provides a backend (Decision 5).

## What exists (checked 2026-11-22)

- **The `Http` facade** (`Code/Foundation/Http.Script/HttpScript.cppm`): static methods on an
  `Object` type (`get`, `post`, `send`, `cancel`), each returning an id (0 when the run has no HTTP,
  its handler then never called), a handler `IScriptDelegate` called on the game's thread from the
  binding's `Pump`. The run holds an `HttpScriptBinding` among its services
  (`Code/Engine/Engine.Run/ApplicationRun.cppm:344`, `Register<HttpScriptBinding>`); the facade
  finds it with `script::CurrentService<T>()`, and the client is made on the first request. `Reset`
  drops every request when the run's scripts stop.
- **Provider slots in the plugin manifest**: `PluginManifest::provides`
  (`Code/Foundation/Runtime/PluginManifest.cppm:222`, "named slots, at most one enabled provider
  each"), enforced as the host loads, the second provider refused with both names
  (`PluginHost.cppm:297`). The host tests already use `online.backend` with two providers.
- **What a plugin can do now**: be enabled per platform; carry typed settings
  (`PluginContext::Settings<T>()`); add a domain (its script facade, run as it is added), scene
  managers and per-run services (`EnginePluginServices::AddDomain`, `AddSceneManager`,
  `AddRunService`); ship deploy files beside the player, each named in its manifest's platform
  entry **relative to the package folder** (`StageDeployFiles`, `ExportImpl.cpp`). A platform
  service reaches scripts through an engine facade with per-platform providers, so a script never
  names a facade a platform may lack.
- **A part that is not built is refused by name**: "its Engine part is not built" for a project's
  package with no library, "is not in this build" for one the binary was linked without
  (`PluginHost.cppm`, `MissingPart`).
- **Native exports** (`ExportKind::Native`, `ExportImpl.cpp`): when the template does not carry an
  enabled plugin, a template export is refused with the reason, and a native export builds the
  game's own player from the engine checkout with the plugin linked in. The exporting machine needs
  the checkout and the toolchain.
- **Play in editor runs the enabled plugins**: the editor loads each enabled plugin's parts, and its
  runs get their domains, scene managers and run services (Millbrook's dialogue runs in play in
  editor).
- **Outside tools named in Preferences**: the Android export's SDK and JDK are folders named on
  Preferences' Export tab, else `ANDROID_HOME` and `JAVA_HOME`, checked before the export runs with
  a message naming what is missing (`CheckAndroidExport`). The pattern Decision 8 follows.
- **The `Save` facade** (`Code/Engine/Engine.Run/RunSave.cppm`, `Documentation/Shipping/
  Scripting.md`, "Saving"): typed values by key in one settings file the host names (the player
  in the user data directory, play in editor in the project's `Editor/` folder), written whole and
  atomically when the run ends or at `Save::flush()`. The web player mounts IndexedDB over the
  user data directory (`Engine.Player/WebMain.cpp:93`), so a file there persists in the browser.
- **The run's services** (`foundation.run`): per-run state a facade finds by type, app-wide state
  on the subsystem manager (`SubsystemManager::RegisterService`).

## Prior art (surveyed 2026-11-03, read again 2026-11-04)

- **Traktor** (`code/Online`): one `ISessionManager` over a provider (Steam, GameCenter, LAN, a
  SQLite Local), chosen at run time by class name from a setting, a per-provider configuration
  asset holding the app id and every achievement, stat, leaderboard and DLC id. Calls return a
  `Result` completed on one of two worker threads (Steam's callbacks are pumped there), its
  deferred function run on that worker; the getters block the calling thread until the first
  enumeration is done. Stats are int32 only. A failed Steam initialisation fails the application's
  start.
- **Flax** (`Source/Engine/Online`, `IOnlinePlatform`, and its MIT Steam plugin): every call is
  synchronous, returning true for failure; Steam's call results are waited on by spinning the game
  thread. Stats are float only; no DLC; rich presence is one status string. A failed Steam
  initialisation leaves the platform null, and every caller must check. The plugin commits the
  Steamworks headers and binaries.
- **LunarSong** (`PlatformServices`): a versioned C table of function pointers with capability
  bits (accounts, local and cloud saves, achievements, entitlements, platform UI, offline mode) and
  result codes (Ok, Unsupported, NotInitialized, InvalidArgument, NotFound, IOError, ProviderError,
  FileTooLarge); a missing optional function is Unsupported. Synchronous, Local only, chosen at
  compile time. No stats, leaderboards or presence.

What to keep: LunarSong's capabilities and result codes (a game asks what is supported instead of
what the backend is, and an unsupported call says so instead of failing silently); Traktor's
configuration of ids per game. What to avoid: every one blocks a thread or completes on a worker,
and none has a defined behaviour for a missing platform that a game can rely on.

**Steamworks** (facts from Valve's documentation, 2026-11-04):
- The SDK access agreement lets a developer reproduce the SDK in source form "solely to develop the
  Licensee Software" (§1.1(a)) and distribute only the `redistributable_bin` folder's libraries with
  the software, in object code (§1.1(b)); other rights are reserved (§1.3). Nothing grants
  publishing the headers. So `steam_api64.dll` and `libsteam_api.so` may ship beside a player; the
  SDK may not go into this public repository.
  (https://partner.steamgames.com/documentation/sdk_access_agreement)
- `SteamAPI_InitFlat` (SDK 1.59) initialises from a dynamically loaded library and reports why it
  failed (`ESteamAPIInitResult`: `NoSteamClient`, `VersionMismatch`, ...); with no Steam client it
  fails. `steam_appid.txt` beside the executable sets the app id for development and must not ship;
  `SteamAPI_RestartAppIfNecessary` relaunches through Steam when the game was started outside it.
- Callbacks and call results arrive in the thread that dispatches them: `SteamAPI_RunCallbacks`,
  or the manual dispatch API (`SteamAPI_ManualDispatch_*`) a host runs once a frame itself.
- Achievements and stats are synchronous in memory and uploaded by `StoreStats`, rate limited ("on
  the order of minutes"); since SDK 1.61 the client syncs the user's stats before the game starts
  (`RequestCurrentStats` is gone). Leaderboards are call results (find the board first; uploads
  limited to 10 per 10 minutes). Cloud saves are either Auto-Cloud (configured on Steam, no code) or
  `ISteamRemoteStorage` (files written and read by the game). Rich presence is up to 30 keys, its
  `steam_display` key a localisation token. DLC ownership is synchronous (`BIsDlcInstalled`).

## Decision 1 - two layers: a Foundation service and an Engine domain

- **`foundation.online`** (Foundation): the interface. The backend interface `IOnlineBackend`, the
  value types (an achievement's state, a stat, a leaderboard entry, a user), the result codes, the
  capabilities, and the request queue every backend completes through, which turns completions
  into handler calls on the thread that pumps it. A platform backend (the Steam plugin) and the
  tests' fake backend link this alone.
- **`foundation.online.local`** (Foundation): the Local backend, over a file and the definitions,
  separate as the script backends are from `foundation.script`. Only `engine.online` links it, to
  make it when no plugin provides a backend.
- **`engine.online`** (an Engine domain): the `OnlineSubsystem`, which owns the process's one
  backend (a platform service is one per process), pumps it each frame on the game's thread and
  stores stats at a rate the backend allows; a per-run `OnlineScriptBinding` among the run's
  services, holding the run's pending handlers (dropped, never called, when the run's scripts
  stop, as `Http`'s are); and the `Online` script facade over it. The subsystem is what falls
  back: it takes the backend the enabled plugin providing `online.backend` hands over through
  `OnlineBackends` (Decision 1b), and makes a Local one itself when there is none.

A native game part uses the same subsystem through C++ (`Function` handlers), never a backend.

## Decision 1a - the backend interface

`foundation.online` holds the interface a backend implements and the queue it answers through. A
sketch of the shape, the names to be settled in O0:

```cpp
namespace foundation::online
{
    enum class OnlineResult : u8 { Ok, Unsupported, Offline, NotFound, InvalidArgument, RateLimited, BackendError };
    enum class OnlineCapability : u32 { Achievements = 1, Stats = 2, Leaderboards = 4, CloudSaves = 8, Presence = 16, Dlc = 32 };
    enum class LeaderboardRange : u8 { Top, AroundMe, Friends };
    using RequestId = u64;

    struct OnlineUser { String id; String name; };
    struct StatValue { String id; f64 value = 0.0; bool integer = true; };
    struct LeaderboardEntry { i32 rank = 0; String name; i64 score = 0; bool isPlayer = false; };

    // A completion: the request it answers, how it went, and what it carries (the entries of a
    // fetch, the bytes of a cloud read), posted from any thread, delivered only by Pump.
    struct OnlineCompletion { RequestId request = 0; OnlineResult result = OnlineResult::Ok; String message; Variant payload; };

    class OnlineRequests
    {
    public:
        void Complete(OnlineCompletion completion);                     // any thread
        void Pump(const Function<void(OnlineCompletion&)>& deliver);    // the game's thread
    };

    class IOnlineBackend
    {
    public:
        virtual ~IOnlineBackend() = default;
        [[nodiscard]] virtual StringView Name() const noexcept = 0;     // "local", "steam"
        [[nodiscard]] virtual u32 Capabilities() const noexcept = 0;    // OnlineCapability bits
        // Offline when the platform cannot start (no client, an unknown app id); `requests`
        // outlives the backend.
        [[nodiscard]] virtual OnlineResult Start(const OnlineDefinitions& definitions, OnlineRequests& requests) = 0;
        virtual void Stop() = 0;
        // The platform's callbacks dispatched, on the game's thread, before the requests pump.
        virtual void Pump() = 0;

        // What the backend holds, answered at once.
        [[nodiscard]] virtual Optional<OnlineUser> User() const = 0;
        [[nodiscard]] virtual bool IsUnlocked(StringView achievement) const = 0;
        [[nodiscard]] virtual f32 Progress(StringView achievement) const = 0;
        [[nodiscard]] virtual Optional<StatValue> Stat(StringView stat) const = 0;
        [[nodiscard]] virtual bool OwnsDlc(StringView dlc) const = 0;

        // What may wait: each completes `request` through the queue, exactly once.
        virtual void Unlock(RequestId request, StringView achievement) = 0;
        virtual void SetProgress(RequestId request, StringView achievement, f32 current) = 0;
        virtual void StoreStats(RequestId request, Span<const StatValue> stats) = 0;
        virtual void SubmitScore(RequestId request, StringView board, i64 score) = 0;
        virtual void FetchScores(RequestId request, StringView board, LeaderboardRange range, i32 start, i32 count) = 0;
        virtual void ReadCloudFile(RequestId request, StringView name) = 0;
        virtual void WriteCloudFile(RequestId request, StringView name, Span<const byte> bytes) = 0;
        virtual void ResetForDevelopment(RequestId request) = 0;

        // Presence is set and forgotten: no answer to wait for.
        virtual void SetPresence(StringView key, StringView value) = 0;
        virtual void ClearPresence() = 0;
    };
}
```

- **Every method is pure**: a backend answers each operation, and one it does not support completes
  `Unsupported` (or reads its fallback) by its own code, so what a backend lacks is written down in
  it, not inherited by accident. `Capabilities()` says the same thing in advance.
- **The subsystem makes the request ids**, not the backend, so ids are unique across a backend
  replaced by a reload, and it routes each completion to the run that asked (its
  `OnlineScriptBinding`) or to a native caller's `Function`. A completion for a run that has
  stopped is dropped.
- **The thread rule is the queue's**: a backend may complete from any thread (Steam does not; a
  future one might), and nothing reaches a handler except through `Pump` on the game's thread.
- **Each frame**, the subsystem calls the backend's `Pump` (Steam dispatches its callbacks there,
  completing requests), then the queue's, delivering what completed.

## Decision 1b - how a plugin hands over its backend

`foundation.online` also holds the service a backend plugin finds, so the plugin links only
`foundation.online`:

```cpp
class OnlineBackends final : public foundation::runtime::IRegistrationRecorder
{
public:
    using Make = Function<UniquePtr<IOnlineBackend>(IAllocator&)>;
    // The backend this process uses; false when one is provided already (the slot makes that a
    // manifest error first).
    [[nodiscard]] bool Provide(StringView name, Make make);
};
```

- **The engine offers it to every host that runs games** (the player, the editor, the headless
  tools that play), as `EnginePluginServices` is offered: `host.Provide(backends)` and the loader's
  recorder, so what a plugin provides is recorded and taken back when it unloads.
- **The plugin provides a maker, not a backend**, from its Engine part's `Load`, capturing its typed
  settings:

```cpp
void Load(foundation::runtime::PluginContext& context)
{
    const steam::SteamSettings* settings = context.Settings<steam::SteamSettings>();
    if (auto* backends = context.Find<foundation::online::OnlineBackends>(); backends != nullptr && settings != nullptr)
    {
        (void)backends->Provide(u8"steam", [settings](IAllocator& allocator) { return steam::MakeBackend(allocator, *settings); });
    }
}
```

- **The subsystem makes the backend when the application starts**, and makes Local when nothing was
  provided. In the editor, a plugin that loads later (enabled while the project is open, or
  reloaded) is taken up at once: the subsystem stops the old backend, drops every pending handler,
  and starts the new one. When the plugin unloads, the recorder takes its maker back and the
  subsystem stops its backend first, then goes on with Local in the editor. A shipped player never
  loses its backend while it runs.
- **Tests use the same door**: the fake backend is provided through `OnlineBackends` by a test
  package that provides `online.backend`, so the subsystem, the facade and Local are proved through
  the route a real plugin takes.

## Decision 2 - asynchronous by request, answers on the game thread

Every operation that may wait (an upload, a fetch, a store) returns a request id and takes a
handler; the backend completes it whenever its platform answers, and the subsystem calls the
handler during its update on the game's thread, never inside the call and never on another thread.
`Online.cancel(id)` drops a handler. A handler hears `(ok, error, ...)`: `error` is a result code
name (`unsupported`, `offline`, `notFound`, `invalidArgument`, `rateLimited`, `backendError`) and a
message.

What the backend already holds is read at once, without a handler: whether an achievement is
unlocked, a stat's value, whether a DLC is owned, the signed-in user. This is the shape Steam has
since its client syncs stats before launch, and the Local backend's.

## Decision 3 - what a game can ask, per feature

A backend reports its **capabilities** (`achievements`, `stats`, `leaderboards`, `cloudSaves`,
`presence`, `dlc`); `Online.supports(name)` asks. An operation the backend does not support answers
`unsupported` (its handler called, or a read returning the fallback), never a crash and never a
silent success.

- **Identity**: `Online.backend()` (`"local"`, `"steam"`), `Online.signedIn()`, `Online.userName()`.
- **Achievements**: `unlock(id)`; `progress(id, current)` (unlocks at the definition's target);
  `isUnlocked(id)`; `achievements()` (each id, unlocked, progress). Clearing is the editor's and the
  Local backend's only (a development reset), never in a shipped player.
- **Stats**: `getInt`/`getFloat`, `setInt`/`setFloat`, `addInt`; written to the backend in batches
  by the subsystem (at most as often as the backend allows, and when the run ends), as `Save` writes
  its file.
- **Leaderboards**: `submit(board, score, handler)`, keeping the player's best unless the board says
  otherwise; `fetch(board, range, count, handler)` with `range` `top`, `aroundMe` or `friends`,
  each entry its rank, name, score and whether it is the player.
- **Cloud saves**: no new API. The `Save` facade's file is what syncs: the backend reads it before
  the run's first read and writes it when `Save` flushes (Decision 6).
- **Rich presence**: `setPresence(key, value)`, `clearPresence()`.
- **DLC**: `owns(id)`, `ownedDlc()`. Installing and buying are not here.

## Decision 4 - the definitions are a project asset

What a game has (its achievements with names, descriptions, hidden flags and progress targets; its
stats with kind, default and bounds; its leaderboards with sort order and score format; its DLC with
names) is an **Online Definitions** asset, set as the project's (a `onlineDefinitionsId` project
setting, as the default input map is). It cooks to a product every host loads, `OnlineDefinitions`,
a `foundation.online` type, so a backend reads it without the asset pipeline. The Local backend
works from it (it knows the names and targets without a platform), the editor edits it as a page,
an agent writes it through the asset tools, and an id a call names that the definitions lack
answers `notFound` in every backend, so a typo shows on Local before it reaches Steam.

The game's ids are the same on every backend (ruling 6). A platform that lets the developer choose
its ids (Steam's API names) is set up to match, and needs nothing more. A platform that assigns its
own (Google Play Games, the consoles) cannot match, so its backend's plugin settings carry a map from
the game's id to the platform's; an id the map lacks answers `notFound`, as one the definitions lack
does. The game's scripts never see a platform's ids.

## Decision 5 - which backend, and a platform without one

- The backend is the enabled plugin that **provides `online.backend`** on the platform; with none,
  the built-in **Local** backend. Local is the engine's, not a plugin, so every platform has one,
  the web included.
- A platform backend that cannot start (no Steam client, an unknown app id) leaves the subsystem
  **offline**: `Online.backend()` still names it, `signedIn()` is false, reads answer their
  fallbacks and operations answer `offline`. It does not fall back to Local in a shipped player: a
  player whose achievements went to a local file would never see them on Steam. Steam's own client
  queues achievements earned offline once it started.
- **Play in editor uses the enabled backend** (ruling 7): with the Steam plugin enabled and the
  Steam client running, a play in editor run reaches the developer's Steam account, as a
  development build started outside Steam does. With no backend plugin enabled it uses Local, its
  state in the project's `Editor/` folder (as `Save` keeps its own there). A development reset
  (clearing achievements and stats) is the editor's for either backend: Local clears its file,
  Steam's backend calls `ResetAllStats` (`ISteamUserStats`; that it is fit for this is checked in O6).
- The Local backend keeps its state in one file in the user data directory (in the browser's
  storage on the web): unlocked achievements, stats, a local table per leaderboard (the one user's
  scores), presence in memory, DLC owned as the definitions mark for development.

## Decision 6 - cloud saves are the save file

The `Save` facade stays the one save API. A backend with `cloudSaves` keeps the save file in its
cloud: it reads the cloud copy before the run's first read (the newer of the cloud and the local
file wins, the other kept beside it), and writes it when `Save` flushes. Steam does this through
`ISteamRemoteStorage` (so the engine decides when and what), rather than Auto-Cloud (configured on
Steam's site, which a project can still choose instead, with the plugin's cloud setting off).

Since the save slots spec (`Archive/save-slots-history.md`, built 2026-11-24) a game's saves are a folder,
`<project>.saves/`: the global store (`global.save.xml`) and a file per slot
(`slot-<name>.save.xml`). The cloud keeps the folder as files. Each write of a store is carried
there, and so is each delete, when it is made (deleting a slot removes the cloud copy too). Before
the run's first read each file is brought in by the newer of the cloud and the local copy, compared
by the time saved inside the store (its `SaveInfo` section), not a file date; a slot only in the
cloud is fetched. One case this leaves for O4 to settle: a machine that was offline when a slot was
deleted elsewhere still has it, and would bring it back (a record of deleted slots in the global
store is the likely answer).

## Decision 7 - the Steam plugin

- `Code/Plugins/Steam` (`plugins.steam`, namespace `steam`): an Engine part providing
  `online.backend`, on Linux64 and Win64; its typed settings: the app id, whether to restart through
  Steam, whether to sync the save file, and the id map (empty for Steam, Decision 4).
- **The SDK is an outside dependency** (Decision 8; its agreement grants no right to publish it).
  The plugin's manifest declares it, located by `STEAMWORKS_SDK_DIR` or the editor's Preferences.
  Where it is provided, the plugin's part builds with the engine and the SDK's redistributable
  library (`redistributable_bin/linux64/libsteam_api.so`, `redistributable_bin/win64/steam_api64.dll`)
  is its deploy file for the platform. Where it is not, the part is not built, and a project that
  enables the plugin is told which dependency is missing and where to name it.
- **Exports are native.** The official templates are built by CI without the SDK, so they do not
  carry the plugin; a project that enables it gets a native export, whose player the exporting
  machine builds with the SDK provided. No secret holds the SDK.
- The plugin talks to the flat C API (`steam_api_flat.h`) and starts with `SteamAPI_InitFlat`.
  Callbacks and call results are dispatched by the plugin with the manual dispatch API from the
  subsystem's update, on the game's thread, so nothing of Steam's runs on a thread of its own.
- **The app id for development runs** (play in editor, and a player started outside Steam) comes
  from the plugin's settings, through `steam_appid.txt` beside the executable or the SDK's
  environment variable, whichever the SDK in use honours; never in an export. A shipped player
  restarts through Steam when its setting says so.

## Decision 8 - a plugin's outside dependencies

The plugin API gains what the Steam plugin needs, for any plugin whose dependency cannot live in
the tree (a platform SDK under its own licence):

- **The manifest declares them**: each outside dependency has an id, a name to show ("Steamworks
  SDK"), the environment variable that locates it, and a file that must exist inside it, which is
  how the engine knows the folder is the right one.
- **Located as the Android SDK is**: a folder named in Preferences (an entry per dependency the
  enabled plugins declare), else the environment variable.
- **Checked where it matters**: enabling the plugin (in Project Settings or `plugin_enable`) reports
  a missing dependency at once; building the project's plugins, starting play in editor and
  exporting refuse with the dependency's name and where to name it, before any work. A plugin whose
  dependencies are provided behaves as any other.
- **Deploy files may come from a dependency**: a manifest's deploy entry may name a file inside a
  dependency (`steamworks:redistributable_bin/linux64/libsteam_api.so`), resolved against its folder
  when the export stages it, as a package-relative entry is resolved against the package.

## Phases

- **O0 - the service and the Local backend.** `foundation.online`: `IOnlineBackend`, the value
  types, result codes and capabilities, the request queue (completions to handlers on the pumping
  thread, cancel), and `OnlineBackends` (Decisions 1a and 1b); `foundation.online.local`: the Local backend over a file and a definitions
  object, tested through the interface as any backend would be. Tests: every
  operation's answer, unsupported and notFound answers, cancel, the file round trip, handlers only
  ever called from the pump.
- **O1 - the subsystem and the facade, achievements and stats.** `engine.online`: the subsystem
  (the backend chosen from the slot, else Local; offline when it cannot start), stats stored in
  batches and at run end, the per-run binding, the `Online` facade's identity, achievements and
  stats in both languages; play in editor on Local in `Editor/`. `Scripting.md` gains "Online".
  Tests: a script unlocks, reads and resets on Local; a run's pending handlers never run after it
  stops; a fake backend provided through the slot is the one used, and an offline one answers
  `offline`.
- **O2 - leaderboards, rich presence, DLC.** The rest of Decision 3 on the facade and Local, the
  fake backend extended. Tests: submit keeps the best, fetch ranges, presence keys, DLC ownership.
- **O3 - the definitions asset.** The asset type, its builder and creator, the project setting, the
  editor's page for it, `project_info` reporting it, an MCP tool reading and resetting play in
  editor's Local state (`online_state`), `McpGuide.md`. Tests: an id the definitions lack answers
  notFound; the page edits and saves; the tool resets.
- **O4 - cloud saves.** The save file through a backend with `cloudSaves` (Decision 6), the fake
  backend's cloud. Tests: the newer copy wins and the other is kept; a flush writes both.
- **O5 - a plugin's outside dependencies.** Decision 8: the manifest's declarations, Preferences
  and the environment variable, the checks at enable, plugin build, play in editor and export, and
  deploy entries resolved inside a dependency. Tests: a test package declaring a dependency is
  reported when enabled without it and refused at export with its name; provided, it exports with
  the dependency's file staged.
- **O6 - the Steam plugin.** Decision 7, built where the SDK is provided; its parts tested where it
  is (a Steam client running, the test app id 480) and its absence tested everywhere (the project
  told which dependency is missing, a template export refused for a native one). Play in editor on
  Steam checked by hand.
- **O7 - a sample uses it.** Sky Hopper (it has scores, stars and five levels) gains achievements,
  stats and a leaderboard, played on Local in the editor and the player.

## Gotchas

- **Rate limits**: Steam's `StoreStats` and leaderboard uploads are limited, so the subsystem
  batches stats and the Steam backend queues uploads, answering `rateLimited` only when its queue
  is full.
- **Leaderboards need finding first** on Steam (a call result); the backend keeps the handles.
- **Rich presence on Steam shows only localisation tokens** for `steam_display`; the docs say so,
  and the facade passes keys and values as given.
- **DLC under Steam Family Sharing**: ownership is the borrower's; the backend reports what Steam
  reports.
- **Tests cannot use Steam** on the lanes: a fake backend in the tests stands in for a platform,
  and the Steam plugin's own tests run only where the SDK and a client are.
- **One backend per process**: play in editor's instances share it (they are one editor process),
  so Local's state is the project's, not an instance's, and on Steam every instance is the same
  account.
- **Reloading the plugin in the editor**: the subsystem drops the backend before the plugin
  unloads and takes the new one after, with every pending handler dropped; Steam is shut down and
  started again (whether the SDK allows a second start in one process is checked in O6; if not,
  a reload keeps Steam running and only the plugin's code is replaced).
- **Play in editor on Steam is the developer's real account**: what a test run unlocks shows on
  their profile until the development reset clears it.

## Open questions

None. Decided with the approval: the file that proves a folder is the Steamworks SDK is
`public/steam/steam_api_flat.h`, since a folder without it cannot build the plugin.

## Tests (the contract)

Each phase lands with its tests (listed per phase). Across the spec: a fake backend, provided
through the `online.backend` slot as a plugin would provide it, proves the subsystem, the facade
and the Local backend behave alike for every operation, in both script languages, on the clang,
gcc, clang-shared, wasm (Local only) and ASAN lanes.
