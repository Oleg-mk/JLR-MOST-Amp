"""Every symbol has a footprint, the footprint exists, and its pad numbers match the symbol's pins."""
import glob, os, re, sys

HW = sys.argv[1] if len(sys.argv) > 1 else ".."
KFP = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/share/kicad/footprints"

parts = {}  # ref -> dict(fp, pins)
for f in glob.glob(os.path.join(HW, "*.kicad_sch")):
    s = open(f, encoding="utf-8").read()
    for m in re.finditer(r'\(symbol \(lib_id "([^"]+)"\).*?\(instances', s, re.S):
        blk = m.group(0)
        ref = re.search(r'\(property "Reference" "([^"]+)"', blk).group(1)
        if ref.startswith("#"):
            continue
        fp = re.search(r'\(property "Footprint" "([^"]*)"', blk).group(1)
        p = parts.setdefault(ref, dict(fp=fp, pins=set(), lib_id=m.group(1)))
        p["pins"] |= set(re.findall(r'\(pin "([^"]+)" \(uuid', blk))

bad = 0
for ref in sorted(parts, key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r) or 0))):
    p = parts[ref]
    if not p["fp"]:
        print("%-5s no footprint" % ref)
        bad += 1
        continue
    lib, name = p["fp"].split(":")
    path = os.path.join(HW, lib + ".pretty", name + ".kicad_mod") if lib == "jlr_most_amp" else os.path.join(KFP, lib + ".pretty", name + ".kicad_mod")
    if not os.path.exists(path):
        print("%-5s %s: file not found" % (ref, p["fp"]))
        bad += 1
        continue
    t = open(path, encoding="utf-8").read()
    pads = set(n for n in re.findall(r'\(pad\s+"([^"]*)"', t) if n)
    missing = p["pins"] - pads
    extra = pads - p["pins"]
    if missing or extra:
        print("%-5s %s: pins without pads %s, pads without pins %s" % (ref, p["fp"], sorted(missing), sorted(extra)))
        bad += bool(missing)
print("parts:", len(parts), "problems:", bad)
