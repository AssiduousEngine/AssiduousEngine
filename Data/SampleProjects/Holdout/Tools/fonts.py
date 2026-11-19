#!/usr/bin/env python3
"""fonts.py: Holdout's UI font, Russo One (fonts/RussoOne-Regular.ttf, SIL Open Font License 1.1,
Licenses/RussoOne-OFL.txt), imported through the MCP tools into Fonts as a distance-field font (sharp
at every size, as the GUI's sizes follow the screen) and made the project's default UI font, so every
label in the game uses it. A rerun imports over the same asset.

    python3 fonts.py
"""
import os, sys
import xml.etree.ElementTree as ET
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mcp

DISTANCE_FIELD = 1  # the font asset's mode: 0 bitmap sizes, 1 distance field

font = mcp.call("asset_import", {"source": os.path.join(HERE, "fonts", "RussoOne-Regular.ttf"), "group": "Fonts"})["guid"]
root = ET.fromstring(mcp.call("asset_data_read", {"guid": font})["xml"])
payload = root.find("object[@name='payload']")
payload.find("*[@name='family']").text = "Russo One"
payload.find("*[@name='mode']").text = str(DISTANCE_FIELD)
mcp.call("asset_data_write", {"guid": font, "xml": ET.tostring(root, encoding="unicode")})
mcp.call("project_settings_set", {"defaultUiFontId": font})
print("Russo One", font)
print(mcp.call("asset_cook", {}))
