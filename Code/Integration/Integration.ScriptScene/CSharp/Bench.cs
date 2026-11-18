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

    // The others' math: Float3's operators and Dot are C#'s own (pure functions the bindings
    // compute in C#); Color has no constructor in the bindings, so it is written by its fields.
    public double math(int n)
    {
        Float3 a = new Float3 { x = 1, y = 2, z = 3 };
        Float3 b = new Float3 { x = 0.5f, y = 0.25f, z = 0.125f };
        double sum = 0;
        for (int i = 0; i < n; i++)
        {
            Float3 d = a + b * 2;
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

    public double checks(int n, Entity e)
    {
        double valid = 0;
        for (int i = 0; i < n; i++)
            if (e.isValid())
                valid += 1;
        return valid;
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
            PathFollowComponent follow = PathFollowComponent.of(m_entity);
        }
    }
}

// A component held as a value: a property set through a fetched handle reaches the engine, isValid
// says whether the entity still has it, and a set on one gone is refused (the check reads the name).
public class Pacer
{
    readonly Entity m_entity;

    public Pacer(Entity entity) { m_entity = entity; }

    public void onUpdate(float dt)
    {
        if (PathFollowComponent.of(m_entity).isValid())
        {
            PathFollowComponent.of(m_entity).speed = 4.0f;
            m_entity.setName("has it");
            return;
        }
        // Gone: a set through the handle is refused, not written anywhere.
        try
        {
            PathFollowComponent.of(m_entity).speed = 4.0f;
            m_entity.setName("set a gone component");
        }
        catch (EngineException)
        {
            m_entity.setName("has none");
        }
    }
}

// Text arguments on both paths: one that surely fits the call's stack storage (85 three-byte
// characters, its 255 bytes the most it holds) and one past it, in native memory; each read back.
public class Namer
{
    readonly Entity m_entity;

    public Namer(Entity entity) { m_entity = entity; }

    bool Kept(string name)
    {
        m_entity.setName(name);
        return m_entity.name() == name;
    }

    public void onUpdate(float dt)
    {
        bool kept = Kept(new string('\u20ac', 85)) && Kept(new string('\u00e9', 100) + "end") && Kept("");
        m_entity.setName(kept ? "kept \u00e9" : "lost");
    }
}
