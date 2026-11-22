#!/usr/bin/env python3
"""kenney.py [packs-folder]: copies the Kenney models Millbrook uses out of the downloaded packs into
generated/ (where importmodels.py imports them from), and each pack's licence into ../Licenses.

The packs are Kenney's (https://kenney.nl), each a zip unpacked into a folder of its own name in
`packs-folder` (default: $KENNEY_DIR, else ~/Downloads/kenney): kenney_mini-characters,
kenney_fantasy-town-kit_2.0, kenney_nature-kit, kenney_mini-forest_1.0. All are CC0. The models are
their GLB versions, with the shared texture beside them where a pack has one.

    python3 kenney.py ~/Downloads/kenney
"""
import os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
PACKS = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("KENNEY_DIR", os.path.expanduser("~/Downloads/kenney"))

# The cast and the player: Mini Characters' skinned people, all on one rig with the same clips.
CHARACTERS = ("character-male-a", "character-female-b", "character-male-c", "character-female-d",
              "character-male-e", "character-knight")

# (pack folder, the folder its models land in under generated/, the models, None for every one)
SETS = (
    ("kenney_mini-characters", "Characters", CHARACTERS),
)

# Each pack's licence, as Licenses/ keeps it.
LICENCES = (("kenney_mini-characters", "Kenney-MiniCharacters-CC0.txt"),
            ("kenney_fantasy-town-kit_2.0", "Kenney-FantasyTownKit-CC0.txt"),
            ("kenney_nature-kit", "Kenney-NatureKit-CC0.txt"),
            ("kenney_mini-forest_1.0", "Kenney-MiniForest-CC0.txt"))


def glb_folder(pack):
    for root, dirs, files in os.walk(os.path.join(PACKS, pack)):
        if os.path.basename(root) == "GLB format":
            return root
    raise SystemExit("no 'GLB format' folder in %s" % os.path.join(PACKS, pack))


for pack, target, names in SETS:
    source = glb_folder(pack)
    out = os.path.join(HERE, "generated", target)
    os.makedirs(out, exist_ok=True)
    wanted = names if names is not None else sorted(f[:-4] for f in os.listdir(source) if f.endswith(".glb"))
    for name in wanted:
        shutil.copy2(os.path.join(source, name + ".glb"), os.path.join(out, name + ".glb"))
    textures = os.path.join(source, "Textures")
    if os.path.isdir(textures):
        shutil.copytree(textures, os.path.join(out, "Textures"), dirs_exist_ok=True)
    print("%s: %d models into generated/%s" % (pack, len(wanted), target))

licences = os.path.join(HERE, "..", "Licenses")
os.makedirs(licences, exist_ok=True)
for pack, name in LICENCES:
    shutil.copy2(os.path.join(PACKS, pack, "License.txt"), os.path.join(licences, name))
print("licences into Licenses/")
