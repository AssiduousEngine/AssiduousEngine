#!/usr/bin/env python3
"""controls.py: Holdout's input map (Controls), written through the MCP tools and made the project's
default.

- Move: WASD or the left stick. Sprint: Left Shift or pressing the left stick, while moving forward.
- Look: the mouse (with the pointer locked) or the right stick.
- Fire: the left mouse button, the right shoulder, or the right trigger (Trigger, its own analog
  action: a button action takes no analog binding; Weapon.cs fires on either). Jump: Space, or
  South (A).
- Reload: R, or West (X). Weapons: 1 to 3, or North (Y) for the next.
- Pause: Escape, or Start. It is also the whole of the Menu set, the one in effect while a menu is
  up, so under a menu no other action reads pressed (without its touch button: a menu is tapped).
- On a touch screen: a floating stick on the left half moves; a drag on the right half looks
  (TouchLook, its own action, scaled so a drag across half the screen turns about 90 degrees);
  buttons at the bottom right fire (TouchFire: a phone has no pointer to lock, which Weapon.cs
  otherwise waits for), jump, reload and change weapon, and one at the top pauses. A press on a
  button also starts a drag, so the player can aim while firing. The engine draws the buttons and
  the stick once the screen is touched.
Codes are the engine's enums' values (shell::KeyCode, shell::MouseButton, shell::GamepadButton,
shell::GamepadAxis, input::StickCode).
"""
import os, sys
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mcp import call as mcp

KEY, MOUSE_BUTTON, MOUSE_DELTA, GAMEPAD_BUTTON, GAMEPAD_AXIS, GAMEPAD_STICK, COMPOSITE = 0, 1, 3, 4, 5, 6, 7
TOUCH_BUTTON, TOUCH_STICK, TOUCH_DRAG = 8, 9, 10
BUTTON, AXIS1D, AXIS2D = 0, 1, 2
A, D, R, S, W = 1, 4, 18, 19, 23
NUM1, NUM2, NUM3 = 28, 29, 30
ESCAPE, SPACE, LEFT_SHIFT = 62, 65, 109
MOUSE_LEFT = 0
SOUTH, WEST, NORTH, RIGHT_SHOULDER, LEFT_STICK_PRESS, START = 0, 2, 3, 5, 6, 14
RIGHT_TRIGGER = 5
LEFT_STICK, RIGHT_STICK = 0, 1

ACTIONS = [
    ("Move", AXIS2D, [dict(source=COMPOSITE, negX=A, posX=D, negY=S, posY=W),
                      dict(source=GAMEPAD_STICK, code=LEFT_STICK, invert=True),
                      dict(source=TOUCH_STICK, region=(0.0, 0.3, 0.45, 0.7), stickRadius=0.12, invert=True)]),
    ("Look", AXIS2D, [dict(source=MOUSE_DELTA)]),
    ("TouchLook", AXIS2D, [dict(source=TOUCH_DRAG, region=(0.45, 0.0, 0.55, 1.0), scale=1500.0)]),
    ("TouchFire", BUTTON, [dict(source=TOUCH_BUTTON, region=(0.85, 0.66, 0.14, 0.3))]),
    ("LookStick", AXIS2D, [dict(source=GAMEPAD_STICK, code=RIGHT_STICK, invert=True)]),
    ("Fire", BUTTON, [dict(source=MOUSE_BUTTON, code=MOUSE_LEFT), dict(source=GAMEPAD_BUTTON, code=RIGHT_SHOULDER)]),
    ("Trigger", AXIS1D, [dict(source=GAMEPAD_AXIS, code=RIGHT_TRIGGER)]),
    ("Jump", BUTTON, [dict(source=KEY, code=SPACE), dict(source=GAMEPAD_BUTTON, code=SOUTH),
                      dict(source=TOUCH_BUTTON, region=(0.72, 0.76, 0.12, 0.22))]),
    ("Sprint", BUTTON, [dict(source=KEY, code=LEFT_SHIFT), dict(source=GAMEPAD_BUTTON, code=LEFT_STICK_PRESS)]),
    ("Reload", BUTTON, [dict(source=KEY, code=R), dict(source=GAMEPAD_BUTTON, code=WEST),
                        dict(source=TOUCH_BUTTON, region=(0.85, 0.44, 0.14, 0.2))]),
    ("Weapon1", BUTTON, [dict(source=KEY, code=NUM1)]),
    ("Weapon2", BUTTON, [dict(source=KEY, code=NUM2)]),
    ("Weapon3", BUTTON, [dict(source=KEY, code=NUM3)]),
    ("NextWeapon", BUTTON, [dict(source=GAMEPAD_BUTTON, code=NORTH),
                            dict(source=TOUCH_BUTTON, region=(0.72, 0.52, 0.12, 0.2))]),
    ("Pause", BUTTON, [dict(source=KEY, code=ESCAPE), dict(source=GAMEPAD_BUTTON, code=START),
                       dict(source=TOUCH_BUTTON, region=(0.44, 0.0, 0.12, 0.12))]),
]


def el(parent, tag, name=None, text=None, **attrs):
    e = ET.SubElement(parent, tag, **({"name": name} if name else {}), **attrs)
    if text is not None:
        e.text = text
    return e


def binding(parent, b):
    """One binding, every field (arrays of structs are written flat)."""
    el(parent, "u8", "source", str(b["source"]))
    el(parent, "u32", "code", str(b.get("code", 0)))
    el(parent, "u32", "modifiers", "0")
    el(parent, "i32", "device", "-1")
    el(parent, "f32", "deadZone", "0.15")
    el(parent, "f32", "scale", str(b.get("scale", 1.0)))
    el(parent, "bool", "invert", "true" if b.get("invert") else "false")
    el(parent, "bool", "normalize", "true")
    for k in ("negX", "posX", "negY", "posY"):
        el(parent, "u32", k, str(b.get(k, 0)))
    region = b.get("region", (0, 0, 1, 1))  # touch: x, y, width, height, normalized over the screen
    for k, v in zip(("regionX", "regionY", "regionW", "regionH"), region):
        el(parent, "f32", k, str(v))
    el(parent, "f32", "stickRadius", str(b.get("stickRadius", 0.15)))


def main():
    assets = mcp("asset_list", {})["assets"]
    found = [a["guid"] for a in assets if a["type"] == "InputMapAsset" and a["name"] == "Controls"]
    guid = found[0] if found else mcp("asset_create", {"creator": "Input Map", "name": "Controls"})["guid"]
    root = ET.fromstring(mcp("asset_data_read", {"guid": guid})["xml"])
    payload = root.find("object[@name='payload']")
    for child in list(payload):
        if child.get("name") == "sets":
            payload.remove(child)
    menu = [(name, kind, [b for b in bindings if b["source"] != TOUCH_BUTTON]) for name, kind, bindings in ACTIONS
            if name == "Pause"]
    sets = el(payload, "array", "sets", count="2")
    write_set(sets, "Gameplay", ACTIONS)
    write_set(sets, "Menu", menu)
    mcp("asset_data_write", {"guid": guid, "xml": ET.tostring(root, encoding="unicode")})
    mcp("project_settings_set", {"defaultInputMapId": guid})
    print("Controls", guid)


def write_set(sets, set_name, set_actions):
    el(sets, "string", "name", set_name)
    el(sets, "i32", "priority", "0")
    actions = el(sets, "array", "actions", count=str(len(set_actions)))
    for name, kind, bindings in set_actions:
        el(actions, "string", "name", name)
        el(actions, "u8", "kind", str(kind))
        for k, v in (("sensitivity", "0"), ("gravity", "0")):
            el(actions, "f32", k, v)
        el(actions, "bool", "snap", "false")
        el(actions, "f32", "responseExponent", "1")
        el(actions, "bool", "timeScale", "false")
        el(actions, "u8", "interaction", "0")
        el(actions, "f32", "interactionSeconds", "0.3")
        b = el(actions, "array", "bindings", count=str(len(bindings)))
        for each in bindings:
            binding(b, each)



main()
