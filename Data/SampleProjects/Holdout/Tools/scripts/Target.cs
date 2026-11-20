using Engine;

// A practice target: flashes red when shot, and counts its hits in the log.
public class Target : EntityBehavior, IDamageable
{
    readonly Entity m_entity;
    Entity m_model;
    float m_flash;
    int m_hits;

    public Target(Entity entity) { m_entity = entity; }

    public void onStart()
    {
        m_model = m_entity.firstChild();
        Combat.Register(m_entity, this);
    }

    public void onDestroy() => Combat.Unregister(m_entity);

    public void OnDamage(Float3 at, float amount)
    {
        m_hits += 1;
        m_flash = 0.15f;
        Log.info($"C# Target: {m_entity.name()} hit {m_hits} times");
    }

    public void onUpdate(float dt)
    {
        if (m_flash <= 0.0f)
            return;
        m_flash -= dt;
        Tint(m_flash > 0.0f ? new Color { r = 1.0f, g = 0.25f, b = 0.2f, a = 1.0f } : new Color { r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f });
    }

    // Every mesh of the target's model, tinted.
    void Tint(Color colour)
    {
        for (Entity child = m_model; child.isValid(); child = child.nextSibling())
            TintTree(child, colour);
    }

    static void TintTree(Entity entity, Color colour)
    {
        MeshComponent mesh = MeshComponent.of(entity);
        if (mesh.isValid())
            mesh.color = colour;
        for (Entity child = entity.firstChild(); child.isValid(); child = child.nextSibling())
            TintTree(child, colour);
    }
}
