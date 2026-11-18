// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script speed measures in C#: the same work the other languages' benchmark classes do
// (Integration.ScriptScene). Bench takes the per-call measures; Empty, Mover and Fetcher are the
// scene measures' behaviours.

using Engine;

namespace Bench;

public class Bench
{
    double m_total;

    public void tick(double dt) { m_total += dt; }

    // The others' math, as C# writes it today: the bindings give Float3 no operators and Color no
    // constructor, so the sum, the scaling and the Color are written field by field; Float3.Dot
    // crosses into the engine.
    public double math(int n)
    {
        Float3 a = new Float3 { x = 1, y = 2, z = 3 };
        Float3 b = new Float3 { x = 0.5f, y = 0.25f, z = 0.125f };
        double sum = 0;
        for (int i = 0; i < n; i++)
        {
            Float3 d = new Float3 { x = a.x + b.x * 2, y = a.y + b.y * 2, z = a.z + b.z * 2 };
            Color c = new Color { r = d.x, g = d.y, b = d.z, a = 1 };
            sum += Float3.Dot(d, a) + c.r;
        }
        return sum;
    }

    public double loops(int n)
    {
        double sum = 0;
        for (int i = 0; i < n; i++)
            sum += i;
        return sum;
    }

    public double calls(int n)
    {
        double sum = 0;
        for (int i = 0; i < n; i++)
            sum += Engine.Math.Abs((float)i);
        return sum;
    }

    public double crosses(int n)
    {
        Float3 a = new Float3 { x = 1, y = 2, z = 3 };
        double sum = 0;
        for (int i = 0; i < n; i++)
            sum += Float3.Cross(a, new Float3 { x = i, y = 0.5f, z = 0.25f }).x;
        return sum;
    }

    public double texts(int n, string s, Entity e)
    {
        for (int i = 0; i < n; i++)
            e.setName(s);
        return n;
    }
}

public class Empty
{
    public Empty(Entity entity) { }
    public void onUpdate(float dt) { }
}

public class Mover
{
    readonly Entity m_entity;
    public float speed = 2.0f;

    public Mover(Entity entity) { m_entity = entity; }

    public void onUpdate(float dt)
    {
        Float3 p = m_entity.position();
        m_entity.setPosition(p.x + speed * dt, p.y, p.z);
    }
}

public class Fetcher
{
    readonly Entity m_entity;

    public Fetcher(Entity entity) { m_entity = entity; }

    public void onUpdate(float dt)
    {
        for (int i = 0; i < 1000; i++)
        {
            PathFollowComponent? follow = PathFollowComponent.of(m_entity);
        }
    }
}
