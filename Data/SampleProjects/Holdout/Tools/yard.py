#!/usr/bin/env python3
"""yard.py: the Yard, Holdout's one level, written through the MCP tools and made the project's
default scene. Run after setup.py (which imports the kit) and importscripts.py (the C# it names),
against the editor (HOLDOUT_MCP=http) to bake the navigation.

A walled lot 36 m by 28 m on the edge of town at dusk. A metal fence rings it, open in three gaps
the enemies come through (north, east and west); the player starts at the south end. Inside, cover
to move between: two long containers and a stack of short ones, sandbag trenches, concrete
barriers, a wrecked car, crates, boxes and pallets, and exploding barrels by the cover. Street
lights stand inside the corners. Outside the fence the town's buildings and trees close the view.
The player starts at the south end, with loose crates in front to practise on, and the SMG and the
shotgun lie in the yard to be picked up; the waves (Game.cs)
bring the enemies in through the gaps. A navigation zone over the lot and its approaches is baked
for them (yard.py run against the editor, which bakes).

Every kit piece is placed as an instance of its imported prefab, with a static box collider of its
own sized from the kit's measured bounds (kit_sizes.json, written by setup.py from the kit's files), so
the character controller, the shots and the navigation see what is drawn. Coordinates: x east,
z south, metres; the player looks north (-z) at the start.
"""
import json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import Doc, mcp, colour, yaw, pitch, quat_mul
from look import look

HALF_X, HALF_Z = 18.0, 14.0  # the lot's half sizes
CHARACTER_SCALE = 0.8        # the kit's characters stand 2.2 m; its props are life size

SIZES = json.load(open(os.path.join(HERE, "kit_sizes.json")))
ASSETS = mcp("asset_list", {})["assets"]


def prefab_of(name):
    """The prefab of an imported kit model, by the model's name (its group's last part)."""
    for a in ASSETS:
        if a["type"] == "PrefabDocument" and (a.get("group") or "").split("/")[-1] == name:
            return a["guid"]
    raise SystemExit("no prefab for %s: run setup.py first" % name)


def script(name):
    """A C# class's asset, imported by importscripts.py."""
    for a in ASSETS:
        if a["type"] == "ScriptClassAsset" and a["name"] == name:
            return a["guid"]
    raise SystemExit("no script %s: run importscripts.py first" % name)


def primitive(name):
    for a in ASSETS:
        if a["type"] == "StaticMeshAsset" and a["name"] == name and "Models/" not in (a.get("group") or ""):
            return a["guid"]
    raise SystemExit("no %s primitive in the project" % name)


def rotate_y(x, z, degrees):
    r = math.radians(degrees)
    return (x * math.cos(r) + z * math.sin(r), -x * math.sin(r) + z * math.cos(r))


class Yard:
    def __init__(self):
        self.d = Doc("Yard")
        look(self.d)
        self.count = {}

    def solid(self, name, centre, size, degrees=0.0):
        """A static box collider of its own, unscaled, so the shape is the size it says."""
        e = self.d.entity(name + "Collider", centre, yaw(degrees))
        self.d.add(e, "physics.RigidBody", motion=0, layer=0, shape=0,
                   halfExtents={"x": size[0] / 2, "y": size[1] / 2, "z": size[2] / 2})
        return e

    def piece(self, kind, x, z, degrees=0.0, y=0.0, collide=True):
        """A kit piece standing at (x, y, z) turned `degrees` about +Y, with its collider."""
        self.d.instance(prefab_of(kind), (x, y, z), yaw(degrees))
        if collide:
            lo, hi = SIZES[kind]["min"], SIZES[kind]["max"]
            cx, cz = rotate_y((lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2, degrees)
            n = self.count[kind] = self.count.get(kind, 0) + 1
            self.solid("%s%d" % (kind, n), (x + cx, y + (lo[1] + hi[1]) / 2, z + cz),
                       (hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]), degrees)

    def fence(self, a, b, gaps=()):
        """Metal fence from a to b (x, z) along one axis, skipping the panels whose middle falls in a
        gap (lo, hi) along that axis; one collider per unbroken stretch."""
        (x0, z0), (x1, z1) = a, b
        along_x = z0 == z1
        width = SIZES["MetalFence"]["max"][0] - SIZES["MetalFence"]["min"][0]
        length = abs((x1 - x0) if along_x else (z1 - z0))
        count = int(round(length / width))
        step = length / count
        sign = 1 if (x1 > x0 if along_x else z1 > z0) else -1
        stretch = []

        def close():
            if stretch:
                lo, hi = min(stretch) - step / 2, max(stretch) + step / 2
                mid = (lo + hi) / 2
                centre = (mid, 1.5, z0) if along_x else (x0, 1.5, mid)
                size = (hi - lo, 3.0, 0.4) if along_x else (0.4, 3.0, hi - lo)
                self.solid("Fence%d_%d" % (int(centre[0]), int(centre[2])), centre, size)
                stretch.clear()

        for i in range(count):
            c = (x0 if along_x else z0) + sign * step * (i + 0.5)
            if any(lo < c < hi for lo, hi in gaps):
                close()
                continue
            at = (c, z0) if along_x else (x0, c)
            self.piece("MetalFence", at[0], at[1], 0.0 if along_x else 90.0, collide=False)
            stretch.append(c)
        close()

    def street_light(self, x, z, degrees):
        """A street light whose arm reaches `degrees` round from +z, and its lamp: a warm spot."""
        self.piece("StreetLight", x, z, degrees)
        ax, az = rotate_y(0.0, 1.7, degrees)
        e = self.d.entity("Lamp%d_%d" % (int(x), int(z)), (x + ax, 5.35, z + az), pitch(-90.0))
        self.d.add(e, "light", type=2, color=colour(1.0, 0.78, 0.5), intensity=14.0, range=16.0,
                   innerAngle=35.0, outerAngle=60.0, castsShadows=True)

    def player(self):
        """The player at the south end looking north: a character body 1.8 m tall (Player.cs moves
        it), the eye 0.7 m above its middle with the camera, the gun in hand at the bottom right (its
        own entity, Gun, so the weapon can kick it back) and the muzzle's light, dark until a shot."""
        d = self.d
        body = d.entity("Player", (0.0, 0.9, 11.0))
        d.add(body, "physics.Character", radius=0.35, halfHeight=0.55, maxSlopeDegrees=45.0, stepUp=0.35)
        d.behavior(body, (script("Player"), {}), (script("Weapon"), {}))
        eye = d.entity("Eye", (0.0, 0.7, 0.0), parent=body)
        d.add(eye, "camera", fovYRadians=math.radians(70.0), nearZ=0.05)
        gun = d.entity("Gun", (0.22, -0.2, -0.42), parent=eye)
        # One entity per weapon under the gun, named as Arsenal.cs names them; Weapon.cs shows the one
        # in hand. The kit's guns lie along their -X: a quarter turn points them ahead (-Z).
        for name in ("Pistol", "SMG", "Shotgun"):
            model = d.entity(name, parent=gun)
            d.instance(prefab_of(name), (0.0, 0.0, 0.0), yaw(-90.0), (0.35, 0.35, 0.35), parent=model)
        muzzle = d.entity("Muzzle", (0.22, -0.12, -0.75), parent=eye)
        d.add(muzzle, "light", type=1, color=colour(1.0, 0.75, 0.4), intensity=0.0, range=7.0)

    def targets(self):
        """Practice: loose crates in front of the player (Target.cs flashes them when shot, and the
        shots push them), one stacked on two."""
        lo, hi = SIZES["Crate"]["min"], SIZES["Crate"]["max"]
        half = (hi[0] - lo[0]) / 2
        for n, (x, y, z) in enumerate(((-3.0, 0.0, 4.0), (-2.0, 0.0, 4.2), (-2.5, 2 * half, 4.1), (2.5, 0.0, 3.5),
                                       (3.5, 0.0, 4.5)), 1):
            e = self.d.entity("Target%d" % n, (x, y + half, z), yaw(10.0 * n))
            self.d.add(e, "physics.RigidBody", motion=2, layer=1, shape=0, mass=15.0,
                       halfExtents={"x": half, "y": half, "z": half})
            self.d.behavior(e, (script("Target"), {}))
            self.d.instance(prefab_of("Crate"), (0.0, -half, 0.0), parent=e)

    def barrel(self, n, x, z):
        """An exploding barrel (Barrel.cs): its collider (Body, what shots hit), the barrel (Model),
        the kit's spilled barrel shown once it has gone (Spilled) and the blast's light (Flash)."""
        d = self.d
        lo, hi = SIZES["ExplodingBarrel"]["min"], SIZES["ExplodingBarrel"]["max"]
        e = d.entity("Barrel%d" % n, (x, 0.0, z))
        d.behavior(e, (script("Barrel"), {}))
        body = d.entity("Body", (0.0, (lo[1] + hi[1]) / 2, 0.0), parent=e)
        d.add(body, "physics.RigidBody", motion=0, layer=0, shape=0,
              halfExtents={"x": (hi[0] - lo[0]) / 2, "y": (hi[1] - lo[1]) / 2, "z": (hi[2] - lo[2]) / 2})
        model = d.entity("Model", parent=e)
        d.instance(prefab_of("ExplodingBarrel"), parent=model)
        spilled = d.entity("Spilled", parent=e)
        d.instance(prefab_of("ExplodingBarrel_Spilled"), rot=yaw(30.0 * n), parent=spilled)
        flash = d.entity("Flash", (0.0, 1.0, 0.0), parent=e)
        d.add(flash, "light", type=1, color=colour(1.0, 0.55, 0.2), intensity=0.0, range=9.0)

    def pickups(self):
        """The SMG by the west container stack and the shotgun behind the east barrier, and three
        health packs, each turning over the ground (Pickup.cs gives it, and brings it back later)."""
        for name, weapon, x, z in (("SMG", 1, -8.5, 2.0), ("Shotgun", 2, 8.5, 6.0)):
            e = self.d.entity("Pickup" + name, (x, 0.6, z))
            self.d.behavior(e, (script("Pickup"), {"weapon": weapon}))
            self.d.instance(prefab_of(name), (0.0, 0.0, 0.0), yaw(-90.0), (0.6, 0.6, 0.6), parent=e)
            glow = self.d.entity(name + "Glow", (0.0, 0.3, 0.0), parent=e)
            self.d.add(glow, "light", type=1, color=colour(1.0, 0.85, 0.5), intensity=3.0, range=2.5)
        # Health packs: one by each side's cover and one at the north, out where it costs to reach.
        for n, (x, z) in enumerate(((-11.0, -1.0), (11.0, 1.0), (0.0, -9.5)), 1):
            e = self.d.entity("PickupHealth%d" % n, (x, 0.5, z))
            self.d.behavior(e, (script("Pickup"), {"heal": 35.0}))
            self.d.instance(prefab_of("Health"), (0.0, -0.3, 0.0), parent=e)
            glow = self.d.entity("HealthGlow%d" % n, (0.0, 0.3, 0.0), parent=e)
            self.d.add(glow, "light", type=1, color=colour(0.5, 1.0, 0.6), intensity=3.0, range=2.5)

    def navigation(self):
        """The navigation zone over the lot and the ground just outside it, so an enemy outside a gap
        finds its way in; baked from the scene's static colliders into YardNav."""
        e = self.d.entity("NavZone", (0.0, 1.5, 0.0))
        found = [a["guid"] for a in ASSETS if a["type"] == "NavigationZoneAsset" and a["name"] == "YardNav"]
        zone = found[0] if found else mcp("asset_create", {"creator": "Navigation Zone", "name": "YardNav",
                                                           "group": "Navigation"})["guid"]
        self.d.add(e, "navigation.Zone", extents={"x": HALF_X + 6.0, "y": 3.0, "z": HALF_Z + 6.0}, zone=zone,
                   agentRadius=0.4, agentHeight=1.8, cellSize=0.2, cellHeight=0.1, agentMaxClimb=0.35,
                   maxAgents=64)

    def write(self):
        d = self.d
        plane = primitive("Plane")

        # The ground: the lot's concrete, the street round it, and one slab collider under both.
        e = d.entity("Lot", (0.0, 0.0, 0.0), scale=(HALF_X * 2, 1.0, HALF_Z * 2))
        d.add(e, "mesh", mesh=plane, color=colour(0.42, 0.40, 0.37))
        e = d.entity("Street", (0.0, -0.02, 0.0), scale=(120.0, 1.0, 120.0))
        d.add(e, "mesh", mesh=plane, color=colour(0.20, 0.20, 0.22))
        self.solid("Ground", (0.0, -0.5, 0.0), (120.0, 1.0, 120.0))

        # The fence, open north (x -2..2), east (z -6..-2) and west (z 4..8).
        self.fence((-HALF_X, -HALF_Z), (HALF_X, -HALF_Z), gaps=[(-2.0, 2.0)])
        self.fence((-HALF_X, HALF_Z), (HALF_X, HALF_Z))
        self.fence((HALF_X, -HALF_Z), (HALF_X, HALF_Z), gaps=[(-6.0, -2.0)])
        self.fence((-HALF_X, -HALF_Z), (-HALF_X, HALF_Z), gaps=[(4.0, 8.0)])
        # Cones and barriers mark the gaps from outside.
        for x, z in ((-2.6, -15.2), (2.6, -15.2), (19.2, -6.6), (19.2, -1.4), (-19.2, 3.4), (-19.2, 8.6)):
            self.piece("TrafficCone", x, z, collide=False)

        # Cover. North half: what the enemies run through from the north gap.
        self.piece("Container_Long", -8.0, -7.0, 0.0)
        self.piece("Container_Long", 9.0, -8.5, 90.0)
        self.piece("SackTrench", 0.0, -6.0, 0.0)
        self.piece("Barrier_Single", -3.5, -10.0, 15.0)
        self.piece("Barrier_Single", 4.0, -11.0, -10.0)
        self.piece("Debris_BrokenCar", 13.5, -3.0, 25.0)
        self.piece("Crate", -10.6, -5.5, 10.0)
        self.piece("Crate", -10.6, -4.6, -5.0)
        self.piece("CardboardBoxes_2", -5.2, -5.6, 0.0)
        # The middle: a stack of short containers, a trench and barriers across.
        self.piece("Container_Small", -11.0, 2.0, 90.0)
        self.piece("Container_Small", -11.0, 2.0, 90.0, y=2.13)
        self.piece("SackTrench_Small", -4.0, 1.5, 0.0)
        self.piece("SackTrench_Small", 5.0, 0.5, 0.0)
        self.piece("Barrier_Large", 11.0, 4.5, 90.0)
        self.piece("Pallet", 8.0, 3.0, 20.0)
        self.piece("WoodPlanks", 8.0, 3.0, 20.0, y=0.19, collide=False)
        self.piece("Debris_Tires", 14.5, 8.5, 0.0)
        self.piece("TrashContainer", -15.0, -10.0, 90.0)
        self.piece("Pipes", 15.0, -11.0, 0.0)
        # South: the player's end, open, with a little cover to fall back to.
        self.piece("Barrier_Single", -2.0, 7.5, 0.0)
        self.piece("Barrier_Single", 2.0, 7.5, 0.0)
        self.piece("CardboardBoxes_1", -7.5, 9.5, 30.0)
        self.piece("WaterTank_Platform", 14.5, 11.5, -90.0)
        self.piece("GasTank", -15.0, 11.5, 0.0)
        self.piece("Debris_Papers_1", 3.0, 4.0, 40.0, collide=False)
        self.piece("Debris_Papers_2", -6.0, -2.0, 0.0, collide=False)

        # Exploding barrels by the cover, where a shot at one catches who is near.
        for n, (x, z) in enumerate(((-6.2, -8.5), (7.6, -6.0), (-12.8, 4.2), (12.5, 6.2), (1.8, -4.8), (-3.0, 11.0)), 1):
            self.barrel(n, x, z)

        # Street lights inside the corners, their arms reaching in.
        self.street_light(-16.5, -12.5, 135.0)
        self.street_light(16.5, -12.5, -135.0)
        self.street_light(-16.5, 12.5, 45.0)
        self.street_light(16.5, 12.5, -45.0)

        # The town beyond the fence, and its trees.
        self.piece("Structure_4", -8.0, -24.0, 180.0)
        self.piece("Structure_2", 12.0, -23.0, 180.0)
        self.piece("Structure_1", 27.0, -2.0, -90.0)
        self.piece("Structure_3", -27.0, 4.0, 90.0)
        self.piece("Structure_1", 6.0, 24.0, 0.0)
        for kind, x, z in (("Tree_1", -22.0, -18.0), ("Tree_2", 23.0, -15.0), ("Tree_3", 24.0, 13.0),
                           ("Tree_4", -23.0, 16.0), ("Tree_2", -12.0, 21.0), ("Tree_1", 20.0, 21.0)):
            self.piece(kind, x, z, collide=False)

        # The sun just down: a low warm light from the west, the shadows long.
        e = d.entity("Sun", rot=quat_mul(yaw(-100.0), pitch(-12.0)))
        d.add(e, "light", type=0, color=colour(1.0, 0.62, 0.38), intensity=1.6, castsShadows=True)

        self.player()
        self.targets()
        self.pickups()
        self.navigation()

        scenes = [a["guid"] for a in ASSETS if a["type"] == "SceneDocument" and a["name"] == "Yard"]
        return d.write(scenes[0] if scenes else None, group="Scenes")


def main():
    guid = Yard().write()
    mcp("project_settings_set", {"defaultSceneId": guid})
    print("Yard", guid)
    # Baking needs the scene open in the editor (HOLDOUT_MCP=http); a headless host says so.
    try:
        mcp("page_open", {"guid": guid})
        print(mcp("navigation_bake", {"page": guid, "entity": "NavZone"}).get("triangles"), "navigation triangles")
    except SystemExit as refused:
        print("navigation not baked (run yard.py against the editor):", str(refused)[:200])
    print(mcp("asset_cook", {}))


main()
