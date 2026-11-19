#!/usr/bin/env python3
"""setup.py <kit folder>: the Holdout project and its art, through the MCP tools.

Creates the project (with the starter content: Roboto as the UI font, the default sky, the
primitives) when it does not exist, its C# project (Code/Holdout.csproj, which every .cs in
Code builds into), and imports the models the game uses from Quaternius's Toon Shooter Game Kit
(CC0): the three characters into Models/Characters, the weapons into Models/Guns and the yard's
pieces into Models/Yard, each model a group of its own. It also measures each yard piece from the
kit's files (its nodes' transforms applied) into kit_sizes.json, which yard.py sizes the colliders
from. They are copied into generated/Kit first,
a folder the MCP host may read (the import then copies each into Sources/). A rerun imports over
the same assets, keeping their ids.

The kit's Google Drive folder refuses scripted downloads, so download it from
quaternius.com/packs/toonshootergamekit.html and pass the folder that holds Characters,
Environment and Guns:

    python3 setup.py "~/Downloads/Toon Shooter Game Kit - Dec 2022"
"""
import itertools, json, os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mcp

CHARACTERS = ["Character_Enemy", "Character_Hazmat", "Character_Soldier"]
GUNS = ["Pistol", "SMG", "Shotgun"]
YARD = [
    "Barrier_Fixed", "Barrier_Large", "Barrier_Single", "Barrier_Trash",
    "BrickWall_1", "BrickWall_2",
    "CardboardBoxes_1", "CardboardBoxes_2", "CardboardBoxes_3", "CardboardBoxes_4",
    "Container_Long", "Container_Small", "Crate",
    "Debris_BrokenCar", "Debris_Papers_1", "Debris_Papers_2", "Debris_Pile", "Debris_Tires",
    "ExplodingBarrel", "ExplodingBarrel_Spilled",
    "Fence", "Fence_Long", "MetalFence",
    "GasCan", "GasTank", "Health", "Pallet", "Pallet_Broken", "Pipes",
    "SackTrench", "SackTrench_Small", "Sign", "StreetLight",
    "Structure_1", "Structure_2", "Structure_3", "Structure_4",
    "TrafficCone", "TrashContainer", "TrashContainer_Open",
    "Tree_1", "Tree_2", "Tree_3", "Tree_4",
    "WaterTank_Floor", "WaterTank_Platform", "WoodPlanks",
]
PARTS = (("Characters", CHARACTERS, "Models/Characters"), ("Guns", GUNS, "Models/Guns"),
         ("Environment", YARD, "Models/Yard"))


def rotate(q, v):
    """`v` turned by the quaternion `q` (x, y, z, w)."""
    x, y, z, w = q
    vx, vy, vz = v
    ix, iy, iz = w * vx + y * vz - z * vy, w * vy + z * vx - x * vz, w * vz + x * vy - y * vx
    iw = -x * vx - y * vy - z * vz
    return (ix * w - iw * x - iy * z + iz * y, iy * w - iw * y - iz * x + ix * z, iz * w - iw * z - ix * y + iy * x)


def bounds(path):
    """The model's axis-aligned bounds in its own space: every mesh's corners through its nodes."""
    gltf = json.load(open(path))
    points = []

    def walk(index, outer):
        node = gltf["nodes"][index]
        t, r, s = node.get("translation", [0, 0, 0]), node.get("rotation", [0, 0, 0, 1]), node.get("scale", [1, 1, 1])

        def place(p):
            p = rotate(r, (p[0] * s[0], p[1] * s[1], p[2] * s[2]))
            return outer((p[0] + t[0], p[1] + t[1], p[2] + t[2]))

        if "mesh" in node:
            for primitive in gltf["meshes"][node["mesh"]]["primitives"]:
                accessor = gltf["accessors"][primitive["attributes"]["POSITION"]]
                points.extend(place(c) for c in itertools.product(*zip(accessor["min"], accessor["max"])))
        for child in node.get("children", []):
            walk(child, place)

    for index in gltf["scenes"][gltf.get("scene", 0)]["nodes"]:
        walk(index, lambda p: p)
    return {"min": [round(min(p[i] for p in points), 3) for i in range(3)],
            "max": [round(max(p[i] for p in points), 3) for i in range(3)]}


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    kit = os.path.expanduser(sys.argv[1])
    for folder, _, _ in PARTS:
        if not os.path.isdir(os.path.join(kit, folder, "glTF")):
            raise SystemExit("%s has no %s/glTF: pass the kit's own folder" % (kit, folder))

    if not os.path.exists(os.path.join(mcp.PROJECT, "Project.xml")):
        print(mcp.call("project_create", {"directory": mcp.PROJECT, "name": "Holdout"}))
        print(mcp.call("project_open", {"directory": mcp.PROJECT}))

    assets = mcp.call("asset_list", {})["assets"]
    if not any(a["type"] == "ScriptAssemblyAsset" for a in assets):
        print("C# project", mcp.call("asset_create", {"creator": "C# Project", "name": "Holdout"})["guid"])

    for folder, names, group in PARTS:
        for name in names:
            original = os.path.join(kit, folder, "glTF", name + ".gltf")
            if not os.path.exists(original):
                raise SystemExit("missing from the kit: " + original)
            source = os.path.join(HERE, "generated", "Kit", name + ".gltf")
            os.makedirs(os.path.dirname(source), exist_ok=True)
            shutil.copyfile(original, source)
            imported = mcp.call("asset_import", {"source": source, "group": group})
            print(group + "/" + name, imported.get("guid"))
    sizes = {name: bounds(os.path.join(kit, "Environment", "glTF", name + ".gltf")) for name in YARD}
    with open(os.path.join(HERE, "kit_sizes.json"), "w") as out:
        json.dump(sizes, out, indent=1, sort_keys=True)
        out.write("\n")
    print(mcp.call("asset_cook", {}))


main()
