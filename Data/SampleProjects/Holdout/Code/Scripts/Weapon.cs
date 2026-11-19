using Engine;

// The weapons in hand (Arsenal): hitscan from the eye along where it looks, each pellet strayed a
// little within its weapon's spread, held down for automatic fire at the weapon's rate. A shot
// flashes the muzzle light, kicks the gun model back, pushes what it hits (a loose crate) and
// damages it if it can be hurt (Combat). A magazine runs out; R (or the pad's West) reloads, as an
// empty one does by itself, the gun dipping meanwhile. 1 to 3 (or the pad's North for the next)
// choose among the weapons carried; the pistol is always carried and never runs out of spare
// rounds, the others are found in the yard (Pickup). The HUD shows the weapon and its rounds.
public class Weapon
{
    sealed class Carried
    {
        public bool Has;
        public int InMagazine;
        public int Spare; // -1: never runs out
    }

    readonly Entity m_entity;
    readonly Carried[] m_carried = { new(), new(), new() };
    readonly System.Random m_random = new();
    Entity m_eye;
    Entity m_gun;
    readonly Entity[] m_models = new Entity[3];
    LightComponent m_flash;
    Float3 m_gunRest;
    WeaponKind m_current = WeaponKind.Pistol;
    float m_cooldown;
    float m_flashLeft;
    float m_kick;
    float m_reloading = -1.0f; // seconds left of a reload; negative: not reloading
    bool m_waitForRelease = true; // the click that started play (or took the pointer) fires nothing

    static Weapon? s_current;

    [Tooltip("How far a shot reaches, metres")]
    public float range = 80.0f;

    [Tooltip("How hard a shot pushes a loose body")]
    public float push = 6.0f;

    public Weapon(Entity entity) { m_entity = entity; }

    WeaponStats Stats => Arsenal.Of(m_current);
    Carried Current => m_carried[(int)m_current];

    public void onStart()
    {
        s_current = this;
        m_eye = m_entity.findChildByName("Eye");
        if (!m_eye.isValid())
        {
            Log.warn("C# Weapon: the player has no Eye to shoot from");
            return;
        }
        m_gun = m_eye.findChildByName("Gun");
        if (m_gun.isValid())
        {
            m_gunRest = m_gun.position();
            foreach (WeaponKind kind in new[] { WeaponKind.Pistol, WeaponKind.SMG, WeaponKind.Shotgun })
                m_models[(int)kind] = m_gun.findChildByName(Arsenal.Of(kind).Name);
        }
        Entity muzzle = m_eye.findChildByName("Muzzle");
        if (muzzle.isValid())
            m_flash = LightComponent.of(muzzle);
        Give(WeaponKind.Pistol);
        Choose(WeaponKind.Pistol);
    }

    public void onDestroy()
    {
        if (s_current == this)
            s_current = null;
    }

    /// <summary>The player's weapons: a pickup gives them a weapon, or rounds for one carried.</summary>
    public static Weapon? Player => s_current;

    /// <summary>Gives `kind`: a full magazine and its spare rounds the first time, more spare rounds
    /// after. Returns whether it was new.</summary>
    public bool Give(WeaponKind kind)
    {
        WeaponStats stats = Arsenal.Of(kind);
        Carried carried = m_carried[(int)kind];
        bool first = !carried.Has;
        if (first)
        {
            carried.Has = true;
            carried.InMagazine = stats.Magazine;
            carried.Spare = stats.Carried;
        }
        else if (carried.Spare >= 0)
        {
            carried.Spare += stats.Carried;
        }
        if (kind == m_current)
            ShowAmmo();
        return first;
    }

    public void onUpdate(float dt)
    {
        m_cooldown -= dt;
        Recover(dt);
        // A phone has no pointer to lock: touch fires without it.
        bool touched = Input.isDown("TouchFire");
        bool held = Input.isDown("Fire") || Input.value("Trigger") > 0.5f || touched;
        if (!m_eye.isValid() || !Game.Playing || (!Input.pointerLocked() && !touched))
        {
            m_waitForRelease = true;
            return;
        }
        // A press that began behind a menu, or that took the pointer back, is not a shot.
        if (m_waitForRelease)
        {
            if (held)
                return;
            m_waitForRelease = false;
        }
        ChooseByInput();
        if (m_reloading >= 0.0f)
        {
            m_reloading -= dt;
            if (m_reloading < 0.0f)
                FinishReload();
            return;
        }
        if (Input.wasPressed("Reload"))
        {
            StartReload();
            return;
        }
        if (!held || m_cooldown > 0.0f)
            return;
        if (Current.InMagazine <= 0)
        {
            StartReload();
            return;
        }
        m_cooldown = 1.0f / Stats.RoundsPerSecond;
        Fire();
    }

    void ChooseByInput()
    {
        if (Input.wasPressed("Weapon1"))
            Choose(WeaponKind.Pistol);
        else if (Input.wasPressed("Weapon2"))
            Choose(WeaponKind.SMG);
        else if (Input.wasPressed("Weapon3"))
            Choose(WeaponKind.Shotgun);
        else if (Input.wasPressed("NextWeapon"))
        {
            for (int step = 1; step <= 3; ++step)
            {
                WeaponKind next = (WeaponKind)(((int)m_current + step) % 3);
                if (m_carried[(int)next].Has)
                {
                    Choose(next);
                    break;
                }
            }
        }
    }

    void Choose(WeaponKind kind)
    {
        if (!m_carried[(int)kind].Has)
            return;
        if (kind != m_current)
            Sounds.Play(Sounds.WeaponSwitch, 0.6f);
        m_current = kind;
        m_reloading = -1.0f;
        m_cooldown = 0.25f; // the moment it takes to bring it up
        for (int i = 0; i < m_models.Length; ++i)
            if (m_models[i].isValid())
                m_models[i].setActive(i == (int)kind);
        ShowAmmo();
    }

    void StartReload()
    {
        Carried carried = Current;
        if (carried.InMagazine >= Stats.Magazine || carried.Spare == 0)
            return;
        m_reloading = Stats.ReloadSeconds;
        Sounds.Play(Sounds.Reload, 0.7f);
        Game.ShowWeapon(Stats.Name, "Reloading");
    }

    void FinishReload()
    {
        Carried carried = Current;
        int wanted = Stats.Magazine - carried.InMagazine;
        int taken = carried.Spare < 0 ? wanted : System.Math.Min(wanted, carried.Spare);
        carried.InMagazine += taken;
        if (carried.Spare >= 0)
            carried.Spare -= taken;
        ShowAmmo();
    }

    void ShowAmmo()
    {
        Carried carried = Current;
        Game.ShowWeapon(Stats.Name, carried.Spare < 0 ? $"{carried.InMagazine}" : $"{carried.InMagazine} / {carried.Spare}");
    }

    void Fire()
    {
        WeaponStats stats = Stats;
        Current.InMagazine -= 1;
        Float3 from = m_eye.worldPosition();
        Quaternion aim = m_eye.worldRotation();
        bool landed = false;
        for (int pellet = 0; pellet < stats.Pellets; ++pellet)
        {
            // A pellet strays within the spread: a random turn and tilt off the aim.
            float yaw = (float)(m_random.NextDouble() * 2.0 - 1.0) * stats.SpreadDegrees;
            float tilt = (float)(m_random.NextDouble() * 2.0 - 1.0) * stats.SpreadDegrees;
            Float3 direction = Aim.Forward(Quaternion.Mul(aim, Quaternion.Mul(Aim.Yaw(yaw), Aim.Pitch(tilt))));
            RayCastHit hit = m_entity.scene.physics.rayCast(from, direction, range, 0xFFFFFFFFu, m_entity);
            if (!hit.hit)
                continue;
            Float3 shove = Aim.Scale(direction, push / stats.Pellets);
            hit.impulse(shove.x, shove.y, shove.z);
            if (Combat.Find(hit.entity()) is IDamageable target)
            {
                target.OnDamage(hit.position, stats.Damage);
                landed = true;
            }
        }
        Sounds.Play(stats.Sound, stats.Volume);
        if (landed)
            Game.ShowHit();
        m_flashLeft = 0.05f;
        m_kick = 1.0f;
        ShowAmmo();
    }

    // The flash goes out and the gun eases back to rest; while reloading it dips out of sight a little.
    void Recover(float dt)
    {
        m_flashLeft -= dt;
        if (m_flash.isValid())
            m_flash.intensity = m_flashLeft > 0.0f ? 30.0f : 0.0f;
        m_kick = System.MathF.Max(0.0f, m_kick - dt * 12.0f);
        if (!m_gun.isValid())
            return;
        float dip = 0.0f;
        if (m_reloading >= 0.0f)
        {
            float t = 1.0f - m_reloading / Stats.ReloadSeconds; // 0 to 1 through the reload
            dip = System.MathF.Sin(t * System.MathF.PI) * 0.18f;
        }
        m_gun.setPosition(m_gunRest.x, m_gunRest.y - dip, m_gunRest.z + m_kick * Stats.Kick);
    }
}
