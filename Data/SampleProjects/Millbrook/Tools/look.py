"""Millbrook's look: two shared profiles (Millbrook Environment, Millbrook Post) the village's scene
names in its settings, so the look is tuned in one place (arranged as Masquerade's look.py is).

Dusk: a sky going from a warm horizon to a deep blue overhead, a low golden sun and a cool ambient,
so the square reads warm and the lanterns tell once they are lit (the ending). TAA, GTAO, a little
bloom for the lanterns and the windows; no SSR (nothing in the village is polished) and no SSGI, so
the web build plays it too. The sun's shadows reach 18 m with most of their detail near the camera
(split 0.9), which suits a camera that follows the player closely, and the ambient is high enough
that shade stays readable rather than near black. Colours are sRGB, as entered anywhere.
"""
import xml.etree.ElementTree as ET
from scenegen import mcp, settings, num

POST = dict(exposureEV=0.0, tonemapOperator=1, bloomEnabled=True, bloomThreshold=1.2, bloomKnee=0.6,
            bloomIntensity=0.05, aoMode=1, aoStrength=1.0, aoRadius=0.8, aoIntensity=1.0, ssrEnabled=False,
            ssrIntensity=1.0, taaBlendFactor=0.97, taaVarianceGamma=1.25, fxaaSubpixel=0.75,
            autoExposure=True, autoExposureKey=0.18, autoExposureSpeed=1.5, autoExposureMinEV=-3.0,
            autoExposureMaxEV=5.0, gradingIntensity=1.0, ssgiEnabled=False, ssgiIntensity=1.0)
ENVIRONMENT = dict(ambientColor={"r": 0.40, "g": 0.42, "b": 0.58, "a": 1.0}, ambientIntensity=0.42, skyMode=0,
                   skyIntensity=0.6, skyBackgroundIntensity=0.8, skyRotation=0.0,
                   skyHorizon={"r": 0.95, "g": 0.62, "b": 0.38, "a": 1.0},
                   skyZenith={"r": 0.16, "g": 0.22, "b": 0.45, "a": 1.0},
                   skyGround={"r": 0.20, "g": 0.18, "b": 0.16, "a": 1.0}, sunIntensity=1.0, sunAngularSize=0.5,
                   turbidity=2.0, iblDiffuseIntensity=0.2, iblSpecularIntensity=0.4,
                   shadowDistance=18.0, shadowCascadeSplit=0.9, shadowFadeDistance=5.0)

ENVIRONMENTS = {"Dusk": ("Millbrook Environment", ENVIRONMENT)}

_profiles = {}


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


def look(doc, environment="Dusk"):
    """Name the shared profiles in a scene's settings: the post profile, and the environment the
    scene asks for (ENVIRONMENTS)."""
    name, values = ENVIRONMENTS[environment]
    if name not in _profiles:
        _profiles[name] = profile("Environment Profile", "EnvironmentProfileAsset", name, values)
    if "postprocess" not in _profiles:
        _profiles["postprocess"] = profile("Post Process Profile", "PostProcessProfileAsset", "Millbrook Post", POST)
    doc.settings.append(settings("environment", source=1, profile=_profiles[name]))
    doc.settings.append(settings("postprocess", source=1, profile=_profiles["postprocess"]))
