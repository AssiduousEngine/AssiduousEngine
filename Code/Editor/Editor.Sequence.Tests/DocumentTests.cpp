// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The sequence panel's UI-free half: the document over a scratch project (its edits, gestures,
// history, unsaved mark and save, each change rebuilding the tracks it holds), and the preview
// posing a scene through the game's own play and putting it back.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import sequence.pipeline;
import editor.core;
import editor.sequence;

using namespace foundation::core;
using namespace editor;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;
namespace scene = foundation::scene;

namespace
{
    void Register()
    {
        static const bool once = []()
        {
            RegisterCoreTypes();
            pipeline::RegisterSequenceAssets();
            return true;
        }();
        (void)once;
    }

    // "Actor" walks along X from 0 to `toX` over a second.
    RefPtr<sequence::PropertySequenceTrack> Walk(f32 toX)
    {
        RefPtr<sequence::PropertySequenceTrack> track = MakeRef<sequence::PropertySequenceTrack>(DefaultAllocator());
        track->binding = 0;
        track->property.componentType = String(propertyanimation::kTransformTarget);
        track->property.propertyPath = String(u8"position");
        track->property.kind = propertyanimation::TrackValueKind::Float3;
        CurveKey a;
        a.time = 0.0f;
        a.value = 0.0f;
        a.interpolation = CurveKeyInterpolation::Linear;
        CurveKey b;
        b.time = 1.0f;
        b.value = toX;
        b.interpolation = CurveKeyInterpolation::Linear;
        track->property.channels[0].AddKey(a);
        track->property.channels[0].AddKey(b);
        return track;
    }

    // A scratch project holding one sequence, "Arrival", read into a document.
    struct Bed
    {
        String dir = String(u8"scratch_sequence_document_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<SequenceDocument> document;
        Guid id;
        usize changes = 0;

        Bed()
        {
            Register();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance = pipeline::CreateSequenceAsset(project->SourceDb().RootGroup(), u8"Arrival");
            REQUIRE(instance != nullptr);
            id = instance->Id();
            document = MakeUnique<SequenceDocument>(DefaultAllocator(), context);
            document->OnChanged = [this]() { ++changes; };
            REQUIRE(document->Load(id));
        }
        ~Bed()
        {
            document.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        void AddWalk(f32 toX)
        {
            document->Edit(u8"test", [toX](sequence::Sequence& edited)
                           {
                               if (edited.bindings.IsEmpty())
                                   edited.bindings.PushBack(sequence::SequenceBinding{String(u8"Actor")});
                               edited.tracks.PushBack(Walk(toX));
                           });
        }
    };
}

TEST_CASE("sequence document: an edit is one step, undone and redone, and the unsaved mark follows the history")
{
    Bed bed;
    CHECK(bed.document->Name() == StringView(u8"Arrival"));
    CHECK_FALSE(bed.document->IsDirty());
    CHECK(bed.document->Length() == doctest::Approx(1.0f)); // at least a second

    const u64 before = bed.document->Version();
    bed.AddWalk(10.0f);
    CHECK(bed.document->Version() != before); // the tracks were rebuilt
    REQUIRE(bed.document->Sequence().tracks.Size() == 1u);
    CHECK(bed.document->Sequence().duration == doctest::Approx(1.0f)); // grew to cover the walk
    CHECK(bed.document->IsDirty());

    bed.document->Commands().Undo();
    CHECK(bed.document->Sequence().tracks.IsEmpty());
    CHECK_FALSE(bed.document->IsDirty()); // back where it was saved
    bed.document->Commands().Redo();
    REQUIRE(bed.document->Sequence().tracks.Size() == 1u);
    auto* walk = Cast<sequence::PropertySequenceTrack>(bed.document->Sequence().tracks[0].Get());
    REQUIRE(walk != nullptr);
    CHECK(walk->property.Sample(1.0f).Get<Float3>().x == doctest::Approx(10.0f));
}

TEST_CASE("sequence document: an undo step keeps its own tracks, not the live ones")
{
    Bed bed;
    bed.AddWalk(10.0f);
    // A gesture changes the live track in place; the step before it still holds 10.
    bed.document->BeginGesture();
    auto* live = Cast<sequence::PropertySequenceTrack>(bed.document->GestureSequence().tracks[0].Get());
    REQUIRE(live != nullptr);
    live->property.channels[0].Keys()[1].value = 20.0f;
    bed.document->PreviewGesture();
    bed.document->CommitGesture(u8"test-drag");
    auto* dragged = Cast<sequence::PropertySequenceTrack>(bed.document->Sequence().tracks[0].Get());
    CHECK(dragged->property.Sample(1.0f).Get<Float3>().x == doctest::Approx(20.0f));
    bed.document->Commands().Undo();
    auto* undone = Cast<sequence::PropertySequenceTrack>(bed.document->Sequence().tracks[0].Get());
    CHECK(undone->property.Sample(1.0f).Get<Float3>().x == doctest::Approx(10.0f));

    // A cancelled gesture leaves no step and puts the sequence back.
    bed.document->BeginGesture();
    Cast<sequence::PropertySequenceTrack>(bed.document->GestureSequence().tracks[0].Get())->property.channels[0].Keys()[1].value = 30.0f;
    bed.document->CancelGesture();
    CHECK(Cast<sequence::PropertySequenceTrack>(bed.document->Sequence().tracks[0].Get())->property.Sample(1.0f).Get<Float3>().x ==
          doctest::Approx(10.0f));
    CHECK(bed.document->Commands().CanRedo()); // the drag's step is still there to redo
}

TEST_CASE("sequence document: a save writes the asset and clears the mark; a load reads it back")
{
    Bed bed;
    bed.AddWalk(4.0f);
    REQUIRE(bed.document->Save().IsOk());
    CHECK_FALSE(bed.document->IsDirty());

    SequenceDocument again(bed.context);
    REQUIRE(again.Load(bed.id));
    REQUIRE(again.Sequence().bindings.Size() == 1u);
    CHECK(again.Sequence().bindings[0].name.AsView() == StringView(u8"Actor"));
    REQUIRE(again.Sequence().tracks.Size() == 1u);
    CHECK(Cast<sequence::PropertySequenceTrack>(again.Sequence().tracks[0].Get()) != nullptr);
    CHECK_FALSE(again.Load(Guid{0x1, 0x2})); // no such sequence
}

TEST_CASE("sequence preview: it poses the cast at a time, begins again on new tracks, and puts the scene back")
{
    Bed bed;
    bed.AddWalk(10.0f);
    scene::Scene world{DefaultAllocator(), u8"stage"};
    const scene::EntityHandle actor = world.CreateEntity(u8"actor");
    world.SetLocalPosition(actor, Float3{-3.0f, 0.0f, 0.0f});

    SequencePreview preview(world);
    preview.Bind(u8"Actor", world.GetEntityId(actor));
    CHECK(preview.BoundEntity(u8"Actor") == world.GetEntityId(actor));
    preview.Pose(bed.document->Sequence(), bed.document->Version(), 0.5f);
    CHECK(preview.IsPosing());
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(5.0f));

    bed.document->Commands().Undo(); // the walk goes: the old play's tracks are gone
    bed.document->Commands().Redo();
    preview.Pose(bed.document->Sequence(), bed.document->Version(), 0.25f);
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(2.5f));

    preview.Stop();
    CHECK_FALSE(preview.IsPosing());
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(-3.0f)); // as it was

    // Another entity in the part: the first goes back before the second is posed.
    const scene::EntityHandle stand = world.CreateEntity(u8"stand-in");
    preview.Pose(bed.document->Sequence(), bed.document->Version(), 1.0f);
    preview.Bind(u8"Actor", world.GetEntityId(stand));
    CHECK(world.GetLocalTransform(actor).position.x == doctest::Approx(-3.0f));
    preview.Pose(bed.document->Sequence(), bed.document->Version(), 1.0f);
    CHECK(world.GetLocalTransform(stand).position.x == doctest::Approx(10.0f));
}
