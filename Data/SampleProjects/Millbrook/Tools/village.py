#!/usr/bin/env python3
"""village.py: writes Millbrook, the village's scene, through the editor's MCP.

The square: paving round the fountain, the fair's stalls, a cart and the lanterns. Round it, the
houses built from the Fantasy Town Kit's walls and roofs: the mayor's hall and a timber house to
the north, the baker's house to the west, the miller's to the east with the watermill turning in
the stream beyond it, and the windmill's tower behind them to the north-east. To the south the
fence and its gate, where the guard stands, and the road out. Grass, trees, bushes and flowers
round it all (the Nature Kit). The villagers and the player stand where the day starts.

Every kit piece is built at twice the kit's size (SCALE), so a wall is 2 m and a villager about
1.6 m. Positions below are in the kit's cells (one cell is 2 m), x east and z south, the square's
middle at the origin; each piece carries a box collider of its own, unscaled, where it stands in
the way.

Run with the editor open on the project and its MCP server on, after importmodels.py:
    python3 village.py
"""
import math, os, random, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scenegen import Doc, mcp, yaw, pitch
from look import look

SCALE = 2.0  # metres to a kit cell
ASSETS = mcp("asset_list", {})["assets"]


def asset(asset_type, name, group=None):
    found = [a["guid"] for a in ASSETS
             if a["type"] == asset_type and a["name"] == name and (group is None or a.get("group") == group)]
    if not found:
        raise SystemExit("no %s %s (run importmodels.py first)" % (asset_type, name))
    return found[0]


def prefab(group):
    found = [a["guid"] for a in ASSETS if a["type"] == "PrefabDocument" and a.get("group") == group]
    if not found:
        raise SystemExit("no model in %s (run kenney.py and importmodels.py)" % group)
    return found[0]


_parts = {}


def parts(group):
    """An imported model's meshes, each with its material slots, read from its prefab."""
    if group not in _parts:
        root = ET.fromstring(mcp("prefab_read", {"guid": prefab(group)})["xml"])
        found = []
        for comp in root.find("array[@name='components']"):
            if comp.find("string[@name='type']").text == "mesh":
                data = comp.find("object[@name='data']")
                found.append((data.find("string[@name='mesh']").text,
                              [e.text for e in data.find("array[@name='materials']")]))
        _parts[group] = found
    return _parts[group]


class Village:
    def __init__(self):
        self.d = Doc("Millbrook")
        self.rng = random.Random("Millbrook")  # the same village every time
        look(self.d)
        self.taken = []  # (x0, z0, x1, z1) in cells: what trees keep out of

    # ---- pieces ----
    def put(self, kind, x, y, z, deg=0.0, scale=1.0, name=None, parent=None):
        """A model at cell (x, y, z), turned `deg` about up; `kind` is "Town/wall" or "Nature/tree_oak"."""
        s = SCALE * scale
        e = self.d.stable_id("entity")
        self.d.entity(name or kind.split("/")[-1], (x * SCALE, y * SCALE, z * SCALE), yaw(deg), (s, s, s),
                      parent=parent, eid=e)
        for k, (mesh, materials) in enumerate(parts("Models/" + kind)):
            target = e
            if k > 0:  # a model of several meshes: one entity each, under the first
                target = self.d.stable_id("entity")
                self.d.entity("%s%d" % (kind.split("/")[-1], k), parent=e, eid=target)
            self.d.add(target, "mesh", mesh=mesh, materials=["<string>%s</string>" % m for m in materials])
        return e

    def solid(self, name, x, z, w, h, d, y=0.0):
        """A static box collider, in cells across and metres high: centred on cell (x, z), w by d
        cells, h metres tall, standing on y (metres)."""
        e = self.d.stable_id("entity")
        self.d.entity(name + "Collider", (x * SCALE, y + h / 2, z * SCALE), eid=e)
        self.d.add(e, "physics.RigidBody", motion=0, layer=0, shape=0,
                   halfExtents={"x": w * SCALE / 2, "y": h / 2, "z": d * SCALE / 2})
        return e

    def keep(self, x0, z0, x1, z1):
        self.taken.append((x0, z0, x1, z1))

    # ---- buildings ----
    def house(self, name, x0, z0, w, d, facing, wall="wall", roof="roof-high", door=0, windows=()):
        """A house over cells x0..x0+w-1, z0..z0+d-1 (before turning), its front door `door` cells
        along the front, `windows` the front's cells that are windows; turned so its front faces
        `facing`: 0 south (+z), 90 east, 180 north, 270 west. The roof's ridge runs front to back,
        so a house is two cells wide: a roof piece slopes across one cell, and two make the gable."""
        cx, cz = x0 + (w - 1) / 2.0, z0 + (d - 1) / 2.0
        a = math.radians(facing)

        def at(i, k):
            # local (i across, k back to front) about the middle, turned to `facing`
            lx, lz = i - (w - 1) / 2.0, k - (d - 1) / 2.0
            return cx + lx * math.cos(a) + lz * math.sin(a), cz - lx * math.sin(a) + lz * math.cos(a)

        parent = self.d.stable_id("entity")
        self.d.entity(name, eid=parent)
        wood = wall.startswith("wall-wood")
        for i in range(w):
            for k in range(d):
                x, z = at(i, k)
                if i == 0:
                    self.put("Town/" + wall, x, 0, z, 180 + facing, parent=parent)
                if i == w - 1:
                    self.put("Town/" + wall, x, 0, z, 0 + facing, parent=parent)
                if k == 0:
                    self.put("Town/" + wall, x, 0, z, 90 + facing, parent=parent)
                if k == d - 1:
                    piece = wall
                    if i == door:
                        piece = "wall-wood-door" if wood else "wall-door"
                    elif i in windows:
                        piece = "wall-wood-window-shutters" if wood else "wall-window-shutters"
                    self.put("Town/" + piece, x, 0, z, 270 + facing, parent=parent)
                self.put("Town/" + roof, x, 1, z, (0 if i < w / 2 else 180) + facing, parent=parent)
        # the collider and the ground the trees keep off
        wx, wz = (w, d) if facing % 180 == 0 else (d, w)
        self.solid(name, cx, cz, wx, 2.0 * SCALE, wz)
        self.keep(cx - wx / 2 - 1, cz - wz / 2 - 1, cx + wx / 2 + 1, cz + wz / 2 + 1)
        return parent

    def windmill(self, x, z):
        """A stone tower three cells high, its sails turning on its west face (Windmill.as turns them)."""
        tower = self.d.stable_id("entity")
        self.d.entity("Windmill", eid=tower)
        for y in range(3):
            self.put("Town/wall-corner", x, y, z, 0, parent=tower)
            self.put("Town/wall-corner", x, y, z, 180, parent=tower)
        self.put("Town/roof-high-corner", x, 3, z, 0, parent=tower)
        self.put("Town/roof-high-corner", x, 3, z, 180, parent=tower)
        self.put("Town/windmill", x - 0.75, 2.4, z, 0, name="Sails", parent=tower)
        self.solid("Windmill", x, z, 1, 3.0 * SCALE, 1)
        self.keep(x - 2, z - 2, x + 2, z + 2)

    # ---- the ground ----
    def ground(self, x0, z0, x1, z1):
        """Grass over cells x0..x1, z0..z1, one tile stretched, and the floor's collider under it."""
        e = self.d.stable_id("entity")
        w, d = x1 - x0, z1 - z0
        self.d.entity("Grass", (((x0 + x1) / 2) * SCALE, 0.0, ((z0 + z1) / 2) * SCALE), scale=(w * SCALE, 1, d * SCALE),
                      eid=e)
        for mesh, materials in parts("Models/Nature/ground_grass"):
            self.d.add(e, "mesh", mesh=mesh, materials=["<string>%s</string>" % m for m in materials])
        f = self.d.stable_id("entity")
        self.d.entity("GroundCollider", (((x0 + x1) / 2) * SCALE, -0.5, ((z0 + z1) / 2) * SCALE), eid=f)
        self.d.add(f, "physics.RigidBody", motion=0, layer=0, shape=0,
                   halfExtents={"x": w * SCALE / 2, "y": 0.5, "z": d * SCALE / 2})

    def square(self, x0, z0, x1, z1):
        """The square's paving over cells x0..x1 inclusive, just above the grass."""
        parent = self.d.stable_id("entity")
        self.d.entity("Square", eid=parent)
        for x in range(x0, x1 + 1):
            for z in range(z0, z1 + 1):
                self.put("Town/road", x, 0.005, z, 0, parent=parent)

    def stream(self, x, z0, z1):
        """The stream down cell column x, its banks walled off (nobody wades in)."""
        parent = self.d.stable_id("entity")
        self.d.entity("Stream", eid=parent)
        for z in range(z0, z1):
            self.put("Nature/ground_riverStraight", x, 0.002, z, 0, parent=parent)
        self.solid("Stream", x, (z0 + z1 - 1) / 2.0, 1, 1.5, z1 - z0)
        self.keep(x - 1, z0, x + 1, z1)

    def fence(self, x0, x1, z, gap=()):
        """A fence along the south edge of cell row z, from cell x0 to x1, leaving the cells in `gap` open."""
        parent = self.d.stable_id("entity")
        self.d.entity("Fence", eid=parent)
        run = []
        for x in range(x0, x1 + 1):
            if x in gap:
                if run:
                    self.solid("Fence", (run[0] + run[-1]) / 2.0, z + 0.5, len(run), 1.0, 0.1)
                    run = []
                continue
            self.put("Town/fence", x, 0, z, 270, parent=parent)
            run.append(x)
        if run:
            self.solid("Fence", (run[0] + run[-1]) / 2.0, z + 0.5, len(run), 1.0, 0.1)

    def bounds(self, x0, z0, x1, z1):
        """Invisible walls round the edge, so nobody walks off the world."""
        for name, x, z, w, d in (("North", (x0 + x1) / 2.0, z0, x1 - x0, 0.2), ("South", (x0 + x1) / 2.0, z1, x1 - x0, 0.2),
                                 ("West", x0, (z0 + z1) / 2.0, 0.2, z1 - z0), ("East", x1, (z0 + z1) / 2.0, 0.2, z1 - z0)):
            self.solid("Edge" + name, x, z, w, 4.0, d)

    def trees(self, count, x0, z0, x1, z1):
        kinds = ("Nature/tree_oak", "Nature/tree_default", "Nature/tree_fat", "Nature/tree_pineRoundA",
                 "Nature/tree_pineTallA", "Town/tree", "Town/tree-high", "Town/tree-high-round")
        parent = self.d.stable_id("entity")
        self.d.entity("Trees", eid=parent)
        placed = 0
        tries = 0
        while placed < count and tries < count * 40:
            tries += 1
            x, z = self.rng.uniform(x0, x1), self.rng.uniform(z0, z1)
            if any(a <= x <= c and b <= z <= d for a, b, c, d in self.taken):
                continue
            kind = self.rng.choice(kinds)
            size = self.rng.uniform(2.2, 3.0) if kind.startswith("Nature") else self.rng.uniform(1.1, 1.4)
            self.put(kind, x, 0, z, self.rng.uniform(0, 360), size, parent=parent)
            self.solid("Tree", x, z, 0.3, 3.0, 0.3)
            self.keep(x - 0.8, z - 0.8, x + 0.8, z + 0.8)
            placed += 1

    def scatter(self, kinds, count, x0, z0, x1, z1, size=(1.6, 2.4)):
        """Small things that stand in nobody's way: bushes, flowers, grass."""
        parent = self.d.stable_id("entity")
        self.d.entity("Plants", eid=parent)
        for _ in range(count):
            x, z = self.rng.uniform(x0, x1), self.rng.uniform(z0, z1)
            if any(a <= x <= c and b <= z <= d for a, b, c, d in self.taken):
                continue
            self.put(self.rng.choice(kinds), x, 0, z, self.rng.uniform(0, 360), self.rng.uniform(*size), parent=parent)

    # ---- people ----
    def person(self, name, model, x, z, facing):
        """A villager (or the player): a character model's instance, turned to face `facing` degrees
        (0 south). An instance's root takes its model's name, and each villager wears a model of
        their own, so the cast names each by its model (character-female-b is the baker); `name` is
        who they are, for this script's reader."""
        e = self.d.stable_id("instance")
        self.d.instance(prefab("Models/Characters/" + model), (x * SCALE, 0.0, z * SCALE), yaw(facing),
                        (SCALE, SCALE, SCALE))
        return e

    def player(self, x, z, facing):
        """The player: an entity with the character controller and Player.as, the model under it
        (feet on the ground), and the camera behind with CameraRig.as."""
        model = "character-male-a"
        group = "Models/Characters/" + model
        e = self.d.stable_id("entity")
        self.d.entity("Player", (x * SCALE, 0.85, z * SCALE), yaw(facing), eid=e)
        self.d.add(e, "physics.Character", radius=0.3, halfHeight=0.55, maxSlopeDegrees=45.0, stepUp=0.3)
        self.d.instance(prefab(group), (0, -0.85, 0), scale=(SCALE, SCALE, SCALE), parent=e)
        cam = self.d.stable_id("entity")
        self.d.entity("Camera", (x * SCALE, 7.0, z * SCALE + 7.0), pitch(-38.0), eid=cam)
        self.d.add(cam, "camera", fovYRadians=0.9)
        clip = lambda name: ("asset", asset("AnimationClipAsset", name, group))
        self.d.script(e, (asset("ScriptClassAsset", "Player"), {"camera": ("entity", cam), "idleClip": clip("idle"),
                                                                "walkClip": clip("walk"), "runClip": clip("sprint")}))
        self.d.script(cam, (asset("ScriptClassAsset", "CameraRig"), {"target": ("entity", e), "distance": 6.0,
                                                                     "height": 5.0, "lookHeight": 1.0}))
        return e

    def sun(self):
        e = self.d.stable_id("entity")
        # low in the west, a golden evening light
        self.d.entity("Sun", (0.0, 20.0, 0.0), (-0.16, 0.66, 0.15, 0.72), eid=e)
        self.d.add(e, "light", type=0, color={"r": 1.0, "g": 0.78, "b": 0.52, "a": 1.0}, intensity=2.5,
                   castsShadows=True)


def main():
    v = Village()
    v.ground(-26, -26, 9, 26)       # west of the stream
    v.ground(11, -26, 26, 26)       # east of it
    v.stream(10, -26, 26)
    v.bounds(-26, -26, 26, 26)
    v.square(-5, -5, 4, 4)
    v.keep(-6, -6, 5, 5)

    # The fountain in the middle of the square.
    v.put("Town/fountain-round-detail", -0.5, 0, -0.5, 0, name="Fountain")
    v.solid("Fountain", -0.5, -0.5, 2, 1.0, 2)

    # North: the mayor's hall (stone) and a timber house; west the baker's; east the miller's, the
    # watermill on its back wall over the stream.
    v.house("Hall", -4, -11, 2, 4, 0, wall="wall", roof="roof-high", door=0, windows=(1,))
    v.house("TimberHouse", 2, -10, 2, 3, 0, wall="wall-wood", roof="roof", door=1, windows=(0,))
    v.house("Bakery", -9, -3, 2, 4, 90, wall="wall-wood", roof="roof-high", door=0, windows=(1,))
    v.house("Mill", 7, -3, 2, 4, 270, wall="wall", roof="roof", door=1, windows=(0,))
    v.put("Town/watermill-wide", 9.0, 0.45, -1.5, 0, name="Waterwheel")
    v.windmill(7, -9)

    # The fair: the baker's stall (no sign yet), a stall of the village's, the miller's cart (its
    # wheel is missing: the cart stands without it until it is found), benches, banners, lanterns.
    v.put("Town/stall-red", -4, 0, -1, 90, name="BakerStall")
    v.solid("BakerStall", -4, -1, 1, 2.5, 1)
    v.put("Town/stall-green", 3, 0, 2, 270, name="FairStall")
    v.solid("FairStall", 3, 2, 1, 2.5, 1)
    v.put("Town/cart", 3, 0, -3, 20, name="Cart")
    v.solid("Cart", 3, -3, 0.7, 1.2, 1.4)
    for x, z, deg in ((-2, 3, 0), (1, 3, 0)):
        v.put("Town/stall-bench", x, 0, z, deg, name="Bench")
    for k, (x, z) in enumerate(((-5, -5), (4, -5), (-5, 4), (4, 4))):
        v.put("Town/lantern", x, 0, z, 0, name="Lantern")
        v.solid("Lantern", x, z, 0.2, 3.0, 0.2)
        # unlit until the fair opens (the ending's LightLanterns event; Millbrook.as)
        light = v.d.stable_id("entity")
        v.d.entity("LanternLight%d" % k, (x * SCALE, 3.0, z * SCALE), eid=light)
        v.d.add(light, "light", type=1, color={"r": 1.0, "g": 0.72, "b": 0.40, "a": 1.0}, intensity=6.0, range=11.0,
                enabled=False)

    # South: the fence and its gate; the road out.
    v.fence(-9, 8, 6, gap=(-1, 0))
    v.put("Town/fence-gate", -1, 0, 6.5, 0, name="Gate")
    for z in range(7, 26):
        v.put("Town/road", -1, 0.005, z, 0, name="Road")
        v.put("Town/road", 0, 0.005, z, 0, name="Road")
    v.keep(-2, 6, 1, 26)

    # Round it all: trees, bushes and flowers.
    v.trees(70, -25, -25, 25, 25)
    v.scatter(("Nature/plant_bushLarge", "Nature/plant_bush", "Nature/flower_redA", "Nature/flower_yellowA",
               "Nature/flower_purpleA", "Nature/grass_large"), 160, -25, -25, 25, 25)

    # The places the conversations name: where the baker hangs her sign, where the miller's wheel
    # lies behind his house by the waterwheel, and the windmill's foot, where Pip's cat is seen.
    for name, x, z in (("SignSpot", -3.4, -1.6), ("WheelSpot", 8.6, 0.8), ("MillerAtWheel", 8.6, 0.0),
                       ("WindmillSpot", 6.0, -8.0)):
        v.d.entity(name, (x * SCALE, 0.0, z * SCALE))

    # The villagers where the day starts, and the player at the gate.
    v.person("Mayor", "character-male-e", -3, -6, 0)
    v.person("Baker", "character-female-b", -3, -1, 270)
    v.person("Miller", "character-male-c", 5, -1, 90)
    v.person("Child", "character-female-d", 1, 1, 200)
    v.person("Guard", "character-knight", 0.6, 4.8, 0)
    v.player(-0.5, 8, 180)
    # The ending's camera, high over the square's south-east corner looking at the fountain; it is
    # the view only while the ending's camera cut holds it (sequences.py).
    cam = v.d.stable_id("entity")
    v.d.entity("EndingCamera", (16.0, 9.0, 16.0), (-0.20, 0.36, 0.08, 0.91), eid=cam)
    v.d.add(cam, "camera", fovYRadians=0.85, primary=False)
    v.sun()

    existing = [a["guid"] for a in ASSETS if a["type"] == "SceneDocument" and a["name"] == "Millbrook"]
    guid = v.d.write(existing[0] if existing else None, group="Scenes")
    mcp("project_settings_set", {"defaultSceneId": guid})
    print("Millbrook", guid)


main()
