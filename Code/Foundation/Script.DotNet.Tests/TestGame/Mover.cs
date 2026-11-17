// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// A behaviour as a game writes one: constructed with its entity, called by handler name, its public
// fields its inspector properties.

using System;
using Engine;

namespace TestGame;

public class Mover
{
    readonly Entity m_entity;

    public float speed = 2.5f;
    public int updates;
    public float travelled;
    public string label = "mover";
    [Tooltip("where it heads")] public Float3 heading = new Float3 { x = 1, y = 2, z = 3 };
    [Replicated] public bool active = true;

    // How many movers this copy of the game has made: a static, so it is per loaded copy.
    static int s_made;

    public Mover(Entity entity)
    {
        m_entity = entity;
        s_made += 1;
    }

    public int madeSoFar() => s_made;

    public void onUpdate(float dt)
    {
        updates += 1;
#if REBUILT
        travelled += 2.0f * speed * dt; // the rebuilt game (TestGameRebuilt) goes twice as far
#else
        travelled += speed * dt;
#endif
    }

    public string greet(string name) => $"hello {name} from {label}";

    public Float3 scaled(float k) => new Float3 { x = heading.x * k, y = heading.y * k, z = heading.z * k };

    // A handler given to the engine, which calls it back: a number triples, and the call is counted.
    public int handled;
    public ScriptHandler tripler() => args =>
    {
        handled += 1;
        return (float)args[0]! * 3.0f;
    };

    // An asset named by its id's text, as a script names a clip or a prefab, and that text written back.
    public Engine.Guid asset(string text) => Engine.Guid.Parse(text);
    public string assetText(string text) => Engine.Guid.Parse(text).ToString();

    public bool hasEntity() => m_entity.isValid();

    public void fail() => throw new InvalidOperationException("failed on purpose");
}
