// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script speed work's measures, in every language against native code. Per call (the script
// benchmark, Script.Tests/BackendBenchmark.h): a call into the engine, a value method, a string
// argument and a handler call, with the language's loop alone to subtract. Per scene: script
// behaviours on a real scene, ticked through the script subsystem, against the same work done by
// native components on the same scene:
//
//   handler-dispatch  1,000 behaviours with an empty update;
//   movers-<N>        N behaviours each reading its entity's position, moving it and writing it
//                     back, N = 100, 1,000 and 5,000;
//   component-fetch   one behaviour fetching its entity's PathFollowComponent 1,000 times a frame.
//
// Each runs 1,000 frames after a warm-up and prints `[bench] <language> <measure> <value> <unit>`
// lines: the median and the 99th percentile frame, and the allocations a frame made through the
// language's allocator (the engine's default heap is not counted). The cases are skipped in a
// normal run; run them with --no-skip from an optimised build (build/clang-reldbg).

#include <doctest/doctest.h>
#include <cstdio>

#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Script.Tests/BackendBenchmark.h"

import foundation.core;
import foundation.run;
import foundation.scene;
import foundation.script;
import foundation.script.facades;
import foundation.script.resource;
import engine.script;
import engine.spline; // PathFollowComponent: the component the fetch measure reaches
#if defined(INTEGRATION_HAS_ANGELSCRIPT)
import foundation.script.angelscript;
#endif
#if defined(INTEGRATION_HAS_LUAU)
import foundation.script.luau;
#endif

using namespace foundation::core;
using namespace foundation::script;
using namespace engine::script;
namespace scene = foundation::scene;

namespace
{
    constexpr u32 kWarmUpFrames = 20;
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
            RegisterCoreTypes();
            RegisterScriptComponentReflection();
            RegisterScriptFacadeReflection();
            engine::spline::RegisterSplineScriptFacade(); // PathFollowComponent and its of()
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
        ScriptRunHost host{counted};
        ScriptComponentManager* components = nullptr;
        ScriptSceneSystem* scripts = nullptr;
        engine::spline::PathFollowComponentManager* follows = nullptr;
        BenchNativeManager* natives = nullptr;

        BenchScene()
        {
            EnsureRegistered();
            scene.SetRun(&run);
            components = scene.AddSystem<ScriptComponentManager>();
            scripts = scene.AddSystem<ScriptSceneSystem>();
            engine::spline::AddSplineSceneManagers(scene);
            follows = scene.GetSystem<engine::spline::PathFollowComponentManager>();
            natives = scene.AddSystem<BenchNativeManager>();
            natives->follows = follows;
            components->SetScriptSystem(scripts);
            scripts->SetRunHost(&host);
        }

        void AddScripted(const RefPtr<ScriptClass>& cls, u32 count)
        {
            for (u32 i = 0; i < count; ++i)
            {
                const scene::EntityHandle e = scene.CreateEntity(u8"bench");
                follows->Add(e);
                ScriptBehavior behavior;
                behavior.script = cls;
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
            for (u32 i = 0; i < kWarmUpFrames; ++i)
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

    [[nodiscard]] RefPtr<ScriptClass> MakeClass(StringView language, StringView className, StringView source)
    {
        RefPtr<ScriptClass> cls = MakeRef<ScriptClass>(DefaultAllocator());
        cls->language = String(language);
        cls->className = String(className);
        cls->source = String(source);
        cls->handlers.PushBack(String(u8"onUpdate"));
        cls->BuildProfileName();
        return cls;
    }

    // The three behaviours in one language.
    struct Behaviours
    {
        StringView language;
        StringView empty;
        StringView mover;
        StringView fetcher;
    };

    void RunSceneMeasures(const Behaviours& in)
    {
        {
            BenchScene bench;
            bench.AddScripted(MakeClass(in.language, u8"Empty", in.empty), kHandlers);
            bench.Measure(in.language, "handler-dispatch");
        }
        const u32 sizes[] = {100, 1000, 5000};
        for (const u32 size : sizes)
        {
            BenchScene bench;
            bench.AddScripted(MakeClass(in.language, u8"Mover", in.mover), size);
            char name[32];
            std::snprintf(name, sizeof(name), "movers-%u", size);
            bench.Measure(in.language, name);
        }
        {
            BenchScene bench;
            bench.AddScripted(MakeClass(in.language, u8"Fetcher", in.fetcher), 1);
            bench.Measure(in.language, "component-fetch");
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
    dialect.benchClass = u8"class Holder { Float3 at; Holder@ self; }\n"
                         u8"class Bench {\n"
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
Bench = {}
Bench.__index = Bench
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

#if defined(INTEGRATION_HAS_ANGELSCRIPT)
TEST_CASE("script scene: the scene measures in AngelScript" * doctest::skip())
{
    Behaviours in;
    in.language = u8"angelscript";
    in.empty = u8"class Empty {\n"
               u8"    Empty(Entity entity) {}\n"
               u8"    void onUpdate(double dt) {}\n"
               u8"}\n";
    in.mover = u8"class Mover {\n"
               u8"    private Entity self;\n"
               u8"    float speed = 2.0f;\n"
               u8"    Mover(Entity entity) { self = entity; }\n"
               u8"    void onUpdate(double dt) {\n"
               u8"        Float3 p = self.position();\n"
               u8"        self.setPosition(p.x + speed * float(dt), p.y, p.z);\n"
               u8"    }\n"
               u8"}\n";
    in.fetcher = u8"class Fetcher {\n"
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
    in.empty = u8"Empty = {}\n"
               u8"Empty.__index = Empty\n"
               u8"function Empty.new(entity) return setmetatable({}, Empty) end\n"
               u8"function Empty:onUpdate(dt) end\n";
    in.mover = u8"Mover = {}\n"
               u8"Mover.__index = Mover\n"
               u8"function Mover.new(entity) return setmetatable({ entity = entity, speed = 2.0 }, Mover) end\n"
               u8"function Mover:onUpdate(dt)\n"
               u8"    local p = self.entity:position()\n"
               u8"    self.entity:setPosition(p.x + self.speed * dt, p.y, p.z)\n"
               u8"end\n";
    in.fetcher = u8"Fetcher = {}\n"
                 u8"Fetcher.__index = Fetcher\n"
                 u8"function Fetcher.new(entity) return setmetatable({ entity = entity }, Fetcher) end\n"
                 u8"function Fetcher:onUpdate(dt)\n"
                 u8"    for i = 1, 1000 do local f = PathFollowComponent.of(self.entity) end\n"
                 u8"end\n";
    RunSceneMeasures(in);
}
#endif

// The suite's normal run checks that each scene runs: a frame of each in each language, so a change
// that breaks a measure fails here rather than when someone next measures.
TEST_CASE("script scene: each measure's scene runs its behaviours")
{
#if defined(INTEGRATION_HAS_LUAU)
    BenchScene bench;
    bench.AddScripted(MakeClass(u8"luau", u8"Mover",
                                u8"Mover = {}\nMover.__index = Mover\n"
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
    BenchScene native;
    native.AddNative(NativeWork::Move, 3);
    native.scene.Start();
    native.scene.SetSimulationEnabled(true);
    native.Frame();
    for (scene::EntityHandle root = native.scene.GetFirstRoot(); root.IsAssigned(); root = native.scene.GetNextSibling(root))
        CHECK(native.scene.GetLocalTransform(root).position.x == doctest::Approx(2.0f * kDelta));
}
