// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:document` partition.
//
// SequenceDocument is the sequence the panel edits. A sequence is an asset of its own, not part of
// the scene it plays in, so it keeps its own history and its own unsaved mark, as a property
// animation clip does. Every edit is one undo step that records the whole sequence before and
// after, as its serialized bytes: its tracks are shared objects, so a copy of the sequence would
// share them with the step, and an undo would change what the step holds. A gesture (dragging a
// key or a section) edits live and becomes one step when it ends, or nothing when it is
// cancelled. Each change rebuilds the tracks the document holds, and its version says so, so the
// preview begins its play again on the new ones.

module;
#include "Core/Prelude.h"

export module editor.sequence:document;

import foundation.core;
import foundation.sequence;
import editor.core;

using namespace foundation::core;

export namespace editor
{
    class SequenceDocument
    {
    public:
        explicit SequenceDocument(EditorContext& context);
        SequenceDocument(const SequenceDocument&) = delete;
        SequenceDocument& operator=(const SequenceDocument&) = delete;

        /// Reads the sequence asset `sequenceId` and drops the current history; false when there is
        /// no such sequence.
        bool Load(const Guid& sequenceId);
        /// Writes the sequence to its asset and asks for a cook; it is then marked saved.
        Status Save();
        /// Closes the sequence and drops its history.
        void Close();

        [[nodiscard]] bool HasSequence() const noexcept { return !m_id.IsNil(); }
        [[nodiscard]] const Guid& SequenceId() const noexcept { return m_id; }
        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] bool IsDirty() const noexcept { return HasSequence() && !m_commands.IsAtSaved(); }
        [[nodiscard]] const foundation::sequence::Sequence& Sequence() const noexcept { return m_sequence; }
        /// The length it plays over: its authored duration or its content's, whichever is later
        /// (at least a second).
        [[nodiscard]] f32 Length() const noexcept;
        /// Moves on whenever the tracks are rebuilt (an edit, an undo, a load): what a play began on
        /// the old ones must begin again.
        [[nodiscard]] u64 Version() const noexcept { return m_version; }
        [[nodiscard]] EditorCommandStack& Commands() noexcept { return m_commands; }

        /// Makes an immediate edit: `mutate` changes the sequence as one undo step (the duration
        /// grows to cover what it holds).
        void Edit(StringView typeId, const Function<void(foundation::sequence::Sequence&)>& mutate);
        /// Starts a gesture: the sequence is edited live (GestureSequence, then PreviewGesture to
        /// show it) and becomes one undo step when it commits; a cancelled gesture restores it.
        void BeginGesture();
        [[nodiscard]] foundation::sequence::Sequence& GestureSequence() noexcept { return m_sequence; }
        /// Puts the sequence back as the gesture found it, so a preview applies its whole change
        /// again from there (a drag's delta is from where it began).
        void RestoreGestureStart();
        void PreviewGesture();
        void CommitGesture(StringView typeId);
        void CancelGesture();
        [[nodiscard]] bool InGesture() const noexcept { return m_inGesture; }

        /// Called after any change: an edit, a gesture's preview, an undo, a load.
        Function<void()> OnChanged;

    private:
        class Step;
        [[nodiscard]] Array<byte> Snapshot();
        void Restore(Span<const byte> bytes);
        void Push(StringView typeId, Array<byte> before);
        void Changed();

        EditorContext* m_context;
        foundation::sequence::Sequence m_sequence;
        Guid m_id;
        String m_name;
        EditorCommandStack m_commands;
        Array<byte> m_gestureBefore;
        bool m_inGesture = false;
        u64 m_version = 0;
    };
}
