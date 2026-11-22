// Player: the visitor to Millbrook. They walk where the stick points, relative to where the camera
// looks (up on the stick is away from the camera, whichever quarter it has turned to), and run while
// Run is held, turning to face where they go. The character controller keeps them on the ground and
// out of the walls; the model's clip follows: idle standing, walk, sprint. While a conversation
// plays they stand still (the game turns them to whoever they talk with: faceTo).
const float kPi = 3.14159265f;

class Player : EntityBehavior
{
    private Entity self;

    ["The camera whose facing makes forward"] Entity camera;
    [1.8, "Walking speed (m/s)"] float walkSpeed;
    [4.2, "Running speed (m/s)"] float runSpeed;
    [10.0, "How fast the body turns toward where it goes"] float turnRate;
    ["asset:AnimationClip", "Standing still"] Guid idleClip;
    ["asset:AnimationClip", "Walking"] Guid walkClip;
    ["asset:AnimationClip", "Running"] Guid runClip;

    private float m_yaw = 0.0f;
    private int m_gait = -1; // the clip shown: 0 idle, 1 walk, 2 run
    private Entity m_figure;

    Player(Entity entity) { self = entity; }

    void onStart()
    {
        m_yaw = 0.0f;
    }

    // The model under the player, whose animator plays the clips.
    private Entity figure()
    {
        if (!m_figure.isValid())
        {
            m_figure = self.findChildByName("character-male-a");
        }
        return m_figure;
    }

    private void gait(int which)
    {
        if (which == m_gait)
        {
            return;
        }
        Entity body = figure();
        if (!body.isValid())
        {
            return;
        }
        m_gait = which;
        Guid clip = which == 2 ? runClip : (which == 1 ? walkClip : idleClip);
        self.scene.animation.setClip(body, clip, 0.25f);
    }

    // Turns to face the point `at` on the ground (the game, as a conversation starts).
    void onFaceTo(Float3 at)
    {
        Float3 here = self.worldPosition();
        m_yaw = Math::Atan2(at.x - here.x, at.z - here.z);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
    }

    void onUpdate(double dt)
    {
        CharacterComponent@ body = CharacterComponent::of(self);
        Float2 stick = Input::value2D("Move");
        float amount = Math::Sqrt(stick.x * stick.x + stick.y * stick.y);
        if (Dialogue::isPlaying() || amount < 0.05f || !camera.isValid())
        {
            body.move(0.0f, 0.0f);
            gait(0);
            return;
        }
        if (amount > 1.0f)
        {
            stick = Float2(stick.x / amount, stick.y / amount);
        }
        // The camera's facing projected onto the ground: forward, and right of it.
        Float3 look = Quaternion::RotateVector(camera.rotation(), Float3(0.0f, 0.0f, -1.0f));
        float flat = Math::Sqrt(look.x * look.x + look.z * look.z);
        if (flat < 0.0001f)
        {
            return;
        }
        Float3 forward = Float3(look.x / flat, 0.0f, look.z / flat);
        Float3 right = Float3(-forward.z, 0.0f, forward.x);
        bool running = Input::isDown("Run");
        float speed = running ? runSpeed : walkSpeed;
        float vx = (forward.x * stick.y + right.x * stick.x) * speed;
        float vz = (forward.z * stick.y + right.z * stick.x) * speed;
        body.move(vx, vz);
        gait(running ? 2 : 1);

        // Face where they go (the model faces +Z), turning the short way round.
        float want = Math::Atan2(vx, vz);
        float delta = want - m_yaw;
        while (delta > kPi) delta -= 2.0f * kPi;
        while (delta < -kPi) delta += 2.0f * kPi;
        m_yaw += delta * Math::Clamp(turnRate * float(dt), 0.0f, 1.0f);
        self.setRotationEuler(0.0f, Math::RadiansToDegrees(m_yaw), 0.0f);
    }
}
