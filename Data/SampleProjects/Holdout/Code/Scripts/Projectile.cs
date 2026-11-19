using Engine;

// A gunner's shot: a glowing round that flies straight along the way it was spawned facing, slow
// enough to dodge. It hurts the player when it passes within `radius` of the body's middle, and ends
// there, on anything solid in its path, or after `lifetime` seconds.
public class Projectile
{
    readonly Entity m_entity;
    float m_age;

    [Tooltip("Metres a second")]
    public float speed = 14.0f;

    [Tooltip("Damage when it hits the player")]
    public float damage = 10.0f;

    [Tooltip("How near the player's middle counts as a hit, metres")]
    public float radius = 0.6f;

    [Tooltip("Seconds before it is gone")]
    public float lifetime = 3.0f;

    public Projectile(Entity entity) { m_entity = entity; }

    public void onUpdate(float dt)
    {
        m_age += dt;
        if (m_age > lifetime)
        {
            m_entity.destroy();
            return;
        }
        Float3 here = m_entity.worldPosition();
        Float3 way = Aim.Forward(m_entity.worldRotation());
        float step = speed * dt;
        Float3 next = Aim.Add(here, Aim.Scale(way, step));

        // The player: the closest point of this step to the body's middle.
        Entity player = Combat.Player;
        if (player.isValid())
        {
            Float3 body = player.worldPosition();
            Float3 toBody = new Float3 { x = body.x - here.x, y = body.y - here.y, z = body.z - here.z };
            float along = System.Math.Clamp(Float3.Dot(toBody, way), 0.0f, step);
            Float3 nearest = Aim.Add(here, Aim.Scale(way, along));
            if (Float3.Distance(nearest, body) < radius)
            {
                if (Combat.Find(player) is IDamageable hurt)
                    hurt.OnDamage(nearest, damage);
                m_entity.destroy();
                return;
            }
        }

        // Anything solid in the way ends it (the player's body is not a physics body here).
        RayCastHit hit = m_entity.scene.physics.rayCast(here, way, step, 0xFFFFFFFFu, m_entity);
        if (hit.hit)
        {
            m_entity.destroy();
            return;
        }
        m_entity.setPosition(next);
    }
}
