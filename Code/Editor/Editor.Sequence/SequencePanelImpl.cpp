// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:panel` partition (implementation; see SequencePanel.cppm).

module;
#include "Core/Prelude.h"

module editor.sequence;

import foundation.core;
import foundation.scene;
import foundation.propertyanimation;
import foundation.sequence;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app; // FindById, ResourceRefEditor, XmlPropertyTextFormat
import editor.propertyanimation;

using namespace foundation::core;
namespace sequence = foundation::sequence;
namespace propertyanimation = foundation::propertyanimation;
namespace gui = foundation::gui;
namespace scene = foundation::scene;

namespace editor
{
    namespace
    {
        // The kind's name, as its reflection gives it.
        [[nodiscard]] String KindLabel(const TypeInfo& kind)
        {
            if (const Variant* shown = FindAttribute(kind, "displayName"); shown != nullptr && shown->Is<String>())
                return shown->Get<String>();
            return String(StringView(reinterpret_cast<const utf8char*>(kind.name)));
        }

        // A new track of `kind`, made as a sequence being read makes it (RegisterSequenceTrack
        // registers each kind's factory), so no list of kinds is kept here.
        [[nodiscard]] RefPtr<sequence::SequenceTrack> MakeTrack(const TypeInfo& kind)
        {
            RefPtr<ISerializable> made = GlobalSerializableRegistry().Create(kind.id);
            return RefPtr<sequence::SequenceTrack>(Cast<sequence::SequenceTrack>(made.Get()));
        }

        [[nodiscard]] bool HasPropertyTrack(const sequence::Sequence& sequence, i32 binding, StringView componentType, StringView propertyPath)
        {
            for (const RefPtr<sequence::SequenceTrack>& track : sequence.tracks)
                if (const auto* property = Cast<sequence::PropertySequenceTrack>(track.Get());
                    property != nullptr && track->binding == binding && property->property.componentType.AsView() == componentType &&
                    property->property.propertyPath.AsView() == propertyPath)
                    return true;
            return false;
        }

        // Keys the value's parts into a new property track at `time`.
        void KeyFirst(propertyanimation::PropertyTrack& track, f32 time, const Variant& value)
        {
            const auto set = [&](u32 channel, f32 v)
            {
                track.channels[channel].AddKey(CurveKey{time, v, 0.0f, 0.0f, CurveKeyInterpolation::Cubic, CurveTangentMode::Auto});
                track.channels[channel].ApplyTangentModes();
            };
            if (track.kind == propertyanimation::TrackValueKind::Float && value.Is<f32>())
                set(0, value.Get<f32>());
            else if (track.kind == propertyanimation::TrackValueKind::Float3 && value.Is<Float3>())
            {
                const Float3 v = value.Get<Float3>();
                set(0, v.x);
                set(1, v.y);
                set(2, v.z);
            }
            else if (track.kind == propertyanimation::TrackValueKind::Color && value.Is<Color>())
            {
                const Color c = value.Get<Color>();
                set(0, c.r);
                set(1, c.g);
                set(2, c.b);
                set(3, c.a);
            }
            else if (track.kind == propertyanimation::TrackValueKind::Quat && value.Is<Quaternion>())
                track.quatKeys.PushBack(propertyanimation::QuatKey{time, value.Get<Quaternion>()});
        }
    }

    template <typename T>
    T* SequencePanel::Find(StringView id) const
    {
        return m_content ? app::FindById<T>(*m_content, id) : nullptr;
    }

    SequencePanel::SequencePanel(EditorContext& context, scene::Scene& scene, RefPtr<gui::Node> content, const app::ResourceRefServices* refs)
        : m_context(&context), m_scene(&scene), m_content(Move(content)), m_document(context), m_source(m_document), m_preview(scene),
          m_seeds(scene), m_format(EditorRootAllocator())
    {
        SequencePanel* self = this;
        m_document.OnChanged = [self]() { self->Changed(); };

        // One axis serves the ruler, the dopesheet and the curve view; a scrub poses the scene.
        m_axis = MakeRef<gui::TimeAxis>(EditorRootAllocator());
        m_axis->SetPixelsPerSecond(80.0f);
        m_axis->SetFrameRate(30.0f);
        m_axis->SetDuration(m_document.Length());
        m_axis->SetOnPlayheadChanged([self](f32 time)
                                     {
                                         if (self->m_moving)
                                             return;
                                         self->m_transport.SetTime(time);
                                         self->Pose();
                                     });
        m_ruler = Find<gui::TimeRuler>(u8"seq-ruler");
        if (m_ruler != nullptr)
        {
            m_ruler->SetTimeAxis(m_axis);
            m_ruler->SetMarkerSource(&m_source);
        }
        m_sheet = Find<gui::Dopesheet>(u8"seq-sheet");
        if (m_sheet != nullptr)
        {
            m_sheet->SetTimeAxis(m_axis);
            m_sheet->SetSource(&m_source);
        }
        m_curves = Find<gui::CurveView>(u8"seq-curves");
        if (m_curves != nullptr)
        {
            m_curves->SetTimeAxis(m_axis);
            m_curves->SetSource(&m_source);
        }
        m_grid = Find<gui::PropertyGrid>(u8"seq-inspector");
        if (m_grid != nullptr && refs != nullptr)
            app::ResourceRefEditor::Register(m_grid->GetEditors(), *refs);

        m_name = Find<gui::Label>(u8"seq-name");
        m_trackName = Find<gui::Label>(u8"seq-track-name");
        m_dirty = Find<gui::Node>(u8"seq-dirty");
        m_empty = Find<gui::Node>(u8"seq-empty");
        m_body = Find<gui::Node>(u8"seq-body");
        m_length = Find<gui::NumberField>(u8"seq-length");
        if (m_length != nullptr)
            m_length->SetOnValueChanged([self](f64 value) { self->SetLength(static_cast<f32>(value)); });
        const auto button = [this](StringView id, Function<void()> click) -> gui::ButtonBase*
        {
            auto* found = Find<gui::ButtonBase>(id);
            if (found != nullptr)
                found->SetOnClick(Move(click));
            return found;
        };
        (void)button(u8"seq-new", [self]()
                     {
                         if (self->RequestNewSequence)
                             self->RequestNewSequence();
                     });
        (void)button(u8"seq-open", [self]()
                     {
                         if (self->RequestOpenSequence)
                             self->RequestOpenSequence();
                     });
        m_save = button(u8"seq-save", [self]() { (void)self->Save(); });
        m_undo = button(u8"seq-undo", [self]() { self->Undo(); });
        m_redo = button(u8"seq-redo", [self]() { self->Redo(); });
        m_cast = button(u8"seq-cast", [self]() { self->OpenCastMenu(); });
        m_addTrack = button(u8"seq-add-track", [self]() { self->OpenTrackMenu(); });
        m_addSection = button(u8"seq-add-section", [self]() { (void)self->AddSectionAtPlayhead(); });
        m_addMarker = button(u8"seq-add-marker", [self]() { self->AddMarkerAtPlayhead(false); });
        m_addHold = button(u8"seq-add-hold", [self]() { self->AddMarkerAtPlayhead(true); });
        m_remove = button(u8"seq-remove", [self]() { (void)self->RemoveChosen(); });
        m_restart = button(u8"seq-restart", [self]() { self->Restart(); });
        (void)button(u8"seq-frame", [self]() { self->FrameCurves(); });
        (void)button(u8"seq-close", [self]()
                     {
                         if (self->RequestClose)
                             self->RequestClose();
                     });
        m_stop = button(u8"seq-stop", [self]() { self->Stop(); });
        m_play = Find<gui::ButtonBase>(u8"seq-play");
        if (m_play != nullptr)
            m_play->SetOnToggled([self](bool on)
                                 {
                                     if (on)
                                         self->Play();
                                     else
                                         self->Pause();
                                 });
        if (auto* loop = Find<gui::ButtonBase>(u8"seq-loop"))
            loop->SetOnToggled([self](bool on) { self->SetLooping(on); });
        m_transport.SetLooping(false); // a cutscene plays once

        // The keys it binds while it holds the focus act on the sequence, never on the scene.
        if (m_content)
        {
            m_commands.PushBack(m_content->OnCommand(u8"edit.undo", [self](gui::CommandEvent&) { self->Undo(); }));
            m_commands.PushBack(m_content->OnCommand(u8"edit.redo", [self](gui::CommandEvent&) { self->Redo(); }));
            m_commands.PushBack(m_content->OnCommand(u8"file.save", [self](gui::CommandEvent&) { (void)self->Save(); }));
        }
        Changed();
    }

    SequencePanel::~SequencePanel()
    {
        m_preview.Stop();
        if (m_grid != nullptr)
            m_grid->SetSource(nullptr);
        if (m_sheet != nullptr)
            m_sheet->SetSource(nullptr);
        if (m_curves != nullptr)
            m_curves->SetSource(nullptr);
        if (m_ruler != nullptr)
            m_ruler->SetMarkerSource(nullptr);
        if (m_content)
            for (const u32 token : m_commands)
                (void)m_content->RemoveCommandHandler(token);
    }

    // === The sequence ===

    bool SequencePanel::LoadSequence(const Guid& sequenceId)
    {
        m_preview.Stop();
        if (!m_document.Load(sequenceId))
            return false;
        m_preview.ClearCast();
        m_transport.Stop();
        m_moving = true;
        m_axis->SetPlayhead(0.0f);
        m_moving = false;
        if (m_sheet != nullptr)
        {
            m_sheet->SetSelection(Span<const gui::TimelineKeyRef>{});
            m_sheet->SetSelectedSection({});
        }
        Pose();
        return true;
    }

    void SequencePanel::RequestEditSequence(const Guid& sequenceId, Span<const SequencePanelCast> cast)
    {
        SequencePanel* self = this;
        Array<SequencePanelCast> parts;
        for (const SequencePanelCast& part : cast)
            parts.PushBack(part);
        Function<void()> open = [self, sequenceId, parts = Move(parts)]()
        {
            if (sequenceId != self->m_document.SequenceId())
                (void)self->LoadSequence(sequenceId);
            for (const SequencePanelCast& part : parts)
                self->m_preview.Bind(part.name.AsView(), part.entity);
            self->Pose();
        };
        if (GuardUnsaved && m_document.IsDirty() && sequenceId != m_document.SequenceId())
            GuardUnsaved(Move(open));
        else
            open();
    }

    Status SequencePanel::Save()
    {
        if (!m_document.HasSequence())
            return Status{ErrorCode::NotFound};
        const Status saved = m_document.Save();
        if (saved.IsOk())
            m_context->Notify(NoticeKind::Info, Format(u8"Saved the sequence '{}'.", m_document.Name()).AsView());
        else
            m_context->Notify(NoticeKind::Warning, Format(u8"The sequence '{}' did not save; the Console says why.", m_document.Name()).AsView());
        return saved;
    }

    void SequencePanel::Undo()
    {
        if (m_document.Commands().CanUndo())
            m_document.Commands().Undo();
    }

    void SequencePanel::Redo()
    {
        if (m_document.Commands().CanRedo())
            m_document.Commands().Redo();
    }

    void SequencePanel::SetLength(f32 seconds)
    {
        m_document.Edit(u8"sequence-length", [seconds](sequence::Sequence& sequence) { sequence.duration = Max(0.0f, seconds); });
    }

    // === The cast ===

    void SequencePanel::CastSelected(StringView name)
    {
        const Guid entity = SelectedEntity ? SelectedEntity() : Guid{};
        if (entity.IsNil())
            return;
        m_preview.Bind(name, entity);
        Pose();
        ShowBar();
    }

    i32 SequencePanel::AddPartForSelected()
    {
        const Guid entity = SelectedEntity ? SelectedEntity() : Guid{};
        const scene::EntityHandle handle = entity.IsNil() ? scene::EntityHandle::Invalid() : m_scene->FindEntity(entity);
        if (!m_document.HasSequence() || !handle.IsAssigned())
            return -1;
        // Named for the entity, made unique among the parts.
        String name(m_scene->GetEntityName(handle));
        if (name.IsEmpty())
            name = String(u8"Actor");
        String unique = name;
        for (u32 n = 2; m_document.Sequence().FindBinding(unique.AsView()) >= 0; ++n)
            unique = Format(u8"{} {}", name, n);
        m_document.Edit(u8"sequence-add-part", [&unique](sequence::Sequence& sequence) { sequence.bindings.PushBack(sequence::SequenceBinding{unique}); });
        m_preview.Bind(unique.AsView(), entity);
        Pose();
        ShowBar();
        return m_document.Sequence().FindBinding(unique.AsView());
    }

    // === The tracks ===

    Array<usize> SequencePanel::ChosenTracks() const
    {
        Array<usize> tracks;
        if (m_sheet == nullptr)
            return tracks;
        const Span<const SequenceSource::Channel> channels = m_source.Channels();
        const auto add = [&tracks](usize track)
        {
            for (const usize known : tracks)
                if (known == track)
                    return;
            tracks.PushBack(track);
        };
        if (const auto& section = m_sheet->GetSelectedSection(); section && section->Channel < channels.Size())
            add(channels[section->Channel].Track);
        for (const gui::TimelineKeyRef& key : m_sheet->GetSelection())
            if (key.Channel < channels.Size())
                add(channels[key.Channel].Track);
        return tracks;
    }

    i32 SequencePanel::ChosenPart() const
    {
        const sequence::Sequence& sequence = m_document.Sequence();
        const Array<usize> tracks = ChosenTracks();
        if (!tracks.IsEmpty() && tracks[0] < sequence.tracks.Size())
        {
            const i32 binding = sequence.tracks[tracks[0]]->binding;
            if (binding >= 0 && binding < static_cast<i32>(sequence.bindings.Size()))
                return binding;
        }
        return sequence.bindings.IsEmpty() ? -1 : 0;
    }

    Array<SequenceTrackOffer> SequencePanel::OfferedTracks() const
    {
        Array<SequenceTrackOffer> offers;
        if (!m_document.HasSequence())
            return offers;
        const sequence::Sequence& sequence = m_document.Sequence();
        const i32 part = ChosenPart();
        const String partName = part >= 0 ? sequence.bindings[static_cast<usize>(part)].name : String{};
        for (const TypeInfo* kind : sequence::SequenceTrackKinds())
        {
            if (kind == &sequence::PropertySequenceTrack::StaticType())
                continue; // offered per property below
            const RefPtr<sequence::SequenceTrack> probe = MakeTrack(*kind);
            if (!probe)
                continue;
            SequenceTrackOffer offer;
            offer.kind = kind;
            offer.binding = probe->NeedsBinding() ? part : -1;
            offer.enabled = !probe->NeedsBinding() || part >= 0;
            offer.label = probe->NeedsBinding() ? (part >= 0 ? Format(u8"{} for {}", KindLabel(*kind), partName) : Format(u8"{} (cast a part first)", KindLabel(*kind)))
                                                : KindLabel(*kind);
            offers.PushBack(Move(offer));
        }
        // A property track for each of the part's animatable properties without one.
        const Guid entity = part >= 0 ? m_preview.BoundEntity(partName.AsView()) : Guid{};
        if (!entity.IsNil())
            for (AnimatablePropertyInfo& seed : m_seeds.TrackSeeds(entity))
            {
                if (HasPropertyTrack(sequence, part, seed.componentType.AsView(), seed.propertyPath.AsView()))
                    continue;
                SequenceTrackOffer offer;
                offer.kind = &sequence::PropertySequenceTrack::StaticType();
                offer.binding = part;
                offer.property = true;
                offer.label = Format(u8"{}: {}.{}", partName, seed.componentType, seed.propertyPath);
                offer.target = Move(seed);
                offers.PushBack(Move(offer));
            }
        return offers;
    }

    void SequencePanel::AddTrack(const SequenceTrackOffer& offer)
    {
        if (!m_document.HasSequence() || offer.kind == nullptr || !offer.enabled)
            return;
        RefPtr<sequence::SequenceTrack> track = MakeTrack(*offer.kind);
        if (!track)
            return;
        track->binding = offer.binding;
        if (offer.property)
        {
            auto* property = Cast<sequence::PropertySequenceTrack>(track.Get());
            if (property == nullptr)
                return;
            property->property.componentType = offer.target.componentType;
            property->property.propertyPath = offer.target.propertyPath;
            property->property.kind = offer.target.kind;
            // It starts with what the part's entity shows now, keyed at the playhead.
            const String partName = m_document.Sequence().bindings[static_cast<usize>(offer.binding)].name;
            m_preview.Stop(); // read the entity as the scene has it, not as posed
            const Variant value = m_seeds.Read(m_preview.BoundEntity(partName.AsView()), offer.target.componentType.AsView(),
                                               offer.target.propertyPath.AsView());
            if (!value.IsEmpty())
                KeyFirst(property->property, m_transport.Time(), value);
        }
        m_document.Edit(u8"sequence-add-track", [&track](sequence::Sequence& sequence) { sequence.tracks.PushBack(track); });
    }

    bool SequencePanel::AddSectionAtPlayhead()
    {
        const Array<usize> tracks = ChosenTracks();
        for (const usize t : tracks)
        {
            const sequence::SequenceTrack& track = *m_document.Sequence().tracks[t];
            if (track.KeyChannelCount() != 0)
                continue; // a track of keys
            const f32 time = m_transport.Time();
            i32 made = -1;
            m_document.Edit(u8"sequence-add-section", [t, time, &made](sequence::Sequence& sequence) { made = sequence.tracks[t]->AddSection(time); });
            return made >= 0;
        }
        return false;
    }

    void SequencePanel::AddMarkerAtPlayhead(bool hold)
    {
        if (!m_document.HasSequence())
            return;
        const f32 time = m_transport.Time();
        m_document.Edit(hold ? StringView(u8"sequence-add-hold") : StringView(u8"sequence-add-marker"), [time, hold](sequence::Sequence& sequence)
                        {
                            usize at = 0;
                            while (at < sequence.markers.Size() && sequence.markers[at].time <= time)
                                ++at;
                            sequence.markers.Insert(at, sequence::SequenceMarker{time, String(hold ? StringView(u8"hold") : StringView(u8"marker")), hold});
                        });
    }

    bool SequencePanel::RemoveChosen()
    {
        Array<usize> tracks = ChosenTracks();
        if (tracks.IsEmpty())
            return false;
        m_preview.Stop(); // what the tracks wrote goes back before they go
        if (m_sheet != nullptr)
        {
            m_sheet->SetSelection(Span<const gui::TimelineKeyRef>{});
            m_sheet->SetSelectedSection({});
        }
        m_document.Edit(u8"sequence-remove-tracks", [&tracks](sequence::Sequence& sequence)
                        {
                            // The last first, so each index still names its track.
                            for (usize i = sequence.tracks.Size(); i-- > 0;)
                                for (const usize t : tracks)
                                    if (t == i)
                                    {
                                        sequence.tracks.RemoveAt(i);
                                        break;
                                    }
                        });
        return true;
    }

    void SequencePanel::OpenCastMenu()
    {
        if (m_cast == nullptr || !m_document.HasSequence())
            return;
        m_menu = MakeRef<gui::Menu>(EditorRootAllocator());
        m_menu->SetAutoWidth(true);
        SequencePanel* self = this;
        const bool selected = SelectedEntity && !SelectedEntity().IsNil();
        for (const sequence::SequenceBinding& binding : m_document.Sequence().bindings)
        {
            const String name = binding.name;
            const Guid entity = m_preview.BoundEntity(name.AsView());
            const scene::EntityHandle handle = entity.IsNil() ? scene::EntityHandle::Invalid() : m_scene->FindEntity(entity);
            const StringView playedBy = handle.IsAssigned() ? m_scene->GetEntityName(handle) : StringView(u8"no one");
            m_menu->AddItem(Format(u8"{} (played by {}): fill with the selection", name, playedBy).AsView(), [self, name]() { self->CastSelected(name.AsView()); })
                ->SetEnabled(selected);
        }
        m_menu->AddItem(u8"Add a part for the selection", [self]() { (void)self->AddPartForSelected(); })->SetEnabled(selected);
        m_menu->OpenBelow(*m_cast, *m_cast);
    }

    void SequencePanel::OpenTrackMenu()
    {
        if (m_addTrack == nullptr)
            return;
        // Built anew each time, so what it offers follows the parts and the tracks.
        m_menu = MakeRef<gui::Menu>(EditorRootAllocator());
        m_menu->SetAutoWidth(true);
        SequencePanel* self = this;
        for (const SequenceTrackOffer& offer : OfferedTracks())
        {
            const SequenceTrackOffer pick = offer;
            m_menu->AddItem(offer.label.AsView(), [self, pick]() { self->AddTrack(pick); })->SetEnabled(offer.enabled);
        }
        m_menu->OpenBelow(*m_addTrack, *m_addTrack);
    }

    // === The transport ===

    void SequencePanel::Play()
    {
        m_transport.Play(m_document.Length());
        ShowBar();
    }

    void SequencePanel::Pause()
    {
        m_transport.Pause();
        ShowBar();
    }

    void SequencePanel::Stop()
    {
        m_transport.Stop();
        m_moving = true;
        m_axis->SetPlayhead(0.0f);
        m_moving = false;
        Pose();
        ShowBar();
    }

    void SequencePanel::Restart()
    {
        m_transport.Stop();
        m_transport.Play(m_document.Length());
        m_moving = true;
        m_axis->SetPlayhead(0.0f);
        m_moving = false;
        Pose();
        ShowBar();
    }

    void SequencePanel::SetLooping(bool looping)
    {
        m_transport.SetLooping(looping);
    }

    void SequencePanel::Scrub(f32 time)
    {
        m_axis->SetPlayhead(time); // as a ruler scrub does; the transport and the pose follow
    }

    void SequencePanel::FrameCurves()
    {
        if (m_curves != nullptr)
            m_curves->FrameAll();
    }

    void SequencePanel::Update(f32 dt, bool simulating)
    {
        if (simulating != m_simulating)
        {
            m_simulating = simulating;
            if (simulating)
            {
                m_transport.Pause(); // the scene's own plays run, so the preview stands down
                m_preview.Stop();
            }
            ShowBar();
        }
        if (!m_simulating && m_transport.Advance(dt, m_document.Length()))
        {
            m_moving = true;
            m_axis->SetPlayhead(m_transport.Time());
            m_moving = false;
            Pose();
            if (!m_transport.IsPlaying())
                ShowBar(); // it stopped at the end
        }
        // The dopesheet does not announce its choice, so the curve view and the inspector follow it here.
        ShowCurves();
        ShowInspector();
    }

    // === Showing it ===

    void SequencePanel::Changed()
    {
        m_source.Refresh();
        m_axis->SetDuration(m_document.Length());
        if (m_length != nullptr && static_cast<f32>(m_length->GetValue()) != m_document.Length())
            m_length->SetValue(static_cast<f64>(m_document.Length()));
        Pose(); // after an edit or an undo, the scene shows the sequence as it is now
        ShowBar();
    }

    void SequencePanel::Pose()
    {
        if (m_simulating || !m_document.HasSequence() || m_document.Sequence().tracks.IsEmpty())
        {
            m_preview.Stop();
            return;
        }
        m_preview.Pose(m_document.Sequence(), m_document.Version(), m_transport.Time());
    }

    void SequencePanel::ShowCurves()
    {
        const Array<usize> tracks = ChosenTracks();
        bool same = tracks.Size() == m_shownTracks.Size();
        for (usize i = 0; same && i < tracks.Size(); ++i)
            same = tracks[i] == m_shownTracks[i];
        if (same)
            return;
        m_shownTracks = tracks;
        m_source.ShowTracks(tracks.AsSpan());
    }

    void SequencePanel::ShowInspector()
    {
        const Array<usize> tracks = ChosenTracks();
        const i32 chosen = tracks.Size() == 1 ? static_cast<i32>(tracks[0]) : -1;
        if (chosen == m_inspectedTrack && m_document.Version() == m_inspectedVersion)
            return;
        m_inspectedTrack = chosen;
        m_inspectedVersion = m_document.Version();
        const sequence::Sequence& sequence = m_document.Sequence();
        if (chosen < 0 || static_cast<usize>(chosen) >= sequence.tracks.Size())
        {
            if (m_grid != nullptr)
                m_grid->SetSource(nullptr);
            m_inspected.Reset();
            if (m_trackName != nullptr)
                m_trackName->SetText(tracks.IsEmpty() ? StringView(u8"No track chosen") : StringView(u8"Several tracks chosen"));
            return;
        }
        sequence::SequenceTrack* track = sequence.tracks[static_cast<usize>(chosen)].Get();
        if (m_trackName != nullptr)
            m_trackName->SetText(Format(u8"{} ({})", SequenceSource::TrackLabel(*track), KindLabel(*track->GetType())).AsView());
        UniquePtr<gui::ReflectedPropertySource> source = MakeUnique<gui::ReflectedPropertySource>(EditorRootAllocator(), EditorRootAllocator());
        source->SetTextFormat(&m_format);
        const Instance object(track, track->GetType());
        source->SetObjects(Span<const Instance>(&object, 1u));
        SequencePanel* self = this;
        const usize index = static_cast<usize>(chosen);
        // The track is rebuilt by each step; the grid finds it again by its place.
        source->SetObjectResolver([self, index](usize)
                                  {
                                      const sequence::Sequence& now = self->m_document.Sequence();
                                      return index < now.tracks.Size() ? Instance(now.tracks[index].Get(), now.tracks[index]->GetType()) : Instance{};
                                  });
        source->SetOnChange([self](const gui::ReflectedChange&) { self->m_document.RecordEdit(u8"sequence-inspector"); });
        if (m_grid != nullptr)
            m_grid->SetSource(source.Get());
        m_inspected = Move(source); // after the grid let go of the old one
    }

    void SequencePanel::ShowBar()
    {
        const bool open = m_document.HasSequence();
        if (m_name != nullptr)
            m_name->SetText(open ? m_document.Name() : StringView(u8"No sequence"));
        if (m_dirty != nullptr)
            m_dirty->SetDisplayed(m_document.IsDirty());
        if (m_empty != nullptr)
            m_empty->SetDisplayed(!open);
        if (m_body != nullptr)
            m_body->SetDisplayed(open);
        const bool editable = open && !m_simulating;
        const auto enable = [](gui::ButtonBase* button, bool on)
        {
            if (button != nullptr)
                button->SetEnabled(on);
        };
        enable(m_save, m_document.IsDirty());
        enable(m_undo, m_document.Commands().CanUndo());
        enable(m_redo, m_document.Commands().CanRedo());
        enable(m_cast, editable);
        enable(m_addTrack, editable);
        enable(m_addSection, editable);
        enable(m_addMarker, editable);
        enable(m_addHold, editable);
        enable(m_remove, editable);
        enable(m_play, editable);
        enable(m_restart, editable);
        enable(m_stop, editable);
        if (m_play != nullptr)
            m_play->SetChecked(m_transport.IsPlaying());
    }
}
