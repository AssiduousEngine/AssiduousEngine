using Engine;

// Helpers the player, the weapons and the enemies share: angles, rotations and directions.
public static class Aim
{
    public const float Radians = System.MathF.PI / 180.0f;

    static readonly Float3 Up = new Float3 { x = 0.0f, y = 1.0f, z = 0.0f };
    static readonly Float3 Side = new Float3 { x = 1.0f, y = 0.0f, z = 0.0f };
    static readonly Float3 Ahead = new Float3 { x = 0.0f, y = 0.0f, z = -1.0f };

    // A turn of `degrees` about +Y (0 faces -Z, the way the camera looks).
    public static Quaternion Yaw(float degrees) => Quaternion.FromAxisAngle(Up, degrees * Radians);

    // A tilt of `degrees` about +X (positive looks up).
    public static Quaternion Pitch(float degrees) => Quaternion.FromAxisAngle(Side, degrees * Radians);

    // The yaw, in degrees, of a rotation about +Y.
    public static float YawOf(Quaternion q) => 2.0f * System.MathF.Atan2(q.y, q.w) / Radians;

    // The way `q` faces: its -Z.
    public static Float3 Forward(Quaternion q) => Quaternion.RotateVector(q, Ahead);

    // The rotation that faces along `way` (turned about +Y, then tilted up or down).
    public static Quaternion Facing(Float3 way)
    {
        float flat = System.MathF.Sqrt(way.x * way.x + way.z * way.z);
        float yaw = System.MathF.Atan2(-way.x, -way.z) / Radians;
        float pitch = System.MathF.Atan2(way.y, flat) / Radians;
        return Quaternion.Mul(Yaw(yaw), Pitch(pitch));
    }

    // `current` moved toward `target` by at most `step`.
    public static float Approach(float current, float target, float step)
    {
        if (current < target)
            return System.MathF.Min(current + step, target);
        return System.MathF.Max(current - step, target);
    }

    public static Float3 Add(Float3 a, Float3 b) => new Float3 { x = a.x + b.x, y = a.y + b.y, z = a.z + b.z };

    public static Float3 Scale(Float3 v, float s) => new Float3 { x = v.x * s, y = v.y * s, z = v.z * s };
}
