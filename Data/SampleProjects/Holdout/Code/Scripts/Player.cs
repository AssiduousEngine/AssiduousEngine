using Engine;

// The player: first-person look and movement on a character controller. Look turns the body (yaw)
// and tilts the eye, its child (pitch); movement steers the body's velocity toward where the stick
// or the keys point, quick on the ground and loose in the air, with a jump that forgives a late
// press and a step off a ledge. It acts only while the game is being played (Game.Playing: not
// behind a menu, paused included), which locks the pointer for mouse look.
public class Player : EntityBehavior, IDamageable
{
    readonly Entity m_entity;
    Entity m_eye;
    CharacterComponent m_body;
    float m_yaw;
    float m_pitch;
    float m_velocityX;
    float m_velocityZ;
    float m_sinceGrounded = 1000.0f;
    float m_sinceJumpPressed = 1000.0f;

    [Tooltip("Health at the start")]
    public float health = 100.0f;

    [Tooltip("Walking speed, metres a second")]
    public float walkSpeed = 4.5f;

    [Tooltip("Sprinting speed, metres a second")]
    public float sprintSpeed = 7.5f;

    [Tooltip("How quickly the body reaches its speed on the ground, metres a second each second")]
    public float acceleration = 45.0f;

    [Tooltip("How quickly it steers in the air")]
    public float airAcceleration = 10.0f;

    [Tooltip("Take-off speed of a jump, metres a second")]
    public float jumpSpeed = 5.2f;

    [Tooltip("Mouse look, degrees a pixel")]
    public float mouseDegrees = 0.12f;

    [Tooltip("Stick look at full tilt, degrees a second")]
    public float stickDegrees = 160.0f;

    public Player(Entity entity) { m_entity = entity; }

    public void onStart()
    {
        m_body = CharacterComponent.of(m_entity);
        m_eye = m_entity.findChildByName("Eye");
        m_yaw = Aim.YawOf(m_entity.rotation());
        Combat.Player = m_entity;
        Combat.Register(m_entity, this);
    }

    public void onDestroy()
    {
        Combat.Unregister(m_entity);
        if (Entity.Equals(Combat.Player, m_entity))
            Combat.Player = default;
    }

    public bool Alive => health > 0.0f;

    [Tooltip("The most health the player can have")]
    public float maxHealth = 100.0f;

    /// <summary>Heals by `amount`, up to the most; false when there was nothing to heal.</summary>
    public bool Heal(float amount)
    {
        if (!Alive || health >= maxHealth)
            return false;
        health = System.MathF.Min(maxHealth, health + amount);
        Game.ShowHealth(health);
        return true;
    }

    public void OnDamage(Float3 at, float amount)
    {
        if (!Alive)
            return;
        health = System.MathF.Max(0.0f, health - amount);
        Sounds.Play(Sounds.Hurt, 0.8f);
        Input.rumble(0.6f, 0.3f, 0.2f);
        Game.ShowDamage(health);
    }

    public void onUpdate(float dt)
    {
        if (!m_body.isValid() || dt <= 0.0f)
            return;
        // Behind the game's menus the player does nothing; the game holds the pointer then.
        if (!Game.Playing)
        {
            m_body.move(0.0f, 0.0f);
            return;
        }
        if (!Alive)
        {
            m_body.move(0.0f, 0.0f);
            return;
        }
        Look(dt);
        Move(dt);
    }

    void Look(float dt)
    {
        Float2 mouse = Input.value2D("Look");
        Float2 drag = Input.value2D("TouchLook"); // a drag on a touch screen, in the mouse's units
        mouse = new Float2 { x = mouse.x + drag.x, y = mouse.y + drag.y };
        Float2 stick = Input.value2D("LookStick");
        m_yaw -= mouse.x * mouseDegrees + stick.x * stickDegrees * dt;
        m_pitch -= mouse.y * mouseDegrees - stick.y * stickDegrees * dt;
        m_pitch = System.Math.Clamp(m_pitch, -85.0f, 85.0f);
        m_entity.setRotation(Aim.Yaw(m_yaw));
        if (m_eye.isValid())
            m_eye.setRotation(Aim.Pitch(m_pitch));
    }

    void Move(float dt)
    {
        bool grounded = m_body.grounded();
        m_sinceGrounded = grounded ? 0.0f : m_sinceGrounded + dt;
        m_sinceJumpPressed = Input.wasPressed("Jump") ? 0.0f : m_sinceJumpPressed + dt;

        // The stick's forward is the way the body faces; right is a quarter turn clockwise.
        Float2 stick = Input.value2D("Move");
        float speed = Input.isDown("Sprint") && stick.y > 0.3f ? sprintSpeed : walkSpeed;
        float radians = m_yaw * Aim.Radians;
        float forwardX = -System.MathF.Sin(radians), forwardZ = -System.MathF.Cos(radians);
        float rightX = -forwardZ, rightZ = forwardX;
        float wantedX = (forwardX * stick.y + rightX * stick.x) * speed;
        float wantedZ = (forwardZ * stick.y + rightZ * stick.x) * speed;
        float rate = (grounded ? acceleration : airAcceleration) * dt;
        m_velocityX = Aim.Approach(m_velocityX, wantedX, rate);
        m_velocityZ = Aim.Approach(m_velocityZ, wantedZ, rate);
        m_body.move(m_velocityX, m_velocityZ);

        // Jump: a press a moment before landing counts, and so does one just after a ledge.
        if (m_sinceJumpPressed <= 0.15f && m_sinceGrounded <= 0.12f)
        {
            m_body.jump(jumpSpeed);
            m_sinceJumpPressed = 1000.0f;
            m_sinceGrounded = 1000.0f;
        }
    }
}
