// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:preview` partition.
//
// SequencePreview poses the scene with the sequence at a time, through the same play the game
// runs (SequencePlay::Pose: no events, no sounds), so what the panel shows is what plays. Its play
// finishes by restoring, so stopping the preview puts back every value it wrote and lets go of
// what it holds (the fade, a camera cut, a clip's overlay). None of it becomes a step in the
// scene's history or touches its unsaved mark. The cast is the preview's own (a name and an
// entity of the scene for each binding), since a sequence names its cast and the game fills it.

module;
#include "Core/Prelude.h"

export module editor.sequence:preview;

import foundation.core;
import foundation.scene;
import foundation.sequence;

using namespace foundation::core;

export namespace editor
{
    class SequencePreview
    {
    public:
        explicit SequencePreview(foundation::scene::Scene& scene) : m_scene(&scene) {}
        ~SequencePreview() { Stop(); }
        SequencePreview(const SequencePreview&) = delete;
        SequencePreview& operator=(const SequencePreview&) = delete;

        /// Fills the binding `name` with the scene's entity `entity` (nil empties it). A preview
        /// posing begins again with it.
        void Bind(StringView name, const Guid& entity);
        /// The entity filling `name`; nil when none.
        [[nodiscard]] Guid BoundEntity(StringView name) const;
        /// Empties every binding.
        void ClearCast();

        /// Poses the scene with `sequence` at `time`. A different sequence, or another `version`
        /// of it (its tracks rebuilt), begins the play again, after restoring what the last one
        /// wrote.
        void Pose(const foundation::sequence::Sequence& sequence, u64 version, f32 time);
        /// Restores what the preview wrote and ends it.
        void Stop();
        [[nodiscard]] bool IsPosing() const noexcept { return static_cast<bool>(m_play); }

    private:
        struct Cast
        {
            String name;
            Guid entity;
        };

        foundation::scene::Scene* m_scene;
        Array<Cast> m_cast;
        UniquePtr<foundation::sequence::SequencePlay> m_play;
        const foundation::sequence::Sequence* m_sequence = nullptr; // what m_play plays
        u64 m_version = 0;
    };
}
