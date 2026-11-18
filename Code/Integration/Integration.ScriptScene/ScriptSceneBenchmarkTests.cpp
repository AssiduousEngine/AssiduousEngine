// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script speed work's measures, in every language against native code. Per call (the script
// benchmark, Script.Tests/BackendBenchmark.h): a call into the engine, an entity method, a value
// method, a string argument and a handler call, with the language's loop alone to subtract. Per
// scene: script behaviours on a real scene, ticked through the script subsystem, against the same
// work done by native behaviors (C++ classes run through the same subsystem, no VM) and by native
// components (a manager's own loop) on the same scene:
//
//   handler-dispatch  1,000 behaviours with an empty update;
//   movers-<N>        N behaviours each reading its entity's position, moving it and writing it
//                     back, N = 100, 1,000 and 5,000;
//   component-fetch   one behaviour fetching its entity's PathFollowComponent 1,000 times a frame.
//
// Each runs 1,000 frames after a second's warm-up and prints `[bench] <language> <measure> <value>
// <unit>` lines: the median and the 99th percentile frame, and the allocations a frame made through
// the language's allocator (the engine's default heap is not counted). The cases are skipped in a
// normal run; run them with --no-skip from an optimised build (build/clang-reldbg).

#include <doctest/doctest.h>
#include <cstdio>

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include "Script.Tests/BackendBenchmark.h"

import foundation.core;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import engine.behavior;
import engine.spline; // PathFollowComponent: the component the fetch measure reaches
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
import foundation.script.angelscript;
#endif
#if defined(INTEGRATION_HAS_LUAU)
import foundation.script.luau;
#endif
#if defined(INTEGRATION_HAS_DOTNET)
import foundation.script.dotnet;
#endif

using namespace foundation::core;
using namespace foundation::script;
using namespace engine::behavior;
namespace scene = foundation::scene;

namespace
{
    // Frames before the timed ones: at least this many, for at least this long. A language that
    // compiles as it runs (C#'s tiered JIT recompiles hot code in the background, after a delay)
    // is timed warm, as a game runs after its first seconds.
    constexpr u32 kWarmUpFrames = 20;
    constexpr f64 kWarmUpSeconds = 1.0;
    constexpr u32 kFrames = 1000;
    constexpr u32 kHandlers = 1000;
    constexpr u32 kFetchesPerFrame = 1000;
    constexpr f32 kDelta = 1.0f / 60.0f;

}

namespace
{
    // The native side of each measure: a component per entity, updated by its manager in the
    // scene's update phase, doing what the script does.
    enum class NativeWork : u8
    {
        Empty, // an empty update, called through a pointer as a handler is
        Move,  // read the position, move it, write it back
        Fetch, // fetch its entity's PathFollowComponent kFetchesPerFrame times
    };
    struct BenchNative
    {
        f32 speed = 2.0f;
    };
    void EmptyUpdate(BenchNative&, f32) {}

    class BenchNativeManager final : public scene::ComponentManager<BenchNative>
    {
    public:
        NativeWork work = NativeWork::Empty;
        engine::spline::PathFollowComponentManager* follows = nullptr;

        void OnSceneCreate(scene::Scene& owner) override
        {
            scene::ComponentManager<BenchNative>::OnSceneCreate(owner);
            m_scene = &owner;
        }
        void OnUpdate(scene::ScenePhase phase, f32 deltaTime) override
        {
            if (phase != scene::ScenePhase::Update)
                return;
            Span<BenchNative> components = Dense();
            Span<const scene::EntityHandle> owners = Owners();
            void (*volatile update)(BenchNative&, f32) = &EmptyUpdate;
            for (usize i = 0; i < components.Size(); ++i)
            {
                switch (work)
                {
                case NativeWork::Empty:
                    update(components[i], deltaTime);
                    break;
                case NativeWork::Move:
                {
                    const Float3 p = m_scene->GetLocalTransform(owners[i]).position;
                    m_scene->SetLocalPosition(owners[i], Float3{p.x + components[i].speed * deltaTime, p.y, p.z});
                    break;
                }
                case NativeWork::Fetch:
                    for (u32 k = 0; k < kFetchesPerFrame; ++k)
                    {
                        engine::spline::PathFollowComponent* volatile fetched = follows->Get(owners[i]);
                        (void)fetched;
                    }
                    break;
                }
            }
        }

    private:
        scene::Scene* m_scene = nullptr;
    };

}

namespace benchnative
{
    // The three behaviours as native behaviors: C++ classes the behavior subsystem runs as it runs
    // a script's, through the same objects and the same facades, with no VM.
    class Empty final : public EntityBehavior
    {
        RTTI_OBJECT(Empty, EntityBehavior)
    public:
        void OnUpdate(f32) {}
    };

    class Mover final : public EntityBehavior
    {
        RTTI_OBJECT(Mover, EntityBehavior)
    public:
        f32 speed = 2.0f;

        void OnUpdate(f32 deltaTime)
        {
            const Float3 p = Self().position();
            Self().setPosition(p.x + speed * deltaTime, p.y, p.z);
        }
    };

    class Fetcher final : public EntityBehavior
    {
        RTTI_OBJECT(Fetcher, EntityBehavior)
    public:
        void OnUpdate(f32)
        {
            for (u32 i = 0; i < kFetchesPerFrame; ++i)
            {
                // What a script's PathFollowComponent.of(self) calls.
                const Variant fetched = ComponentOf<engine::spline::PathFollowComponent>(Self());
                (void)fetched;
            }
        }
    };

    REFLECT_MEMBERS(Empty, "rtti::benchnative")
    {
        builder.Method<&Empty::OnUpdate>("onUpdate");
        builder.Creator();
    }

    REFLECT_MEMBERS(Mover, "rtti::benchnative")
    {
        builder.Property<&Mover::speed>("speed");
        builder.Method<&Mover::OnUpdate>("onUpdate");
        builder.Creator();
    }

    REFLECT_MEMBERS(Fetcher, "rtti::benchnative")
    {
        builder.Method<&Fetcher::OnUpdate>("onUpdate");
        builder.Creator();
    }
}

namespace
{
    void EnsureRegistered()
    {
        static const bool once = []()
        {
            static ConsoleSink sink; // a script's errors reach the output
            GlobalLogger().AddSink(&sink);
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
            foundation::script::angelscript::RegisterAngelScriptBackend();
#endif
#if defined(INTEGRATION_HAS_LUAU)
            foundation::script::RegisterLuauScriptBackend();
#endif
#if defined(INTEGRATION_HAS_DOTNET)
            foundation::script::dotnet::RegisterDotNetScriptBackend();
#endif
            RegisterCoreTypes();
            RegisterBehaviorComponentReflection();
            RegisterScriptFacadeReflection();
            engine::spline::RegisterSplineScriptFacade(); // PathFollowComponent and its of()
            GlobalTypeRegistry().Register(benchnative::Empty::StaticType());
            GlobalTypeRegistry().Register(benchnative::Mover::StaticType());
            GlobalTypeRegistry().Register(benchnative::Fetcher::StaticType());
#if defined(INTEGRATION_HAS_DOTNET)
            // The process's C# runtime keeps the allocator it starts with for every object it hands
            // C# later, so it starts on one that lives as long as the process, as a game's and the
            // editor's do (the measures' managers each count over a shorter-lived one); and after
            // the types are reflected, which it reads when it starts.
            (void)foundation::script::dotnet::ProcessRuntime(DefaultAllocator());
#endif
            return true;
        }();
        (void)once;
    }

    // A scene with the script systems, as a run has them, its run host's managers over a counting
    // allocator.
    struct BenchScene
    {
        TrackingAllocator counted{DefaultAllocator()};
        foundation::run::RunBase run{DefaultAllocator(), u8"script-bench"};
        scene::Scene scene{DefaultAllocator(), u8"script-bench"};
        BehaviorRunHost host{counted};
        BehaviorComponentManager* components = nullptr;
        EntityBehaviorSystem* scripts = nullptr;
        engine::spline::PathFollowComponentManager* follows = nullptr;
        BenchNativeManager* natives = nullptr;

        BenchScene()
        {
            EnsureRegistered();
            scene.SetRun(&run);
            components = scene.AddSystem<BehaviorComponentManager>();
            scripts = scene.AddSystem<EntityBehaviorSystem>();
            engine::spline::AddSplineSceneManagers(scene);
            follows = scene.GetSystem<engine::spline::PathFollowComponentManager>();
            natives = scene.AddSystem<BenchNativeManager>();
            natives->follows = follows;
            components->SetScriptSystem(scripts);
            scripts->SetRunHost(&host);
        }

        void AddScripted(const RefPtr<BehaviorClass>& cls, u32 count)
        {
            for (u32 i = 0; i < count; ++i)
            {
                const scene::EntityHandle e = scene.CreateEntity(u8"bench");
                follows->Add(e);
                AttachedBehavior behavior;
                behavior.behaviorClass = cls;
                components->Add(e).behaviors.PushBack(Move(behavior));
            }
        }
        void AddNative(NativeWork work, u32 count)
        {
            natives->work = work;
            for (u32 i = 0; i < count; ++i)
            {
                const scene::EntityHandle e = scene.CreateEntity(u8"bench");
                follows->Add(e);
                natives->Add(e);
            }
        }

        void Frame()
        {
            host.Binding().timeSeconds += static_cast<f64>(kDelta);
            host.Binding().deltaSeconds = kDelta;
            run.Events().Drain();
            scene.Update(kDelta);
        }

        // Starts the scene, warms it up, then times kFrames frames and prints the measure.
        void Measure(StringView language, const char* measure)
        {
            scene.Start();
            scene.SetSimulationEnabled(true);
            const Stopwatch warming = Stopwatch::StartNew();
            for (u32 i = 0; i < kWarmUpFrames || warming.Elapsed().AsSeconds() < kWarmUpSeconds; ++i)
                Frame();
            Array<f64> frames;
            frames.Reserve(kFrames);
            const u64 allocations = counted.TotalAllocations();
            for (u32 i = 0; i < kFrames; ++i)
            {
                const Stopwatch clock = Stopwatch::StartNew();
                Frame();
                frames.PushBack(clock.Elapsed().AsSeconds() * 1e3);
            }
            const f64 allocationsPerFrame = static_cast<f64>(counted.TotalAllocations() - allocations) / kFrames;
            // Sorted, for the median and the 99th percentile.
            for (usize i = 1; i < frames.Size(); ++i)
                for (usize k = i; k > 0 && frames[k] < frames[k - 1]; --k)
                {
                    const f64 moved = frames[k];
                    frames[k] = frames[k - 1];
                    frames[k - 1] = moved;
                }
            const auto report = [&](const char* suffix, f64 value, const char* unit)
            {
                std::printf("[bench] %.*s %s%s %.4f %s\n", static_cast<int>(language.Size()),
                            reinterpret_cast<const char*>(language.Data()), measure, suffix, value, unit);
            };
            report("-median", frames[frames.Size() / 2], "ms/frame");
            report("-p99", frames[(frames.Size() * 99) / 100], "ms/frame");
            if (language != StringView(u8"native"))
                report("-allocations", allocationsPerFrame, "per frame");
            CHECK(scripts->InstanceCount() == components->Count());
        }
    };

    /// A class from its source, or for a language that loads a built assembly (C#) from `image`.
    [[nodiscard]] RefPtr<BehaviorClass> MakeClass(StringView language, StringView className, StringView source,
                                                Span<const byte> image = {})
    {
        RefPtr<BehaviorClass> cls = MakeRef<BehaviorClass>(DefaultAllocator());
        cls->language = String(language);
        cls->className = String(className);
        cls->kind = BehaviorKind::Entity; // every measured behavior sits on an entity
        cls->source = String(source);
        cls->sourceName = Format(u8"{}.bench", className);
        for (const byte b : image)
            cls->bytecode.PushBack(b);
        cls->handlers.PushBack(String(u8"onUpdate"));
        cls->BuildProfileName();
        return cls;
    }

    // The three behaviours in one language: each one's class name and source (the class names
    // alone for native behaviors, which have no source; C#'s built assembly holds all three), and
    // the column they print as.
    struct Behaviours
    {
        StringView language;
        StringView column;
        StringView emptyClass = u8"Empty";
        StringView moverClass = u8"Mover";
        StringView fetcherClass = u8"Fetcher";
        StringView empty;
        StringView mover;
        StringView fetcher;
        Span<const byte> image; // C#: the built assembly holding all three
    };

    void RunSceneMeasures(const Behaviours& in)
    {
        {
            BenchScene bench;
            bench.AddScripted(MakeClass(in.language, in.emptyClass, in.empty, in.image), kHandlers);
            bench.Measure(in.column, "handler-dispatch");
        }
        const u32 sizes[] = {100, 1000, 5000};
        for (const u32 size : sizes)
        {
            BenchScene bench;
            bench.AddScripted(MakeClass(in.language, in.moverClass, in.mover, in.image), size);
            char name[32];
            std::snprintf(name, sizeof(name), "movers-%u", size);
            bench.Measure(in.column, name);
        }
        {
            BenchScene bench;
            bench.AddScripted(MakeClass(in.language, in.fetcherClass, in.fetcher, in.image), 1);
            bench.Measure(in.column, "component-fetch");
        }
    }
}

TEST_CASE("script per call: native, the baseline" * doctest::skip())
{
    foundation::script::benchmark::RunNativeBenchmark();
}

#if defined(INTEGRATION_HAS_ANGELSCRIPT)
TEST_CASE("script per call: AngelScript" * doctest::skip())
{
    foundation::script::benchmark::Dialect dialect;
    dialect.languageId = u8"angelscript";
    dialect.benchClass = u8"class Holder : EntityBehavior { Float3 at; Holder@ self; }\n"
                         u8"class Bench : EntityBehavior {\n"
                         u8"  double total = 0;\n"
                         u8"  void tick(double dt) { total += dt; }\n"
                         u8"  double math(int n) {\n"
                         u8"    Float3 a = Float3(1, 2, 3);\n"
                         u8"    Float3 b = Float3(0.5f, 0.25f, 0.125f);\n"
                         u8"    double sum = 0;\n"
                         u8"    for (int i = 0; i < n; i++) {\n"
                         u8"      Float3 d = a + b * 2.0f;\n"
                         u8"      Color c = Color(d.x, d.y, d.z, 1);\n"
                         u8"      sum += Float3::Dot(d, a) + c.r;\n"
                         u8"    }\n"
                         u8"    return sum;\n"
                         u8"  }\n"
                         u8"  void Sleeper() { wait(1000.0f); }\n"
                         u8"  void spawn(int n) { for (int i = 0; i < n; i++) startCoroutine(ScriptCoroutine(this.Sleeper)); }\n"
                         u8"  void churn(int n) {\n"
                         u8"    for (int i = 0; i < n; i++) { Holder h; @h.self = h; h.at = Float3(1, 2, 3); }\n"
                         u8"  }\n"
                         u8"  double loops(int n) { double sum = 0; for (int i = 0; i < n; i++) sum += float(i); return sum; }\n"
                         u8"  double calls(int n) { double sum = 0; for (int i = 0; i < n; i++) sum += Math::Abs(float(i)); return sum; }\n"
                         u8"  double crosses(int n) {\n"
                         u8"    Float3 a = Float3(1, 2, 3);\n"
                         u8"    double sum = 0;\n"
                         u8"    for (int i = 0; i < n; i++) sum += Float3::Cross(a, Float3(float(i), 0.5f, 0.25f)).x;\n"
                         u8"    return sum;\n"
                         u8"  }\n"
                         u8"  double checks(int n, Entity e) { double valid = 0; for (int i = 0; i < n; i++) if (e.isValid()) valid += 1; return valid; }\n"
                         u8"  double texts(int n, string s, Entity e) { for (int i = 0; i < n; i++) e.setName(s); return n; }\n"
                         u8"}\n";
    foundation::script::benchmark::RunScriptBackendBenchmark(
        [](IAllocator& allocator) { return foundation::script::angelscript::CreateScriptManager(allocator); }, dialect);
}
#endif

#if defined(INTEGRATION_HAS_LUAU)
TEST_CASE("script per call: Luau" * doctest::skip())
{
    foundation::script::benchmark::Dialect dialect;
    dialect.languageId = u8"luau";
    dialect.benchClass = u8R"lua(
Bench = EntityBehavior.class()
function Bench.new() return setmetatable({ total = 0 }, Bench) end
function Bench:tick(dt) self.total = self.total + dt end
function Bench:math(n)
    local a = Float3.new(1, 2, 3)
    local b = Float3.new(0.5, 0.25, 0.125)
    local sum = 0
    for i = 1, n do
        local d = a + b * 2
        local c = Color.new(d.x, d.y, d.z, 1)
        sum = sum + Float3.Dot(d, a) + c.r
    end
    return sum
end
function Bench:spawn(n)
    for i = 1, n do startCoroutine(function() waitSeconds(1000) end, self) end
end
function Bench:churn(n)
    for i = 1, n do local h = {}; h.self = h; h.at = Float3.new(1, 2, 3) end
end
function Bench:loops(n)
    local sum = 0
    for i = 1, n do sum = sum + i end
    return sum
end
function Bench:calls(n)
    local sum = 0
    for i = 1, n do sum = sum + Math.Abs(i) end
    return sum
end
function Bench:crosses(n)
    local a = Float3.new(1, 2, 3)
    local sum = 0
    for i = 1, n do sum = sum + Float3.Cross(a, Float3.new(i, 0.5, 0.25)).x end
    return sum
end
function Bench:checks(n, e)
    local valid = 0
    for i = 1, n do if e:isValid() then valid = valid + 1 end end
    return valid
end
function Bench:texts(n, s, e)
    for i = 1, n do e:setName(s) end
    return n
end
)lua";
    foundation::script::benchmark::RunScriptBackendBenchmark(
        [](IAllocator& allocator) { return foundation::script::CreateLuauScriptManager(allocator); }, dialect);
}
#endif

TEST_CASE("script scene: the scene measures in native components, the baseline" * doctest::skip())
{
    {
        BenchScene bench;
        bench.AddNative(NativeWork::Empty, kHandlers);
        bench.Measure(u8"native", "handler-dispatch");
    }
    const u32 sizes[] = {100, 1000, 5000};
    for (const u32 size : sizes)
    {
        BenchScene bench;
        bench.AddNative(NativeWork::Move, size);
        char name[32];
        std::snprintf(name, sizeof(name), "movers-%u", size);
        bench.Measure(u8"native", name);
    }
    {
        BenchScene bench;
        bench.AddNative(NativeWork::Fetch, 1);
        bench.Measure(u8"native", "component-fetch");
    }
}

TEST_CASE("script scene: the scene measures in native behaviors" * doctest::skip())
{
    Behaviours in;
    in.language = kNativeLanguage;
    in.column = u8"native-behavior";
    in.emptyClass = u8"rtti::benchnative::Empty";
    in.moverClass = u8"rtti::benchnative::Mover";
    in.fetcherClass = u8"rtti::benchnative::Fetcher";
    RunSceneMeasures(in);
}

#if defined(INTEGRATION_HAS_ANGELSCRIPT)
TEST_CASE("script scene: the scene measures in AngelScript" * doctest::skip())
{
    Behaviours in;
    in.language = u8"angelscript";
    in.column = in.language;
    in.empty = u8"class Empty : EntityBehavior {\n"
               u8"    Empty(Entity entity) {}\n"
               u8"    void onUpdate(double dt) {}\n"
               u8"}\n";
    in.mover = u8"class Mover : EntityBehavior {\n"
               u8"    private Entity self;\n"
               u8"    float speed = 2.0f;\n"
               u8"    Mover(Entity entity) { self = entity; }\n"
               u8"    void onUpdate(double dt) {\n"
               u8"        Float3 p = self.position();\n"
               u8"        self.setPosition(p.x + speed * float(dt), p.y, p.z);\n"
               u8"    }\n"
               u8"}\n";
    in.fetcher = u8"class Fetcher : EntityBehavior {\n"
                 u8"    private Entity self;\n"
                 u8"    Fetcher(Entity entity) { self = entity; }\n"
                 u8"    void onUpdate(double dt) {\n"
                 u8"        for (int i = 0; i < 1000; i++) { PathFollowComponent@ f = PathFollowComponent::of(self); }\n"
                 u8"    }\n"
                 u8"}\n";
    RunSceneMeasures(in);
}
#endif

#if defined(INTEGRATION_HAS_LUAU)
TEST_CASE("script scene: the scene measures in Luau" * doctest::skip())
{
    Behaviours in;
    in.language = u8"luau";
    in.column = in.language;
    in.empty = u8"Empty = EntityBehavior.class()\n"
               u8"function Empty.new(entity) return setmetatable({}, Empty) end\n"
               u8"function Empty:onUpdate(dt) end\n";
    in.mover = u8"Mover = EntityBehavior.class()\n"
               u8"function Mover.new(entity) return setmetatable({ entity = entity, speed = 2.0 }, Mover) end\n"
               u8"function Mover:onUpdate(dt)\n"
               u8"    local p = self.entity:position()\n"
               u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
               u8"end\n";
    in.fetcher = u8"Fetcher = EntityBehavior.class()\n"
                 u8"function Fetcher.new(entity) return setmetatable({ entity = entity }, Fetcher) end\n"
                 u8"function Fetcher:onUpdate(dt)\n"
                 u8"    for i = 1, 1000 do local f = PathFollowComponent.of(self.entity) end\n"
                 u8"end\n";
    RunSceneMeasures(in);
}
#endif

#if defined(INTEGRATION_HAS_DOTNET)
namespace
{
    // The C# measures' assembly, as the cook builds a game's (Debug) or optimised (Release).
    [[nodiscard]] Array<byte> CSharpBench(const char* folder)
    {
        Result<Array<byte>> image = ReadFile(PathJoin(StringView(reinterpret_cast<const utf8char*>(folder)), u8"Bench.dll").AsView());
        REQUIRE(image.HasValue());
        return Move(image.Value());
    }

    void RunCSharpPerCall(const char* folder, StringView label)
    {
        EnsureRegistered(); // the C# runtime on a process-lived allocator first
        const Array<byte> image = CSharpBench(folder);
        foundation::script::benchmark::Dialect dialect;
        dialect.languageId = label;
        dialect.collector = false; // C#'s collector is .NET's; the run steps none
        dialect.load = [&image](IScriptContext& context)
        {
            const BehaviorModuleClass bench{u8"Bench.cs", u8"", {}, Span<const byte>{image.Data(), image.Size()}};
            return context.LoadBehaviorModule(Span<const BehaviorModuleClass>{&bench, 1}, u8"bench").IsOk();
        };
        foundation::script::benchmark::RunScriptBackendBenchmark(
            [](IAllocator& allocator) { return foundation::script::dotnet::CreateDotNetScriptManager(allocator); }, dialect);
    }
}

TEST_CASE("script per call: C#" * doctest::skip())
{
    RunCSharpPerCall(BENCH_CSHARP_DEBUG_DIR, u8"csharp-debug");
    RunCSharpPerCall(BENCH_CSHARP_RELEASE_DIR, u8"csharp-release");
}

TEST_CASE("script scene: the scene measures in C#" * doctest::skip())
{
    const char* folders[] = {BENCH_CSHARP_DEBUG_DIR, BENCH_CSHARP_RELEASE_DIR};
    const StringView labels[] = {u8"csharp-debug", u8"csharp-release"};
    for (usize i = 0; i < 2; ++i)
    {
        const Array<byte> image = CSharpBench(folders[i]);
        Behaviours in;
        in.language = foundation::script::dotnet::kCSharpLanguage;
        in.image = Span<const byte>{image.Data(), image.Size()};
        in.column = labels[i];
        RunSceneMeasures(in);
    }
}
#endif

// The suite's normal run checks that each scene runs: movers in Luau, as native behaviors, in C#
// where it is built, and as native components, so a change that breaks a measure fails here rather
// than when someone next measures.
TEST_CASE("script scene: each measure's scene runs its behaviours")
{
#if defined(INTEGRATION_HAS_LUAU)
    BenchScene bench;
    bench.AddScripted(MakeClass(u8"luau", u8"Mover",
                                u8"Mover = EntityBehavior.class()\n"
                                u8"function Mover.new(entity) return setmetatable({ entity = entity }, Mover) end\n"
                                u8"function Mover:onUpdate(dt) local p = self.entity:position(); self.entity:setPosition(p.x + 1, p.y, p.z) end\n"),
                      3);
    bench.scene.Start();
    bench.scene.SetSimulationEnabled(true);
    bench.Frame();
    bench.Frame();
    CHECK(bench.scripts->InstanceCount() == 3u);
    for (scene::EntityHandle root = bench.scene.GetFirstRoot(); root.IsAssigned(); root = bench.scene.GetNextSibling(root))
        CHECK(bench.scene.GetLocalTransform(root).position.x == doctest::Approx(2.0f));
#endif
    BenchScene behaviors;
    behaviors.AddScripted(MakeClass(kNativeLanguage, u8"rtti::benchnative::Mover", u8""), 3);
    behaviors.scene.Start();
    behaviors.scene.SetSimulationEnabled(true);
    behaviors.Frame();
    CHECK(behaviors.scripts->InstanceCount() == 3u);
    for (scene::EntityHandle root = behaviors.scene.GetFirstRoot(); root.IsAssigned(); root = behaviors.scene.GetNextSibling(root))
        CHECK(behaviors.scene.GetLocalTransform(root).position.x == doctest::Approx(2.0f * kDelta));

#if defined(INTEGRATION_HAS_DOTNET)
    {
        const Array<byte> image = CSharpBench(BENCH_CSHARP_DEBUG_DIR);
        BenchScene csharp;
        csharp.AddScripted(MakeClass(foundation::script::dotnet::kCSharpLanguage, u8"Mover", u8"", Span<const byte>{image.Data(), image.Size()}), 3);
        csharp.scene.Start();
        csharp.scene.SetSimulationEnabled(true);
        csharp.Frame();
        csharp.Frame();
        CHECK(csharp.scripts->InstanceCount() == 3u);
        for (scene::EntityHandle root = csharp.scene.GetFirstRoot(); root.IsAssigned(); root = csharp.scene.GetNextSibling(root))
            CHECK(csharp.scene.GetLocalTransform(root).position.x == doctest::Approx(2.0f * 2.0f * kDelta));

        // A component C# holds as a value: set through its handle, valid while the entity has it.
        BenchScene components;
        components.AddScripted(MakeClass(foundation::script::dotnet::kCSharpLanguage, u8"Pacer", u8"",
                                         Span<const byte>{image.Data(), image.Size()}),
                               1);
        components.scene.Start();
        components.scene.SetSimulationEnabled(true);
        components.Frame();
        const scene::EntityHandle paced = components.scene.GetFirstRoot();
        REQUIRE(components.follows->Get(paced) != nullptr);
        CHECK(components.follows->Get(paced)->speed == doctest::Approx(4.0f));
        CHECK(components.scene.GetEntityName(paced) == StringView(u8"has it"));
        CHECK(components.scene.RemoveComponent(*components.follows, paced));
        components.Frame();
        CHECK(components.follows->Get(paced) == nullptr);
        CHECK(components.scene.GetEntityName(paced) == StringView(u8"has none"));
    }
#endif
    BenchScene native;
    native.AddNative(NativeWork::Move, 3);
    native.scene.Start();
    native.scene.SetSimulationEnabled(true);
    native.Frame();
    for (scene::EntityHandle root = native.scene.GetFirstRoot(); root.IsAssigned(); root = native.scene.GetNextSibling(root))
        CHECK(native.scene.GetLocalTransform(root).position.x == doctest::Approx(2.0f * kDelta));
}
