// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Run - :services partition.
//
// The services a run gets from outside its own members: each contribution installs one into a run
// (a plugin's per-run state, such as the dialogue's runner), as a scene's managers come from the
// scene contributions. Every ApplicationRun installs them all as it is made. A contribution added
// while runs exist (a plugin loading under a running game) is installed into those runs at once,
// and one removed (the plugin unloading) is taken out of every live run, so no run keeps a service
// whose code is gone.

module;

#include "Core/Prelude.h"

export module engine.run:services;

import foundation.core;
import foundation.run;

using namespace foundation::core;

export namespace engine::runtime
{
    /// A service every run gets: its type, and what installs one into a run (`run.Add<T>(...)`).
    struct RunServiceContribution
    {
        StringView id = {};
        TypeId type = 0;
        void (*install)(foundation::run::RunBase& run) = nullptr;
    };

    /// The process's run service contributions, and the live runs they are installed in.
    class RunServiceContributions
    {
    public:
        [[nodiscard]] static RunServiceContributions& Global();

        /// Adds `contribution` and installs it into every live run; its token for Remove (0, nothing
        /// added, for one with no type or no install).
        u64 Add(const RunServiceContribution& contribution);
        /// Removes the contribution `token`, taking its service out of every live run.
        void Remove(u64 token);
        /// Installs every contribution into `run` and keeps it as a live run (a run's construction).
        void Attach(foundation::run::RunBase& run);
        /// Forgets `run` (its destruction): its services go with it.
        void Detach(foundation::run::RunBase& run);

        [[nodiscard]] usize Size() const noexcept { return m_contributions.Size(); }

    private:
        struct Entry
        {
            u64 token = 0;
            RunServiceContribution contribution;
        };

        Array<Entry> m_contributions;
        Array<foundation::run::RunBase*> m_runs;
        u64 m_nextToken = 1;
    };
}
