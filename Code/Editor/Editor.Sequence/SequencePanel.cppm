// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:panel` partition.
//
// SequencePanel authors a sequence in the scene, built on the GUI from its markup
// (Data/Editor/SequencePanel.xml). The scene page holds it under its viewport, beside the property
// animation panel, and shows it only when asked (its toggle, the Scene menu, a sequence
// component's Edit Sequence): selecting an entity never opens it. It edits a sequence asset of its
// own, never the scene: it has its own history, Undo, Redo and Save on its bar, and its own unsaved
// mark; while it holds the focus Ctrl+Z, Ctrl+Y and Ctrl+S act on the sequence.
//
// The ruler (the sequence's markers), the dopesheet (key rows and section rows, by part) and the
// curve view share one time axis (:source). The inspector beside them shows the chosen track's
// fields (a section's clip, an event's name), and each edit there is one step of the sequence's
// history. The cast is the panel's own (Cast fills a part with the scene's selection), and
// scrubbing or playing poses the scene through the game's own play (:preview), put back when the
// preview stops; the preview stands down while the scene simulates. Track offers every registered
// kind (and a property track for each of the chosen part's animatable properties), so a kind a
// domain or a plugin adds is offered with no list to extend.

module;
#include "Core/Prelude.h"

export module editor.sequence:panel;

import foundation.core;
import foundation.scene;
import foundation.sequence;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.propertyanimation; // ClipTransport, the track seeds (ClipPreview)
import :document;
import :preview;
import :source;

using namespace foundation::core;

export namespace editor
{
    /// The panel's markup, at namespace scope for another library to read (MSVC does not export a
    /// class's static constexpr member from a shared library).
    inline constexpr StringView kSequencePanelPath = u8"Editor/SequencePanel.xml";

    /// A track Track offers: a kind, and for a property track the property it animates.
    struct SequenceTrackOffer
    {
        const TypeInfo* kind = nullptr;
        String label;
        i32 binding = -1; // the part it is for; -1 for the whole sequence
        bool property = false;
        AnimatablePropertyInfo target; // a property track's
        bool enabled = true;
    };

    /// A name of the cast and the entity filling it, as a sequence component's cast gives it.
    struct SequencePanelCast
    {
        String name;
        Guid entity;
    };

    class SequencePanel
    {
    public:
        static constexpr StringView kPath = kSequencePanelPath;

        /// Builds the panel over `scene` (the page's, which outlives it) from its loaded markup
        /// `content`; `refs` (may be null) picks the assets the inspector refers to.
        SequencePanel(EditorContext& context, foundation::scene::Scene& scene, RefPtr<foundation::gui::Node> content,
                      const app::ResourceRefServices* refs);
        ~SequencePanel();
        SequencePanel(const SequencePanel&) = delete;
        SequencePanel& operator=(const SequencePanel&) = delete;

        [[nodiscard]] RefPtr<foundation::gui::Node> Content() const { return m_content; }

        // === Hooks the page sets ===
        /// The scene's selected entity; unset means none.
        Function<Guid()> SelectedEntity;
        /// New and Open open the page's dialogs, which end by calling LoadSequence.
        Function<void()> RequestNewSequence;
        Function<void()> RequestOpenSequence;
        /// Runs `proceed` now, or once the user has saved or discarded the unsaved sequence.
        Function<void(Function<void()>)> GuardUnsaved;
        /// The bar's Close: the page hides the panel (the sequence stays open in it).
        Function<void()> RequestClose;

        // === The sequence ===
        /// Edits the sequence asset `sequenceId` (false when there is no such sequence), with the
        /// transport stopped at the start.
        bool LoadSequence(const Guid& sequenceId);
        /// Asks about an unsaved sequence, then opens `sequenceId` and fills its cast with `cast`
        /// (a sequence component's).
        void RequestEditSequence(const Guid& sequenceId, Span<const SequencePanelCast> cast);
        Status Save();
        void Undo();
        void Redo();
        void SetLength(f32 seconds);

        // === The cast ===
        /// Fills the part `name` with the entity selected in the scene.
        void CastSelected(StringView name);
        /// Adds a part named for the selected entity and fills it with it; its index, or -1.
        i32 AddPartForSelected();
        [[nodiscard]] Guid CastEntity(StringView name) const { return m_preview.BoundEntity(name); }

        // === The tracks ===
        /// What Track offers: every registered kind (for the chosen part when the kind needs one)
        /// and a property track for each of the chosen part's animatable properties without one.
        [[nodiscard]] Array<SequenceTrackOffer> OfferedTracks() const;
        void AddTrack(const SequenceTrackOffer& offer);
        /// Adds a section at the playhead to the chosen section track; false when none is chosen.
        bool AddSectionAtPlayhead();
        /// Adds a marker (a hold, when `hold`) at the playhead.
        void AddMarkerAtPlayhead(bool hold);
        /// The tracks of the keys or the section chosen in the dopesheet.
        [[nodiscard]] Array<usize> ChosenTracks() const;
        /// The part of the first chosen track; else the first part; -1 when there is none.
        [[nodiscard]] i32 ChosenPart() const;
        /// Removes the chosen tracks as one step; false when none is chosen.
        bool RemoveChosen();

        // === The transport ===
        void Play();
        void Pause();
        void Stop();
        void Restart();
        void SetLooping(bool looping);
        void Scrub(f32 time);
        void FrameCurves();

        /// Called once a frame by the page: the transport, the preview (stood down while
        /// `simulating`), the inspector and the bar.
        void Update(f32 dt, bool simulating);
        /// Puts back what the preview posed (the page saves the scene or simulates); Repose poses
        /// it again.
        void StopPreview() { m_preview.Stop(); }
        void Repose() { Pose(); }

        // === For tests ===
        [[nodiscard]] SequenceDocument& Document() noexcept { return m_document; }
        [[nodiscard]] SequenceSource& Source() noexcept { return m_source; }
        [[nodiscard]] SequencePreview& Preview() noexcept { return m_preview; }
        [[nodiscard]] ClipTransport& Transport() noexcept { return m_transport; }
        [[nodiscard]] foundation::gui::Dopesheet* Sheet() const noexcept { return m_sheet; }
        [[nodiscard]] foundation::gui::TimeAxis& Axis() const noexcept { return *m_axis; }
        [[nodiscard]] foundation::gui::ReflectedPropertySource* Inspected() const noexcept { return m_inspected.Get(); }

    private:
        void Changed();
        void Pose();
        void ShowBar();
        void ShowCurves();
        void ShowInspector();
        void OpenCastMenu();
        void OpenTrackMenu();
        template <typename T>
        [[nodiscard]] T* Find(StringView id) const;

        EditorContext* m_context;
        foundation::scene::Scene* m_scene;
        RefPtr<foundation::gui::Node> m_content;
        SequenceDocument m_document;
        SequenceSource m_source;
        SequencePreview m_preview;
        ClipPreview m_seeds; // reads the cast's properties (Track's property offers, their first key)
        ClipTransport m_transport;
        RefPtr<foundation::gui::TimeAxis> m_axis;
        app::XmlPropertyTextFormat m_format;
        UniquePtr<foundation::gui::ReflectedPropertySource> m_inspected;
        i32 m_inspectedTrack = -1;
        u64 m_inspectedVersion = 0;
        bool m_simulating = false;
        bool m_moving = false;
        Array<usize> m_shownTracks;
        Array<u32> m_commands;
        RefPtr<foundation::gui::Menu> m_menu;

        foundation::gui::Dopesheet* m_sheet = nullptr;
        foundation::gui::CurveView* m_curves = nullptr;
        foundation::gui::TimeRuler* m_ruler = nullptr;
        foundation::gui::PropertyGrid* m_grid = nullptr;
        foundation::gui::Label* m_name = nullptr;
        foundation::gui::Label* m_trackName = nullptr;
        foundation::gui::Node* m_dirty = nullptr;
        foundation::gui::NumberField* m_length = nullptr;
        foundation::gui::Node* m_empty = nullptr;
        foundation::gui::Node* m_body = nullptr;
        foundation::gui::ButtonBase* m_save = nullptr;
        foundation::gui::ButtonBase* m_undo = nullptr;
        foundation::gui::ButtonBase* m_redo = nullptr;
        foundation::gui::ButtonBase* m_play = nullptr;
        foundation::gui::ButtonBase* m_cast = nullptr;
        foundation::gui::ButtonBase* m_addTrack = nullptr;
        foundation::gui::ButtonBase* m_addSection = nullptr;
        foundation::gui::ButtonBase* m_addMarker = nullptr;
        foundation::gui::ButtonBase* m_addHold = nullptr;
        foundation::gui::ButtonBase* m_remove = nullptr;
        foundation::gui::ButtonBase* m_restart = nullptr;
        foundation::gui::ButtonBase* m_stop = nullptr;
    };
}
