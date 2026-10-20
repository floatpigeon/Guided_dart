#!/usr/bin/env python3
"""Generate an uncalibrated mono bring-up scene from the SDK SC035HGS schema."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--sdk", type=Path, default=Path("/mnt/mydata/kRTOSSDK"))
parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent / "isp")
args = parser.parse_args()
source = args.sdk / "src/rtsmart/mpp/userapps/src/sensor/config"
target = args.output
target.mkdir(parents=True, exist_ok=True)
prefix = "sc038hgs-640x480"
hashes = {}
for suffix in (".xml", "_auto.json", "_manual.json"):
    original = source / ("sc035hgs-640x480" + suffix)
    hashes[original.name] = hashlib.sha256(original.read_bytes()).hexdigest()
    if suffix == ".xml":
        tree = ET.parse(original)
        root = tree.getroot()
        for tag, value in (("sensor_name", "sc038hgs"), ("sample_name", "dart_bringup_uncalibrated")):
            element = root.find("header/" + tag)
            element.text = value
            element.set("size", f"[1 {len(value)}]")
        # The template FPS entries are metadata, not a measurement or a clock
        # setting for SC038HGS. Retain the parser's schema with an unknown FPS.
        rates = root.find("header/resolution/cell/framerate")
        rates.set("size", "[1 1]")
        for child in list(rates)[1:]:
            rates.remove(child)
        rates[0].find("name").text = "FPS_UNKNOWN"
        rates[0].find("name").set("size", "[1 11]")
        rates[0].find("fps").text = "[ 0]"
        for element in root.iter():
            if element.tag in ("manualccMatrix", "ccMatrix"):
                element.text = "[1 0 0 0 1 0 0 0 1]"
            elif element.tag in ("manualccOffsets", "ccOffsets"):
                element.text = "[0 0 0]"
            elif element.tag == "blsData":
                element.text = "[0 0 0 0]"
        ET.indent(tree, space="   ")
        tree.write(target / (prefix + suffix), encoding="utf-8", xml_declaration=True)
    else:
        data = json.loads(original.read_text())
        if suffix == "_auto.json":
            def disable(value):
                if isinstance(value, dict):
                    if "enable" in value:
                        value["enable"] = False
                    for item in value.values():
                        disable(item)
                elif isinstance(value, list):
                    for item in value:
                        disable(item)
            disable(data)
        else:
            for module in data["root"][0]["driver"]:
                # Only neutral CPROC remains enabled: zero chroma saturation,
                # unit contrast and no brightness shift. All automatic and
                # uncalibrated optical/noise corrections are disabled.
                if "enable" in module:
                    module["enable"] = module["class"] == "CCproc"
                if module["class"] == "Bls":
                    module["bls"] = [0, 0, 0, 0]
                if module["class"] == "CCproc":
                    module.update(saturation=0, contrast=1, bright=0, hue=0)
        (target / (prefix + suffix)).write_text(json.dumps(data, indent=2) + "\n")
(target / "source-sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
print(target)
