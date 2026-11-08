// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_simulation partition.
//
// Edit-mode Simulate over a page's scene. Starting takes a snapshot, then starts and runs the
// scene, with the page's command stack locked meanwhile (runtime mutations do not belong in the
// edit history, and undoing into entities the restore recreates would tangle their guids). Paused,
// the simulation holds. Stopped, the scene stops and the snapshot is restored into the same scene,
// so borrowed scene pointers stay valid and the guid-keyed selection resolves again (what the run
// spawned drops out). The game's UI stays non-interactive meanwhile: Simulate's pointer serves
// selection and the camera, and the Game tab is the interactive run.
//
// The page's scene belongs to a run of its own (SimulationRun), as a game's scenes belong to the
// game's: its behaviours run on the run's script host, and what they put on screen and play is the
// run's, ended when the run's session stops. Each Simulate is one session of the run; edit mode is
// between sessions. The run lives as long as the page, so its scenes always carry it.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module editor.scene:scene_simulation;

import foundation.core;
import foundation.resource;
import foundation.run; // RunBase: the page's run
import foundation.scene;
import foundation.scene.resource;
import engine.script; // ScriptRunHost: the run's scripts
import editor.core;

using namespace foundation::core;

export namespace editor
{
    /// A scene page's run: its Simulate's scripts, screens and sounds. It holds a script host and
    /// nothing a game has besides (no input map, save, network or `run` facade), as Simulate has
    /// none of those.
    class SimulationRun final : public foundation::run::RunBase
    {
    public:
        SimulationRun(IAllocator& allocator, StringView name) : RunBase(allocator, name), m_scripts(allocator)
        {
            Register<engine::script::ScriptRunHost>(m_scripts);
            m_scripts.Binding().run = this; // its scripts find this run (gui::root, Audio)
        }
        // Disposed while the host it registered still exists; the host's context goes after.
        ~SimulationRun() override
        {
            Dispose();
            m_scripts.Teardown();
        }

        [[nodiscard]] engine::script::ScriptRunHost& Scripts() noexcept { return m_scripts; }

    private:
        engine::script::ScriptRunHost m_scripts;
    };

    class SceneSimulation
    {
    public:
        SceneSimulation(IAllocator& allocator, StringView name) : m_run(allocator, name) {}

        /// The page's run: its scenes carry it whether or not a Simulate is under way.
        [[nodiscard]] SimulationRun& Run() noexcept { return m_run; }
        [[nodiscard]] bool IsRunning() const noexcept { return m_running; }
        [[nodiscard]] bool IsPaused() const noexcept { return m_paused; }

        /// Snapshot `scene` and run it with `commands` locked; false when it is already running or
        /// the snapshot failed (reported in the log).
        bool Start(foundation::scene::Scene& scene, EditorCommandStack& commands)
        {
            if (m_running)
                return false;
            m_snapshot = foundation::scene::SceneSnapshot::Capture(scene);
            if (!m_snapshot)
            {
                LOG_ERROR(u8"Editor", u8"Simulate: scene snapshot capture failed");
                return false;
            }
            m_run.Start(); // the session first, so the scene starts inside it
            scene.Start();
            scene.SetSimulationEnabled(true);
            commands.SetLocked(true);
            m_running = true;
            m_paused = false;
            return true;
        }
        /// Holds or resumes a running simulation (the per-frame freeze, not the start and stop
        /// callbacks the systems get on the big transitions); the run pauses with it.
        void Pause(foundation::scene::Scene& scene, bool paused)
        {
            if (!m_running)
                return;
            m_paused = paused;
            scene.SetSimulationEnabled(!paused);
            m_run.SetPaused(paused);
        }
        /// Stops and restores the snapshot into `scene`, its resources resolved through `resources`
        /// (null: none), `commands` unlocked. The run's session ends last, after the scene's
        /// behaviours have stopped, taking down what they left on screen and playing.
        void Stop(foundation::scene::Scene& scene, EditorCommandStack& commands, foundation::resource::ResourceManager* resources)
        {
            if (!m_running)
                return;
            scene.Stop();
            if (m_snapshot)
            {
                if (!m_snapshot->Restore(scene, resources).IsOk())
                    LOG_ERROR(u8"Editor", u8"Simulate: snapshot restore failed");
                m_snapshot = nullptr;
            }
            scene.SetSimulationEnabled(false);
            commands.SetLocked(false);
            m_running = false;
            m_paused = false;
            m_run.Stop();
        }
        /// A page closing with its scene running: the scene stops and the session ends, with no
        /// restore (the scene is going).
        void Abandon(foundation::scene::Scene& scene)
        {
            if (!m_running)
                return;
            scene.Stop();
            m_snapshot = nullptr;
            m_running = false;
            m_paused = false;
            m_run.Stop();
        }

    private:
        SimulationRun m_run;
        UniquePtr<foundation::scene::SceneSnapshot> m_snapshot;
        bool m_running = false;
        bool m_paused = false;
    };
}
