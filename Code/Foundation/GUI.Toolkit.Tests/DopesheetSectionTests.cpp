// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - the dopesheet's section rows. A channel that holds sections shows them as bars on
// its row, beside rows of keys under the same parent. A click selects a bar (and lets go of the
// keys), a drag on its body moves it and a drag on an end trims it, snapping to frames and staying
// in the clip, previewed through the source and committed once; Escape cancels, Delete removes.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::gui;
namespace core = foundation::core;
using core::f32;
using core::usize;

namespace
{
    // Channels "Actor.walk" (keys at 0.4 and 1.2) and "Actor.clips" (sections 0.2 for 0.6 and 1.0 for
    // 0.4); rows Actor, walk, clips.
    class Staged final : public ITimelineSource
    {
    public:
        core::Array<f32> walk{core::DefaultAllocator()};
        core::Array<TimelineSection> clips{core::DefaultAllocator()};
        core::Array<TimelineSection> before{core::DefaultAllocator()};
        usize commits = 0;
        usize cancels = 0;

        Staged()
        {
            walk.PushBack(0.4f);
            walk.PushBack(1.2f);
            clips.PushBack(TimelineSection{0.2f, 0.6f, core::String(u8"wave")});
            clips.PushBack(TimelineSection{1.0f, 0.4f, core::String(u8"bow")});
        }

        [[nodiscard]] usize ChannelCount() const override { return 2; }
        [[nodiscard]] core::String GetChannelPath(usize channel) const override
        {
            return core::String(channel == 0 ? core::StringView(u8"Actor.walk") : core::StringView(u8"Actor.clips"));
        }
        void GetKeyTimes(usize channel, core::Array<f32>& out) const override
        {
            out.Clear();
            if (channel == 0)
                for (const f32 t : walk)
                    out.PushBack(t);
        }
        void BeginKeysMove() override {}
        void PreviewKeysMove(core::Span<const TimelineKeyRef>, f32, core::Array<TimelineKeyRef>&) override {}
        void CommitKeysMove() override {}
        void CancelKeysMove() override {}
        bool DeleteKeys(core::Span<const TimelineKeyRef>) override { return false; }
        bool InsertKey(usize, f32, TimelineKeyRef&) override { return false; }

        [[nodiscard]] bool IsSectionChannel(usize channel) const override { return channel == 1; }
        void GetSections(usize channel, core::Array<TimelineSection>& out) const override
        {
            out.Clear();
            if (channel == 1)
                for (const TimelineSection& s : clips)
                    out.PushBack(s);
        }
        void BeginSectionEdit() override { before = clips; }
        void PreviewSectionEdit(const TimelineSectionRef& section, f32 start, f32 length) override
        {
            clips = before;
            clips[section.Index].Start = start;
            clips[section.Index].Length = length;
            NotifyTimelineChanged();
        }
        void CommitSectionEdit() override { ++commits; }
        void CancelSectionEdit() override
        {
            clips = before;
            ++cancels;
            NotifyTimelineChanged();
        }
        bool DeleteSection(const TimelineSectionRef& section) override
        {
            clips.RemoveAt(section.Index);
            NotifyTimelineChanged();
            return true;
        }
        bool InsertSection(usize channel, f32 time, TimelineSectionRef& made) override
        {
            clips.PushBack(TimelineSection{time, 0.25f, core::String(u8"new")});
            made = TimelineSectionRef{channel, clips.Size() - 1};
            NotifyTimelineChanged();
            return true;
        }
    };

    // A 660 by 200 dopesheet with names 160 wide, over 0 to 2 s at 250 px per second: time t on row r
    // is at (160 + 250 t, 10 + 20 r).
    struct Bed
    {
        core::RefPtr<SceneRoot> root = core::MakeRef<SceneRoot>(core::DefaultAllocator(), core::MakeRef<GUIContext>(core::DefaultAllocator()));
        core::RefPtr<TimeAxis> axis = core::MakeRef<TimeAxis>(core::DefaultAllocator());
        core::RefPtr<Dopesheet> sheet = core::MakeRef<Dopesheet>(core::DefaultAllocator());
        Staged source;

        Bed()
        {
            root->SetSize(core::Float2{700.0f, 300.0f});
            root->EditLayoutStyle([](LayoutStyle& s) { s.AlignItems = Align::Start; });
            sheet->EditLayoutStyle([](LayoutStyle& s)
            {
                s.Width = LayoutLength::Px(660.0f);
                s.Height = LayoutLength::Px(200.0f);
            });
            root->AddChild(sheet.Get());
            axis->SetDuration(2.0f);
            axis->SetPixelsPerSecond(250.0f);
            sheet->SetTimeAxis(axis);
            sheet->SetSource(&source);
            (void)root->UpdateLayout();
        }
        ~Bed() { sheet->SetSource(nullptr); }
        EventDispatcher& D() { return *root->GetEventDispatcher(); }
        static core::Float2 At(usize row, f32 t) { return core::Float2{160.0f + 250.0f * t, 10.0f + 20.0f * static_cast<f32>(row)}; }
        void Click(core::Float2 at)
        {
            D().InjectMouseMove(at);
            D().InjectMouseDown(at, MouseButton::Left);
            D().InjectMouseUp(at, MouseButton::Left);
        }
        void Drag(core::Float2 from, core::Float2 to)
        {
            D().InjectMouseMove(from);
            D().InjectMouseDown(from, MouseButton::Left);
            D().InjectMouseMove(core::Float2{(from.x + to.x) * 0.5f, from.y});
            D().InjectMouseMove(to);
            D().InjectMouseUp(to, MouseButton::Left);
        }
    };
}

TEST_CASE("Dopesheet sections: a section channel is a row of bars beside rows of keys; a parent summarizes keys only")
{
    Bed bed;
    const core::Array<DopesheetRow>& rows = bed.sheet->GetRows();
    REQUIRE(rows.Size() == 3);
    CHECK(rows[2].Name.AsView() == core::StringView(u8"clips"));
    core::Array<f32> times;
    core::Array<core::Array<TimelineKeyRef>> keys;
    bed.sheet->GetRowKeys(0, times, keys); // Actor: the walk's keys, not the clips
    CHECK(times.Size() == 2);
    bed.sheet->GetRowKeys(2, times, keys);
    CHECK(times.IsEmpty());
    const Rect bar = bed.sheet->SectionBar(2, bed.source.clips[0]);
    CHECK(bar.x == doctest::Approx(Bed::At(2, 0.2f).x));
    CHECK(bar.width == doctest::Approx(150.0f)); // 0.6 s
}

TEST_CASE("Dopesheet sections: a click selects a bar and lets go of keys; a body drag moves it on frames, in the clip")
{
    Bed bed;
    bed.sheet->SetSelection(core::Span<const TimelineKeyRef>{});
    bed.Click(Bed::At(2, 0.5f)); // the wave's middle
    REQUIRE(bed.sheet->GetSelectedSection());
    CHECK(bed.sheet->GetSelectedSection()->Index == 0u);
    CHECK(bed.source.commits == 0u); // a click moves nothing

    bed.axis->SetFrameRate(10.0f);
    bed.Drag(Bed::At(2, 0.5f), Bed::At(2, 0.83f));
    CHECK(bed.source.commits == 1u);
    CHECK(bed.source.clips[0].Start == doctest::Approx(0.5f)); // 0.2 + 0.33 snapped to a frame
    CHECK(bed.source.clips[0].Length == doctest::Approx(0.6f)); // a move keeps its length

    bed.Drag(Bed::At(2, 0.8f), core::Float2{690.0f, Bed::At(2, 0.0f).y}); // far right: it stops at the end
    CHECK(bed.source.clips[0].Start + bed.source.clips[0].Length == doctest::Approx(2.0f));
}

TEST_CASE("Dopesheet sections: a drag on an end trims it, the other end staying; Escape cancels; Delete removes")
{
    Bed bed;
    bed.axis->SetFrameRate(10.0f);
    bed.Drag(Bed::At(2, 0.2f) + core::Float2{1.0f, 0.0f}, Bed::At(2, 0.4f)); // the wave's start
    CHECK(bed.source.clips[0].Start == doctest::Approx(0.4f));
    CHECK(bed.source.clips[0].Length == doctest::Approx(0.4f)); // its end stays at 0.8
    bed.Drag(Bed::At(2, 0.8f) - core::Float2{1.0f, 0.0f}, Bed::At(2, 0.9f)); // its end
    CHECK(bed.source.clips[0].Start == doctest::Approx(0.4f));
    CHECK(bed.source.clips[0].Length == doctest::Approx(0.5f));
    CHECK(bed.source.commits == 2u);

    bed.D().InjectMouseMove(Bed::At(2, 1.2f));
    bed.D().InjectMouseDown(Bed::At(2, 1.2f), MouseButton::Left); // the bow
    bed.D().InjectMouseMove(Bed::At(2, 1.5f));
    CHECK(bed.source.clips[1].Start == doctest::Approx(1.3f)); // previewed
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Escape));
    CHECK(bed.source.clips[1].Start == doctest::Approx(1.0f));
    CHECK(bed.source.cancels == 1u);
    bed.D().InjectMouseUp(Bed::At(2, 1.5f), MouseButton::Left);
    CHECK(bed.source.commits == 2u);

    REQUIRE(bed.sheet->GetSelectedSection());
    bed.D().InjectKeyDown(static_cast<core::u32>(KeyCode::Delete));
    REQUIRE(bed.source.clips.Size() == 1u);
    CHECK(bed.source.clips[0].Label.AsView() == core::StringView(u8"wave"));
    CHECK_FALSE(bed.sheet->GetSelectedSection());
}

TEST_CASE("Dopesheet sections: a double click on a section row's empty space adds one there, chosen")
{
    Bed bed;
    bed.axis->SetFrameRate(10.0f);
    bed.Click(Bed::At(2, 1.63f));
    bed.Click(Bed::At(2, 1.63f));
    REQUIRE(bed.source.clips.Size() == 3u);
    CHECK(bed.source.clips[2].Start == doctest::Approx(1.6f)); // on a frame
    REQUIRE(bed.sheet->GetSelectedSection());
    CHECK(bed.sheet->GetSelectedSection()->Index == 2u);
}
