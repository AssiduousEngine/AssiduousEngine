// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The sequence panel's source: rows by binding then track then channel, whatever the track kinds
// (each says what it holds), key channels and section rows side by side; key moves, section trims
// and curve edits as gestures that preview from where they began and become one undo step; an edit
// that changes nothing leaves no step.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import foundation.gui;
import foundation.gui.toolkit;
import sequence.pipeline;
import editor.core;
import editor.sequence;

using namespace foundation::core;
using namespace editor;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;
namespace gui = foundation::gui;

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

    CurveKey Linear(f32 time, f32 value)
    {
        return CurveKey{time, value, 0.0f, 0.0f, CurveKeyInterpolation::Linear, CurveTangentMode::Broken};
    }

    // "Hero" walks (Transform.position, x 0 to 10 over a second) and plays a nested sequence from
    // 0.5 for a second; an event track bound to no one cues at 0.25.
    struct Bed
    {
        String dir = String(u8"scratch_sequence_source_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<SequenceDocument> document;
        UniquePtr<SequenceSource> source;

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
            document = MakeUnique<SequenceDocument>(DefaultAllocator(), context);
            REQUIRE(document->Load(instance->Id()));
            source = MakeUnique<SequenceSource>(DefaultAllocator(), *document);
            document->OnChanged = [this]() { source->Refresh(); };
            document->Edit(u8"setup", [](sequence::Sequence& s)
                           {
                               s.bindings.PushBack(sequence::SequenceBinding{String(u8"Hero")});
                               RefPtr<sequence::PropertySequenceTrack> walk = MakeRef<sequence::PropertySequenceTrack>(DefaultAllocator());
                               walk->binding = 0;
                               walk->property.target = String(propertyanimation::kTransformTarget);
                               walk->property.propertyPath = String(u8"position");
                               walk->property.kind = propertyanimation::TrackValueKind::Float3;
                               walk->property.channels[0].AddKey(Linear(0.0f, 0.0f));
                               walk->property.channels[0].AddKey(Linear(1.0f, 10.0f));
                               s.tracks.PushBack(walk);
                               RefPtr<sequence::NestedSequenceTrack> inner = MakeRef<sequence::NestedSequenceTrack>(DefaultAllocator());
                               inner->binding = 0;
                               inner->start = 0.5f;
                               inner->length = 1.0f;
                               s.tracks.PushBack(inner);
                               RefPtr<sequence::EventSequenceTrack> cues = MakeRef<sequence::EventSequenceTrack>(DefaultAllocator());
                               cues->keys.PushBack(sequence::SequenceEventKey{0.25f, String(u8"cue"), String(u8"gate")});
                               s.tracks.PushBack(cues);
                           });
        }
        ~Bed()
        {
            source.Reset();
            document.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        [[nodiscard]] sequence::PropertySequenceTrack& Walk() const { return *Cast<sequence::PropertySequenceTrack>(document->Sequence().tracks[0].Get()); }
        [[nodiscard]] sequence::NestedSequenceTrack& Inner() const { return *Cast<sequence::NestedSequenceTrack>(document->Sequence().tracks[1].Get()); }
    };
}

TEST_CASE("sequence source: rows by binding, track and channel; a track without key channels is a row of sections")
{
    Bed bed;
    REQUIRE(bed.source->ChannelCount() == 5u); // x, y, z, the nested section row, the events
    CHECK(bed.source->GetChannelPath(0).AsView() == StringView(u8"Hero.Transform.position.x"));
    CHECK(bed.source->GetChannelPath(3).AsView() == StringView(u8"Hero.Sequence"));
    CHECK(bed.source->GetChannelPath(4).AsView() == StringView(u8"Sequence.Event"));
    CHECK_FALSE(bed.source->IsSectionChannel(0));
    CHECK(bed.source->IsSectionChannel(3));
    Array<gui::TimelineSection> sections;
    bed.source->GetSections(3, sections);
    REQUIRE(sections.Size() == 1u);
    CHECK(sections[0].Start == doctest::Approx(0.5f));
    CHECK(sections[0].Length == doctest::Approx(1.0f));
    Array<f32> times;
    bed.source->GetKeyTimes(4, times);
    REQUIRE(times.Size() == 1u);
    CHECK(times[0] == doctest::Approx(0.25f));
}

TEST_CASE("sequence source: a key move previews from where it began and is one undo step")
{
    Bed bed;
    const usize stepsBefore = bed.document->Commands().CanUndo() ? 1u : 0u;
    (void)stepsBefore;
    const gui::TimelineKeyRef second[] = {gui::TimelineKeyRef{0, 1}};
    Array<gui::TimelineKeyRef> moved;
    bed.source->BeginKeysMove();
    bed.source->PreviewKeysMove(Span<const gui::TimelineKeyRef>(second, 1), 0.5f, moved);
    bed.source->PreviewKeysMove(Span<const gui::TimelineKeyRef>(second, 1), 0.25f, moved); // from the start, not from 1.5
    REQUIRE(moved.Size() == 1u);
    CHECK(moved[0].Index == 1u);
    bed.source->CommitKeysMove();
    Array<f32> times;
    bed.source->GetKeyTimes(0, times);
    CHECK(times[1] == doctest::Approx(1.25f));
    bed.document->Commands().Undo();
    bed.source->GetKeyTimes(0, times);
    CHECK(times[1] == doctest::Approx(1.0f));
}

TEST_CASE("sequence source: a section trims through its kind; a deletion that changes nothing leaves no step")
{
    Bed bed;
    bed.source->BeginSectionEdit();
    bed.source->PreviewSectionEdit(gui::TimelineSectionRef{3, 0}, 0.75f, 0.75f);
    bed.source->CommitSectionEdit();
    CHECK(bed.Inner().start == doctest::Approx(0.75f));
    CHECK(bed.Inner().length == doctest::Approx(0.75f));

    bed.document->Commands().Undo();
    CHECK(bed.Inner().start == doctest::Approx(0.5f));
    bed.document->Commands().Redo();
    CHECK_FALSE(bed.document->Commands().CanRedo());
    // A nested track has one section and keeps it: no change, so nothing to undo but the trim.
    (void)bed.source->DeleteSection(gui::TimelineSectionRef{3, 0});
    bed.document->Commands().Undo();
    CHECK(bed.Inner().start == doctest::Approx(0.5f)); // the undo took the trim, not an empty step
}

TEST_CASE("sequence source: inserting and deleting keys are one step each; the curve view edits a shown track's curves")
{
    Bed bed;
    gui::TimelineKeyRef made;
    REQUIRE(bed.source->InsertKey(4, 0.75f, made));
    CHECK(made.Channel == 4u);
    CHECK(made.Index == 1u);
    auto* cues = Cast<sequence::EventSequenceTrack>(bed.document->Sequence().tracks[2].Get());
    REQUIRE(cues->keys.Size() == 2u);
    CHECK(cues->keys[1].name.AsView() == StringView(u8"event"));
    const gui::TimelineKeyRef first[] = {gui::TimelineKeyRef{4, 0}};
    CHECK(bed.source->DeleteKeys(Span<const gui::TimelineKeyRef>(first, 1)));
    cues = Cast<sequence::EventSequenceTrack>(bed.document->Sequence().tracks[2].Get());
    REQUIRE(cues->keys.Size() == 1u);
    CHECK(cues->keys[0].name.AsView() == StringView(u8"event"));

    const usize walk[] = {0};
    bed.source->ShowTracks(Span<const usize>(walk, 1));
    REQUIRE(bed.source->CurveCount() == 3u);
    CHECK(bed.source->GetCurveInfo(0).Name.AsView() == StringView(u8"Transform.position.x"));
    CHECK(bed.source->GetKeys(0).Size() == 2u);
    const CurveKey raised[] = {Linear(0.0f, 0.0f), Linear(1.0f, 20.0f)};
    bed.source->BeginKeysEdit();
    bed.source->PreviewKeys(0, Span<const CurveKey>(raised, 2));
    bed.source->CommitKeysEdit();
    CHECK(bed.Walk().property.channels[0].Evaluate(1.0f) == doctest::Approx(20.0f));
    bed.document->Commands().Undo();
    CHECK(bed.Walk().property.channels[0].Evaluate(1.0f) == doctest::Approx(10.0f));
}

TEST_CASE("sequence document: a change made in place (the inspector's) is recorded as one step from where it stood")
{
    Bed bed;
    bed.Inner().loop = true; // as the inspector writes a field
    bed.document->RecordEdit(u8"sequence-inspector");
    CHECK(bed.Inner().loop);
    bed.document->Commands().Undo();
    CHECK_FALSE(bed.Inner().loop);
    bed.document->Commands().Redo();
    CHECK(bed.Inner().loop);
    bed.document->RecordEdit(u8"sequence-inspector"); // nothing changed since: no step
    bed.document->Commands().Undo();
    CHECK_FALSE(bed.Inner().loop);

    // The kinds show their fields to the inspector.
    const TypeInfo& nested = sequence::NestedSequenceTrack::StaticType();
    CHECK(FindProperty(nested, "sequence") != nullptr);
    CHECK(FindProperty(nested, "name") != nullptr); // the base's, inherited
    CHECK(FindProperty(sequence::EventSequenceTrack::StaticType(), "keys") != nullptr);
}

TEST_CASE("sequence source: a section row takes a new section through its kind; a nested track takes none")
{
    Bed bed;
    gui::TimelineSectionRef made;
    CHECK_FALSE(bed.source->InsertSection(3, 0.25f, made)); // a nested track has its one section
    CHECK_FALSE(bed.source->InsertSection(0, 0.25f, made)); // a row of keys
    CHECK_FALSE(bed.document->Commands().CanRedo());
}
