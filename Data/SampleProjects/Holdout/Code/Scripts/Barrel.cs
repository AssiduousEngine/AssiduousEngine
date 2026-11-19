using Engine;

// An exploding barrel: a few hits set it off. It blows apart, hurting everything that can be hurt
// near it (Combat), the more the nearer, other barrels included, so a row can go off together; its
// light flashes, pads rumble, and what is left is the kit's spilled barrel. Its children: Body (the
// collider shots hit), Model (the barrel), Spilled (shown once it has gone) and Flash (its light).
public class Barrel : IDamageable
{
    readonly Entity m_entity;
    Entity m_model;
    Entity m_spilled;
    Entity m_body;
    LightComponent m_flash;
    float m_flashLeft;
    bool m_gone;

    [Tooltip("Damage it takes to go off")]
    public float health = 30.0f;

    [Tooltip("How far the blast reaches, metres")]
    public float radius = 4.5f;

    [Tooltip("Damage at the middle of the blast, falling to none at its edge")]
    public float blastDamage = 90.0f;

    public Barrel(Entity entity) { m_entity = entity; }

    public void onStart()
    {
        m_model = m_entity.findChildByName("Model");
        m_spilled = m_entity.findChildByName("Spilled");
        m_body = m_entity.findChildByName("Body");
        Entity flash = m_entity.findChildByName("Flash");
        if (flash.isValid())
            m_flash = LightComponent.of(flash);
        if (m_spilled.isValid())
            m_spilled.setActive(false);
        Combat.Register(m_entity, this);
    }

    public void onDestroy() => Combat.Unregister(m_entity);

    public void OnDamage(Float3 at, float amount)
    {
        if (m_gone)
            return;
        health -= amount;
        if (health <= 0.0f)
            Explode();
    }

    void Explode()
    {
        m_gone = true;
        Combat.Unregister(m_entity);
        if (m_model.isValid())
            m_model.setActive(false);
        if (m_spilled.isValid())
            m_spilled.setActive(true);
        if (m_body.isValid())
            RigidBodyComponent.removeFrom(m_body);
        m_flashLeft = 0.25f;
        Input.rumble(0.9f, 0.6f, 0.4f);

        // Everything near, hurt by how near: the middle of the barrel, half a metre up.
        Float3 centre = Aim.Add(m_entity.worldPosition(), new Float3 { x = 0.0f, y = 0.5f, z = 0.0f });
        Sounds.PlayAt(Sounds.Explosion, centre);
        foreach ((Entity near, IDamageable target) in Combat.Near(centre, radius))
        {
            float distance = Float3.Distance(near.worldPosition(), centre);
            float share = 1.0f - System.Math.Clamp(distance / radius, 0.0f, 1.0f);
            target.OnDamage(centre, blastDamage * share);
        }
    }

    public void onUpdate(float dt)
    {
        if (m_flashLeft <= 0.0f || !m_flash.isValid())
            return;
        m_flashLeft -= dt;
        m_flash.intensity = m_flashLeft > 0.0f ? 120.0f * (m_flashLeft / 0.25f) : 0.0f;
    }
}
