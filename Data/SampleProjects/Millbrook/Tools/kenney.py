#!/usr/bin/env python3
"""kenney.py [packs-folder]: copies the Kenney models Millbrook uses out of the downloaded packs into
generated/ (where importmodels.py imports them from), and each pack's licence into ../Licenses.

The packs are Kenney's (https://kenney.nl), each a zip unpacked into a folder of its own name in
`packs-folder` (default: $KENNEY_DIR, else ~/Downloads/kenney): kenney_mini-characters,
kenney_fantasy-town-kit_2.0, kenney_nature-kit. All are CC0. The models are their GLB versions
(Nature Kit keeps its in "GLTF format"), with the shared texture beside them where a pack has one.

Nature Kit's models (exported by UniGLTF 1.27) are not valid glTF as they come: each scene lists as
a root a node that is another node's child, which a reader refuses (cgltf does). The copy lists the
scene's true roots instead (repair_scenes); nothing else in a file changes.

    python3 kenney.py ~/Downloads/kenney
"""
import json, os, shutil, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
PACKS = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("KENNEY_DIR", os.path.expanduser("~/Downloads/kenney"))

# The cast and the player: Mini Characters' skinned people, all on one rig with the same clips.
CHARACTERS = ("character-male-a", "character-female-b", "character-male-c", "character-female-d",
              "character-male-e", "character-knight")

# The village: houses built from walls and roofs on the kit's 1-unit grid, the square's fountain and
# paving, the fair's stalls, carts and banners, the mills, fences, hedges and lanterns.
TOWN = ("wall", "wall-door", "wall-doorway-round", "wall-window-shutters", "wall-window-glass", "wall-corner",
        "wall-wood", "wall-wood-door", "wall-wood-doorway-round", "wall-wood-window-shutters",
        "wall-wood-window-glass", "wall-wood-corner", "wall-wood-detail-cross",
        "roof-high", "roof-high-gable", "roof-high-gable-end", "roof-high-corner", "roof-high-window",
        "roof", "roof-gable", "roof-gable-end", "roof-corner", "chimney", "chimney-base", "chimney-top",
        "road", "road-edge", "road-corner", "road-corner-inner", "road-bend",
        "fountain-round-detail", "stall-red", "stall-green", "stall", "stall-bench", "stall-stool",
        "cart", "cart-high", "wheel", "banner-red", "banner-green", "lantern", "overhang",
        "fence", "fence-gate", "fence-curved", "hedge", "hedge-large", "hedge-large-curved", "hedge-gate",
        "watermill-wide", "windmill", "pillar-stone", "pillar-wood", "planks", "stairs-stone",
        "rock-large", "rock-small", "tree", "tree-high", "tree-high-round", "poles")

# Around the village: grass and a stream, trees, bushes, flowers and stones.
NATURE = ("ground_grass", "ground_riverStraight", "ground_riverBend", "ground_riverSide", "ground_riverOpen",
          "ground_pathStraight", "tree_oak", "tree_default", "tree_fat", "tree_pineRoundA", "tree_pineTallA",
          "plant_bushLarge", "plant_bush", "flower_redA", "flower_yellowA", "flower_purpleA", "grass_large",
          "rock_largeA", "rock_smallA", "rock_tallA", "log_stack", "stump_round", "mushroom_redGroup")

# (pack folder, the folder its models land in under generated/, the models, None for every one)
SETS = (
    ("kenney_mini-characters", "Characters", CHARACTERS),
    ("kenney_fantasy-town-kit_2.0", "Town", TOWN),
    ("kenney_nature-kit", "Nature", NATURE),
)

# Each pack's licence, as Licenses/ keeps it.
LICENCES = (("kenney_mini-characters", "Kenney-MiniCharacters-CC0.txt"),
            ("kenney_fantasy-town-kit_2.0", "Kenney-FantasyTownKit-CC0.txt"),
            ("kenney_nature-kit", "Kenney-NatureKit-CC0.txt"))


def repair_scenes(path):
    """Lists each scene's true roots (nodes with no parent) when it lists a child as one."""
    data = open(path, "rb").read()
    length, kind = struct.unpack_from("<I4s", data, 12)
    if kind != b"JSON":
        return False
    gltf = json.loads(data[20:20 + length])
    children = {c for node in gltf.get("nodes", []) for c in node.get("children", [])}
    roots = [i for i in range(len(gltf.get("nodes", []))) if i not in children]
    changed = False
    for scene in gltf.get("scenes", []):
        if any(n in children for n in scene.get("nodes", [])):
            scene["nodes"] = roots
            changed = True
    if not changed:
        return False
    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * ((4 - len(text) % 4) % 4)
    rest = data[20 + length:]
    out = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(text) + len(rest)) + struct.pack("<I4s", len(text), b"JSON") + text + rest
    open(path, "wb").write(out)
    return True


def glb_folder(pack):
    for root, dirs, files in os.walk(os.path.join(PACKS, pack)):
        if os.path.basename(root) in ("GLB format", "GLTF format") and any(f.endswith(".glb") for f in files):
            return root
    raise SystemExit("no 'GLB format' folder in %s" % os.path.join(PACKS, pack))


for pack, target, names in SETS:
    source = glb_folder(pack)
    out = os.path.join(HERE, "generated", target)
    os.makedirs(out, exist_ok=True)
    wanted = names if names is not None else sorted(f[:-4] for f in os.listdir(source) if f.endswith(".glb"))
    repaired = 0
    for name in wanted:
        target_path = os.path.join(out, name + ".glb")
        shutil.copy2(os.path.join(source, name + ".glb"), target_path)
        repaired += 1 if repair_scenes(target_path) else 0
    textures = os.path.join(source, "Textures")
    if os.path.isdir(textures):
        shutil.copytree(textures, os.path.join(out, "Textures"), dirs_exist_ok=True)
    print("%s: %d models into generated/%s%s" % (pack, len(wanted), target,
                                                 " (%d scenes repaired)" % repaired if repaired else ""))

licences = os.path.join(HERE, "..", "Licenses")
os.makedirs(licences, exist_ok=True)
for pack, name in LICENCES:
    shutil.copy2(os.path.join(PACKS, pack, "License.txt"), os.path.join(licences, name))
print("licences into Licenses/")
