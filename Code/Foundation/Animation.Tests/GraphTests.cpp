// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Animation graph stack: parameters, conditions, transitions, states, layers, blend trees, the
// graph, and a graph-player state-machine check.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;

using namespace foundation::core;
using namespace foundation::animation;

static IAnimationStateNode* kNullNode = static_cast<IAnimationStateNode*>(nullptr);

// ---- BoneMask ----
TEST_CASE("graph: BoneMask weights, clamping, out-of-range")
{
    BoneMask mask{4, 1.0f};
    CHECK(mask.BoneCount() == 4);
    CHECK(mask.GetWeight(0) == 1.0f);
    mask.SetWeight(2, 0.75f);
    CHECK(mask.GetWeight(2) == 0.75f);
    mask.SetWeight(0, 2.0f);
    CHECK(mask.GetWeight(0) == 1.0f); // clamp high
    mask.SetWeight(1, -1.0f);
    CHECK(mask.GetWeight(1) == 0.0f);  // clamp low
    CHECK(mask.GetWeight(-1) == 0.0f); // out of range
    CHECK(mask.GetWeight(99) == 0.0f);
    BoneMask m2{2, 0.0f};
    m2.SetAll(5.0f);
    CHECK(m2.GetWeight(0) == 1.0f); // SetAll clamps
}

TEST_CASE("graph: BoneMask SetBoneChainWeight follows hierarchy")
{
    Skeleton s{3};
    s.Bones()[0].index = 0;
    s.Bones()[0].parentIndex = -1;
    s.Bones()[1].index = 1;
    s.Bones()[1].parentIndex = 0;
    s.Bones()[2].index = 2;
    s.Bones()[2].parentIndex = 1;
    s.FindRootBones();
    s.BuildChildIndices();
    BoneMask mask{3, 0.0f};
    mask.SetBoneChainWeight(s, 0, 1.0f); // root and all descendants
    CHECK(mask.GetWeight(0) == 1.0f);
    CHECK(mask.GetWeight(1) == 1.0f);
    CHECK(mask.GetWeight(2) == 1.0f);
}

// ---- AnimationGraphParameter ----
TEST_CASE("graph: parameter set/get + trigger consume")
{
    AnimationGraphParameter sp{u8"Speed", AnimationParameterType::Float};
    CHECK(sp.floatValue == 0.0f);
    CHECK(sp.type == AnimationParameterType::Float);
    CHECK(sp.Name() == StringView{u8"Speed"});
    sp.floatValue = 1.5f;
    CHECK(sp.floatValue == 1.5f);

    AnimationGraphParameter trig{u8"Fire", AnimationParameterType::Trigger};
    trig.boolValue = true;
    trig.ConsumeTrigger();
    CHECK(trig.boolValue == false); // trigger resets

    AnimationGraphParameter b{u8"Flag", AnimationParameterType::Bool};
    b.boolValue = true;
    b.ConsumeTrigger();
    CHECK(b.boolValue == true); // bool not affected
}

// ---- AnimationGraphCondition ----
TEST_CASE("graph: condition float/int/bool comparisons")
{
    AnimationGraphParameter sp{u8"Speed", AnimationParameterType::Float};
    sp.floatValue = 0.5f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 0.1f}.Evaluate(&sp) == true);
    sp.floatValue = 0.05f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 0.1f}.Evaluate(&sp) == false);
    sp.floatValue = 0.1f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::LessEqual, 0.1f}.Evaluate(&sp) == true);
    sp.floatValue = 1.0f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Equal, 1.0f}.Evaluate(&sp) == true);

    AnimationGraphParameter lvl{u8"Level", AnimationParameterType::Int};
    lvl.intValue = 5;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 3.0f}.Evaluate(&lvl) == true);

    AnimationGraphParameter g{u8"Grounded", AnimationParameterType::Bool};
    g.boolValue = true;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Equal, 1.0f}.Evaluate(&g) == true); // "is true"
    g.boolValue = false;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Equal, 0.0f}.Evaluate(&g) == true); // "is false"

    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 0.0f}.Evaluate(nullptr) == false);
}

// ---- AnimationGraphTransition ----
TEST_CASE("graph: transition condition evaluation")
{
    Array<AnimationGraphParameter> params;
    params.PushBack(AnimationGraphParameter{u8"Speed", AnimationParameterType::Float});
    params.PushBack(AnimationGraphParameter{u8"Grounded", AnimationParameterType::Bool});
    params[0].floatValue = 1.0f;
    params[1].boolValue = true;
    const Span<const AnimationGraphParameter> pv{params.Data(), params.Size()};

    AnimationGraphTransition t;
    CHECK(t.sourceStateIndex == -1); // defaults
    CHECK(t.duration == 0.25f);
    CHECK(t.EvaluateConditions(pv) == true); // no conditions: unconditional

    t.AddFloatCondition(0, ComparisonOp::Greater, 0.1f);
    t.AddBoolCondition(1, true);
    CHECK(t.EvaluateConditions(pv) == true); // both met
    params[1].boolValue = false;
    CHECK(t.EvaluateConditions(pv) == false); // one fails

    AnimationGraphTransition bad;
    bad.AddFloatCondition(5, ComparisonOp::Greater, 0.0f); // index out of range
    CHECK(bad.EvaluateConditions(pv) == false);
}

// ---- AnimationGraphState ----
TEST_CASE("graph: state defaults + node ownership")
{
    AnimationGraphState idle{u8"Idle", kNullNode};
    CHECK(idle.Name() == StringView{u8"Idle"});
    CHECK(idle.Node() == nullptr);
    CHECK(idle.speed == 1.0f);
    CHECK(idle.loop == true);
    CHECK(idle.OwnsNode() == false);
    CHECK(idle.Duration() == 0.0f);

    // An owned node is freed on destruction (UniquePtr), with no leak or crash.
    {
        AnimationGraphState owned{
            u8"Owned",
            MakeUnique<ClipStateNode>(DefaultAllocator(), static_cast<AnimationClip*>(nullptr))};
        CHECK(owned.OwnsNode() == true);
    }
    // Borrowed node survives the state.
    ClipStateNode node{nullptr};
    {
        AnimationGraphState borrow{u8"Borrow", &node};
        CHECK(borrow.OwnsNode() == false);
    }
    CHECK(node.Duration() == 0.0f);
}

// ---- AnimationLayer ----
TEST_CASE("graph: layer states/transitions/mask")
{
    AnimationLayer layer{u8"Base"};
    CHECK(layer.Name() == StringView{u8"Base"});
    CHECK(layer.defaultStateIndex == 0);
    CHECK(layer.blendMode == LayerBlendMode::Override);
    CHECK(layer.weight == 1.0f);
    CHECK(layer.Mask() == nullptr);

    CHECK(layer.AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Idle"},
                                                         kNullNode)) == 0);
    CHECK(layer.AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Walk"},
                                                         kNullNode)) == 1);
    CHECK(layer.States().Size() == 2);
    CHECK(layer.GetState(1)->Name() == StringView{u8"Walk"});
    CHECK(layer.GetState(-1) == nullptr);
    CHECK(layer.GetState(99) == nullptr);
    CHECK(layer.FindStateIndex(u8"Walk") == 1);

    layer.AddTransition(MakeUnique<AnimationGraphTransition>(DefaultAllocator()));
    CHECK(layer.Transitions().Size() == 1);

    layer.SetMask(MakeUnique<BoneMask>(DefaultAllocator(), 4, 0.0f));
    layer.Mask()->SetWeight(0, 1.0f);
    CHECK(layer.Mask()->GetWeight(0) == 1.0f);
    CHECK(layer.Mask()->GetWeight(1) == 0.0f);
}

// ---- BlendTree1D / 2D ----
TEST_CASE("graph: BlendTree1D sorts by threshold, duration, empty-safe")
{
    BlendTree1D tree;
    tree.AddEntry(1.0f, nullptr);
    tree.AddEntry(0.0f, nullptr);
    tree.AddEntry(0.5f, nullptr);
    CHECK(tree.Entries().Size() == 3);
    CHECK(tree.Entries()[0].threshold == 0.0f);
    CHECK(tree.Entries()[1].threshold == 0.5f);
    CHECK(tree.Entries()[2].threshold == 1.0f);
    CHECK(tree.parameter == 0.0f);
    CHECK(tree.Duration() == 0.0f); // null clips
    tree.AddEntry(0.5f, nullptr);
    CHECK(tree.Entries().Size() == 4); // duplicates allowed

    Skeleton empty{0};
    BoneTransform poses[4] = {};
    BlendTree1D blank;
    blank.Evaluate(empty, 0.0f, Span<BoneTransform>{poses, 4}); // empty: no crash
}

TEST_CASE("graph: BlendTree2D entries + positions + duration")
{
    BlendTree2D tree;
    tree.AddEntry(0.0f, 0.0f, nullptr);
    tree.AddEntry(Float2{1, 0}, nullptr);
    tree.AddEntry(-1.0f, 2.5f, nullptr);
    CHECK(tree.Entries().Size() == 3);
    CHECK(tree.Entries()[1].position.x == 1.0f);
    CHECK(tree.Entries()[2].position.x == -1.0f);
    CHECK(tree.Entries()[2].position.y == 2.5f);
    CHECK(tree.parameterX == 0.0f);
    CHECK(tree.Duration() == 0.0f);
}

// ---- AnimationGraph ----
TEST_CASE("graph: parameters + layers + find (case sensitive)")
{
    AnimationGraph graph;
    CHECK(graph.AddParameter(u8"Speed", AnimationParameterType::Float) == 0);
    CHECK(graph.AddParameter(u8"Grounded", AnimationParameterType::Bool) == 1);
    CHECK(graph.Parameters().Size() == 2);
    CHECK(graph.FindParameter(u8"Speed") == 0);
    CHECK(graph.FindParameter(u8"Missing") == -1);
    CHECK(graph.FindParameter(u8"speed") == -1); // case sensitive
    CHECK(graph.GetParameter(0)->type == AnimationParameterType::Float);
    CHECK(graph.GetParameter(-1) == nullptr);

    CHECK(graph.AddLayer(MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"})) ==
          0);
    CHECK(graph.AddLayer(MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Upper"})) ==
          1);
    CHECK(graph.Layers().Size() == 2);
}

// ---- AnimationGraphPlayer (state-machine integration) ----
TEST_CASE("graph: player transitions states on a bool parameter")
{
    Skeleton skel{1};
    skel.Bones()[0].index = 0;
    skel.Bones()[0].parentIndex = -1;
    skel.FindRootBones();
    skel.BuildChildIndices();
    skel.ComputeInverseBindPoses();

    // Two clips so states have non-zero duration (the player advances normalized time by dt/duration).
    AnimationClip idleClip{u8"idle", 1.0f, true};
    idleClip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
    idleClip.GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 0, 0});
    AnimationClip walkClip{u8"walk", 1.0f, true};
    walkClip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
    walkClip.GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 0, 0});

    AnimationGraph graph;
    const i32 pMoving = graph.AddParameter(u8"Moving", AnimationParameterType::Bool);
    auto layer = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
    layer->AddState(MakeUnique<AnimationGraphState>(
        DefaultAllocator(), StringView{u8"Idle"},
        MakeUnique<ClipStateNode>(DefaultAllocator(), &idleClip))); // state 0
    layer->AddState(MakeUnique<AnimationGraphState>(
        DefaultAllocator(), StringView{u8"Walk"},
        MakeUnique<ClipStateNode>(DefaultAllocator(), &walkClip))); // state 1
    auto toWalk = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
    toWalk->sourceStateIndex = 0;
    toWalk->destStateIndex = 1;
    toWalk->duration = 0.001f;
    toWalk->AddBoolCondition(pMoving, true);
    layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toWalk));
    graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));

    AnimationGraphPlayer player{graph, skel};
    CHECK(player.GetCurrentStateIndex() == 0); // starts at default (Idle)

    player.Update(0.016f);
    CHECK(player.GetCurrentStateIndex() == 0); // condition false: stays

    player.SetBool(pMoving, true);
    player.Update(0.016f);
    CHECK(player.GetCurrentStateIndex() == 1); // transitioned to Walk
    Span<const Float4x4> skin = player.GetSkinningMatrices();
    CHECK(skin.Size() == 1);
}

namespace
{
    // Idle, Walk and Run each hold the bone at x = 0, 10 and 20; Go fades Idle to Walk, Fast fades
    // Walk to Run, each over one second.
    struct ThreeStates
    {
        Skeleton skel{1};
        AnimationClip clips[3] = {AnimationClip{u8"idle", 1.0f, true}, AnimationClip{u8"walk", 1.0f, true}, AnimationClip{u8"run", 1.0f, true}};
        AnimationGraph graph;
        i32 go = -1;
        i32 fast = -1;

        explicit ThreeStates(bool interruptible)
        {
            skel.Bones()[0].index = 0;
            skel.Bones()[0].parentIndex = -1;
            skel.FindRootBones();
            skel.BuildChildIndices();
            skel.ComputeInverseBindPoses();
            go = graph.AddParameter(u8"Go", AnimationParameterType::Bool);
            fast = graph.AddParameter(u8"Fast", AnimationParameterType::Bool);
            auto layer = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
            const char8_t* names[] = {u8"Idle", u8"Walk", u8"Run"};
            for (i32 i = 0; i < 3; ++i)
            {
                const f32 x = 10.0f * static_cast<f32>(i);
                clips[i].GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{x, 0, 0});
                clips[i].GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{x, 0, 0});
                layer->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{names[i]},
                                                                MakeUnique<ClipStateNode>(DefaultAllocator(), &clips[i])));
            }
            auto toWalk = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
            toWalk->sourceStateIndex = 0;
            toWalk->destStateIndex = 1;
            toWalk->duration = 1.0f;
            toWalk->interruptible = interruptible;
            toWalk->AddBoolCondition(go, true);
            layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toWalk));
            auto toRun = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
            toRun->sourceStateIndex = 1;
            toRun->destStateIndex = 2;
            toRun->duration = 1.0f;
            toRun->AddBoolCondition(fast, true);
            layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toRun));
            graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));
        }
    };
}

TEST_CASE("graph: an interruptible fade gives way to its destination's transition, fading on from the pose of that moment")
{
    ThreeStates bed{true};
    AnimationGraphPlayer player{bed.graph, bed.skel};
    player.SetBool(bed.go, true);
    player.Update(0.25f);
    CHECK(player.IsTransitioning());
    CHECK(player.GetLocalPoses()[0].position.x == doctest::Approx(2.5f)); // a quarter of Idle to Walk

    player.SetBool(bed.fast, true);
    player.Update(0.25f); // Walk to Run starts mid-fade, from x = 2.5
    CHECK(player.GetCurrentStateIndex() == 2);
    CHECK(player.GetLocalPoses()[0].position.x == doctest::Approx(2.5f + 0.25f * 17.5f));
    player.Update(0.75f);
    CHECK_FALSE(player.IsTransitioning());
    CHECK(player.GetLocalPoses()[0].position.x == doctest::Approx(20.0f));
}

TEST_CASE("graph: a fade that is not interruptible finishes before its destination may leave")
{
    ThreeStates bed{false};
    AnimationGraphPlayer player{bed.graph, bed.skel};
    player.SetBool(bed.go, true);
    player.Update(0.25f);
    player.SetBool(bed.fast, true);
    player.Update(0.25f);
    CHECK(player.GetCurrentStateIndex() == 1);
    CHECK(player.GetLocalPoses()[0].position.x == doctest::Approx(5.0f));
    player.Update(0.5f); // the fade ends
    player.Update(0.0f); // then Walk may leave
    CHECK(player.GetCurrentStateIndex() == 2);
}

TEST_CASE("graph: a trigger set during a crossfade fires once the fade is done, and is spent")
{
    // Every update consumed every trigger, and no transition is looked at mid-fade: a trigger set
    // while a layer crossfaded was gone before anything could test it.
    Skeleton skel{1};
    skel.Bones()[0].index = 0;
    skel.Bones()[0].parentIndex = -1;
    skel.FindRootBones();
    skel.BuildChildIndices();
    skel.ComputeInverseBindPoses();
    AnimationClip clip{u8"pose", 1.0f, true};
    clip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
    clip.GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 0, 0});

    AnimationGraph graph;
    const i32 pGo = graph.AddParameter(u8"Go", AnimationParameterType::Bool);
    const i32 pJump = graph.AddParameter(u8"Jump", AnimationParameterType::Trigger);
    auto layer = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
    const char8_t* names[] = {u8"Idle", u8"Walk", u8"Jump"};
    for (const char8_t* name : names)
    {
        layer->AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{name},
                                                        MakeUnique<ClipStateNode>(DefaultAllocator(), &clip)));
    }
    auto toWalk = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
    toWalk->sourceStateIndex = 0;
    toWalk->destStateIndex = 1;
    toWalk->duration = 0.5f; // a long fade
    toWalk->AddBoolCondition(pGo, true);
    layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toWalk));
    auto toJump = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
    toJump->sourceStateIndex = 1;
    toJump->destStateIndex = 2;
    toJump->duration = 0.001f;
    toJump->AddBoolCondition(pJump, true);
    layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toJump));
    graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));

    AnimationGraphPlayer player{graph, skel};
    player.SetBool(pGo, true);
    player.Update(0.016f);
    REQUIRE(player.GetCurrentStateIndex() == 1); // fading into Walk
    player.SetTrigger(pJump);                    // mid-fade
    player.Update(0.1f);
    CHECK(player.GetCurrentStateIndex() == 1);   // still fading: nothing transitions yet
    for (int i = 0; i < 10; ++i)
    {
        player.Update(0.1f);
    }
    CHECK(player.GetCurrentStateIndex() == 2);   // the trigger fired once the fade was done
    CHECK_FALSE(player.GetBool(pJump)); // and is spent
}

// ---- blend-tree event firing (the dominant clip's events) ----
TEST_CASE("graph: BlendTree1D fires the dominant clip's events once (incl. loop wrap)")
{
    AnimationClip walk{u8"walk", 1.0f};
    walk.AddEvent(0.5f, u8"step.walk");
    AnimationClip run{u8"run", 1.0f};
    run.AddEvent(0.5f, u8"step.run");
    BlendTree1D tree;
    tree.AddEntry(0.0f, &walk);
    tree.AddEntry(1.0f, &run);

    Array<String> fired;
    const AnimationEventHandler handler{[&fired](StringView name, f32)
                                        { fired.PushBack(String(name)); }};

    // Near the walk end of the axis: walk's event fires, exactly once.
    tree.parameter = 0.2f;
    tree.FireEvents(0.4f, 0.6f, false, handler);
    REQUIRE(fired.Size() == 1u);
    CHECK(fired[0] == u8"step.walk");

    // The blend parameter decides which clip's events fire.
    fired.Clear();
    tree.parameter = 0.9f;
    tree.FireEvents(0.4f, 0.6f, false, handler);
    REQUIRE(fired.Size() == 1u);
    CHECK(fired[0] == u8"step.run");

    // Loop wrap fires the event that sits in the wrapped span, once.
    fired.Clear();
    tree.FireEvents(0.9f, 0.1f, true, handler);
    CHECK(fired.Size() == 0u); // event at 0.5 is in neither (0.9,1.0] nor [0,0.1]
    tree.FireEvents(0.4f, 0.1f, true, handler);
    REQUIRE(fired.Size() == 1u); // crossed 0.5 before the wrap
    CHECK(fired[0] == u8"step.run");

    // No crossing, no event.
    fired.Clear();
    tree.FireEvents(0.6f, 0.8f, false, handler);
    CHECK(fired.IsEmpty());
}

TEST_CASE("graph: BlendTree2D fires the nearest entry's events, never both")
{
    AnimationClip idle{u8"idle", 1.0f};
    idle.AddEvent(0.25f, u8"breath");
    AnimationClip strafe{u8"strafe", 1.0f};
    strafe.AddEvent(0.25f, u8"scuff");
    BlendTree2D tree;
    tree.AddEntry(0.0f, 0.0f, &idle);
    tree.AddEntry(1.0f, 0.0f, &strafe);

    Array<String> fired;
    const AnimationEventHandler handler{[&fired](StringView name, f32)
                                        { fired.PushBack(String(name)); }};

    tree.parameterX = 0.1f;
    tree.parameterY = 0.0f;
    tree.FireEvents(0.2f, 0.3f, false, handler);
    REQUIRE(fired.Size() == 1u); // dominant only: a shared-timing event cannot fire twice
    CHECK(fired[0] == u8"breath");

    fired.Clear();
    tree.parameterX = 0.9f;
    tree.FireEvents(0.2f, 0.3f, false, handler);
    REQUIRE(fired.Size() == 1u);
    CHECK(fired[0] == u8"scuff");
}
