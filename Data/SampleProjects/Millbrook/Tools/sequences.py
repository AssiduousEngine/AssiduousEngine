#!/usr/bin/env python3
"""sequences.py: Millbrook's sequences, written through the editor's MCP (sequence_edit), into
Sequences/.

- Sequences/CartRolls: once its wheel is back on, the miller's cart rolls from where it stood by the
  stalls into the square, beside the benches, in four seconds. Its one part is named Cart, which no
  speaker is, so a conversation's `sequence` step binds it to the level entity named Cart
  (village.py); the miller's conversation plays it and waits for it (Mill.dialogue).

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


def cart_rolls():
    guid, fresh = sequence("CartRolls")
    if not fresh:
        # written afresh each time: what it held goes
        read = mcp("sequence_read", {"guid": guid})
        edits = [{"op": "remove_track", "track": 0} for _ in read.get("tracks", [])]
        edits += [{"op": "remove_part", "part": 0} for _ in read.get("parts", [])]
        if edits:
            mcp("sequence_edit", {"guid": guid, "edits": edits})
    edits = [{"op": "length", "seconds": 4.0},
             {"op": "add_part", "name": "Cart"},
             {"op": "add_track", "kind": "Property", "part": "Cart", "component": "Transform", "property": "position"}]
    for channel, a, b in zip("xyz", START, END):
        edits.append({"op": "key", "track": 0, "channel": channel, "time": 0.0, "value": a})
        edits.append({"op": "key", "track": 0, "channel": channel, "time": 4.0, "value": b})
    r = mcp("sequence_edit", {"guid": guid, "edits": edits})
    print("Sequences/CartRolls", guid, r.get("written", r))


cart_rolls()
print(mcp("asset_cook", {}))
