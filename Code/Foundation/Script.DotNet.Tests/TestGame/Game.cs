// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The Game tier as a game writes it in C#: one per run, made with no arguments, launched, updated
// and exited by name; its on<Event> handlers hear the run's bus.

using Engine;

namespace TestGame;

public class Game
{
    int m_updates;

    public void launch() => Log.info("C# game launched");

    public void update(float dt)
    {
        m_updates += 1;
        if (m_updates == 2)
            Log.info("C# game updated twice");
    }

    public void onGoalReached(int x) => Log.info($"C# game heard GoalReached {x}");

    public void exit() => Log.info("C# game exited");
}
