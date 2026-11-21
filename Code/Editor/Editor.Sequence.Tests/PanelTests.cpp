// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The sequence panel over its markup (Data/Editor/SequencePanel.xml), a scratch project and a
// scene, without a host: the state with no sequence; a part cast from the selection; Track offering
// the registered kinds and the part's properties, a property track starting with what the entity
// shows; the preview posing the scene at the playhead and putting it back; markers and holds on the
// ruler; the inspector editing the chosen track as one step of the sequence's history; and the
// preview standing down while the scene simulates.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.sequence.resource;
import sequence.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.sequence;

using namespace foundation::core;
using namespace editor;
namespace sequence = foundation::sequence;
namespace scene = foundation::scene;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(TEST_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // A scratch project with one sequence, "Arrival", and a scene with one entity, "Hero", at
    // (1, 2, 3); the panel in a root.
    struct Bed
    {
        String dir = String(u8"scratch_sequence_panel_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        scene::Scene sc{DefaultAllocator(), u8"Level"};
        scene::EntityHandle hero;
        Guid sequence;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<SequencePanel> panel;

        Bed()
        {
            RegisterCoreTypes();
            pipeline::RegisterSequenceAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance = pipeline::CreateSequenceAsset(project->SourceDb().RootGroup(), u8"Arrival");
            REQUIRE(instance != nullptr);
            sequence = instance->Id();
            hero = sc.CreateEntity(u8"Hero");
            sc.SetLocalPosition(hero, Float3{1.0f, 2.0f, 3.0f});

            root->SetSize(Float2{1400.0f, 500.0f});
            RefPtr<gui::Node> content = LoadMarkup(kSequencePanelPath);
            REQUIRE(content.Get() != nullptr);
            root->AddChild(content.Get());
            panel = MakeUnique<SequencePanel>(DefaultAllocator(), context, sc, content, nullptr);
            const Guid heroId = sc.GetEntityId(hero);
            panel->SelectedEntity = [heroId]() { return heroId; };
        }
        ~Bed()
        {
            panel.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        [[nodiscard]] Float3 Position() const { return sc.GetLocalTransform(hero).position; }
        [[nodiscard]] const sequence::Sequence& Sequence() { return panel->Document().Sequence(); }
        [[nodiscard]] SequenceTrackOffer Offer(StringView label)
        {
            for (const SequenceTrackOffer& offer : panel->OfferedTracks())
                if (offer.label.AsView() == label)
                    return offer;
            for (const SequenceTrackOffer& offer : panel->OfferedTracks())
                MESSAGE(String(offer.label).AsView());
            FAIL("no such offer");
            return {};
        }
    };
}

TEST_CASE("sequence panel: with no sequence a state says so; a sequence opened shows its name")
{
    Bed bed;
    auto* empty = app::FindById<gui::Node>(*bed.panel->Content(), u8"seq-empty");
    auto* body = app::FindById<gui::Node>(*bed.panel->Content(), u8"seq-body");
    auto* name = app::FindById<gui::Label>(*bed.panel->Content(), u8"seq-name");
    REQUIRE(empty != nullptr);
    REQUIRE(body != nullptr);
    CHECK(empty->IsDisplayed());
    CHECK_FALSE(body->IsDisplayed());
    CHECK(name->GetText() == StringView(u8"No sequence"));
    CHECK(bed.panel->OfferedTracks().IsEmpty());

    REQUIRE(bed.panel->LoadSequence(bed.sequence));
    CHECK_FALSE(empty->IsDisplayed());
    CHECK(body->IsDisplayed());
    CHECK(name->GetText() == StringView(u8"Arrival"));
}

TEST_CASE("sequence panel: a part cast from the selection; Track offers the kinds and the part's properties")
{
    Bed bed;
    REQUIRE(bed.panel->LoadSequence(bed.sequence));
    // Every registered kind is offered (here the foundation's: no domain is registered).
    CHECK(bed.Offer(u8"Sequence").enabled);
    CHECK(bed.Offer(u8"Event").enabled);

    CHECK(bed.panel->AddPartForSelected() == 0);
    REQUIRE(bed.Sequence().bindings.Size() == 1u);
    CHECK(bed.Sequence().bindings[0].name.AsView() == StringView(u8"Hero"));
    CHECK(bed.panel->CastEntity(u8"Hero") == bed.sc.GetEntityId(bed.hero));

    // A property track starts with what the entity shows, keyed at the playhead.
    bed.panel->AddTrack(bed.Offer(u8"Hero: Transform.position"));
    REQUIRE(bed.Sequence().tracks.Size() == 1u);
    const auto* walk = Cast<sequence::PropertySequenceTrack>(bed.Sequence().tracks[0].Get());
    REQUIRE(walk != nullptr);
    CHECK(walk->binding == 0);
    CHECK(walk->property.Sample(0.0f).Get<Float3>().y == doctest::Approx(2.0f));
    // It is offered no more.
    for (const SequenceTrackOffer& offer : bed.panel->OfferedTracks())
        CHECK(offer.label.AsView() != StringView(u8"Hero: Transform.position"));
    bed.panel->Undo();
    CHECK(bed.Sequence().tracks.IsEmpty());
}

TEST_CASE("sequence panel: the preview poses the scene at the playhead and puts it back; it stands down while simulating")
{
    Bed bed;
    REQUIRE(bed.panel->LoadSequence(bed.sequence));
    REQUIRE(bed.panel->AddPartForSelected() == 0);
    bed.panel->AddTrack(bed.Offer(u8"Hero: Transform.position"));
    // Key x at 1 s to 11 through the source, as a double click and a drag would.
    gui::TimelineKeyRef made;
    REQUIRE(bed.panel->Source().InsertKey(0, 1.0f, made));
    const usize channel = made.Channel;
    bed.panel->Document().Edit(u8"test", [channel](sequence::Sequence& s)
                               { s.tracks[0]->KeyCurve(channel)->Keys()[1].value = 11.0f; });
    bed.panel->Scrub(1.0f);
    CHECK(bed.Position().x == doctest::Approx(11.0f));
    bed.panel->Scrub(0.0f);
    CHECK(bed.Position().x == doctest::Approx(1.0f));
    bed.panel->Scrub(1.0f);
    bed.panel->Update(0.016f, true); // the scene simulates: what the preview posed goes back
    CHECK(bed.Position().x == doctest::Approx(1.0f));
    CHECK_FALSE(bed.panel->Preview().IsPosing());
    bed.panel->Update(0.016f, false);
    bed.panel->StopPreview();
    CHECK(bed.Position().x == doctest::Approx(1.0f));
}

TEST_CASE("sequence panel: markers and holds at the playhead show on the ruler; the inspector edits the chosen track as one step")
{
    Bed bed;
    REQUIRE(bed.panel->LoadSequence(bed.sequence));
    bed.panel->Scrub(0.5f);
    bed.panel->AddMarkerAtPlayhead(true);
    bed.panel->Scrub(0.25f);
    bed.panel->AddMarkerAtPlayhead(false);
    REQUIRE(bed.panel->Source().MarkerCount() == 2u);
    CHECK(bed.panel->Source().GetMarker(0).Time == doctest::Approx(0.25f)); // kept by time
    CHECK(bed.panel->Source().GetMarker(1).Label.AsView() == StringView(u8"hold (hold)"));
    CHECK(bed.Sequence().markers[1].hold);

    bed.panel->AddTrack(bed.Offer(u8"Event"));
    REQUIRE(bed.Sequence().tracks.Size() == 1u);
    // Choose its row (an event key there), and the inspector shows it.
    gui::TimelineKeyRef made;
    REQUIRE(bed.panel->Source().InsertKey(0, 0.5f, made));
    REQUIRE(bed.panel->Sheet() != nullptr);
    bed.panel->Sheet()->SetSelection(Span<const gui::TimelineKeyRef>(&made, 1));
    bed.panel->Update(0.016f, false);
    gui::ReflectedPropertySource* inspected = bed.panel->Inspected();
    REQUIRE(inspected != nullptr);
    inspected->BeginEdit(u8"muted");
    inspected->CommitEdit(u8"muted", gui::PropertyWrite{gui::Variant(true)});
    CHECK(bed.Sequence().tracks[0]->muted);
    bed.panel->Undo();
    CHECK_FALSE(bed.Sequence().tracks[0]->muted);
    bed.panel->Redo();
    CHECK(bed.Sequence().tracks[0]->muted);
}
