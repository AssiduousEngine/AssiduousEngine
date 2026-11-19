using Engine;

// The game's sounds (Tools/sounds.py: the gunshots made there, the rest from Kenney's packs), and
// playing them: the player's own on the effects bus as heard where they stand, another's (an
// enemy's, a barrel's) at its place in the yard, the menus' on the UI bus.
public static class Sounds
{
    public static readonly Engine.Guid PistolShot = Engine.Guid.Parse("787126f6-64fe-484d-8d4f-52187d609988");
    public static readonly Engine.Guid SmgShot = Engine.Guid.Parse("f3da1a8d-bc6e-4e1d-be9b-e64d1571f8ac");
    public static readonly Engine.Guid ShotgunShot = Engine.Guid.Parse("77bb8dc6-918d-4815-8fc4-2fa0442c7a5f");
    public static readonly Engine.Guid Explosion = Engine.Guid.Parse("9dec6feb-c35d-4590-890d-207492460e0f");
    public static readonly Engine.Guid Hit = Engine.Guid.Parse("ea7e8308-a3e7-46bc-8ab2-c1acfbc7fed8");
    public static readonly Engine.Guid Hurt = Engine.Guid.Parse("637f8f7d-b578-410b-8f91-35aa57cee988");
    public static readonly Engine.Guid EnemyDown = Engine.Guid.Parse("08e236cf-c0c4-4c4e-bfb3-5fb1c62fa7a3");
    public static readonly Engine.Guid EnemyShot = Engine.Guid.Parse("b08d2258-f08f-4dec-80e9-e3f5713a4a34");
    public static readonly Engine.Guid Reload = Engine.Guid.Parse("f8553767-06bd-468c-9774-92c3ae743c3b");
    public static readonly Engine.Guid WeaponSwitch = Engine.Guid.Parse("c894b1f7-71fc-4d33-b05e-ce624dba38b9");
    public static readonly Engine.Guid PickupWeapon = Engine.Guid.Parse("2da5685f-9aec-4939-8c7d-4de94265879d");
    public static readonly Engine.Guid PickupHealth = Engine.Guid.Parse("af07c18d-a391-4706-b903-7d4d5d4dd51c");
    public static readonly Engine.Guid Wave = Engine.Guid.Parse("65f1330b-4c95-42b9-a9f4-315c8bd32a94");
    public static readonly Engine.Guid Click = Engine.Guid.Parse("fc6e8b1f-96da-4f87-b163-316d8323a00a");
    public static readonly Engine.Guid Held = Engine.Guid.Parse("3278b4b0-acbd-4b65-b5ee-c07776e8d1aa");
    public static readonly Engine.Guid Overrun = Engine.Guid.Parse("af060505-b290-4006-8686-57b6386319f8");

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
