#!/usr/bin/env python3
"""importscripts.py [name ...]: the game's screens on the engine GUI (ui/*.xml and its sheet
ui/*.css, group UI, each checked with gui_validate first) and its C# (scripts/*.cs, group Scripts)
imported through the MCP tools, the sheet made the project's GUI sheet, then cooked: the C# project
(Holdout.csproj, made by setup.py) builds every .cs into one assembly and each class cooks from it.
Game.cs (the reserved class Game) is made the project's startup script. With names, only
those (Guest, Hud, ...). A re-run re-imports over the same assets, keeping their ids. An asset is
named by its file, so a sheet and a document may not share a name.

The documents go first, so a script can name them: a `{{Type:Name}}` in a script, a document or the
sheet (Type one of GUI, Scene, Prefab, Audio, Texture, Clip; Name may start with its model's
folder, Character_Enemy/Run) is replaced by that asset's id before it is imported (the filled copy
goes to generated/scripts/), and by the nil id when there is no such asset yet (a level not built).
"""
import os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

TYPES = {"GUI": "GuiDocumentAsset", "Scene": "SceneDocument", "Prefab": "PrefabDocument", "Audio": "AudioClipAsset",
         "Texture": "TextureAsset", "Clip": "AnimationClipAsset"}
NIL = "00000000-0000-0000-0000-000000000000"
FILLED = os.path.join(HERE, "generated", "scripts")


def filled(path):
    """The script with its {{Type:Name}} ids filled in: the original when it names none."""
    text = open(path).read()
    if "{{" not in text:
        return path
    assets = mcp("asset_list", {})["assets"]

    def one(m):
        # A name may carry its model's group, as the clips of the kit's characters share names:
        # {{Clip:Character_Enemy/Run}} is the Run clip under Models/Characters/Character_Enemy.
        model, _, name = m.group(2).rpartition("/")
        found = [a["guid"] for a in assets if a["type"] == TYPES[m.group(1)] and a["name"] == name and
                 (not model or (a.get("group") or "").endswith("/" + model))]
        return found[0] if found else NIL

    os.makedirs(FILLED, exist_ok=True)
    out = os.path.join(FILLED, os.path.basename(path))
    open(out, "w").write(re.sub(r"\{\{(\w+):([\w/]+)\}\}", one, text))
    return out


only = set(sys.argv[1:])
sheets = []
for folder, ext, group in (("ui", ".xml", "UI"), ("ui", ".css", "UI"), ("scripts", ".cs", "Scripts")):
    if not os.path.isdir(os.path.join(HERE, folder)):
        continue
    for f in sorted(os.listdir(os.path.join(HERE, folder))):
        name, e = os.path.splitext(f)
        if e != ext or (only and name not in only):
            continue
        source = os.path.join(HERE, folder, f)
        if ext in (".xml", ".css"):
            # Checked as its cook will check it: a problem stops the import.
            check = mcp("gui_validate", {"source": open(filled(source)).read(), "kind": "document" if ext == ".xml" else "sheet"})
            if not check["valid"] or check["diagnostics"]:
                sys.exit("%s: %s" % (f, check["diagnostics"]))
        r = mcp("asset_import", {"source": filled(source), "group": group})
        print(group + "/" + name, r["guid"])
        if ext == ".css":
            sheets.append(r["guid"])
        if name == "Game":
            mcp("project_settings_set", {"startupScriptId": r["guid"]})
if sheets:
    mcp("project_settings_set", {"guiStyleSheetIds": sheets}) # the game's look, over the GUI's theme
cooked = mcp("asset_cook", {})
print(cooked)
if cooked.get("failed"):
    sys.exit("the cook failed: read the log (log_read, category Script)")
