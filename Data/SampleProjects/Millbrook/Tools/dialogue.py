#!/usr/bin/env python3
"""dialogue.py: Millbrook's conversations, written through the editor's MCP.

- The dialogue files (dialogue/*.dialogue: Square, Bakery, Mill), imported into Dialogue/.
- The cast (Dialogue/Cast): the player ("you") and the five villagers, each with their shown name,
  moods, the clips `play` may name (from their own model) and their entity in the square, which is
  their model's name (an instance's root takes it; village.py).
- The dialogue assets (Dialogue/Staging): the village (the level the conversations check their
  entity names against), the three files, so a jump reaches across them, and what the staging plays:
  the sounds (sounds.py) and the cart's sequence (sequences.py).
- The stock box's sheet (box/MillbrookBox.css, UI/MillbrookBox), checked with gui_validate.
- Each file names the cast and the dialogue assets; the plugin's settings name them as a new
  file's defaults, declare the game's commands (walk, errand) and its flags (fair_*), and turn on
  the stock box with Millbrook's sheet.

Run with the editor open on the project, after village.py, sounds.py, sequences.py and
importscripts.py; then checks every file with dialogue_validate.
"""
import os, sys
import xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

# (id, shown name, plate colour, moods, model, clips `play` may name)
SPEAKERS = (
    ("you", "You", (0.35, 0.45, 0.65), ("neutral",), "character-male-a", ()),
    ("mayor", "Mayor Hale", (0.55, 0.35, 0.20), ("neutral", "happy", "worried"), "character-male-e",
     ("emote-yes", "emote-no")),
    ("baker", "Marta the Baker", (0.70, 0.30, 0.28), ("neutral", "happy", "worried"), "character-female-b",
     ("interact-right", "emote-yes")),
    ("miller", "Old Tobin", (0.40, 0.50, 0.30), ("neutral", "happy", "worried"), "character-male-c",
     ("pick-up", "emote-yes")),
    ("pip", "Pip", (0.80, 0.55, 0.25), ("neutral", "happy", "worried"), "character-female-d",
     ("emote-yes", "emote-no")),
    ("guard", "Sergeant Brook", (0.35, 0.38, 0.45), ("neutral", "happy", "stern"), "character-knight",
     ("emote-yes", "emote-no")),
)
FILES = ("Square", "Bakery", "Mill")
COMMANDS = ("walk <who:speaker> to <where:entity> [and wait]", "errand <where:entity> leaves")
PROVIDED = ("fair_*",)


def assets():
    return mcp("asset_list", {})["assets"]


def find(listed, asset_type, name, group=None):
    found = [a["guid"] for a in listed
             if a["type"] == asset_type and a["name"] == name and (group is None or a.get("group") == group)]
    return found[0] if found else None


def el(parent, tag, name=None, text=None, **attrs):
    e = ET.SubElement(parent, tag, **({"name": name} if name else {}), **attrs)
    if text is not None:
        e.text = text
    return e


def rewrite(guid, fill):
    """Reads an asset's record, lets `fill` change its payload, writes it back."""
    root = ET.fromstring(mcp("asset_data_read", {"guid": guid})["xml"])
    fill(root.find("object[@name='payload']"))
    mcp("asset_data_write", {"guid": guid, "xml": ET.tostring(root, encoding="unicode")})


def main():
    NIL = "00000000-0000-0000-0000-000000000000"
    listed = assets()
    files = {}
    for name in FILES:
        r = mcp("asset_import", {"source": os.path.join(HERE, "dialogue", name + ".dialogue"), "group": "Dialogue"})
        files[name] = r["guid"]
        print("Dialogue/" + name, r["guid"])
    listed = assets()
    cast = find(listed, "DialogueCastAsset", "Cast") or mcp("asset_create", {"creator": "Dialogue Cast", "name": "Cast",
                                                                              "group": "Dialogue"})["guid"]
    staging = find(listed, "DialogueAssetListAsset", "Staging") or mcp(
        "asset_create", {"creator": "Dialogue Assets", "name": "Staging", "group": "Dialogue"})["guid"]

    def speakers(payload):
        for child in list(payload):
            if child.get("name") == "speakers":
                payload.remove(child)
        arr = el(payload, "array", "speakers", count=str(len(SPEAKERS)))
        for sid, shown, colour, moods, model, clips in SPEAKERS:
            group = "Models/Characters/" + model
            o = el(arr, "object")
            el(o, "string", "id", sid)
            el(o, "string", "shownName", shown)
            el(o, "bool", "plate", "true")
            c = el(o, "object", "plateColor")
            for k, v in zip("xyzw", colour + (1.0,)):
                el(c, "f32", k, str(v))
            m = el(o, "array", "moods", count=str(len(moods)))
            for mood in moods:
                mo = el(m, "object")
                el(mo, "string", "name", mood)
                el(mo, "string", "portrait", NIL)
            el(o, "string", "portrait", NIL)
            cl = el(o, "array", "clips", count=str(len(clips)))
            for clip in clips:
                guid = find(listed, "AnimationClipAsset", clip, group)
                if guid is None:
                    raise SystemExit("no clip %s in %s (run importmodels.py)" % (clip, group))
                el(cl, "string", None, guid)
            el(o, "string", "talk", NIL)
            el(o, "string", "voice", NIL)
            el(o, "string", "entity", model)

    rewrite(cast, speakers)
    village = find(listed, "SceneDocument", "Millbrook")
    entries = [("village", village)] + [(name.lower(), files[name]) for name in FILES]
    for name in ("Rumble", "Splash", "Knock", "Meow", "Chime"):
        entries.append((name.lower(), find(listed, "AudioClipAsset", name)))
    entries.append(("cart_rolls", find(listed, "SequenceAsset", "CartRolls")))
    missing = [name for name, guid in entries if guid is None]
    if missing:
        raise SystemExit("not made yet: %s (run village.py, sounds.py and sequences.py)" % ", ".join(missing))

    def staging_entries(payload):
        for child in list(payload):
            if child.get("name") == "entries":
                payload.remove(child)
        arr = el(payload, "array", "entries", count=str(len(entries)))
        for name, guid in entries:
            o = el(arr, "object")
            el(o, "string", "name", name)
            el(o, "string", "asset", guid)

    rewrite(staging, staging_entries)

    def names(payload):
        payload.find("string[@name='cast']").text = cast
        payload.find("string[@name='assets']").text = staging

    for name in FILES:
        rewrite(files[name], names)
    sheet_source = os.path.join(HERE, "box", "MillbrookBox.css")
    check = mcp("gui_validate", {"source": open(sheet_source).read(), "kind": "sheet"})
    if not check["valid"] or check["diagnostics"]:
        raise SystemExit("MillbrookBox.css: %s" % check["diagnostics"])
    sheet = mcp("asset_import", {"source": sheet_source, "group": "UI"})["guid"]
    mcp("plugin_settings_set", {"id": "dialogue", "settings": {
        "cast": cast, "assets": staging, "commands": list(COMMANDS), "providedFlags": list(PROVIDED),
        "stockBox": True, "stockBoxSheet": sheet}})
    report = mcp("dialogue_validate", {})
    for f in report["files"]:
        for p in f.get("problems", []):
            print("  %s:%d: %s: %s" % (f["name"], p["line"], p["severity"], p["message"]))
    print("errors:", report["errors"])
    print(mcp("asset_cook", {}))


main()
