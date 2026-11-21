// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Run - the run service contributions (see RunServices.cppm).

module;

#include "Core/Prelude.h"

module engine.run;

import foundation.core;
import foundation.run;

using namespace foundation::core;

namespace engine::runtime
{
    RunServiceContributions& RunServiceContributions::Global()
    {
        static RunServiceContributions contributions;
        return contributions;
    }

    u64 RunServiceContributions::Add(const RunServiceContribution& contribution)
    {
        if (contribution.type == 0 || contribution.install == nullptr)
        {
            return 0;
        }
        const u64 token = m_nextToken++;
        m_contributions.PushBack(Entry{token, contribution});
        for (foundation::run::RunBase* run : m_runs)
        {
            contribution.install(*run);
        }
        return token;
    }

    void RunServiceContributions::Remove(u64 token)
    {
        for (usize i = 0; i < m_contributions.Size(); ++i)
        {
            if (m_contributions[i].token != token)
            {
                continue;
            }
            const TypeId type = m_contributions[i].contribution.type;
            m_contributions.RemoveAt(i);
            for (foundation::run::RunBase* run : m_runs)
            {
                run->RemoveService(type);
            }
            return;
        }
    }

    void RunServiceContributions::Attach(foundation::run::RunBase& run)
    {
        for (foundation::run::RunBase* known : m_runs)
        {
            if (known == &run)
            {
                return;
            }
        }
        m_runs.PushBack(&run);
        for (const Entry& entry : m_contributions)
        {
            entry.contribution.install(run);
        }
    }

    void RunServiceContributions::Detach(foundation::run::RunBase& run)
    {
        for (usize i = 0; i < m_runs.Size(); ++i)
        {
            if (m_runs[i] == &run)
            {
                m_runs.RemoveAt(i);
                return;
            }
        }
    }
}
