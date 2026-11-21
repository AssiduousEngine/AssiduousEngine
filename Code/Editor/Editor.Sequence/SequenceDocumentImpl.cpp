// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:document` partition (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.sequence;

import foundation.core;
import foundation.content;
import foundation.resource;
import foundation.sequence;
import sequence.pipeline;
import editor.core;

using namespace foundation::core;
namespace sequence = foundation::sequence;

namespace editor
{
    // One undo step: the whole sequence before and after, as bytes.
    class SequenceDocument::Step final : public IEditorCommand
    {
    public:
        Step(SequenceDocument& document, StringView typeId, Array<byte> before, Array<byte> after)
            : m_document(&document), m_typeId(typeId), m_before(Move(before)), m_after(Move(after))
        {
        }
        [[nodiscard]] bool Execute() override
        {
            m_document->Restore(m_after.AsSpan());
            return true;
        }
        void Undo() override { m_document->Restore(m_before.AsSpan()); }
        [[nodiscard]] StringView TypeId() const override { return m_typeId.AsView(); }

    private:
        SequenceDocument* m_document;
        String m_typeId;
        Array<byte> m_before;
        Array<byte> m_after;
    };

    SequenceDocument::SequenceDocument(EditorContext& context) : m_context(&context)
    {
        // Notified once the history has moved, so the unsaved mark reflects its new position.
        SequenceDocument* self = this;
        m_commands.OnChanged = [self]() { self->Changed(); };
    }

    bool SequenceDocument::Load(const Guid& sequenceId)
    {
        foundation::content::Instance* instance = m_context->Project() != nullptr ? m_context->Project()->SourceDb().GetInstance(sequenceId) : nullptr;
        RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
        auto* asset = Cast<pipeline::SequenceAsset>(object.Get());
        if (asset == nullptr)
            return false;
        CancelGesture();
        m_sequence = Move(asset->sequence); // the read object is this document's alone
        if (foundation::resource::ResourceManager* resources = m_context->Resources())
            for (RefPtr<sequence::SequenceTrack>& track : m_sequence.tracks)
                track->ResolveResources(*resources);
        m_id = sequenceId;
        m_name = String(instance->Name());
        m_current = Snapshot();
        m_commands.Clear();
        m_commands.MarkSaved();
        Changed();
        return true;
    }

    Status SequenceDocument::Save()
    {
        foundation::content::Instance* instance = HasSequence() && m_context->Project() != nullptr ? m_context->Project()->SourceDb().GetInstance(m_id) : nullptr;
        if (instance == nullptr)
            return Status{ErrorCode::NotFound};
        pipeline::SequenceAsset asset;
        asset.sequence = m_sequence; // shares the tracks; the write only reads them
        const Status written = instance->WriteObject(asset);
        if (!written.IsOk())
        {
            LOG_WARNING(u8"Editor", u8"writing the sequence '{}' failed", m_name);
            return written;
        }
        m_commands.MarkSaved();
        m_context->RequestCook(false);
        Changed(); // updates the unsaved mark
        return written;
    }

    void SequenceDocument::Close()
    {
        CancelGesture();
        m_sequence = sequence::Sequence{};
        m_current.Clear();
        m_id = Guid{};
        m_name = String{};
        m_commands.Clear();
        m_commands.MarkSaved();
        Changed();
    }

    f32 SequenceDocument::Length() const noexcept
    {
        return Max(Max(m_sequence.duration, m_sequence.ComputeDuration()), 1.0f);
    }

    void SequenceDocument::Edit(StringView typeId, const Function<void(sequence::Sequence&)>& mutate)
    {
        if (!HasSequence() || m_inGesture)
            return;
        Array<byte> before = Snapshot();
        mutate(m_sequence);
        Push(typeId, Move(before));
    }

    void SequenceDocument::RecordEdit(StringView typeId)
    {
        if (!HasSequence() || m_inGesture)
            return;
        Array<byte> before = m_current;
        Push(typeId, Move(before));
    }

    void SequenceDocument::BeginGesture()
    {
        if (!HasSequence())
            return;
        if (m_inGesture)
            CancelGesture();
        m_gestureBefore = Snapshot();
        m_inGesture = true;
    }

    void SequenceDocument::RestoreGestureStart()
    {
        if (m_inGesture)
            Restore(m_gestureBefore.AsSpan());
    }

    void SequenceDocument::PreviewGesture()
    {
        ++m_version; // the tracks changed in place: a play on them begins again
        Changed();
    }

    void SequenceDocument::CommitGesture(StringView typeId)
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        Push(typeId, Move(m_gestureBefore));
    }

    void SequenceDocument::CancelGesture()
    {
        if (!m_inGesture)
            return;
        m_inGesture = false;
        Restore(m_gestureBefore.AsSpan());
        Changed();
    }

    Array<byte> SequenceDocument::Snapshot()
    {
        MemoryStream stream;
        {
            BinarySerializer writer(stream, SerializeMode::Write);
            sequence::SerializeSequence(writer, m_sequence);
        }
        Array<byte> bytes;
        for (const byte b : stream.Bytes())
            bytes.PushBack(b);
        return bytes;
    }

    void SequenceDocument::Restore(Span<const byte> bytes)
    {
        MemoryStream stream;
        (void)stream.Write(bytes.Data(), bytes.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        sequence::Sequence restored;
        {
            BinarySerializer reader(stream, SerializeMode::Read);
            sequence::SerializeSequence(reader, restored);
        }
        if (foundation::resource::ResourceManager* resources = m_context->Resources())
            for (RefPtr<sequence::SequenceTrack>& track : restored.tracks)
                track->ResolveResources(*resources);
        m_sequence = Move(restored);
        m_current.Clear();
        for (const byte b : bytes)
            m_current.PushBack(b);
        ++m_version; // the history notifies once it has moved
    }

    void SequenceDocument::Push(StringView typeId, Array<byte> before)
    {
        // The duration never cuts off what the sequence holds.
        m_sequence.duration = Max(m_sequence.duration, m_sequence.ComputeDuration());
        Array<byte> after = Snapshot();
        if (after.Size() == before.Size() && (before.IsEmpty() || MemCompare(after.Data(), before.Data(), before.Size()) == 0))
        {
            Changed(); // nothing changed (a gesture back where it began): no step to undo
            return;
        }
        (void)m_commands.Execute(UniquePtr<IEditorCommand>(EditorRootAllocator().New<Step>(*this, typeId, Move(before), Move(after)), EditorRootAllocator()));
    }

    void SequenceDocument::Changed()
    {
        ++m_version;
        if (OnChanged)
            OnChanged();
    }
}
