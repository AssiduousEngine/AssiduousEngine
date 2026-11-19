#!/usr/bin/env python3
"""sounds.py --kenney <dir>: Holdout's sounds, imported through the editor's MCP into the group Audio.

Generated here (no third-party content): the three guns' shots, each a crack of noise that dies in a
few hundredths of a second over a low thump and a short rumbling tail; the pistol's dry and middling,
the SMG's short and light (it fires eleven a second), the shotgun's a deep boom with a long tail.

Taken from Kenney's packs (CC0; `--kenney` is the folder holding the unzipped Impact Sounds,
Interface Sounds and Sci-Fi Sounds), each imported under the name the game knows it by (KENNEY):
a barrel's blast, a shot landing, the player hurt, an enemy down, a gunner's round, a reload, a
weapon change, the pickups, a wave starting, the menus' click, and the end of a run held or overrun.
"""
import math, os, random, shutil, struct, sys, wave
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

RATE = 44100


def shot(path, seconds, crack, thump_hz, thump_decay, tail_decay, tail_level, seed):
    """A gunshot: a noise crack dying in `crack` seconds, a low thump, and a filtered rumbling tail."""
    rnd = random.Random(seed)
    low = 0.0
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        white = rnd.uniform(-1.0, 1.0)
        low += (white - low) * 0.08  # the tail: noise with its hiss taken off
        attack = min(1.0, t / 0.0008)
        value = white * math.exp(-t / crack) * attack
        value += 0.9 * math.sin(2 * math.pi * thump_hz * t * (1.0 - 0.4 * t)) * math.exp(-t / thump_decay) * attack
        value += tail_level * low * 3.0 * math.exp(-t / tail_decay)
        out.append(value)
    fade = int(0.01 * RATE)  # no click at the end
    for i in range(fade):
        out[-1 - i] *= i / fade
    peak = max(abs(v) for v in out) or 1.0
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v / peak * 0.9 * 32767)) for v in out))


SHOTS = {
    "PistolShot": dict(seconds=0.35, crack=0.018, thump_hz=140.0, thump_decay=0.05, tail_decay=0.09, tail_level=0.5, seed=7),
    "SmgShot": dict(seconds=0.2, crack=0.012, thump_hz=170.0, thump_decay=0.03, tail_decay=0.05, tail_level=0.35, seed=11),
    "ShotgunShot": dict(seconds=0.7, crack=0.03, thump_hz=85.0, thump_decay=0.09, tail_decay=0.2, tail_level=0.8, seed=13),
}

# name: (pack, file) under the Kenney folder.
KENNEY = {
    "Explosion": ("sci-fi-sounds", "Audio/lowFrequency_explosion_000.ogg"),
    "Hit": ("impact-sounds", "Audio/impactPunch_medium_001.ogg"),
    "Hurt": ("impact-sounds", "Audio/impactPunch_heavy_000.ogg"),
    "EnemyDown": ("impact-sounds", "Audio/impactSoft_heavy_000.ogg"),
    "EnemyShot": ("sci-fi-sounds", "Audio/laserSmall_001.ogg"),
    "Reload": ("impact-sounds", "Audio/impactMetal_light_001.ogg"),
    "WeaponSwitch": ("interface-sounds", "Audio/switch_004.ogg"),
    "PickupWeapon": ("interface-sounds", "Audio/confirmation_001.ogg"),
    "PickupHealth": ("interface-sounds", "Audio/maximize_006.ogg"),
    "Wave": ("interface-sounds", "Audio/bong_001.ogg"),
    "Click": ("interface-sounds", "Audio/click_002.ogg"),
    "Held": ("interface-sounds", "Audio/confirmation_004.ogg"),
    "Overrun": ("interface-sounds", "Audio/error_006.ogg"),
}


def main():
    args = sys.argv[1:]
    if "--kenney" not in args:
        sys.exit("sounds.py --kenney <dir>: the folder holding Kenney's unzipped sound packs")
    kenney = args[args.index("--kenney") + 1]
    out = os.path.join(HERE, "generated", "Audio")
    os.makedirs(out, exist_ok=True)
    for name, settings in SHOTS.items():
        path = os.path.join(out, name + ".wav")
        shot(path, **settings)
        print(name, mcp("asset_import", {"source": path, "group": "Audio", "importer": "Audio"})["guid"])
    for name, (pack, rel) in KENNEY.items():
        path = os.path.join(out, name + ".ogg")
        shutil.copyfile(os.path.join(kenney, pack, rel), path)
        print(name, mcp("asset_import", {"source": path, "group": "Audio", "importer": "Audio"})["guid"])


main()
