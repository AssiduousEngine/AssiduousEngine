using Engine;

// The weapons, by number: what each fires, how fast, how much it holds and how long it takes to
// reload. The player carries the pistol from the start and finds the others in the yard.
public enum WeaponKind
{
    Pistol = 0,
    SMG = 1,
    Shotgun = 2,
}

public sealed class WeaponStats
{
    public required string Name;
    public required float RoundsPerSecond; // held down, it fires this often
    public required float Damage;          // a pellet's (a single shot is one pellet)
    public required int Pellets;           // pellets a shot spreads
    public required float SpreadDegrees;   // the widest a pellet strays from the aim
    public required int Magazine;          // rounds before a reload
    public required int Carried;           // spare rounds when picked up; -1: never runs out
    public required float ReloadSeconds;
    public required float Kick;            // how far the gun kicks back, metres
    public required Engine.Guid Sound;     // a shot's (Sounds)
    public required float Volume;          // and how loud
}

public static class Arsenal
{
    public static readonly WeaponStats[] Weapons =
    {
        new WeaponStats { Name = "Pistol", RoundsPerSecond = 4.0f, Damage = 25.0f, Pellets = 1, SpreadDegrees = 0.4f,
                          Magazine = 12, Carried = -1, ReloadSeconds = 1.0f, Kick = 0.06f,
                          Sound = Sounds.PistolShot, Volume = 0.7f },
        new WeaponStats { Name = "SMG", RoundsPerSecond = 11.0f, Damage = 14.0f, Pellets = 1, SpreadDegrees = 2.2f,
                          Magazine = 30, Carried = 150, ReloadSeconds = 1.5f, Kick = 0.035f,
                          Sound = Sounds.SmgShot, Volume = 0.5f },
        new WeaponStats { Name = "Shotgun", RoundsPerSecond = 1.3f, Damage = 13.0f, Pellets = 8, SpreadDegrees = 6.0f,
                          Magazine = 6, Carried = 30, ReloadSeconds = 2.0f, Kick = 0.12f,
                          Sound = Sounds.ShotgunShot, Volume = 0.9f },
    };

    public static WeaponStats Of(WeaponKind kind) => Weapons[(int)kind];
}
