using Engine;

// Something lying in the yard, turning slowly: a weapon, which walked over gives the player the
// weapon (or more rounds for it), or a health pack (`heal`), which heals the player when they are
// hurt. Taken, it comes back after `respawnSeconds`.
public class Pickup
{
    readonly Entity m_entity;
    Float3 m_home;
    float m_time;
    float m_gone = -1.0f;

    [Tooltip("The weapon it gives: 0 pistol, 1 SMG, 2 shotgun")]
    public int weapon = 1;

    [Tooltip("Health it gives instead of a weapon; 0 for a weapon")]
    public float heal = 0.0f;

    [Tooltip("How near the player must come, metres")]
    public float reach = 1.4f;

    [Tooltip("Seconds before it is back once taken")]
    public float respawnSeconds = 25.0f;

    public Pickup(Entity entity) { m_entity = entity; }

    public void onStart() => m_home = m_entity.position();

    public void onUpdate(float dt)
    {
        m_time += dt;
        if (m_gone >= 0.0f)
        {
            m_gone += dt;
            if (m_gone >= respawnSeconds)
            {
                m_gone = -1.0f;
                Show(true);
            }
            return;
        }
        m_entity.setRotation(Aim.Yaw(m_time * 90.0f));
        m_entity.setPosition(m_home.x, m_home.y + 0.1f + 0.08f * System.MathF.Sin(m_time * 2.5f), m_home.z);
        Entity player = Combat.Player;
        if (!player.isValid() || Weapon.Player == null)
            return;
        Float3 at = player.worldPosition();
        Float3 here = m_entity.worldPosition();
        float dx = at.x - here.x, dz = at.z - here.z;
        if (dx * dx + dz * dz > reach * reach)
            return;
        if (heal > 0.0f)
        {
            // A health pack waits for a player who needs it.
            if (Combat.Find(player) is not Player hurt || !hurt.Heal(heal))
                return;
            Sounds.Play(Sounds.PickupHealth, 0.8f);
        }
        else
        {
            Weapon.Player.Give((WeaponKind)System.Math.Clamp(weapon, 0, 2));
            Sounds.Play(Sounds.PickupWeapon, 0.8f);
        }
        m_gone = 0.0f;
        Show(false);
    }

    void Show(bool shown)
    {
        for (Entity child = m_entity.firstChild(); child.isValid(); child = child.nextSibling())
            child.setActive(shown);
    }
}
