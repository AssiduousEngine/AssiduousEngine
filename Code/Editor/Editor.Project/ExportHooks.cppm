// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :export_hooks partition.
//
// What a plugin's Tooling part adds to an export, through the service its host provides
// (PluginContext::Find<ExportPluginServices>), for what a plugin's data cannot say:
//   - hooks that run once a preset's dist is staged (a platform's own packaging step, a file written
//     from the project's settings); a hook that fails fails the export, with its reason;
//   - player hooks, asked before the export chooses its player: whether the project needs one of its
//     own (a language compiled into it), why not at all, and what that player's build is configured
//     with.
// The service records what each plugin added, so the plugin host's unload takes it out again.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.project:export_hooks;

import foundation.core;
import foundation.runtime;
import :project;
import :export_preset;

using namespace foundation::core;

export namespace editor
{
    /// What an export hook is handed: the project, the preset and the staged dist's folder.
    struct ExportHookContext
    {
        EditorProject* project = nullptr;
        const ExportPreset* preset = nullptr;
        StringView outputDir;
    };

    /// A hook: runs after the dist is staged, and answers why it failed (empty when it did not), which
    /// fails the export.
    using ExportHook = Function<String(const ExportHookContext&)>;

    /// What a player hook is asked: the project, the platform the export is for and the plugins the
    /// project enables there.
    struct PlayerHookContext
    {
        const EditorProject* project = nullptr;
        StringView platform;
        Span<const String> enabled;
    };

    /// A player hook's answer: whether the project needs its own player there and why; why it cannot
    /// be exported there at all (empty when it can); and the CMake definitions that player's build is
    /// configured with (`-DNAME=VALUE`), which a later build of the same tree drops when they are
    /// not given again.
    struct PlayerNeed
    {
        bool build = false;
        String reason;
        String refusal;
        Array<String> definitions;
    };

    using PlayerHook = Function<PlayerNeed(const PlayerHookContext&)>;

    class ExportPluginServices final : public foundation::runtime::IRegistrationRecorder
    {
    public:
        explicit ExportPluginServices(IAllocator& allocator) : m_hooks(allocator), m_playerHooks(allocator) {}
        ~ExportPluginServices() override = default;

        ExportPluginServices(const ExportPluginServices&) = delete;
        ExportPluginServices& operator=(const ExportPluginServices&) = delete;

        /// Hands this service to `host`'s plugins and records what they add through it. Before the
        /// host loads; this outlives the host.
        [[nodiscard]] bool Attach(foundation::runtime::PluginHost& host)
        {
            if (!host.Provide(*this))
            {
                return false;
            }
            host.Loader().AddRecorder(this);
            return true;
        }

        /// Adds a hook every export runs after staging, in the order they were added. `name` says
        /// whose it is in a failure. False for an empty hook.
        bool AddHook(StringView name, ExportHook hook)
        {
            if (!hook)
            {
                return false;
            }
            const u64 token = m_nextToken++;
            m_hooks.PushBack(Entry{token, String(name), Move(hook)});
            if (m_sink != nullptr)
            {
                m_sink->PushBack(token);
            }
            return true;
        }

        [[nodiscard]] usize Count() const noexcept { return m_hooks.Size(); }

        /// Adds a player hook every export asks before it chooses its player. `name` says whose it is
        /// in a refusal. False for an empty hook.
        bool AddPlayerHook(StringView name, PlayerHook hook)
        {
            if (!hook)
            {
                return false;
            }
            const u64 token = m_nextToken++;
            m_playerHooks.PushBack(PlayerEntry{token, String(name), Move(hook)});
            if (m_sink != nullptr)
            {
                m_sink->PushBack(token);
            }
            return true;
        }

        [[nodiscard]] usize PlayerHookCount() const noexcept { return m_playerHooks.Size(); }

        /// Every player hook's answer together: the first refusal as "<name>: <refusal>", a player of
        /// its own when any needs one (the first reason), and every definition, in the order the hooks
        /// were added.
        [[nodiscard]] PlayerNeed AskPlayer(const PlayerHookContext& context) const
        {
            PlayerNeed combined;
            for (const PlayerEntry& entry : m_playerHooks)
            {
                PlayerNeed need = entry.hook(context);
                if (!need.refusal.IsEmpty())
                {
                    combined.refusal = Format(u8"{}: {}", entry.name, need.refusal);
                    return combined;
                }
                if (need.build && !combined.build)
                {
                    combined.build = true;
                    combined.reason = Move(need.reason);
                }
                for (String& definition : need.definitions)
                {
                    combined.definitions.PushBack(Move(definition));
                }
            }
            return combined;
        }

        /// Runs every hook over a staged dist; the first failure as "<name>: <reason>", or empty.
        [[nodiscard]] String Run(const ExportHookContext& context) const
        {
            for (const Entry& entry : m_hooks)
            {
                const String failed = entry.hook(context);
                if (!failed.IsEmpty())
                {
                    return Format(u8"{}: {}", entry.name, failed);
                }
                LOG_INFO(u8"Export", u8"export hook '{}' ran", entry.name);
            }
            return String{};
        }

        // IRegistrationRecorder: armed around each part's load, so what it added is recorded
        // against it, and reversed when it unloads.
        void Arm(Array<TypeId>& sink) override { m_sink = &sink; }
        void Disarm() override { m_sink = nullptr; }
        void Reverse(TypeId id) override
        {
            for (usize i = 0; i < m_hooks.Size(); ++i)
            {
                if (m_hooks[i].token == id)
                {
                    m_hooks.RemoveAt(i);
                    return;
                }
            }
            for (usize i = 0; i < m_playerHooks.Size(); ++i)
            {
                if (m_playerHooks[i].token == id)
                {
                    m_playerHooks.RemoveAt(i);
                    return;
                }
            }
        }

    private:
        struct Entry
        {
            u64 token; // this service's, what the recording keeps
            String name;
            ExportHook hook;
        };

        struct PlayerEntry
        {
            u64 token;
            String name;
            PlayerHook hook;
        };

        Array<Entry> m_hooks;
        Array<PlayerEntry> m_playerHooks;
        Array<TypeId>* m_sink = nullptr;
        u64 m_nextToken = 1;
    };
}
