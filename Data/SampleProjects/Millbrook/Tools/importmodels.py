#!/usr/bin/env python3
"""importmodels.py [Name ...]: the Kenney models kenney.py copied into generated/, imported through
the editor's MCP, then cooked: the cast's and the player's characters (generated/Characters) into
Models/Characters, the village's pieces (generated/Town) into Models/Town and what grows round it
(generated/Nature) into Models/Nature, each model its own group (Models/Characters/character-knight). With names,
only those. A re-run re-imports over the same assets, keeping their ids.

    python3 kenney.py ~/Downloads/kenney
    python3 importmodels.py
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

# Each folder of copied models, and the group its models land under.
FOLDERS = (("Characters", "Models/Characters"), ("Town", "Models/Town"), ("Nature", "Models/Nature"))

only = set(sys.argv[1:])
for folder, group in FOLDERS:
    path = os.path.join(HERE, "generated", folder)
    if not os.path.isdir(path):
        continue
    for f in sorted(os.listdir(path)):
        name, ext = os.path.splitext(f)
        if ext != ".glb" or (only and name not in only):
            continue
        r = mcp("asset_import", {"source": os.path.join(path, f), "group": group})
        print(group + "/" + name, r["guid"])
print(mcp("asset_cook", {}))
