using Engine;

// The game's sounds (Tools/sounds.py: the gunshots made there, the rest from Kenney's packs), and
// playing them: the player's own on the effects bus as heard where they stand, another's (an
// enemy's, a barrel's) at its place in the yard, the menus' on the UI bus.
public static class Sounds
{
    public static readonly Engine.Guid PistolShot = Engine.Guid.Parse("{{Audio:PistolShot}}");
    public static readonly Engine.Guid SmgShot = Engine.Guid.Parse("{{Audio:SmgShot}}");
    public static readonly Engine.Guid ShotgunShot = Engine.Guid.Parse("{{Audio:ShotgunShot}}");
    public static readonly Engine.Guid Explosion = Engine.Guid.Parse("{{Audio:Explosion}}");
    public static readonly Engine.Guid Hit = Engine.Guid.Parse("{{Audio:Hit}}");
    public static readonly Engine.Guid Hurt = Engine.Guid.Parse("{{Audio:Hurt}}");
    public static readonly Engine.Guid EnemyDown = Engine.Guid.Parse("{{Audio:EnemyDown}}");
    public static readonly Engine.Guid EnemyShot = Engine.Guid.Parse("{{Audio:EnemyShot}}");
    public static readonly Engine.Guid Reload = Engine.Guid.Parse("{{Audio:Reload}}");
    public static readonly Engine.Guid WeaponSwitch = Engine.Guid.Parse("{{Audio:WeaponSwitch}}");
    public static readonly Engine.Guid PickupWeapon = Engine.Guid.Parse("{{Audio:PickupWeapon}}");
    public static readonly Engine.Guid PickupHealth = Engine.Guid.Parse("{{Audio:PickupHealth}}");
    public static readonly Engine.Guid Wave = Engine.Guid.Parse("{{Audio:Wave}}");
    public static readonly Engine.Guid Click = Engine.Guid.Parse("{{Audio:Click}}");
    public static readonly Engine.Guid Held = Engine.Guid.Parse("{{Audio:Held}}");
    public static readonly Engine.Guid Overrun = Engine.Guid.Parse("{{Audio:Overrun}}");

    static readonly System.Random s_random = new();

    /// <summary>One of the player's own, its pitch varied a little so a burst does not drone.</summary>
    public static void Play(Engine.Guid clip, float volume = 1.0f)
    {
        if (!clip.IsNil())
            Audio.playOneShot(clip, AudioBus.Effects, volume, 0.94f + 0.12f * (float)s_random.NextDouble());
    }

    /// <summary>One made at `at` in the yard: heard from there.</summary>
    public static void PlayAt(Engine.Guid clip, Float3 at)
    {
        if (!clip.IsNil())
            Audio.playOneShot3D(clip, at);
    }

    /// <summary>A menu's.</summary>
    public static void PlayUI(Engine.Guid clip)
    {
        if (!clip.IsNil())
            Audio.playOneShot(clip, AudioBus.UI);
    }
}
