// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A run behavior as a game writes it in C#: one per run, made with no arguments, started, updated
// and stopped by name; its on<Event> handlers hear the run's bus.

using Engine;

namespace TestGame;

public class Game : RunBehavior
{
    int m_updates;

    public void onStart() => Log.info("C# game started");

    public void onUpdate(float dt)
    {
        m_updates += 1;
        if (m_updates == 2)
            Log.info("C# game updated twice");
    }

    public void onGoalReached(int x) => Log.info($"C# game heard GoalReached {x}");

    public void onStop() => Log.info("C# game stopped");
}
