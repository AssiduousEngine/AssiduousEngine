// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Behavior - the `engine.behavior` module.
//
// Entity behaviors: a EntityBehaviorSystem per scene instantiates each behavior's cooked
// BehaviorClass in the run's one gameplay script context, applies harvested defaults and
// hash-keyed overrides, and dispatches the declared lifecycle handlers (onStart through a
// deferred start, onUpdate(dt), onEnable and onDisable, onDestroy), gated to simulation
// (IsSimulationOnly: the editor's Simulate toggle and play-in-editor both count). A faulting
// behavior is disabled and logged; scripting keeps running.
//
// Context ownership: the subsystem's BehaviorRunHost owns the run's single IScriptContext,
// created lazily at first use (the first behavior instantiate, or the game script's
// StartRunBehavior, which shares it) and torn down when the run ends (game script released,
// no live instances, nothing simulating). The backend is resolved through the
// ScriptBackendRegistry by language; no backend type is ever named here.
//
// Hot reload: the cook swaps the product inside the resource handle; the dispatch loop
// notices the behavior's bound product pointer changed, re-instantiates, and re-applies
// the hash-keyed overrides (editor-set values survive; transient script state resets).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"
#include "Profiler/Profiler.h"

export module engine.behavior;

export import :components;
export import foundation.script.facades; // Entity, Log, Time, Random and the run-service binding

import foundation.core;
import engine.domain;
import foundation.runtime;
import foundation.run; // IRun: a scene's run, whose script host its behaviours use
import foundation.scene;
import engine.scene;
import foundation.resource;
import foundation.script;
import foundation.script.resource;
import foundation.profiler;

using namespace foundation::core;
using namespace foundation;
using namespace foundation::script;
namespace core = foundation::core;
namespace resource = foundation::resource;

export namespace engine::behavior
{
    // Foundation aliases (sibling engine::* namespaces would otherwise shadow these).
    namespace scene = foundation::scene;

    namespace scene = foundation::scene;

    // ---- the script event bridge: scene bus events reach on<Event> handlers ----

    /// The on<X> handlers that are not user bus events: the fixed lifecycle set and the reserved
    /// physics-contact handlers (those are delivered by the contact path, not the bus). Every
    /// other on<Upper> handler names a bus event, so declaring `onOrbCollected(x)` subscribes
    /// to the "OrbCollected" event, with no separate `events` list to keep in sync.
    [[nodiscard]] inline bool IsReservedHandler(StringView handler) noexcept
    {
        static const StringView kReserved[] = {
            u8"onStart",        u8"onUpdate",     u8"onEnable",      u8"onDisable",
            u8"onDestroy",      u8"onFixedUpdate", u8"onStop",       u8"onContactBegin",
            u8"onContactEnd",   u8"onTriggerEnter", u8"onTriggerExit"};
        for (StringView reserved : kReserved)
        {
            if (handler == reserved)
            {
                return true;
            }
        }
        return false;
    }

    /// The bus event name a handler subscribes to: "onOrbCollected" gives "OrbCollected" (verbatim
    /// after "on"). A behavior emits with that exact name (`scene.events.emit("OrbCollected", ...)`).
    /// Empty for anything that is not an `on<Upper>` handler.
    [[nodiscard]] inline StringView EventNameForHandler(StringView handler) noexcept
    {
        if (handler.Size() <= 2 || handler[0] != u8'o' || handler[1] != u8'n')
        {
            return StringView{};
        }
        const utf8char third = handler[2];
        if (third < u8'A' || third > u8'Z')
        {
            return StringView{}; // "on" and an upper-case letter only (the handler-scan convention)
        }
        return handler.SubStr(2, handler.Size() - 2);
    }

    /// One script system's lazy, deduped subscriptions on a scene's native EventBus. As classes
    /// bind, EnsureFor() subscribes (once per event name) to each non-reserved on<Event> handler
    /// they declare, routing every fired event to a supplied sink at bus-drain time. The bus drains
    /// at the scene tick's top level (no VM call active), so the sink may dispatch script handlers
    /// directly: emitting publishes and the bus drains later, which already provides the deferral
    /// that entity.send needs its message queue for. Owns its bus handles; Clear() drops them on
    /// scene stop.
    class ScriptEventSubscriptions
    {
    public:
        /// Wire the target bus and the sink (called with (eventName, payload) per fired event). The bus is
        /// tier-agnostic: a scene passes &scene.Events(), the run tier passes &instance.Events().
        void Bind(messaging::EventBus* bus, Function<void(StringView, const Variant&)> sink)
        {
            m_bus = bus;
            m_sink = Move(sink);
        }

        /// Subscribe (deduped) to every non-reserved on<Event> handler `scriptClass` declares.
        void EnsureFor(const BehaviorClass& scriptClass)
        {
            EnsureFor(Span<const String>(scriptClass.handlers.Data(), scriptClass.handlers.Size()));
        }

        /// Subscribe (deduped) to every non-reserved on<Event> among `handlers`: the tier that has no
        /// BehaviorClass resource (the Game tier, whose handlers are threaded in) uses this directly.
        void EnsureFor(Span<const String> handlers)
        {
            if (m_bus == nullptr)
            {
                return;
            }
            for (const String& handler : handlers)
            {
                if (IsReservedHandler(handler.AsView()))
                {
                    continue;
                }
                const StringView eventName = EventNameForHandler(handler.AsView());
                if (eventName.IsEmpty())
                {
                    continue;
                }
                const StringHash key(eventName);
                if (AlreadySubscribed(key))
                {
                    continue;
                }
                String name(eventName); // owned copy captured by the callback
                const u32 handle = m_bus->Subscribe(
                    key,
                    [this, name](const Variant& payload)
                    {
                        if (m_sink)
                        {
                            m_sink(name.AsView(), payload);
                        }
                    });
                m_keys.PushBack(key);
                m_handles.PushBack(handle);
            }
        }

        /// Unsubscribe everything (scene stop). The bus is scene-owned and still alive here; after
        /// this the sink is never called until EnsureFor re-subscribes on the next run.
        void Clear()
        {
            if (m_bus != nullptr)
            {
                for (const u32 handle : m_handles)
                {
                    m_bus->Unsubscribe(handle);
                }
            }
            m_handles.Clear();
            m_keys.Clear();
        }

    private:
        [[nodiscard]] bool AlreadySubscribed(StringHash key) const
        {
            for (const StringHash existing : m_keys)
            {
                if (existing == key)
                {
                    return true;
                }
            }
            return false;
        }

        messaging::EventBus* m_bus = nullptr;
        Function<void(StringView, const Variant&)> m_sink;
        Array<StringHash> m_keys; // dedup guard (one subscription per event name)
        Array<u32> m_handles;     // bus handles to unsubscribe on Clear
    };

    // ---- the run's script host (one gameplay context per run) ----

    // Logs behavior errors (Script category) and forwards to an optional external sink
    // (the editor's console and notice path).
    class RunErrorSink final : public IScriptErrorHandler
    {
    public:
        IScriptErrorHandler* external = nullptr;
        /// The last error's line, "<module> (line N): <message>", kept until taken: what a
        /// faulting call's caller reports as the reason.
        String last;

        void OnError(const ScriptError& error) override
        {
            if (error.kind == ScriptErrorKind::Warning)
            {
                foundation::script::LogScriptLine(LogLevel::Warning, error.module, error.line, error.column, error.message);
                if (external != nullptr)
                {
                    external->OnError(error);
                }
                return; // not the script's fault: `last` keeps the last error
            }
            last = Format(u8"{} (line {}): {}", error.module, error.line, error.message);
            if (error.kind == ScriptErrorKind::Compile)
            {
                foundation::script::LogScriptLine(LogLevel::Error, error.module, error.line, error.column, error.message);
            }
            else
            {
                // With the call stack below the message, innermost first, when the backend gave one.
                foundation::script::LogScriptFault(error.module, error.line, error.message, error.stack);
            }
            if (external != nullptr)
            {
                external->OnError(error);
            }
        }
    };

    /// Tracks the debugger's pause state for the run (the game-pause flag the scene tick
    /// checks) and forwards state changes to an optional external listener (the editor's
    /// debugger panels). Owned by the run host so the flag survives across the whole run.
    class DebugPauseTracker final : public IScriptDebuggerListener
    {
    public:
        IScriptDebuggerListener* external = nullptr;
        [[nodiscard]] bool Paused() const noexcept { return m_paused; }
        void Reset() noexcept { m_paused = false; }
        void OnDebuggerStateChanged(ScriptDebuggerState state) override
        {
            m_paused =
                (state == ScriptDebuggerState::Breakpoint || state == ScriptDebuggerState::Stepped);
            if (external != nullptr)
            {
                external->OnDebuggerStateChanged(state);
            }
        }

    private:
        bool m_paused = false;
    };

    /// Owns the run's manager and context and the behavior modules loaded into it. Each class
    /// loads into a module of its own, named by generation ("behaviors#N"): a fresh name per
    /// load gives hot reload clean semantics (live instances of the replaced class keep
    /// running on its old module until they end).
    /// Plain class (no runtime deps) so headless tests drive it directly.
    class BehaviorRunHost
    {
    public:
        // The allocator (required: the owner decides) becomes the run's script
        // manager tree root: every context, object and blob inherits it through
        // MemoryAllocator().
        explicit BehaviorRunHost(foundation::core::IAllocator& allocator) noexcept
            : m_allocator(&allocator)
        {
        }

        /// Where this host's context finds the application's services by type (the app's
        /// subsystem manager, wired by BehaviorSubsystem::ConfigureRunHost): what the context's
        /// resolver asks after this host's own binding and its run's services.
        void SetApplicationServices(Function<void*(TypeId)> services) { m_applicationServices = Move(services); }

        /// The service of type `type` for this host's scripts: the runtime binding, then the run's
        /// services (Binding().run), then the application's. What its context resolves through.
        [[nodiscard]] void* FindService(TypeId type) const
        {
            if (type == TypeOf<ScriptRuntimeBinding>().id)
            {
                return const_cast<ScriptRuntimeBinding*>(&m_binding);
            }
            if (m_binding.run != nullptr)
            {
                if (void* service = m_binding.run->FindService(type))
                {
                    return service;
                }
            }
            return m_applicationServices ? m_applicationServices(type) : nullptr;
        }
        void SetExternalErrorSink(IScriptErrorHandler* sink) noexcept
        {
            m_errorSink.external = sink;
        }

        // The game-script hold: true while a game script is loaded into this
        // host, so teardown accounting keeps the context alive. Per-host.
        void SetGameScriptHold(bool held) noexcept { m_gameScriptHold = held; }
        [[nodiscard]] bool HasGameScriptHold() const noexcept { return m_gameScriptHold; }

        // ---- step debugging (the play-in-editor debugger, or any other transport) ----

        /// Ask that this run be debuggable: a debugger is created on the run's manager
        /// (lazily, when the context exists) and `configurator` is called once with it so the
        /// caller can apply breakpoints and capture the pointer. The internal pause tracker is
        /// always the debugger's listener; a caller forwards through SetExternalDebugListener.
        void RequestDebugger(Function<void(IScriptDebugger&)> configurator)
        {
            m_debuggerConfigurator = Move(configurator);
            m_debuggerRequested = true;
            EnsureDebugger();
        }
        /// The run's debugger, or null (not requested, the backend lacks the capability, or no
        /// context yet). Contract-typed so a caller never depends on a backend.
        [[nodiscard]] IScriptDebugger* Debugger() const noexcept { return m_debugger.Get(); }
        /// Forward debugger state changes to an external listener (the editor's panels), on
        /// top of the internal pause tracking. Cleared on teardown.
        void SetExternalDebugListener(IScriptDebuggerListener* listener) noexcept
        {
            m_debugTracker.external = listener;
        }
        /// The game-pause flag: true while suspended at a breakpoint or step. The scene tick
        /// checks this to hold the world still; InvokeHandler checks it to tell a
        /// debug-suspended handler apart from a fault.
        [[nodiscard]] bool IsDebugPaused() const noexcept { return m_debugTracker.Paused(); }
        /// The last script error this run reported, then forgotten: the reason a faulting call
        /// gives. Empty when none was reported since the last take.
        [[nodiscard]] String TakeLastError() { return Move(m_errorSink.last); }

        [[nodiscard]] IScriptContext* ScriptContext() const noexcept { return m_scriptContext.Get(); }
        [[nodiscard]] IScriptManager* Manager() const noexcept { return m_manager.Get(); }
        [[nodiscard]] StringView Language() const noexcept { return m_language.AsView(); }
        [[nodiscard]] ScriptRuntimeBinding& Binding() noexcept { return m_binding; }

        /// One frame of this host's clock (what `Time` reads: the seconds since it was made and
        /// the frame's delta) and a frame-budgeted step of its collector, once a context exists.
        /// Whoever owns the host calls it once a frame: a game's run, an editor page's run, the
        /// subsystem for its default host. Only scripts read this clock.
        void Advance(f32 deltaSeconds)
        {
            m_binding.timeSeconds += static_cast<f64>(deltaSeconds);
            m_binding.deltaSeconds = deltaSeconds;
            if (m_manager.Get() != nullptr)
            {
                m_manager->StepGarbage(m_manager->Budget().collectSeconds);
            }
        }

        /// A full collection, where a pause is expected (a scene stopped or unloaded).
        void CollectAllGarbage()
        {
            if (m_manager.Get() != nullptr)
            {
                m_manager->CollectGarbage();
            }
        }

        /// The run context for `languageId`, created through the backend registry on
        /// first use. A second language during the same run is refused (one gameplay
        /// context per run) and logged once.
        [[nodiscard]] IScriptContext* EnsureContext(StringView languageId)
        {
            if (m_scriptContext.Get() != nullptr)
            {
                if (m_language.AsView() == languageId)
                {
                    return m_scriptContext.Get();
                }
                WarnLanguageMismatchOnce(languageId);
                return nullptr;
            }
            // The full curated surface: core math and the behavior facades, so a backend's
            // behavior-module framing and any explicit engine-type imports a script writes
            // resolve identically at cook and at runtime. Both idempotent.
            RegisterCoreTypes();
            RegisterScriptFacadeReflection();
            m_manager = CreateScriptManagerForLanguage(languageId, *m_allocator);
            if (m_manager.Get() == nullptr)
            {
                LOG_ERROR(u8"Script", u8"no script backend for language '{}'", languageId);
                return nullptr;
            }
            RegisterReflectedTypes(*m_manager);
            m_scriptContext = m_manager->CreateContext();
            if (m_scriptContext.Get() == nullptr)
            {
                m_manager = nullptr;
                return nullptr;
            }
            m_language = String(languageId);
            m_binding.timeSeconds = 0.0;
            m_binding.deltaSeconds = 0.0f;
            m_binding.random = core::Random{}; // fresh per-run RNG
            m_scriptContext->SetErrorHandler(&m_errorSink);
            const BehaviorRunHost* self = this;
            m_scriptContext->SetServiceResolver(
                IScriptContext::ServiceResolver{[self](TypeId type) { return self->FindService(type); }});
            EnsureDebugger(); // a debugger requested before the context existed attaches now
            LOG_DEBUG(u8"Script", u8"run script context created ({})", languageId);
            return m_scriptContext.Get();
        }

        /// The run context resolved by a script file's extension (the game-script path).
        [[nodiscard]] IScriptContext* EnsureContextForFile(StringView path)
        {
            StringView extension;
            for (usize i = path.Size(); i-- > 0;)
            {
                if (path[i] == u8'.')
                {
                    extension = path.SubStr(i + 1, path.Size() - (i + 1));
                    break;
                }
                if (path[i] == u8'/' || path[i] == u8'\\')
                {
                    break;
                }
            }
            const ScriptBackendRegistry& registry = ScriptBackendRegistry::Get();
            const ScriptBackendDesc* backend =
                extension.IsEmpty() ? nullptr : registry.FindByExtension(extension);
            if (backend == nullptr && registry.All().Size() == 1)
            {
                backend = &registry.All()[0];
            }
            if (backend == nullptr)
            {
                LOG_ERROR(u8"Script", u8"no script backend matches '{}'", path);
                return nullptr;
            }
            return EnsureContext(backend->languageId.AsView());
        }

        /// Instantiate a behavior class instance (loads or reloads the behaviors module as
        /// needed). Null on failure (logged), and for a class not of the kind `kind` the place it is
        /// set takes (a scene's Level slot a SceneBehavior, an entity's an EntityBehavior).
        [[nodiscard]] RefPtr<BehaviorObject> Instantiate(BehaviorClass& scriptClass, Span<Variant> args, BehaviorKind kind)
        {
            if (scriptClass.kind != kind)
            {
                const String is = scriptClass.kind == BehaviorKind::None
                                      ? String(u8"declares no behavior class")
                                      : Format(u8"derives from {}", BehaviorKinds::TypeName(scriptClass.kind));
                LOG_ERROR(u8"Script", u8"'{}' {}: it is set where a class deriving from {} goes", scriptClass.sourceName, is,
                          BehaviorKinds::TypeName(kind));
                return nullptr;
            }
            if (!EnsureClassLoaded(scriptClass))
            {
                return nullptr;
            }
            RefPtr<BehaviorObject> instance =
                m_scriptContext->CreateInstance(scriptClass.className.AsView(), args);
            if (instance.Get() == nullptr)
            {
                LOG_ERROR(u8"Script", u8"class '{}' failed to instantiate",
                                   scriptClass.className);
            }
            return instance;
        }

        /// Loads the class into the run's context unless it is already there. Each class is
        /// compiled and cooked alone, so it loads alone, in a module of its own: no class waits on
        /// another, a class that fails to load is not kept (so it fails no class after it), and a
        /// reloaded product (the same class name) loads beside its predecessor, whose live instances
        /// keep running; each language finds a class in any module it loaded, newest first. False on
        /// a load failure or a language mismatch.
        [[nodiscard]] bool EnsureClassLoaded(BehaviorClass& scriptClass)
        {
            const StringView language = scriptClass.language.IsEmpty()
                                            ? StringView(u8"angelscript")
                                            : scriptClass.language.AsView();
            if (EnsureContext(language) == nullptr)
            {
                return false;
            }
            usize replaced = m_loadedClasses.Size();
            for (usize i = 0; i < m_loadedClasses.Size(); ++i)
            {
                BehaviorClass* loaded = m_loadedClasses[i].Get();
                if (loaded == &scriptClass)
                {
                    return true;
                }
                if (loaded->className.AsView() == scriptClass.className.AsView())
                {
                    replaced = i;
                    break;
                }
            }
            // Carry the cooked bytecode too: a backend with per-class bytecode loads it instead of
            // compiling the source (the player path, no compiler). The section is the class's
            // sourceName, so breakpoints and errors key on the authored (file, line).
            Array<ScriptSection> includes; // the files its source includes, for a compile from source
            for (const ScriptIncludedSource& included : scriptClass.includes)
            {
                includes.PushBack(ScriptSection{included.path.AsView(), included.source.AsView()});
            }
            // A class built into its project's assembly (C#) loads the assembly: the backend loads an
            // image once, however many of its classes ask.
            const ScriptAssembly* assembly = scriptClass.assembly.Get();
            const Span<const byte> bytecode = assembly != nullptr ? Span<const byte>{assembly->image.Data(), assembly->image.Size()}
                                                                  : Span<const byte>{scriptClass.bytecode.Data(), scriptClass.bytecode.Size()};
            const Span<const byte> symbols =
                assembly != nullptr ? Span<const byte>{assembly->symbols.Data(), assembly->symbols.Size()} : Span<const byte>{};
            const BehaviorModuleClass entry{scriptClass.sourceName.AsView(), scriptClass.source.AsView(),
                                            Span<const ScriptSection>{includes.Data(), includes.Size()}, bytecode, symbols};
            ++m_generation;
            const String moduleName = Format(u8"behaviors#{}", m_generation);
            if (!m_scriptContext->LoadBehaviorModule(Span<const BehaviorModuleClass>{&entry, 1}, moduleName.AsView()).IsOk())
            {
                LOG_ERROR(u8"Script", u8"behavior class '{}' failed to load", scriptClass.className);
                return false;
            }
            if (replaced < m_loadedClasses.Size())
            {
                m_loadedClasses[replaced] = RefPtr<BehaviorClass>(&scriptClass);
            }
            else
            {
                m_loadedClasses.PushBack(RefPtr<BehaviorClass>(&scriptClass));
            }
            return true;
        }

        /// Total run teardown (the Stop bracket): drops the context, the manager, and
        /// every loaded class reference. Live BehaviorObjects held elsewhere keep the
        /// context alive until released, so callers destroy instances first.
        void Teardown()
        {
            if (m_scriptContext.Get() == nullptr && m_manager.Get() == nullptr)
            {
                return;
            }
            if (m_scriptContext.Get() != nullptr)
            {
                m_scriptContext->SetErrorHandler(nullptr);
            }
            // The debugger holds engine contexts (a paused one), so release it before the
            // manager and engine it borrows. Clear the request and configurator too: they capture
            // the caller (an editor GamePage) that may be destroyed before this host is reused, so a
            // stale configurator must never fire on a later EnsureDebugger (a use after free).
            m_debugger = nullptr;
            m_debugTracker.Reset();
            m_debuggerRequested = false;
            m_debuggerConfigurator = Function<void(IScriptDebugger&)>{};
            m_scriptContext = nullptr;
            m_manager = nullptr;
            m_language = String{};
            m_loadedClasses.Clear();
            m_warnedLanguageMismatch = false;
            LOG_DEBUG(u8"Script", u8"run script context torn down");
        }

        [[nodiscard]] bool IsActive() const noexcept { return m_scriptContext.Get() != nullptr; }

    private:
        // Create the debugger once the manager exists and it was requested (idempotent). The
        // internal tracker is always the listener; the caller's configurator applies the
        // initial breakpoints and grabs the pointer for live changes.
        void EnsureDebugger()
        {
            if (m_debugger || !m_debuggerRequested || m_manager.Get() == nullptr)
            {
                return;
            }
            if (!HasScriptCapability(m_manager->Capabilities(), ScriptCapabilities::Debugger))
            {
                return;
            }
            m_debugger = m_manager->CreateDebugger();
            if (!m_debugger)
            {
                return;
            }
            m_debugger->SetListener(&m_debugTracker);
            if (m_debuggerConfigurator)
            {
                m_debuggerConfigurator(*m_debugger);
            }
        }

        void WarnLanguageMismatchOnce(StringView languageId)
        {
            if (m_warnedLanguageMismatch)
            {
                return;
            }
            m_warnedLanguageMismatch = true;
            LOG_ERROR(u8"Script",
                               u8"behavior language '{}' differs from the run context's '{}' - one "
                               u8"gameplay context per run; these behaviors stay disabled",
                               languageId, m_language);
        }

        RefPtr<IScriptManager> m_manager;
        RefPtr<IScriptContext> m_scriptContext;
        String m_language;
        RunErrorSink m_errorSink;
        ScriptRuntimeBinding m_binding;
        foundation::core::IAllocator* m_allocator;
        Function<void*(TypeId)> m_applicationServices; // SetApplicationServices
        Array<RefPtr<BehaviorClass>> m_loadedClasses; // the classes loaded into the context, one each
        UniquePtr<IScriptDebugger> m_debugger; // the run's step debugger (opt-in)
        DebugPauseTracker m_debugTracker;      // the game-pause flag and external forward
        Function<void(IScriptDebugger&)> m_debuggerConfigurator;
        u32 m_generation = 0; // names each class's module
        bool m_warnedLanguageMismatch = false;
        bool m_debuggerRequested = false;
        bool m_gameScriptHold = false; // a game script is loaded into this host (teardown pin)
    };

    // ---- shared property application (the behavior tier and the scene-script Level tier) ----

    /// Marshal one harvested property value into a Variant for the generated setter. Entity and Asset
    /// values resolve against `scene` (nullptr gives a null Variant, as an unresolved reference does).
    /// Shared so the behavior tier and the Level tier apply properties identically.
    [[nodiscard]] inline Variant ScriptPropertyValueToVariant(const BehaviorPropertyValue& value,
                                                              BehaviorPropertyType declaredType,
                                                              scene::Scene* scene)
    {
        const BehaviorPropertyType kind =
            value.kind != BehaviorPropertyType::None ? value.kind : declaredType;
        switch (kind)
        {
        case BehaviorPropertyType::Float:
        case BehaviorPropertyType::Int:
            return Variant::From<f64>(value.number);
        case BehaviorPropertyType::Bool:
            return Variant::From<bool>(value.boolean);
        case BehaviorPropertyType::String:
            return Variant::From<String>(String(value.text.AsView()));
        case BehaviorPropertyType::Color:
            return Variant::From<Color>(value.color);
        case BehaviorPropertyType::Vec3:
            return Variant::From<Float3>(value.vector);
        case BehaviorPropertyType::Entity:
        {
            // An entity that names none (unset, or not in this scene) is an invalid Entity in
            // either language, as a lookup that finds nothing is: a value, never nil or null.
            if (value.guid.IsNil() || scene == nullptr)
            {
                return Variant::From(Entity{});
            }
            const scene::EntityHandle target = scene->FindEntity(value.guid);
            if (!target.IsAssigned())
            {
                return Variant::From(Entity{});
            }
            Entity handle;
            handle.scene = scene;
            handle.entityIndex = target.index;
            handle.entityGeneration = target.generation;
            return Variant::From(handle);
        }
        case BehaviorPropertyType::Asset:
            return Variant::From<Guid>(value.guid);
        case BehaviorPropertyType::None:
        default:
            return Variant{};
        }
    }

    /// Push a class's harvested defaults, then hash-keyed overrides (overrides win), through each
    /// property's generated per-property setter ("<name>="). `contextName` names the owner in the
    /// no-setter warning (an entity name for a behavior, the scene name for a Level).
    inline void ApplyScriptProperties(BehaviorObject& instance, const BehaviorClass& scriptClass,
                                      Span<const BehaviorPropertyOverride> overrides,
                                      scene::Scene* scene, StringView contextName)
    {
        for (const BehaviorProperty& property : scriptClass.properties)
        {
            const BehaviorPropertyValue* chosen = &property.defaultValue;
            for (const BehaviorPropertyOverride& over : overrides)
            {
                if (over.nameHash == property.hash)
                {
                    chosen = &over.value;
                    break;
                }
            }
            Variant marshalled = ScriptPropertyValueToVariant(*chosen, property.type, scene);
            String setter(property.name.AsView());
            setter += u8"=";
            Variant args[1] = {Move(marshalled)};
            if (auto result = instance.Invoke(setter.AsView(), Span<Variant>{args, 1});
                !result.HasValue())
            {
                LOG_WARNING(u8"Script",
                            u8"'{}': class '{}' has no setter '{}=' for its declared property",
                            contextName, scriptClass.className, property.name);
            }
        }
    }

    // ---- per-scene dispatch ----

    /// Where the entity behaviors run in Update: before the systems that act on what a script
    /// writes (the navigation crowd at -100, audio at -100, animation at -2 and -1), so a
    /// destination, a sound or a graph parameter set in onUpdate takes effect that frame; a script
    /// reads the positions those systems left the frame before. The scene's Level script runs
    /// just before the behaviors.
    inline constexpr i32 kScriptUpdateOrder = -110;

    class EntityBehaviorSystem final : public scene::SceneSystem, public scene::IEntityEventSink
    {
    public:
        ~EntityBehaviorSystem() override
        {
            if (m_scene != nullptr)
                m_scene->RemoveEntityEventSink(*this);
        }
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return kScriptUpdateOrder; }
        void OnSceneCreate(scene::Scene& scene) override
        {
            m_scene = &scene;
            // What happens to an entity (a contact, an animation ending), posted by the domain to
            // the scene, reaches the entity's behaviors as on<Event>.
            scene.AddEntityEventSink(*this);
            // Route this scene's bus events to the entity behaviors that declare on<Event>.
            m_eventSubs.Bind(scene.Events(), Function<void(StringView, const Variant&)>{
                                                  [this](StringView eventName, const Variant& payload)
                                                  { BroadcastEvent(eventName, payload); }});
        }

        /// The subsystem (or a headless test) wires the shared run host in.
        void SetRunHost(BehaviorRunHost* host) noexcept { m_host = host; }
        [[nodiscard]] BehaviorRunHost* Host() const noexcept { return m_host; }
        /// Optional: the subsystem hears about run-participation changes (teardown check).
        void SetRunObserver(Function<void()> observer) { m_runObserver = Move(observer); }

        [[nodiscard]] bool Started() const noexcept { return m_started; }
        [[nodiscard]] scene::Scene* ScenePtr() const noexcept { return m_scene; }

        // Behaviors tick only under simulation (the Simulate toggle and play-in-editor both count).
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }

        void OnSceneStarted() override { m_started = true; }

        void OnSceneStopped() override
        {
            m_started = false;
            DestroyAllInstances();
            m_eventSubs.Clear(); // drop bus subscriptions; re-subscribed as behaviors rebind on run
            if (m_runObserver)
            {
                m_runObserver();
            }
        }

        // Each fixed step, onFixedUpdate(dt) reaches every behavior that has started and is
        // running (enabled, not faulted, its entity active). Starting stays with the frame's
        // tick, so a behavior's first fixed step comes after its onStart.
        void OnFixedUpdate(f32 fixedDeltaTime) override
        {
            if (!m_started || m_scene == nullptr || m_host == nullptr || m_host->IsDebugPaused())
            {
                return;
            }
            auto* components = m_scene->GetSystem<BehaviorComponentManager>();
            if (components == nullptr || components->Count() == 0)
            {
                return;
            }
            PROFILE_SCOPE("Script.FixedUpdate");
            m_fixedOwners.Clear();
            for (scene::EntityHandle owner : components->Owners())
            {
                m_fixedOwners.PushBack(owner);
            }
            Variant dt = Variant::From(fixedDeltaTime);
            for (scene::EntityHandle entity : m_fixedOwners)
            {
                if (!m_scene->IsEffectivelyActive(entity))
                {
                    continue;
                }
                BehaviorComponent* component = components->Get(entity);
                for (usize i = 0; component != nullptr && i < component->behaviors.Size(); ++i)
                {
                    AttachedBehavior& behavior = component->behaviors[i];
                    if (!behavior.started || !behavior.active || !behavior.enabled || behavior.faulted ||
                        behavior.instance.Get() == nullptr || behavior.boundClass == nullptr ||
                        !behavior.lifecycle.fixedUpdate.IsValid())
                    {
                        continue;
                    }
                    const u64 storage = components->StorageRevision();
                    (void)InvokeHandler(behavior, *behavior.boundClass, entity, behavior.lifecycle.fixedUpdate, kOnFixedUpdate,
                                        Span<Variant>{&dt, 1});
                    if (m_host->IsDebugPaused())
                    {
                        return;
                    }
                    if (components->StorageRevision() != storage)
                    {
                        component = components->Get(entity);
                    }
                }
            }
        }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::Update)
            {
                return;
            }
            if (!m_started || m_scene == nullptr || m_host == nullptr)
            {
                return;
            }
            // Frozen at a breakpoint: the world holds still (behaviors and coroutines stop
            // advancing) while the debugger owns a suspended handler. The editor loop keeps
            // running; step and continue drive the held context directly, not this tick.
            if (m_host->IsDebugPaused())
            {
                return;
            }
            PROFILE_SCOPE("Script.Update");
            m_deltaTime = deltaTime;
            m_elapsed += static_cast<f64>(deltaTime);
            TickBehaviors(deltaTime);
            DrainMessages(); // deferred entity.send delivery: same frame, never nested
            AddPendingBehaviors(); // SceneBehaviors.addBehavior: started on the next tick
            // Resume due coroutines once per simulated frame, at the tick's top level (no
            // VM call active, so the backend's resume is safe here). Gated to a backend that
            // actually has the scheduler; a non-supporting one no-ops anyway.
            if (IScriptManager* manager = m_host->Manager();
                manager != nullptr &&
                HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines))
            {
                PROFILE_SCOPE("Script.Coroutines");
                manager->AdvanceCoroutines(static_cast<f64>(deltaTime));
            }
        }

        /// The scene's last delivered frame time and its simulated seconds since it started
        /// (scene.behaviors.deltaTime and elapsed): scaled time, so both stand still at time scale 0.
        [[nodiscard]] f32 DeltaTime() const noexcept { return m_deltaTime; }
        [[nodiscard]] f64 Elapsed() const noexcept { return m_elapsed; }

        /// A behaviour of the script class `scriptClass` (an asset id) for `entity`, added after
        /// this tick's behaviours ran and started on the next: the component is made when the
        /// entity has none. Deferred because a behaviour asking for
        /// it is running inside the very array it would grow. False for a stale entity or a nil
        /// id; an id naming no class leaves a behaviour that never starts, as an authored one.
        bool QueueBehavior(scene::EntityHandle entity, const Guid& scriptClass)
        {
            if (m_scene == nullptr || !m_scene->IsValid(entity) || scriptClass.IsNil())
            {
                return false;
            }
            m_pendingBehaviors.PushBack(PendingBehavior{entity, scriptClass});
            return true;
        }

        /// Live instance count (the subsystem's context-teardown bookkeeping).
        [[nodiscard]] u32 InstanceCount() const
        {
            auto* components =
                m_scene != nullptr ? m_scene->GetSystem<BehaviorComponentManager>() : nullptr;
            if (components == nullptr)
            {
                return 0;
            }
            u32 count = 0;
            components->ForEach(
                [&count](BehaviorComponent& component, scene::EntityHandle)
                {
                    for (const AttachedBehavior& behavior : component.behaviors)
                    {
                        if (behavior.instance.Get() != nullptr)
                        {
                            ++count;
                        }
                    }
                });
            return count;
        }

        /// onDestroy and release for one component's behaviors (entity or component removal).
        void ReleaseComponentInstances(BehaviorComponent& component, scene::EntityHandle entity)
        {
            for (AttachedBehavior& behavior : component.behaviors)
            {
                StopBehavior(behavior, entity, true);
            }
        }

        /// Behavior messaging: queue `on<Message>(args)` for every enabled behavior of
        /// `target` that declares the handler. Reached from the `Entity::send` facade through
        /// the run binding's route. Delivery is deferred (drained at the tick's top level)
        /// because a send happens inside a running script call and re-entrant VM calls are
        /// unsafe, so messages arrive later the same frame, never nested.
        void EnqueueMessage(scene::EntityHandle target, StringView message,
                            Span<const Variant> args)
        {
            if (message.IsEmpty())
            {
                return;
            }
            PendingMessage pending;
            pending.target = target;
            pending.handler = BuildMessageHandlerName(message);
            pending.args.Reserve(args.Size());
            for (const Variant& arg : args)
            {
                pending.args.PushBack(arg);
            }
            m_messages.PushBack(Move(pending));
        }

        /// An entity event for this scene's behaviors: queued as `on<Name>` (the send() rule:
        /// "contactBegin" calls onContactBegin) with its arguments, each EntityHandle among them made
        /// the Entity a behavior is given. Only queued, onto the same deferred queue as entity.send,
        /// drained at the tick's top level: the domain posts from its own step, and delivery is
        /// gated by HasHandler in InvokeHandler, as messages are.
        void OnEntityEvent(scene::Scene& scene, const scene::EntityEvent& event) override
        {
            if (!event.target.IsAssigned() || event.name.IsEmpty())
                return;
            PendingMessage pending;
            pending.target = event.target;
            pending.handler = BuildMessageHandlerName(event.name);
            pending.args.Reserve(event.args.Size());
            for (const Variant& arg : event.args)
            {
                if (const scene::EntityHandle* entity = arg.TryGet<scene::EntityHandle>())
                {
                    Entity facade;
                    facade.scene = &scene;
                    facade.entityIndex = entity->index;
                    facade.entityGeneration = entity->generation;
                    pending.args.PushBack(Variant::From<Entity>(facade));
                }
                else
                {
                    pending.args.PushBack(arg);
                }
            }
            m_messages.PushBack(Move(pending));
        }

        /// Drains queued messages at the tick's top level (no VM call is active here, so
        /// InvokeHandler's VM call is safe). A handler may send again; those are drained
        /// in the same pass, capped to break runaway send loops.
        void DrainMessages()
        {
            if (m_messages.IsEmpty())
            {
                return;
            }
            auto* components =
                m_scene != nullptr ? m_scene->GetSystem<BehaviorComponentManager>() : nullptr;
            usize delivered = 0;
            for (usize m = 0; m < m_messages.Size(); ++m)
            {
                if (++delivered > kMaxMessagesPerDrain)
                {
                    LOG_WARNING(
                        u8"Script",
                        u8"message drain hit the {} cap - dropping the rest (send loop?)",
                        kMaxMessagesPerDrain);
                    break;
                }
                // Copy out before dispatch: delivering may append (reallocating m_messages).
                const scene::EntityHandle target = m_messages[m].target;
                const String handler = m_messages[m].handler;
                Array<Variant> args = m_messages[m].args;
                if (components == nullptr)
                {
                    continue;
                }
                if (m_scene != nullptr && !m_scene->IsEffectivelyActive(target))
                {
                    continue; // inactive entities receive nothing (messages and contacts)
                }
                const Span<Variant> argSpan{args.Data(), args.Size()};
                for (usize i = 0;; ++i)
                {
                    BehaviorComponent* component = components->Get(target);
                    if (component == nullptr || i >= component->behaviors.Size())
                    {
                        break;
                    }
                    AttachedBehavior& behavior = component->behaviors[i];
                    if (!behavior.enabled || behavior.faulted ||
                        behavior.instance.Get() == nullptr || behavior.boundClass == nullptr)
                    {
                        continue;
                    }
                    (void)InvokeHandler(behavior, *behavior.boundClass, target, handler.AsView(),
                                        argSpan);
                }
                // A message handler that hit a breakpoint pauses the game: stop draining (the
                // rest of this pass is dropped, a rare edge case of message-handler breakpoints).
                if (m_host != nullptr && m_host->IsDebugPaused())
                {
                    break;
                }
            }
            m_messages.Clear();
        }

        /// The bus sink for entity behaviors: dispatch `on<Event>(payload)` to every enabled
        /// behavior of this scene that declares it. Called at bus-drain time (the scene tick's top
        /// level, no VM call active), so it invokes directly: emitting publishes and the bus drains
        /// later, which already provides the deferral entity.send needs its message queue for.
        /// Owners are snapshotted first because a handler may spawn or destroy entities
        /// (swap-remove moves slots). The arg count follows the payload: a no-payload emit()
        /// delivers `on<Event>()`, a valued one `on<Event>(payload)`, so handler arity matches
        /// the emit site.
        void BroadcastEvent(StringView eventName, const Variant& payload)
        {
            if (!m_started || m_scene == nullptr || m_host == nullptr || m_host->IsDebugPaused())
            {
                return;
            }
            auto* components = m_scene->GetSystem<BehaviorComponentManager>();
            if (components == nullptr || components->Count() == 0)
            {
                return;
            }
            String handler(u8"on");
            handler += eventName; // "on" and "OrbCollected" reconstruct the exact declared handler
            Variant arg = payload;
            const bool hasArg = !payload.IsEmpty();
            const Span<Variant> args = hasArg ? Span<Variant>{&arg, 1} : Span<Variant>{};
            m_tickOwners.Clear(); // reuse the per-tick snapshot buffer
            for (scene::EntityHandle owner : components->Owners())
            {
                m_tickOwners.PushBack(owner);
            }
            for (scene::EntityHandle entity : m_tickOwners)
            {
                if (!m_scene->IsEffectivelyActive(entity))
                {
                    continue; // inactive entities receive no bus events
                }
                for (usize i = 0;; ++i)
                {
                    BehaviorComponent* component = components->Get(entity);
                    if (component == nullptr || i >= component->behaviors.Size())
                    {
                        break;
                    }
                    AttachedBehavior& behavior = component->behaviors[i];
                    if (!behavior.enabled || behavior.faulted ||
                        behavior.instance.Get() == nullptr || behavior.boundClass == nullptr)
                    {
                        continue;
                    }
                    // Gated by HasHandler inside InvokeHandler: behaviors without on<Event> skip.
                    (void)InvokeHandler(behavior, *behavior.boundClass, entity, handler.AsView(),
                                        args);
                }
            }
        }

    private:
        // The result of one handler dispatch: Ok (ran), Faulted (disabled), or Suspended
        // (hit a breakpoint: the game is paused, the debugger owns the mid-flight handler).
        enum class HandlerOutcome
        {
            Ok,
            Faulted,
            Suspended
        };

        static constexpr StringView kOnStart = u8"onStart";
        static constexpr StringView kOnUpdate = u8"onUpdate";
        static constexpr StringView kOnFixedUpdate = u8"onFixedUpdate";
        static constexpr StringView kOnEnable = u8"onEnable";
        static constexpr StringView kOnDisable = u8"onDisable";
        static constexpr StringView kOnDestroy = u8"onDestroy";

        // "heal" becomes "onHeal": the send() message convention. First char uppercased.
        [[nodiscard]] static String BuildMessageHandlerName(StringView message)
        {
            String name(u8"on");
            for (usize i = 0; i < message.Size(); ++i)
            {
                utf8char c = message[i];
                if (i == 0 && c >= u8'a' && c <= u8'z')
                {
                    c = static_cast<utf8char>(c - 32);
                }
                name += c;
            }
            return name;
        }

        Variant m_frameDelta; // this tick's delta, for every behaviour's onUpdate (TickBehaviors)

        void TickBehaviors(f32 deltaTime)
        {
            auto* components = m_scene->GetSystem<BehaviorComponentManager>();
            if (components == nullptr || components->Count() == 0)
            {
                return;
            }

            // Snapshot the owner list: scripts may destroy entities (swap-remove moves
            // pool slots) or spawn new ones (picked up next tick: the deferred start).
            m_tickOwners.Clear();
            for (scene::EntityHandle owner : components->Owners())
            {
                m_tickOwners.PushBack(owner);
            }

            // One delta for every behaviour's onUpdate this tick (a handler reads its arguments).
            m_frameDelta = Variant::From(deltaTime);
            for (scene::EntityHandle entity : m_tickOwners)
            {
                // A dispatch can add or remove script components, moving the pool: the component
                // is looked up again after one only when it did.
                BehaviorComponent* component = components->Get(entity);
                for (usize i = 0; component != nullptr && i < component->behaviors.Size(); ++i)
                {
                    const u64 storage = components->StorageRevision();
                    TickBehavior(component->behaviors[i], entity, deltaTime);
                    // A breakpoint hit inside that dispatch pauses the game mid-tick: stop
                    // advancing the rest of this tick (the world holds still).
                    if (m_host != nullptr && m_host->IsDebugPaused())
                    {
                        return;
                    }
                    if (components->StorageRevision() != storage)
                    {
                        component = components->Get(entity);
                    }
                }
            }
        }

        void TickBehavior(AttachedBehavior& behavior, scene::EntityHandle entity, f32 deltaTime)
        {
            // An effectively-inactive entity's behaviors freeze: no lifecycle events fire,
            // onStart waits for the first active tick (so an entity that loads inactive behaves
            // like one spawned disabled), and updates stop. Pending coroutines are cancelled once
            // on the edge, as for a disabled behavior (the scheduler has no per-behavior pause).
            if (m_scene != nullptr && !m_scene->IsEffectivelyActive(entity))
            {
                if (!behavior.entitySuspended)
                {
                    behavior.entitySuspended = true;
                    CancelCoroutines(behavior);
                }
                return;
            }
            behavior.entitySuspended = false;

            BehaviorClass* scriptClass = behavior.behaviorClass.Get();
            if (scriptClass == nullptr)
            {
                if (behavior.instance.Get() != nullptr)
                {
                    StopBehavior(behavior, entity, true);
                }
                return;
            }

            // Hot reload: the resource handle swapped its product, so re-instantiate and
            // re-apply overrides below (transient script state resets).
            if (behavior.instance.Get() != nullptr && behavior.boundClass != scriptClass)
            {
                StopBehavior(behavior, entity, false);
                behavior.faulted = false;
            }

            if (!behavior.enabled)
            {
                if (behavior.instance.Get() != nullptr && behavior.active)
                {
                    behavior.active =
                        false; // set before dispatch (no re-dispatch on suspend or fault)
                    (void)InvokeHandler(behavior, *behavior.boundClass, entity, behavior.lifecycle.disable, kOnDisable, {});
                    CancelCoroutines(behavior); // a disabled behavior's coroutines stop too
                }
                return;
            }
            if (behavior.faulted)
            {
                return;
            }

            if (behavior.instance.Get() == nullptr)
            {
                InstantiateBehavior(behavior, entity, *scriptClass);
                if (behavior.instance.Get() == nullptr)
                {
                    return;
                }
            }

            // Lifecycle flags are set before the dispatch: whether it faults or debug-suspends
            // (mid-handler, game paused), it must not re-dispatch onEnable/onStart. A
            // debug-suspended handler runs to completion later via the debugger's Continue.
            if (!behavior.active)
            {
                behavior.active = true;
                if (InvokeHandler(behavior, *scriptClass, entity, behavior.lifecycle.enable, kOnEnable, {}) !=
                    HandlerOutcome::Ok)
                {
                    return;
                }
            }
            if (!behavior.started)
            {
                behavior.started = true;
                if (InvokeHandler(behavior, *scriptClass, entity, behavior.lifecycle.start, kOnStart, {}) !=
                    HandlerOutcome::Ok)
                {
                    return;
                }
            }
            // updateInterval throttling: 0 means every tick with the raw dt; otherwise
            // bank time and deliver once the interval elapses, passing the accumulated dt
            // (so movement integrates correctly at a lower call rate).
            if (behavior.updateInterval > 0.0f)
            {
                behavior.updateAccumulator += deltaTime;
                if (behavior.updateAccumulator + 1e-6f >= behavior.updateInterval)
                {
                    Variant dt = Variant::From(behavior.updateAccumulator);
                    behavior.updateAccumulator =
                        0.0f; // consume before dispatch (no double on resume)
                    (void)InvokeHandler(behavior, *scriptClass, entity, behavior.lifecycle.update, kOnUpdate,
                                        Span<Variant>{&dt, 1});
                }
            }
            else
            {
                (void)InvokeHandler(behavior, *scriptClass, entity, behavior.lifecycle.update, kOnUpdate,
                                    Span<Variant>{&m_frameDelta, 1});
            }
        }

        void InstantiateBehavior(AttachedBehavior& behavior, scene::EntityHandle entity,
                                 BehaviorClass& scriptClass)
        {
            if (scriptClass.className.IsEmpty())
            {
                // A utility module reference: nothing to instantiate; not an error.
                behavior.boundClass = &scriptClass;
                return;
            }
            PROFILE_SCOPE(scriptClass.ProfileName());
            Entity handle;
            handle.scene = m_scene;
            handle.entityIndex = entity.index;
            handle.entityGeneration = entity.generation;
            Variant arg = Variant::From(handle);
            behavior.instance = m_host->Instantiate(scriptClass, Span<Variant>{&arg, 1}, BehaviorKind::Entity);
            behavior.boundClass = &scriptClass;
            behavior.started = false;
            behavior.active = false;
            behavior.updateAccumulator = 0.0f; // fresh instance banks from zero
            if (behavior.instance.Get() == nullptr)
            {
                behavior.faulted = true;
                LOG_ERROR(
                    u8"Script", u8"'{}': behavior '{}' failed to instantiate - behavior disabled",
                    m_scene->GetEntityName(entity), scriptClass.className);
                return;
            }
            // The lifecycle handlers the class declares, bound once: each frame's dispatch then
            // tests a handle rather than comparing names.
            const auto bind = [&](StringView name, usize argCount)
            {
                return scriptClass.HasHandler(name) ? behavior.instance->Bind(name, argCount) : BehaviorMethod{};
            };
            behavior.lifecycle.enable = bind(kOnEnable, 0);
            behavior.lifecycle.start = bind(kOnStart, 0);
            behavior.lifecycle.update = bind(kOnUpdate, 1);
            behavior.lifecycle.fixedUpdate = bind(kOnFixedUpdate, 1);
            behavior.lifecycle.disable = bind(kOnDisable, 0);
            behavior.lifecycle.destroy = bind(kOnDestroy, 0);
            ApplyProperties(behavior, scriptClass, entity);
            // Subscribe this scene's bus to the on<Event> handlers this class declares (deduped),
            // so a fired event reaches every declaring behavior through BroadcastEvent.
            m_eventSubs.EnsureFor(scriptClass);
        }

        // Defaults first, then hash-keyed overrides win (shared with the Level tier).
        void ApplyProperties(AttachedBehavior& behavior, const BehaviorClass& scriptClass,
                             scene::EntityHandle entity)
        {
            ApplyScriptProperties(*behavior.instance, scriptClass,
                                  Span<const BehaviorPropertyOverride>{behavior.overrides.Data(),
                                                                     behavior.overrides.Size()},
                                  m_scene, m_scene->GetEntityName(entity));
        }

        // One handler dispatch: declared-handler gate (no method-missing probing), the
        // per-behavior profile scope, and the fault-disables-this-behavior rule. A handler
        // that debug-suspended (hit a breakpoint) is reported as Suspended, not a fault: the
        // game is now paused and the debugger owns the mid-flight handler; on Continue it runs
        // to completion and normal flow resumes.
        [[nodiscard]] HandlerOutcome InvokeHandler(AttachedBehavior& behavior,
                                                   const BehaviorClass& scriptClass,
                                                   scene::EntityHandle entity, StringView method,
                                                   Span<Variant> args)
        {
            if (behavior.instance.Get() == nullptr || !scriptClass.HasHandler(method))
            {
                return HandlerOutcome::Ok;
            }
            PROFILE_SCOPE(scriptClass.ProfileName());
            return HandlerResult(behavior, scriptClass, entity, method, behavior.instance->Invoke(method, args));
        }

        // A lifecycle handler, by the method its instance bound: no name looked up per call. One
        // the instance could not bind goes by `name`, which also names a fault: a declared handler
        // the object lacks (the wrong parameters) faults as it always has, and an undeclared one
        // is skipped.
        [[nodiscard]] HandlerOutcome InvokeHandler(AttachedBehavior& behavior,
                                                   const BehaviorClass& scriptClass,
                                                   scene::EntityHandle entity, BehaviorMethod method,
                                                   StringView name, Span<Variant> args)
        {
            if (!method.IsValid())
            {
                return InvokeHandler(behavior, scriptClass, entity, name, args);
            }
            if (behavior.instance.Get() == nullptr)
            {
                return HandlerOutcome::Ok;
            }
            PROFILE_SCOPE(scriptClass.ProfileName());
            return HandlerResult(behavior, scriptClass, entity, name, behavior.instance->Invoke(method, args));
        }

        // What a handler's result means: a fault disables the behaviour; a debug pause is not one.
        [[nodiscard]] HandlerOutcome HandlerResult(AttachedBehavior& behavior, const BehaviorClass& scriptClass,
                                                   scene::EntityHandle entity, StringView method,
                                                   const Result<Variant>& result)
        {
            if (result.HasValue())
            {
                return HandlerOutcome::Ok;
            }
            if (m_host != nullptr && m_host->IsDebugPaused())
            {
                return HandlerOutcome::Suspended;
            }
            behavior.faulted = true;
            LOG_ERROR(
                u8"Script", u8"'{}': behavior '{}' faulted in {} - behavior disabled",
                m_scene != nullptr ? m_scene->GetEntityName(entity) : StringView(u8"?"),
                scriptClass.className, method);
            return HandlerOutcome::Faulted;
        }

        // Stop every coroutine the behavior's instance started (disable, destroy,
        // reload). Gated to a coroutine-capable backend and a class that opted in
        // (usesCoroutines): a backend or class without coroutines pays nothing.
        void CancelCoroutines(AttachedBehavior& behavior)
        {
            if (behavior.instance.Get() == nullptr || behavior.boundClass == nullptr ||
                !behavior.boundClass->usesCoroutines || m_host == nullptr)
            {
                return;
            }
            IScriptManager* manager = m_host->Manager();
            if (manager == nullptr ||
                !HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines))
            {
                return;
            }
            manager->CancelCoroutinesFor(*behavior.instance);
        }

        void StopBehavior(AttachedBehavior& behavior, scene::EntityHandle entity, bool invokeDestroy)
        {
            if (behavior.instance.Get() != nullptr && invokeDestroy && behavior.started &&
                behavior.boundClass != nullptr && !behavior.faulted)
            {
                (void)InvokeHandler(behavior, *behavior.boundClass, entity, behavior.lifecycle.destroy, kOnDestroy, {});
            }
            CancelCoroutines(behavior); // drop pending coroutines before releasing the instance
            behavior.instance = nullptr;
            behavior.lifecycle = BehaviorLifecycle{};
            behavior.boundClass = nullptr;
            behavior.started = false;
            behavior.active = false;
        }

        void DestroyAllInstances()
        {
            auto* components =
                m_scene != nullptr ? m_scene->GetSystem<BehaviorComponentManager>() : nullptr;
            if (components == nullptr)
            {
                return;
            }
            components->ForEach([this](BehaviorComponent& component, scene::EntityHandle entity)
                                { ReleaseComponentInstances(component, entity); });
        }

        struct PendingMessage
        {
            scene::EntityHandle target;
            String handler;      // prebuilt "on<Message>"
            Array<Variant> args; // marshalled at send time
        };
        struct PendingBehavior
        {
            scene::EntityHandle entity;
            Guid scriptClass;
        };

        void AddPendingBehaviors()
        {
            if (m_pendingBehaviors.IsEmpty())
            {
                return;
            }
            Array<PendingBehavior> pending = Move(m_pendingBehaviors);
            m_pendingBehaviors = Array<PendingBehavior>{};
            auto* components = m_scene->GetSystem<BehaviorComponentManager>();
            if (components == nullptr)
            {
                return;
            }
            resource::ResourceManager* resources =
                (m_host != nullptr && m_host->Binding().resolveResources)
                    ? m_host->Binding().resolveResources()
                    : nullptr;
            for (const PendingBehavior& request : pending)
            {
                if (!m_scene->IsValid(request.entity))
                {
                    continue; // destroyed in the meantime
                }
                BehaviorComponent* component = components->Get(request.entity);
                if (component == nullptr)
                {
                    component = &components->Add(request.entity);
                }
                AttachedBehavior behavior;
                behavior.behaviorClass.SetId(request.scriptClass);
                if (resources != nullptr)
                {
                    behavior.behaviorClass.Bind(*resources);
                }
                component->behaviors.PushBack(Move(behavior));
            }
        }
        static constexpr usize kMaxMessagesPerDrain = 4096;

        scene::Scene* m_scene = nullptr;
        BehaviorRunHost* m_host = nullptr;
        Function<void()> m_runObserver;
        Array<scene::EntityHandle> m_tickOwners; // per-tick snapshot (reused)
        Array<scene::EntityHandle> m_fixedOwners; // per-fixed-step snapshot (reused)
        Array<PendingMessage> m_messages;        // deferred entity.send queue
        Array<PendingBehavior> m_pendingBehaviors; // SceneBehaviors.addBehavior, after the tick
        ScriptEventSubscriptions m_eventSubs;    // this scene's bus subscriptions, sunk into BroadcastEvent
        f32 m_deltaTime = 0.0f;
        f64 m_elapsed = 0.0;
        bool m_started = false;
    };

    /// The scene's scripting, a property of the scene: `scene.behaviors`. The scene's script time
    /// (scaled: it stands still at time scale 0), the messages between behaviours (`send`, as `entity.send` on the target), the events
    /// on the scene's bus (`emit`, as `scene.events.emit`), and a behaviour added at runtime by
    /// its class's asset id. A value type; a null scene makes every call a safe no-op.
    struct SceneBehaviorsFacade
    {
        scene::WeakSceneRef scene; // read through the scene's life: null once it is gone

        [[nodiscard]] EntityBehaviorSystem* System() const
        {
            return scene != nullptr ? scene->GetSystem<EntityBehaviorSystem>() : nullptr;
        }

        [[nodiscard]] f32 deltaTime() const
        {
            EntityBehaviorSystem* system = System();
            return system != nullptr ? system->DeltaTime() : 0.0f;
        }
        [[nodiscard]] f64 elapsed() const
        {
            EntityBehaviorSystem* system = System();
            return system != nullptr ? system->Elapsed() : 0.0;
        }
        /// Queues `on<Message>(...)` for every behaviour of `target` that declares it.
        void send(Entity target, String message) const
        {
            if (target.scene == scene)
            {
                target.send(Move(message));
            }
        }
        void send(Entity target, String message, Variant payload) const
        {
            if (target.scene == scene)
            {
                target.send(Move(message), Move(payload));
            }
        }
        /// Publishes on the scene's bus: every behaviour and the Level declaring `on<Event>`.
        void emit(String name) const
        {
            SceneEventsFacade events;
            events.scene = scene;
            events.emit(Move(name));
        }
        void emit(String name, Variant payload) const
        {
            SceneEventsFacade events;
            events.scene = scene;
            events.emit(Move(name), Move(payload));
        }
        /// A behaviour of the class (a script asset id) on the entity, started on the next tick.
        bool addBehavior(Entity entity, Guid scriptClass) const
        {
            EntityBehaviorSystem* system = System();
            return system != nullptr && entity.scene == scene &&
                   system->QueueBehavior(entity.Handle(), scriptClass);
        }

        // The scene's own, read by scripts as a property of the scene (ExtendScene).
        [[nodiscard]] static SceneBehaviorsFacade ForScene(const foundation::script::Scene& sceneHandle)
        {
            SceneBehaviorsFacade scripts;
            scripts.scene = sceneHandle.scene;
            return scripts;
        }
    };

    void RegisterBehaviorSceneFacade();

    // ---- scene-level scripting (the third tier): one `Level` script object per scene ----

    /// The one-per-scene authored block: the `Level` script and an enable flag. Serialized with
    /// the scene (as SceneSystem settings), resolved on the async level-load path
    /// (ResolveResources rides ResolveSceneResources' AsyncBindScope), and rendered in the
    /// scene-settings inspector (reflected) with a script-asset Ref picker.
    struct SceneBehaviorSettings
    {
        resource::Ref<BehaviorClass> behaviorClass; // the scene behavior (a cooked script asset); nil means none
        bool enabled = true;
        // Hash-keyed property overrides (values differing from the Level class's harvested
        // defaults): the same default-diff model as a behavior's overrides, one set per scene.
        Array<BehaviorPropertyOverride> overrides;

        [[nodiscard]] const BehaviorPropertyOverride* FindOverride(u64 nameHash) const
        {
            for (const BehaviorPropertyOverride& entry : overrides)
            {
                if (entry.nameHash == nameHash)
                {
                    return &entry;
                }
            }
            return nullptr;
        }
        void SetOverride(u64 nameHash, const BehaviorPropertyValue& value)
        {
            for (BehaviorPropertyOverride& entry : overrides)
            {
                if (entry.nameHash == nameHash)
                {
                    entry.value = value;
                    return;
                }
            }
            overrides.PushBack(BehaviorPropertyOverride{nameHash, value});
        }
        void RemoveOverride(u64 nameHash)
        {
            for (usize i = 0; i < overrides.Size(); ++i)
            {
                if (overrides[i].nameHash == nameHash)
                {
                    overrides.RemoveAt(i);
                    return;
                }
            }
        }
    };

    inline void SerializeSceneScriptSettings(ISerializer& ar, SceneBehaviorSettings& s)
    {
        foundation::core::Serialize(ar, "class", s.behaviorClass);
        foundation::core::Serialize(ar, "enabled", s.enabled);
        // Level property overrides (when empty, the class defaults apply).
        foundation::core::Serialize(ar, "overrides", s.overrides); // count-prefixed array scope
    }

    /// The scene-root script tier. One `Level` object per scene, constructed with the scene's bound
    /// Scene handle (`construct new(scene)`), gated to simulation like behaviors, and dispatched
    /// before the entity behaviors of its scene (UpdateOrder). `onStart`, `onUpdate(dt)`,
    /// `onFixedUpdate(dt)` and `onStop` are all optional (dispatch by presence). A faulting handler
    /// disables this scene's level only.
    class SceneBehaviorSystem final : public scene::SceneSystem
    {
    public:
        void OnSceneCreate(scene::Scene& scene) override
        {
            m_scene = &scene;
            // The Level's named inbox: route this scene's bus events to the Level's
            // on<Event> handler, so a behavior's scene.events.emit reaches an onPlayerFell here.
            m_eventSubs.Bind(scene.Events(), Function<void(StringView, const Variant&)>{
                                                  [this](StringView eventName, const Variant& payload)
                                                  { DispatchEvent(eventName, payload); }});
        }

        void SetRunHost(BehaviorRunHost* host) noexcept { m_host = host; }
        [[nodiscard]] BehaviorRunHost* Host() const noexcept { return m_host; }
        [[nodiscard]] SceneBehaviorSettings& Settings() noexcept { return m_settings; }
        /// True once a Level object is live (test/inspection).
        [[nodiscard]] bool LevelActive() const noexcept { return m_level.Get() != nullptr; }
        [[nodiscard]] bool LevelFaulted() const noexcept { return m_faulted; }
        /// Reads a property of the live Level object (a playtest probes a level's state with it).
        /// NotFound with no Level live or no such property.
        [[nodiscard]] core::Result<Variant> GetLevelProperty(StringView name) const
        {
            if (m_level.Get() == nullptr)
            {
                return core::Err(ErrorCode::NotFound);
            }
            return m_level->GetProperty(name);
        }

        // ---- SceneSystem ----
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        // Before the entity behaviors of this scene (the level orchestrates, entities react), and
        // so before the systems that act on what scripts write (see kScriptUpdateOrder).
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return kScriptUpdateOrder - 10; }

        [[nodiscard]] const TypeInfo* SettingsType() const noexcept override
        {
            return &TypeOf<SceneBehaviorSettings>();
        }
        [[nodiscard]] void* SettingsInstance() noexcept override { return &m_settings; }
        [[nodiscard]] StringView SettingsId() const noexcept override { return u8"sceneBehavior"; }
        void SerializeSettings(ISerializer& ar) override
        {
            SerializeSceneScriptSettings(ar, m_settings);
        }

        /// The Level's asset-valued overrides, which the script binds itself.
        void CollectUnboundReferences(Array<Guid>& outIds) override
        {
            CollectOverrideAssetIds(m_settings.overrides.AsSpan(), outIds);
        }

        void ResolveResources(resource::ResourceManager& manager) override
        {
            m_settings.behaviorClass.Bind(manager);
        }

        void OnSceneStarted() override
        {
            m_started = true;
            InstantiateLevel();
            Dispatch(kLevelOnStart, {});
        }

        void OnSceneStopped() override
        {
            Dispatch(kLevelOnStop, {});
            m_eventSubs.Clear(); // drop bus subscriptions before the Level object goes away
            DestroyLevel();      // no state survives a stop (same rule as behaviors)
            m_started = false;
        }

        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::Update || !Ready())
            {
                return;
            }
            PROFILE_SCOPE("Script.Level");
            Variant dt = Variant::From(deltaTime);
            Dispatch(kLevelOnUpdate, Span<Variant>{&dt, 1});
        }

        void OnFixedUpdate(f32 fixedDeltaTime) override
        {
            if (!Ready())
            {
                return;
            }
            Variant dt = Variant::From(fixedDeltaTime);
            Dispatch(kLevelOnFixedUpdate, Span<Variant>{&dt, 1});
        }

    private:
        static constexpr StringView kLevelOnStart = u8"onStart";
        static constexpr StringView kLevelOnUpdate = u8"onUpdate";
        static constexpr StringView kLevelOnFixedUpdate = u8"onFixedUpdate";
        static constexpr StringView kLevelOnStop = u8"onStop";

        [[nodiscard]] bool Ready() const noexcept
        {
            return m_started && !m_faulted && m_level.Get() != nullptr && m_host != nullptr &&
                   !m_host->IsDebugPaused();
        }

        void InstantiateLevel()
        {
            m_faulted = false;
            if (!m_settings.enabled || m_scene == nullptr || m_host == nullptr)
            {
                return;
            }
            BehaviorClass* scriptClass = m_settings.behaviorClass.Get();
            if (scriptClass == nullptr)
            {
                return; // no Level bound (or still pending on an async load): the system is inert
            }
            Scene handle; // the bound Scene ctor arg (foundation::script::Scene, this scene)
            handle.scene = m_scene;
            Variant arg = Variant::From(handle);
            m_boundClass = scriptClass;
            m_level = m_host->Instantiate(*scriptClass, Span<Variant>{&arg, 1}, BehaviorKind::Scene);
            if (m_level.Get() == nullptr)
            {
                m_faulted = true;
                LOG_ERROR(u8"Script", u8"scene '{}': level script '{}' failed to instantiate",
                          m_scene->Name(), scriptClass->className);
                return;
            }
            // Apply harvested defaults and hash-keyed overrides before onStart (as for behaviors),
            // so a Level's [metadata] properties are real inspector-authored values, not runtime
            // fallbacks. The scene name labels the no-setter warning.
            ApplyScriptProperties(*m_level, *scriptClass,
                                  Span<const BehaviorPropertyOverride>{m_settings.overrides.Data(),
                                                                     m_settings.overrides.Size()},
                                  m_scene, m_scene->Name());
            // Subscribe this scene's bus to the Level's on<Event> handlers (its named inbox).
            m_eventSubs.EnsureFor(*scriptClass);
        }

        /// The bus sink for the Level: dispatch `on<Event>(payload)` to the Level object.
        /// Called at bus-drain time (no VM active), so it dispatches directly, and reuses Dispatch,
        /// so a faulting event handler disables the Level exactly like a faulting lifecycle handler.
        /// Arg count follows the payload (a no-payload emit() delivers `on<Event>()`).
        void DispatchEvent(StringView eventName, const Variant& payload)
        {
            String handler(u8"on");
            handler += eventName; // "on" and "PlayerFell" reconstruct the declared handler
            Variant arg = payload;
            const bool hasArg = !payload.IsEmpty();
            const Span<Variant> args = hasArg ? Span<Variant>{&arg, 1} : Span<Variant>{};
            Dispatch(handler.AsView(), args);
        }

        void Dispatch(StringView method, Span<Variant> args)
        {
            if (m_faulted || m_level.Get() == nullptr || m_boundClass == nullptr ||
                !m_boundClass->HasHandler(method))
            {
                return;
            }
            PROFILE_SCOPE(m_boundClass->ProfileName());
            auto result = m_level->Invoke(method, args);
            if (result.HasValue())
            {
                return;
            }
            if (m_host != nullptr && m_host->IsDebugPaused())
            {
                return; // suspended at a breakpoint, not a fault
            }
            m_faulted = true;
            LOG_ERROR(u8"Script",
                               u8"scene '{}': level script '{}' faulted in {} - level disabled",
                               m_scene != nullptr ? m_scene->Name() : StringView(u8"?"),
                               m_boundClass->className, method);
        }

        void CancelLevelCoroutines()
        {
            if (m_level.Get() == nullptr || m_boundClass == nullptr ||
                !m_boundClass->usesCoroutines || m_host == nullptr)
            {
                return;
            }
            IScriptManager* manager = m_host->Manager();
            if (manager != nullptr &&
                HasScriptCapability(manager->Capabilities(), ScriptCapabilities::Coroutines))
            {
                manager->CancelCoroutinesFor(*m_level);
            }
        }

        void DestroyLevel()
        {
            CancelLevelCoroutines(); // drop pending coroutines before releasing the instance
            m_level = nullptr;
            m_boundClass = nullptr;
        }

        scene::Scene* m_scene = nullptr;
        BehaviorRunHost* m_host = nullptr;
        SceneBehaviorSettings m_settings;
        RefPtr<BehaviorObject> m_level;
        BehaviorClass* m_boundClass = nullptr;
        ScriptEventSubscriptions m_eventSubs; // this scene's bus subscriptions, sunk into DispatchEvent
        bool m_started = false;
        bool m_faulted = false;
    };

    // ---- the runtime subsystem ----

    // The script manager set for a scene, injected by the subsystem at runtime and by headless
    // scene consumers (Engine.Composition), so a manager added here reaches both. The cross-link
    // between manager and system is in-scene wiring and belongs here; run-host binding is
    // runtime-only and stays with the subsystem (the systems are inert without a host).
    inline void AddBehaviorSceneManagers(scene::Scene& scene)
    {
        auto* components = scene.AddSystem<BehaviorComponentManager>();
        EntityBehaviorSystem* system = scene.AddSystem<EntityBehaviorSystem>();
        components->SetScriptSystem(system);
        scene.AddSystem<SceneBehaviorSystem>(); // scene-root script (the Level tier)
    }

    class BehaviorSubsystem final : public foundation::runtime::Subsystem, public scene::ISceneObserver
    {
    public:
        /// The default run host: the one for the editor's editing and loose scenes.
        /// An ApplicationRun owns its own run host for its game scenes and game script; this is not
        /// that. Editing-scene Simulate runs its behaviors on this host.
        [[nodiscard]] BehaviorRunHost& RunHost() noexcept { return m_ownedRunHost; }

        /// The host `scene`'s scripts run on: the one its run holds (a run registers its
        /// BehaviorRunHost as a service), else the default host.
        [[nodiscard]] BehaviorRunHost* RunHostFor(const scene::Scene& scene) noexcept
        {
            if (foundation::run::IRun* run = scene.Run())
            {
                if (BehaviorRunHost* host = run->Get<BehaviorRunHost>())
                {
                    return host;
                }
            }
            return &m_ownedRunHost;
        }

        /// Wire a run host (this default or an ApplicationRun's) with the app services and routing so
        /// its context, once created, has the facades, the Scene.spawn spawner, and entity.send
        /// routing. The wrappers read the subsystem manager, spawner and m_systems live, so one
        /// call per host suffices and the same message route serves every host. Call once per run
        /// host before its first context is created.
        void ConfigureRunHost(BehaviorRunHost& host)
        {
            BehaviorSubsystem* self = this;
            // The application's services are its subsystem manager's, read at each call, so a
            // subsystem registered after this host was configured is still found.
            host.SetApplicationServices(Function<void*(TypeId)>{
                [self](TypeId type) -> void*
                { return self->Manager() != nullptr ? self->Manager()->FindService(type) : nullptr; }});
            // Resource swaps: forward to the app's getter at call time, so a manager
            // created after this host was configured is still seen.
            host.Binding().resolveResources = Function<resource::ResourceManager*()>{
                [self]() -> resource::ResourceManager*
                { return self->m_resourcesGetter ? self->m_resourcesGetter() : nullptr; }};
            host.Binding().dispatchMessage =
                Function<void(scene::Scene*, scene::EntityHandle, StringView, Span<const Variant>)>{
                    [self](scene::Scene* scene, scene::EntityHandle target, StringView message,
                           Span<const Variant> args)
                    {
                        for (const SceneEntry& entry : self->m_systems)
                        {
                            if (entry.scene == scene && entry.system != nullptr)
                            {
                                entry.system->EnqueueMessage(target, message, args);
                                return;
                            }
                        }
                    }};
        }

        /// Tear down `host` if nothing pins it: no game-script hold, no scene bound to it simulating,
        /// no live behavior instances in those scenes. Per-host, so an
        /// instance's host and the editor's host tear down independently. Called by the scene-stop
        /// observer, OnSceneDestroyed, and an ApplicationRun on its own host.
        /// A host that stays collects in full instead: a scene stopped or went, so the frame is
        /// one where a pause is expected, and what it left is garbage now.
        void MaybeTeardownRunHost(BehaviorRunHost& host)
        {
            if (!host.IsActive())
            {
                return;
            }
            if (RunHostPinned(host))
            {
                host.CollectAllGarbage();
                return;
            }
            host.Teardown();
        }

        [[nodiscard]] bool RunHostPinned(BehaviorRunHost& host) const
        {
            if (host.HasGameScriptHold())
            {
                return true;
            }
            for (const SceneEntry& entry : m_systems)
            {
                if (entry.system == nullptr || entry.scene == nullptr)
                {
                    continue;
                }
                if (entry.system->Host() != &host)
                {
                    continue;
                } // only scenes bound to this host
                if (entry.system->Started() && entry.scene->SimulationEnabled())
                {
                    return true;
                }
                if (entry.system->InstanceCount() > 0)
                {
                    return true;
                }
            }
            return false;
        }

        // ---- contact events (neutral ingress) ----

        /// Host-app wiring: a getter for the run's resource manager, behind the resource-swap
        /// operations (SceneRender.setMesh, ...). A getter, not a pointer, because the app's manager
        /// is created after Configure runs: resolveResources reads it per call, so it sees the
        /// manager once the app has one, whatever the wiring order. The app owns the manager; this
        /// borrows it.
        void SetResourceManager(Function<resource::ResourceManager*()> getter)
        {
            m_resourcesGetter = Move(getter);
        }
        // The game-script run context is not acquired here: a
        // ApplicationRun owns its run host and drives its own game script through it. This subsystem is
        // machinery (routing, ISceneObserver, reflection, ConfigureRunHost and MaybeTeardownRunHost).

        // ---- scene integration ----

        void OnSystemsReady(scene::Scene& scene) override
        {
            EntityBehaviorSystem* system = scene.GetSystem<EntityBehaviorSystem>();
            // The scene's run's host, so its behaviours share that run's context (a game's, an
            // editor page's Simulate); a scene outside every run uses the default host. A scene's
            // run is set before its systems assemble. The teardown observer checks the system's
            // current host.
            BehaviorRunHost* host = RunHostFor(scene);
            system->SetRunHost(host);
            BehaviorSubsystem* self = this;
            system->SetRunObserver(Function<void()>{[self, system]()
                                                    {
                                                        if (system->Host() != nullptr)
                                                        {
                                                            self->MaybeTeardownRunHost(
                                                                *system->Host());
                                                        }
                                                    }});
            m_systems.PushBack(SceneEntry{&scene, system});

            // The scene-root script (the Level tier) shares the same run host.
            scene.GetSystem<SceneBehaviorSystem>()->SetRunHost(host);
        }
        void OnDestroying(scene::Scene& scene) override
        {
            BehaviorRunHost* host = nullptr;
            for (usize i = 0; i < m_systems.Size(); ++i)
            {
                if (m_systems[i].scene == &scene)
                {
                    if (m_systems[i].system != nullptr)
                    {
                        host = m_systems[i].system->Host();
                    }
                    m_systems.RemoveAt(i);
                    break;
                }
            }
            if (host != nullptr)
            {
                MaybeTeardownRunHost(*host);
            }
        }

        // Drives the default run host. A run drives its own host.
        void Update(f32 deltaTime) override { m_ownedRunHost.Advance(deltaTime); }

    protected:
        void OnInit() override
        {
            RegisterBehaviorComponentReflection();
            RegisterScriptFacadeReflection();
        }
        void OnReady() override
        {
            ConfigureRunHost(m_ownedRunHost); // wire the default (editor-scene) run host once
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::SystemsReady);
                scenes->RegisterObserver(this, scene::SceneLifecycleStage::Destroying);
            }
            }
        }
        void OnShutdown() override
        {
            if (foundation::runtime::SubsystemManager* context = Manager())
            {
            if (auto* scenes = context->GetSubsystem<engine::scene::SceneSubsystem>())
            {
                scenes->UnregisterObserver(this);
            }
            }
            for (const SceneEntry& entry : m_systems)
            {
                // Instances hold the context alive, so release them before the host.
                if (entry.system != nullptr && entry.scene != nullptr)
                {
                    entry.system->OnSceneStopped();
                }
            }
            m_ownedRunHost.Teardown(); // instance hosts are torn down by their owners
        }

    private:
        struct SceneEntry
        {
            scene::Scene* scene = nullptr;
            EntityBehaviorSystem* system = nullptr;
        };

        BehaviorRunHost m_ownedRunHost{
            foundation::core::DefaultAllocator()}; // the default run host (editor and editing
                                                   // scenes), on the process root allocator
        Array<SceneEntry> m_systems;
        Function<resource::ResourceManager*()>
            m_resourcesGetter; // resource swaps (late-bound; the app owns the manager)
    };
}

export namespace engine::behavior
{
    /// This domain's declaration: what it brings to a scene, to
    /// reflection, to the script surface and which resource modules come with it. Defined in the
    /// implementation unit (one instance per process); Engine.Composition lists it once.
    [[nodiscard]] const engine::DomainModule& BehaviorDomain() noexcept;
}
