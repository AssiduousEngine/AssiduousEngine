#!/usr/bin/env python3
"""enemies.py: the enemy prefabs and the gunners' shot (Prefabs/), written through the MCP tools.
Run after setup.py and importscripts.py (the classes they name); the waves (Game.cs) spawn them.

Each enemy's root is a navigation agent the size of a person, which moves it, with the Enemy
behaviour set for its kind. Under it, Body is a kinematic capsule the shots hit (Combat finds the
enemy from it), and one of the kit's characters, scaled to a person's height (they stand 2.2 m)
and turned to look down -Z, the way the root faces.

- Enemy: the kit's enemy, a brawler with no weapon shown.
- Gunner: the kit's enemy with its pistol, keeping its distance and shooting.
- Hazmat: the hazmat suit, a heavy brawler: slower, much tougher, punching harder.
- Shot: a gunner's round, a small glowing sphere with its light (Projectile).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import Doc, mcp, yaw, colour

CHARACTER_SCALE = 0.8

ASSETS = mcp("asset_list", {})["assets"]


def asset(type_name, name, group_suffix=None):
    for a in ASSETS:
        if a["type"] == type_name and a["name"] == name and (group_suffix is None or (a.get("group") or "").endswith(group_suffix)):
            return a["guid"]
    return None


def need(guid, what):
    if guid is None:
        raise SystemExit("no %s: run setup.py and importscripts.py first" % what)
    return guid


def enemy(name, character, overrides, size=1.0):
    model = need(asset("PrefabDocument", "Prefab", "/" + character), character)
    d = Doc(name)
    root = d.entity(name)
    d.add(root, "navigation.Agent", radius=0.4 * size, height=1.8 * size, maxSpeed=4.0, maxAcceleration=12.0,
          avoidOthers=True)
    d.behavior(root, (need(asset("ScriptClassAsset", "Enemy"), "Enemy"), overrides))
    body = d.entity("Body", (0.0, 0.9 * size, 0.0), parent=root)
    d.add(body, "physics.RigidBody", motion=1, layer=2, shape=2, radius=0.35 * size, halfHeight=0.55 * size)
    d.instance(model, (0.0, 0.0, 0.0), yaw(180.0), (CHARACTER_SCALE * size,) * 3, parent=root)
    print(name, d.write(asset("PrefabDocument", name), prefab=True, group="Prefabs"))


def shot():
    sphere = need(next((a["guid"] for a in ASSETS if a["type"] == "StaticMeshAsset" and a["name"] == "Sphere"
                        and "Models/" not in (a.get("group") or "")), None), "the Sphere primitive")
    d = Doc("Shot")
    root = d.entity("Shot", scale=(0.16, 0.16, 0.16))
    d.add(root, "mesh", mesh=sphere, color=colour(1.0, 0.6, 0.2))
    d.behavior(root, (need(asset("ScriptClassAsset", "Projectile"), "Projectile"), {}))
    glow = d.entity("Glow", parent=root)
    d.add(glow, "light", type=1, color=colour(1.0, 0.55, 0.2), intensity=4.0, range=3.0)
    print("Shot", d.write(asset("PrefabDocument", "Shot"), prefab=True, group="Prefabs"))


shot()
enemy("Enemy", "Character_Enemy", {})
enemy("Gunner", "Character_Enemy", {"gunner": True, "weapon": "Pistol", "health": 45.0, "speed": 3.2,
                                    "punchInterval": 1.8})
enemy("Hazmat", "Character_Hazmat", {"model": 1, "health": 170.0, "speed": 2.6, "punchDamage": 22.0,
                                     "punchInterval": 1.6}, size=1.15)
