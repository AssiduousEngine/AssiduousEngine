using Engine;

// An enemy. A brawler runs the navigation mesh to the player and punches once in reach. A gunner
// (`gunner`) keeps its distance instead: it closes in until it stands between `keepNear` and
// `keepFar` of the player with a clear line to it, then stops and fires slow shots the player can
// dodge or hide from (Projectile). Either flinches when hit and falls when its health is gone, then
// is removed. Its model is one of the kit's characters (`model`: the enemy, or the hazmat suit for
// the heavy one), which carries every hand weapon; it shows only `weapon`, or none.
public class Enemy : EntityBehavior, IDamageable
{
    // Each model's clips: its own rig, so its own ids. Run, Idle, Punch, HitReact, Death, Idle_Shoot,
    // Run_Gun, in that order.
    static readonly string[][] Clips =
    {
        new[]
        {
            "{{Clip:Character_Enemy/Run}}", "{{Clip:Character_Enemy/Idle}}", "{{Clip:Character_Enemy/Punch}}",
            "{{Clip:Character_Enemy/HitReact}}", "{{Clip:Character_Enemy/Death}}", "{{Clip:Character_Enemy/Idle_Shoot}}",
            "{{Clip:Character_Enemy/Run_Gun}}",
        },
        new[]
        {
            "{{Clip:Character_Hazmat/Run}}", "{{Clip:Character_Hazmat/Idle}}", "{{Clip:Character_Hazmat/Punch}}",
            "{{Clip:Character_Hazmat/HitReact}}", "{{Clip:Character_Hazmat/Death}}", "{{Clip:Character_Hazmat/Idle_Shoot}}",
            "{{Clip:Character_Hazmat/Run_Gun}}",
        },
    };
    const int RunClip = 0, IdleClip = 1, PunchClip = 2, HitClip = 3, DeathClip = 4, ShootClip = 5, RunGunClip = 6;

    const string ShotPrefab = "{{Prefab:Shot}}";

    // The weapons the kit's characters hold, one mesh each under the right hand.
    static readonly string[] Weapons =
    {
        "AK", "GrenadeLauncher", "Knife_1", "Knife_2", "Pistol", "Revolver", "Revolver_Small",
        "RocketLauncher", "ShortCannon", "Shotgun", "Shovel", "SMG", "Sniper", "Sniper_2",
    };

    readonly Entity m_entity;
    NavAgentComponent m_agent;
    Entity m_animated;
    Entity m_gun;
    int m_clip = -1;
    float m_attackCooldown;
    float m_punchLands = -1.0f;
    float m_deadFor = -1.0f;
    float m_deathLength;
    float m_repath;
    Float3 m_pathEnd;

    [Tooltip("The model's clips: 0 the enemy, 1 the hazmat suit")]
    public int model = 0;

    [Tooltip("The hand weapon it shows, by its mesh's name; empty for none")]
    public string weapon = "";

    [Tooltip("Keeps its distance and shoots, rather than punching")]
    public bool gunner = false;

    [Tooltip("Health at the start")]
    public float health = 60.0f;

    [Tooltip("Running speed, metres a second")]
    public float speed = 3.6f;

    [Tooltip("How close it gets to punch, metres")]
    public float reach = 1.5f;

    [Tooltip("Damage a punch does")]
    public float punchDamage = 12.0f;

    [Tooltip("Seconds between punches, or between a gunner's shots")]
    public float punchInterval = 1.3f;

    [Tooltip("A gunner comes no nearer than this, metres")]
    public float keepNear = 8.0f;

    [Tooltip("A gunner closes in when further than this, metres")]
    public float keepFar = 14.0f;

    public Enemy(Entity entity) { m_entity = entity; }

    bool Dead => m_deadFor >= 0.0f;

    public void onStart()
    {
        // Later waves come faster and take more hits.
        health *= Game.Difficulty;
        speed *= System.MathF.Min(1.5f, Game.Difficulty);
        model = System.Math.Clamp(model, 0, Clips.Length - 1);
        m_agent = NavAgentComponent.of(m_entity);
        m_animated = FindAnimated(m_entity);
        ShowWeapon(m_entity);
        Combat.Register(m_entity, this);
        Play(IdleClip);
    }

    public void onDestroy() => Combat.Unregister(m_entity);

    public void onUpdate(float dt)
    {
        if (Dead)
        {
            // The fall plays once and its last pose holds, then the body goes.
            float before = m_deadFor;
            m_deadFor += dt;
            if (before < m_deathLength - 0.05f && m_deadFor >= m_deathLength - 0.05f && m_animated.isValid())
                m_entity.scene.animation.pause(m_animated);
            if (m_deadFor > m_deathLength + 3.0f)
                m_entity.destroy();
            return;
        }
        m_attackCooldown -= dt;
        Entity player = Combat.Player;
        if (!player.isValid() || !m_agent.isValid())
            return;
        Float3 here = m_entity.worldPosition();
        Float3 target = player.worldPosition();
        float dx = target.x - here.x, dz = target.z - here.z;
        float distance = System.MathF.Sqrt(dx * dx + dz * dz);
        if (gunner)
            UpdateGunner(dt, here, target, dx, dz, distance);
        else
            UpdateBrawler(dt, here, target, dx, dz, distance, player);
    }

    void UpdateBrawler(float dt, Float3 here, Float3 target, float dx, float dz, float distance, Entity player)
    {
        if (m_punchLands >= 0.0f)
        {
            // The punch connects partway through its swing, if the player is still in reach.
            m_punchLands -= dt;
            if (m_punchLands < 0.0f && distance <= reach + 0.4f && Combat.Find(player) is IDamageable hurt)
                hurt.OnDamage(target, punchDamage);
            Face(dx, dz);
            return;
        }
        if (distance > reach)
        {
            Chase(dt, here, target, reach * 0.8f);
            Play(RunClip);
            return;
        }
        Halt(dx, dz);
        Play(IdleClip);
        if (m_attackCooldown <= 0.0f)
        {
            m_attackCooldown = punchInterval;
            m_punchLands = 0.35f;
            OneShot(PunchClip);
        }
    }

    void UpdateGunner(float dt, Float3 here, Float3 target, float dx, float dz, float distance)
    {
        bool seen = CanSee(here, target);
        if (distance > keepFar || (!seen && distance > 3.0f))
        {
            // Too far, or no clear line: closer, gun in hand.
            Chase(dt, here, target, keepNear);
            Play(RunGunClip);
            return;
        }
        Halt(dx, dz);
        Play(ShootClip);
        if (distance < keepNear * 0.6f)
            return; // too close to shoot well: it waits for the player to come apart
        if (m_attackCooldown <= 0.0f)
        {
            m_attackCooldown = punchInterval;
            Shoot(target);
        }
    }

    // A shot from the gun's muzzle (its hand when it shows none) at the player's chest.
    void Shoot(Float3 target)
    {
        Float3 from = m_gun.isValid() ? m_gun.worldPosition() : Aim.Add(m_entity.worldPosition(), new Float3 { x = 0.0f, y = 1.3f, z = 0.0f });
        Float3 chest = new Float3 { x = target.x, y = target.y + 0.3f, z = target.z };
        Float3 way = new Float3 { x = chest.x - from.x, y = chest.y - from.y, z = chest.z - from.z };
        Engine.Guid shot = Engine.Guid.Parse(ShotPrefab);
        if (!shot.IsNil())
            m_entity.scene.prefabs.spawn(shot, from, Aim.Facing(way));
        Sounds.PlayAt(Sounds.EnemyShot, from);
    }

    // A path toward the player twice a second, or sooner when the player has moved off the last
    // one's end, stopping `arrive` short of it.
    void Chase(float dt, Float3 here, Float3 target, float arrive)
    {
        m_repath -= dt;
        float mx = target.x - m_pathEnd.x, mz = target.z - m_pathEnd.z;
        if (m_repath <= 0.0f || mx * mx + mz * mz > 1.0f)
        {
            // The goal on the ground: the player's position is its body's middle, 0.9 m up, and a
            // goal that high finds no path.
            m_agent.navigateAt(target.x, here.y, target.z, speed, arrive);
            m_pathEnd = target;
            m_repath = 0.5f;
        }
        float vx = m_agent.velocityX(), vz = m_agent.velocityZ();
        if (vx * vx + vz * vz > 0.04f)
            Face(vx, vz);
    }

    void Halt(float dx, float dz)
    {
        m_agent.stop();
        m_repath = 0.0f;
        Face(dx, dz);
    }

    // Whether nothing solid stands between its eyes and the player's.
    bool CanSee(Float3 here, Float3 target)
    {
        Float3 eye = new Float3 { x = here.x, y = here.y + 1.5f, z = here.z };
        Float3 to = new Float3 { x = target.x - eye.x, y = target.y + 0.6f - eye.y, z = target.z - eye.z };
        float length = Float3.Length(to);
        if (length < 0.01f)
            return true;
        RayCastHit hit = m_entity.scene.physics.rayCast(eye, Aim.Scale(to, 1.0f / length), length, 0xFFFFFFFFu, m_entity.findChildByName("Body"));
        return !hit.hit || Combat.Find(hit.entity()) == Combat.Find(Combat.Player);
    }

    public void OnDamage(Float3 at, float amount)
    {
        if (Dead)
            return;
        health -= amount;
        if (health > 0.0f)
        {
            OneShot(HitClip);
            return;
        }
        // Down: it stops, falls, and no longer counts as a target.
        m_deadFor = 0.0f;
        if (m_agent.isValid())
            m_agent.stop();
        Combat.Unregister(m_entity);
        RigidBodyComponent.removeFrom(m_entity.findChildByName("Body"));
        Sounds.PlayAt(Sounds.EnemyDown, m_entity.worldPosition());
        if (m_animated.isValid())
        {
            Engine.Guid death = Engine.Guid.Parse(Clips[model][DeathClip]);
            m_entity.scene.animation.stopOneShot(m_animated);
            m_entity.scene.animation.setClip(m_animated, death, 0.1f);
            m_deathLength = m_entity.scene.animation.clipLength(death);
        }
        Game.EnemyDown();
    }

    // Faces along (x, z): the model looks down -Z, as the camera does.
    void Face(float x, float z)
    {
        if (x * x + z * z > 0.0001f)
            m_entity.setRotation(Aim.Yaw(System.MathF.Atan2(-x, -z) / Aim.Radians));
    }

    void Play(int clip)
    {
        if (clip == m_clip || !m_animated.isValid())
            return;
        m_clip = clip;
        m_entity.scene.animation.setClip(m_animated, Engine.Guid.Parse(Clips[model][clip]), 0.2f);
    }

    void OneShot(int clip)
    {
        if (m_animated.isValid())
            m_entity.scene.animation.playOneShot(m_animated, Engine.Guid.Parse(Clips[model][clip]));
    }

    // The entity whose skeletal animation drives the model: the first one under the root that has it.
    static Entity FindAnimated(Entity entity)
    {
        if (SkeletalAnimationComponent.of(entity).isValid())
            return entity;
        for (Entity child = entity.firstChild(); child.isValid(); child = child.nextSibling())
        {
            Entity found = FindAnimated(child);
            if (found.isValid())
                return found;
        }
        return default;
    }

    // Shows the weapon it carries and hides the rest; remembers the shown one as its muzzle.
    void ShowWeapon(Entity entity)
    {
        for (Entity child = entity.firstChild(); child.isValid(); child = child.nextSibling())
        {
            string name = child.name();
            MeshComponent mesh = MeshComponent.of(child);
            if (System.Array.IndexOf(Weapons, name) >= 0 && mesh.isValid())
            {
                bool shown = name == weapon;
                mesh.visible = shown;
                if (shown)
                    m_gun = child;
            }
            ShowWeapon(child);
        }
    }
}
