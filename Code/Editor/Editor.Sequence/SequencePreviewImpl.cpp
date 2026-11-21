// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Sequence - the `:preview` partition (implementation).

module;
#include "Core/Prelude.h"

module editor.sequence;

import foundation.core;
import foundation.scene;
import foundation.sequence;
import editor.core;

using namespace foundation::core;
namespace sequence = foundation::sequence;
namespace scene = foundation::scene;

namespace editor
{
    void SequencePreview::Bind(StringView name, const Guid& entity)
    {
        for (usize i = 0; i < m_cast.Size(); ++i)
            if (m_cast[i].name.AsView() == name)
            {
                if (m_cast[i].entity == entity)
                    return;
                Stop(); // what the old entity showed goes back first
                if (entity.IsNil())
                    m_cast.RemoveAt(i);
                else
                    m_cast[i].entity = entity;
                return;
            }
        if (entity.IsNil())
            return;
        Stop();
        m_cast.PushBack(Cast{String(name), entity});
    }

    Guid SequencePreview::BoundEntity(StringView name) const
    {
        for (const Cast& cast : m_cast)
            if (cast.name.AsView() == name)
                return cast.entity;
        return Guid{};
    }

    void SequencePreview::ClearCast()
    {
        Stop();
        m_cast.Clear();
    }

    void SequencePreview::Pose(const sequence::Sequence& sequence, u64 version, f32 time)
    {
        if (m_play && (m_sequence != &sequence || m_version != version))
            Stop();
        if (!m_play)
        {
            m_play = MakeUnique<sequence::SequencePlay>(EditorRootAllocator(), EditorRootAllocator(), *m_scene, sequence);
            m_play->finish = sequence::SequenceFinish::Restore;
            for (const Cast& cast : m_cast)
            {
                const scene::EntityHandle entity = m_scene->FindEntity(cast.entity);
                if (entity.IsAssigned())
                    (void)m_play->Bind(cast.name.AsView(), entity);
            }
            m_sequence = &sequence;
            m_version = version;
        }
        m_play->Pose(time);
    }

    void SequencePreview::Stop()
    {
        if (!m_play)
            return;
        m_play->Stop(); // the Restore finish puts every written value back
        m_play.Reset();
        m_sequence = nullptr;
    }
}
