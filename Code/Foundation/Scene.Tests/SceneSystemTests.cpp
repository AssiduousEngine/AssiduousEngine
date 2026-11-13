// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Systems wired into the Scene: ownership and lookup, the phase-ordered update
// loop, deferred destroy during update, entity-destroy freeing components, active-change
// and start/stop notification, UpdateOrder, and simulation gating.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.scene;
import foundation.run; // RunBase: a scene's run, whose bus is the scene's

using namespace foundation::core;
using namespace foundation::scene;

namespace
{
    struct Health
    {
        f32 value = 100.0f;
    };

    class HealthManager : public ComponentManager<Health>
    {
    public:
        int initialized = 0, destroyed = 0;

    protected:
        void OnComponentInitialized(Health&, EntityHandle) override { ++initialized; }
        void OnComponentDestroyed(Health&, EntityHandle) override { ++destroyed; }
    };

    // Records which phases/notifications it received.
    class Recorder : public SceneSystem
    {
    public:
        Array<ScenePhase> phases;
        int started = 0, stopped = 0, entityDestroyed = 0, activeChanged = 0, fixedUpdates = 0;
        void OnUpdate(ScenePhase p, f32) override { phases.PushBack(p); }
        void OnFixedUpdate(f32) override { ++fixedUpdates; }
        void OnSceneStarted() override { ++started; }
        void OnSceneStopped() override { ++stopped; }
        void OnEntityDestroyed(EntityHandle) override { ++entityDestroyed; }
        void OnEntityActiveChanged(EntityHandle, bool) override { ++activeChanged; }
    };

    class SimOnly : public SceneSystem
    {
    public:
        int updates = 0;
        [[nodiscard]] bool IsSimulationOnly() const noexcept override { return true; }
        void OnUpdate(ScenePhase, f32) override { ++updates; }
    };
}

TEST_CASE("add/get systems by type; the Scene owns them")
{
    Scene scene{DefaultAllocator()};
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    REQUIRE(mgr != nullptr);
    CHECK(scene.GetSystem<HealthManager>() == mgr);
    CHECK(scene.HasSystem<HealthManager>());
    CHECK(scene.GetSystem<Recorder>() == nullptr);
}

TEST_CASE("update runs the gameplay phases in ScenePhase order")
{
    Scene scene{DefaultAllocator()};
    Recorder* rec = scene.AddSystem<Recorder>();
    scene.Update(0.016f);

    REQUIRE(rec->phases.Size() == 5);
    CHECK(rec->phases[0] == ScenePhase::PreUpdate);
    CHECK(rec->phases[1] == ScenePhase::Update);
    CHECK(rec->phases[2] == ScenePhase::AsyncUpdate);
    CHECK(rec->phases[3] == ScenePhase::PostUpdate);
    CHECK(rec->phases[4] == ScenePhase::PostTransform); // after the transform recompute
}

TEST_CASE("component init is deferred to the scene's Initialize phase")
{
    Scene scene{DefaultAllocator()};
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    EntityHandle e = scene.CreateEntity();
    mgr->Add(e).value = 50.0f;
    CHECK(mgr->initialized == 0); // not yet

    scene.Update(0.016f); // Initialize phase runs pending init
    CHECK(mgr->initialized == 1);
}

TEST_CASE("destroying an entity frees its component via the manager")
{
    Scene scene{DefaultAllocator()};
    HealthManager* mgr = scene.AddSystem<HealthManager>();
    EntityHandle e = scene.CreateEntity();
    mgr->Add(e);
    CHECK(mgr->Has(e));

    scene.DestroyEntity(e); // not during update, so immediate
    CHECK_FALSE(mgr->Has(e));
    CHECK(mgr->destroyed == 1);
}

TEST_CASE("destroy during update is deferred to Cleanup")
{
    Scene scene{DefaultAllocator()};
    EntityHandle target = scene.CreateEntity();

    // a system that destroys `target` during the Update phase
    struct Destroyer : SceneSystem
    {
        Scene* scene = nullptr;
        EntityHandle target = EntityHandle::Invalid();
        bool validDuringUpdate = false;
        void OnSceneCreate(Scene& s) override { scene = &s; }
        void OnUpdate(ScenePhase p, f32) override
        {
            if (p == ScenePhase::Update)
            {
                validDuringUpdate = scene->IsValid(target);
                scene->DestroyEntity(target);
            }
        }
    };
    Destroyer* d = scene.AddSystem<Destroyer>();
    d->target = target;

    scene.Update(0.016f);
    CHECK(d->validDuringUpdate);        // still alive mid-update (deferred)
    CHECK_FALSE(scene.IsValid(target)); // destroyed in Cleanup
}

TEST_CASE("active change + start/stop notify systems; fixed update ticks")
{
    Scene scene{DefaultAllocator()};
    Recorder* rec = scene.AddSystem<Recorder>();
    EntityHandle e = scene.CreateEntity();

    scene.SetActive(e, false);
    CHECK(rec->activeChanged == 1);

    scene.Start();
    CHECK(rec->started == 1);
    CHECK(scene.IsStarted());
    scene.FixedUpdate(0.02f);
    CHECK(rec->fixedUpdates == 1);
    scene.Stop();
    CHECK(rec->stopped == 1);
    CHECK_FALSE(scene.IsStarted());
}

TEST_CASE("simulation gating: sim-only systems skip when simulation is disabled")
{
    Scene scene{DefaultAllocator()};
    SimOnly* sim = scene.AddSystem<SimOnly>();

    scene.Update(0.016f);     // enabled by default
    CHECK(sim->updates == 5); // 5 phases

    scene.SetSimulationEnabled(false);
    scene.Update(0.016f);
    CHECK(sim->updates == 5); // unchanged: skipped
    scene.FixedUpdate(0.02f); // also skipped
    CHECK(sim->updates == 5);
}

TEST_CASE("systems run within a phase in UpdateOrder")
{
    Array<i32> order;
    struct Ordered : SceneSystem
    {
        i32 ord;
        Array<i32>* log;
        Ordered(i32 o, Array<i32>* l) : ord(o), log(l) {}
        [[nodiscard]] i32 UpdateOrder() const noexcept override { return ord; }
        void OnUpdate(ScenePhase p, f32) override
        {
            if (p == ScenePhase::Update)
            {
                log->PushBack(ord);
            }
        }
    };
    Scene scene{DefaultAllocator()};
    scene.AddSystem<Ordered>(10, &order); // added first, but higher order
    scene.AddSystem<Ordered>(-5, &order); // added second, lower order, runs first
    scene.Update(0.016f);
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == -5);
    CHECK(order[1] == 10);
}

TEST_CASE("scene: Events() is its run's bus - null outside every run, never drained by the scene")
{
    // A scene has no bus of its own: its run's is its bus. A scene outside every run has none and
    // never emits; the scene never drains (only its run does).
    Scene scene{DefaultAllocator()};
    CHECK(scene.Events() == nullptr); // outside every run: no bus, nothing to emit into

    foundation::run::RunBase run(DefaultAllocator(), u8"run");
    scene.SetRun(&run);
    REQUIRE(scene.Events() == &run.Events());
    int fired = 0;
    (void)run.Events().Subscribe(StringHash(u8"Ping"), [&](const Variant&) { ++fired; });
    scene.Events()->Publish(StringHash(u8"Ping"), Variant{});
    scene.Update(0.016f);
    CHECK(fired == 0);                        // the scene did not drain; its run does
    CHECK(run.Events().PendingCount() == 1u); // still queued for the run
    run.Events().Drain();
    CHECK(fired == 1);

    scene.SetRun(nullptr); // leaving the run leaves its bus
    CHECK(scene.Events() == nullptr);
}

namespace
{
    // A system that owns level geometry: one floor quad at y 0, when the box reaches it.
    class FloorSource final : public SceneSystem, public IStaticGeometrySource
    {
    public:
        IStaticGeometrySource* AsStaticGeometrySource() noexcept override { return this; }
        void CollectStaticGeometry(Scene&, const AABB& bounds, f32, Array<Float3>& out) override
        {
            if (bounds.min.y > 0.0f || bounds.max.y < 0.0f)
            {
                return;
            }
            const Float3 quad[] = {{-1, 0, -1}, {-1, 0, 1}, {1, 0, 1}, {-1, 0, -1}, {1, 0, 1}, {1, 0, -1}};
            for (const Float3& p : quad)
            {
                out.PushBack(p);
            }
        }
    };
}

// Navigation bakes from every system that answers AsStaticGeometrySource; the rest answer null.
TEST_CASE("a system with static geometry answers the capability, others answer null")
{
    Scene scene{DefaultAllocator()};
    scene.AddSystem<HealthManager>();
    scene.AddSystem<FloorSource>();
    Array<Float3> triangles;
    usize sources = 0;
    const AABB around{Float3{-5, -1, -5}, Float3{5, 1, 5}};
    scene.ForEachSystem(
        [&](SceneSystem& system)
        {
            if (IStaticGeometrySource* source = system.AsStaticGeometrySource())
            {
                ++sources;
                source->CollectStaticGeometry(scene, around, 0.3f, triangles);
            }
        });
    CHECK(sources == 1);
    CHECK(triangles.Size() == 6);
}

namespace
{
    // A system that measures one entity as a fixed box: what a render or physics domain answers.
    template <int Tag>
    class BoxMeasure final : public SceneSystem, public ISceneEntityBounds
    {
    public:
        EntityHandle entity = EntityHandle::Invalid();
        AABB box{};
        [[nodiscard]] ISceneEntityBounds* AsEntityBounds() noexcept override { return this; }
        [[nodiscard]] bool EntityBounds(Scene&, EntityHandle e, AABB& out) override
        {
            if (e != entity)
            {
                return false;
            }
            out = box;
            return true;
        }
    };
}

TEST_CASE("EntityWorldBounds merges what every measuring system answers, and says when none does")
{
    Scene scene{DefaultAllocator()};
    const EntityHandle crate = scene.CreateEntity(u8"Crate");
    const EntityHandle marker = scene.CreateEntity(u8"Marker");
    AABB out{Float3{7.0f, 7.0f, 7.0f}, Float3{7.0f, 7.0f, 7.0f}};

    // No system measures anything yet.
    CHECK_FALSE(EntityWorldBounds(scene, crate, out));
    CHECK(out.min.x == 7.0f); // untouched

    // One system: its box.
    auto* mesh = scene.AddSystem<BoxMeasure<0>>();
    mesh->entity = crate;
    mesh->box = AABB{Float3{0.0f, 0.0f, 0.0f}, Float3{1.0f, 2.0f, 1.0f}};
    REQUIRE(EntityWorldBounds(scene, crate, out));
    CHECK(out.max.y == 2.0f);

    // A second one (a collider reaching further): the box holding both.
    auto* collider = scene.AddSystem<BoxMeasure<1>>();
    collider->entity = crate;
    collider->box = AABB{Float3{-1.0f, 0.0f, 0.0f}, Float3{0.5f, 1.0f, 3.0f}};
    REQUIRE(EntityWorldBounds(scene, crate, out));
    CHECK(out.min.x == -1.0f);
    CHECK(out.max.y == 2.0f);
    CHECK(out.max.z == 3.0f);

    // An entity neither measures.
    CHECK_FALSE(EntityWorldBounds(scene, marker, out));
}

namespace
{
    // On its first fixed step, destroys `target` and records whether it was still valid after.
    class FixedDestroyer final : public SceneSystem
    {
    public:
        Scene* scene = nullptr;
        void OnSceneCreate(Scene& s) override { scene = &s; }
        Scene& GetScene() const { return *scene; }
        EntityHandle target = EntityHandle::Invalid();
        bool validAfterAsk = false;
        void OnFixedUpdate(f32) override
        {
            if (!target.IsAssigned())
                return;
            GetScene().DestroyEntity(target);
            validAfterAsk = GetScene().IsValid(target);
            target = EntityHandle::Invalid();
        }
    };

    // Destroying one entity destroys `also` from its hook, as a behaviour's onDestroy might.
    class ChainDestroyer final : public SceneSystem
    {
    public:
        Scene* scene = nullptr;
        void OnSceneCreate(Scene& s) override { scene = &s; }
        Scene& GetScene() const { return *scene; }
        EntityHandle trigger = EntityHandle::Invalid();
        EntityHandle also = EntityHandle::Invalid();
        int seen = 0;
        void OnEntityDestroyed(EntityHandle e) override
        {
            ++seen;
            if (e == trigger && GetScene().IsValid(also))
                GetScene().DestroyEntity(also);
        }
    };
}

TEST_CASE("scene: a destroy asked in the fixed step waits for the end of the step")
{
    Scene scene(DefaultAllocator(), u8"fixed");
    FixedDestroyer* destroyer = scene.AddSystem<FixedDestroyer>();
    const EntityHandle e = scene.CreateEntity(u8"e");
    destroyer->target = e;
    scene.SetFixedTiming(1.0f / 60.0f, 4);
    (void)scene.AdvanceTime(1.0f / 30.0f); // two steps
    CHECK(destroyer->validAfterAsk); // the step still saw it
    CHECK_FALSE(scene.IsValid(e));   // gone once the steps ended
}

TEST_CASE("scene: a destroy hook that destroys a sibling destroys it after, never inside, the first")
{
    Scene scene(DefaultAllocator(), u8"hooks");
    ChainDestroyer* chain = scene.AddSystem<ChainDestroyer>();
    const EntityHandle parent = scene.CreateEntity(u8"parent");
    const EntityHandle first = scene.CreateEntity(u8"first");
    const EntityHandle second = scene.CreateEntity(u8"second");
    scene.SetParent(first, parent);
    scene.SetParent(second, parent);
    chain->trigger = first;
    chain->also = second;
    scene.DestroyEntity(first);
    CHECK_FALSE(scene.IsValid(first));
    CHECK_FALSE(scene.IsValid(second)); // destroyed in the same call, after the first
    CHECK(scene.IsValid(parent));
    CHECK(chain->seen == 2);
    const EntityHandle next = scene.CreateEntity(u8"next"); // the free list is sound
    CHECK(scene.IsValid(next));
    CHECK(scene.EntityCount() == 2u);
}

TEST_CASE("scene: while its run defers, a destroy waits for the run's flush; a scene gone first cancels it")
{
    foundation::run::RunBase run(DefaultAllocator(), u8"run");
    {
        Scene scene(DefaultAllocator(), u8"deferred");
        scene.SetRun(&run);
        const EntityHandle e = scene.CreateEntity(u8"e");
        {
            foundation::run::RunMutations::Scope scripts(run.Mutations());
            scene.DestroyEntity(e);
            CHECK(scene.IsValid(e));
        }
        CHECK(scene.IsValid(e));
        run.Mutations().Flush();
        CHECK_FALSE(scene.IsValid(e));
    }
    {
        UniquePtr<Scene> scene = MakeUnique<Scene>(DefaultAllocator(), DefaultAllocator(), u8"gone");
        scene->SetRun(&run);
        const EntityHandle e = scene->CreateEntity(u8"e");
        {
            foundation::run::RunMutations::Scope scripts(run.Mutations());
            scene->DestroyEntity(e);
        }
        scene.Reset(); // the scene goes before the flush
        CHECK(run.Mutations().PendingCount() == 0u);
        run.Mutations().Flush(); // nothing reaches the freed scene
    }
}

namespace
{
    // When `trigger` is destroyed, moves `away` to the roots and `into` under `parent`, as a
    // behaviour's onDestroy might.
    class Reparenter final : public SceneSystem
    {
    public:
        Scene* scene = nullptr;
        EntityHandle trigger = EntityHandle::Invalid();
        EntityHandle away = EntityHandle::Invalid();
        EntityHandle into = EntityHandle::Invalid();
        EntityHandle parent = EntityHandle::Invalid();
        void OnSceneCreate(Scene& s) override { scene = &s; }
        void OnEntityDestroyed(EntityHandle e) override
        {
            if (e != trigger)
                return;
            scene->SetParent(away, EntityHandle::Invalid());
            scene->SetParent(into, parent);
        }
    };
}

TEST_CASE("scene: a subtree's destroy follows its children as hooks move them, never a stale sibling")
{
    Scene scene(DefaultAllocator(), u8"subtree");
    Reparenter* hooks = scene.AddSystem<Reparenter>();
    const EntityHandle parent = scene.CreateEntity(u8"parent");
    const EntityHandle first = scene.CreateEntity(u8"first");
    const EntityHandle second = scene.CreateEntity(u8"second");
    const EntityHandle outsider = scene.CreateEntity(u8"outsider");
    scene.SetParent(first, parent);
    scene.SetParent(second, parent);
    hooks->trigger = first;
    hooks->away = second;   // the sibling read before the first went, moved out
    hooks->into = outsider; // moved in while the parent goes
    hooks->parent = parent;
    scene.DestroyEntity(parent);
    CHECK_FALSE(scene.IsValid(parent));
    CHECK_FALSE(scene.IsValid(first));
    CHECK(scene.IsValid(second)); // no longer a child when its turn would have come
    CHECK_FALSE(scene.GetParent(second).IsAssigned());
    CHECK_FALSE(scene.IsValid(outsider)); // a child by then: it went with the parent
    CHECK(scene.EntityCount() == 1u);
    const EntityHandle next = scene.CreateEntity(u8"next"); // the free list is sound
    const EntityHandle more = scene.CreateEntity(u8"more");
    CHECK(next != more);
    CHECK(scene.EntityCount() == 3u);
}
