"""Holdout's look: two shared profiles (Holdout Environment, Holdout Post) the yard's scene names in
its settings, so the look is tuned in one place.

Dusk: the sun just down behind the town, an orange horizon going to a deep violet overhead, a cool
ambient, and the street lights coming on, warm, so the yard reads in pools of light with the sky
still bright behind it. TAA, GTAO and bloom on the lamps and the muzzle flashes; no SSR (nothing in
the kit is polished) and no SSGI, for the web build. Colours are sRGB, as entered anywhere.
"""
import xml.etree.ElementTree as ET
from scenegen import mcp, settings, num

POST = dict(exposureEV=0.0, tonemapOperator=1, bloomEnabled=True, bloomThreshold=1.0, bloomKnee=0.6,
            bloomIntensity=0.08, aoMode=1, aoStrength=1.0, aoRadius=0.8, aoIntensity=1.0, ssrEnabled=False,
            taaBlendFactor=0.97, taaVarianceGamma=1.25, fxaaSubpixel=0.75,
            autoExposure=False, gradingIntensity=1.0, ssgiEnabled=False)
ENVIRONMENT = dict(ambientColor={"r": 0.45, "g": 0.42, "b": 0.62, "a": 1.0}, ambientIntensity=0.18, skyMode=0,
                   skyIntensity=0.6, skyBackgroundIntensity=0.9, skyRotation=0.0,
                   skyHorizon={"r": 0.96, "g": 0.55, "b": 0.30, "a": 1.0},
                   skyZenith={"r": 0.16, "g": 0.12, "b": 0.34, "a": 1.0},
                   skyGround={"r": 0.12, "g": 0.10, "b": 0.12, "a": 1.0}, sunIntensity=1.0, sunAngularSize=0.5,
                   turbidity=2.0, iblDiffuseIntensity=0.35, iblSpecularIntensity=0.3,
                   shadowDistance=45.0, shadowCascadeSplit=0.75, shadowFadeDistance=8.0)


def profile(creator, asset_type, name, values):
    """The shared profile asset `name`, made the first time and its fields rewritten each time
    (through its envelope, as the editor reads it), so a scene's settings only name it."""
    assets = mcp("asset_list", {})["assets"]
    found = [a["guid"] for a in assets if a["name"] == name and a["type"] == asset_type]
    guid = found[0] if found else mcp("asset_create", {"creator": creator, "name": name})["guid"]
    root = ET.fromstring(mcp("asset_data_read", {"guid": guid})["xml"])
    payload = root.find("object[@name='payload']")
    for key, value in values.items():
        field = payload.find("*[@name='%s']" % key)
        if field is None:
            raise SystemExit("%s has no field %s" % (name, key))
        if isinstance(value, dict):
            for channel, v in value.items():
                field.find("*[@name='%s']" % channel).text = num(float(v))
        else:
            field.text = num(value)
    mcp("asset_data_write", {"guid": guid, "xml": ET.tostring(root, encoding="unicode")})
    return guid


def look(doc):
    """Name the shared profiles in a scene's settings."""
    environment = profile("Environment Profile", "EnvironmentProfileAsset", "Holdout Environment", ENVIRONMENT)
    post = profile("Post Process Profile", "PostProcessProfileAsset", "Holdout Post", POST)
    doc.settings.append(settings("environment", source=1, profile=environment))
    doc.settings.append(settings("postprocess", source=1, profile=post))
