#!/usr/bin/env python3
"""sequences.py: Millbrook's sequences, written through the editor's MCP (sequence_edit), into
Sequences/.

- Sequences/CartRolls: once its wheel is back on, the miller's cart rolls from where it stood by the
  stalls into the square, beside the benches, in four seconds. Its one part is named Cart, which no
  speaker is, so a conversation's `sequence` step binds it to the level entity named Cart
  (village.py); the miller's conversation plays it and waits for it (Mill.dialogue).

- Sequences/FairOpens: the ending, which Millbrook.as plays once the mayor has opened the fair. The
  view cuts to the ending's camera over the square (EndingCamera, a level entity), the lanterns
  light (the LightLanterns event, which the game handles) to the chime, the mayor and Pip speak on a
  Dialogue Lines track (the cast's speakers; each line holds the play until it is read), and the
  picture fades to black for the credits.

Positions are the scene's, in metres (village.py's cells times two).
"""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scenegen import mcp

START = (6.0, 0.0, -6.0)  # where village.py stands the cart
END = (3.0, 0.0, 1.5)


def sequence(name):
    listed = mcp("asset_list", {})["assets"]
    found = [a["guid"] for a in listed if a["type"] == "SequenceAsset" and a["name"] == name]
    if found:
        return found[0], False
    return mcp("asset_create", {"creator": "Sequence", "name": name, "group": "Sequences"})["guid"], True


def fresh(name):
    """The sequence `name`, made the first time and emptied each time after, so it is written anew."""
    guid, made = sequence(name)
    if not made:
        read = mcp("sequence_read", {"guid": guid})
        edits = [{"op": "remove_track", "track": 0} for _ in read.get("tracks", [])]
        edits += [{"op": "remove_part", "name": part} for part in read.get("parts", [])]
        if edits:
            mcp("sequence_edit", {"guid": guid, "edits": edits})
    return guid


def cart_rolls():
    guid = fresh("CartRolls")
    edits = [{"op": "length", "seconds": 4.0},
             {"op": "add_part", "name": "Cart"},
             {"op": "add_track", "kind": "Property", "part": "Cart", "component": "Transform", "property": "position"}]
    for channel, a, b in zip("xyz", START, END):
        edits.append({"op": "key", "track": 0, "channel": channel, "time": 0.0, "value": a})
        edits.append({"op": "key", "track": 0, "channel": channel, "time": 4.0, "value": b})
    r = mcp("sequence_edit", {"guid": guid, "edits": edits})
    print("Sequences/CartRolls", guid, r.get("written", r))


def asset(asset_type, name):
    found = [a["guid"] for a in mcp("asset_list", {})["assets"] if a["type"] == asset_type and a["name"] == name]
    if not found:
        raise SystemExit("no %s %s (run sounds.py and dialogue.py first)" % (asset_type, name))
    return found[0]


# The ending's lines: (start, length on the timeline, speaker, mood, text). Each holds the play at
# its end until the player goes on.
ENDING_LINES = ((2.8, 3.0, "mayor", "happy", "Friends! The sign is up, the cart is mended, and Whiskers is home."),
                (6.2, 2.6, "mayor", "happy", "So light the lanterns, strike up the tune: Millbrook's fair is open!"),
                (9.2, 2.2, "pip", "happy", "Look, Whiskers, all the lights!"))


def fair_opens():
    guid = fresh("FairOpens")
    edits = [{"op": "length", "seconds": 14.0},
             {"op": "add_part", "name": "EndingCamera"},
             {"op": "add_track", "kind": "Camera Cut", "part": "EndingCamera"},          # track 0
             {"op": "section", "track": 0, "start": 0.0, "length": 14.0},
             {"op": "add_track", "kind": "Event"},                                      # track 1
             {"op": "key", "track": 1, "time": 2.0},
             {"op": "set", "track": 1, "path": "keys.0.name", "value": "LightLanterns"},
             {"op": "add_track", "kind": "Sound"},                                      # track 2
             {"op": "section", "track": 2, "start": 2.0, "length": 2.6,
              "fields": {"clip": asset("AudioClipAsset", "Chime")}},
             {"op": "add_track", "kind": "Dialogue Lines"},                             # track 3
             {"op": "set", "track": 3, "path": "cast", "value": asset("DialogueCastAsset", "Cast")},
             {"op": "add_track", "kind": "Fade"},                                       # track 4
             {"op": "key", "track": 4, "channel": "amount", "time": 0.0, "value": 0.0},
             {"op": "key", "track": 4, "channel": "amount", "time": 12.0, "value": 0.0},
             {"op": "key", "track": 4, "channel": "amount", "time": 14.0, "value": 1.0}]
    for start, length, speaker, mood, text in ENDING_LINES:
        edits.append({"op": "section", "track": 3, "start": start, "length": length,
                      "fields": {"speaker": speaker, "mood": mood, "text": text}})
    r = mcp("sequence_edit", {"guid": guid, "edits": edits})
    print("Sequences/FairOpens", guid, [t["kindName"] for t in r["tracks"]])


cart_rolls()
fair_opens()
print(mcp("asset_cook", {}))
