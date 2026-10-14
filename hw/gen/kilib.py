import re, os
KLIB = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/share/kicad/symbols"
_cache = {}
BS = chr(92)


def block(s, i):
    depth = 0
    j = i
    instr = False
    while True:
        c = s[j]
        if instr:
            if c == BS:
                j += 2
                continue
            if c == '"':
                instr = False
        else:
            if c == '"':
                instr = True
            elif c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
                if depth == 0:
                    return s[i:j + 1]
        j += 1


def lib_symbol(lib, name):
    key = (lib, name)
    if key in _cache:
        return _cache[key]
    path = os.path.join(KLIB, lib + ".kicad_sym")
    s = open(path, encoding="utf-8").read()
    i = s.find('(symbol "%s"' % name)
    if i < 0:
        raise KeyError(key)
    blk = block(s, i)
    m = re.search(r'\(extends "([^"]+)"\)', blk)
    if m:
        raise ValueError("%s:%s extends %s" % (lib, name, m.group(1)))
    _cache[key] = blk
    return blk


PIN_RE = re.compile(r'\(pin (\w+) (\w+)\s*\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)\s*\(length ([-\d.]+)\)(.*?)\(name "([^"]*)"(.*?)\(number "([^"]*)"', re.S)


def pins(blk):
    """list of dict(unit, number, name, type, x, y, angle); unit 0 = common to all units"""
    out = []
    for m in re.finditer(r'\(symbol "([^"]+)_(\d+)_(\d+)"', blk):
        sub = block(blk, m.start())
        unit = int(m.group(2))
        for p in PIN_RE.finditer(sub):
            out.append(dict(unit=unit, type=p.group(1), x=float(p.group(3)), y=float(p.group(4)),
                            angle=float(p.group(5)), name=p.group(8), number=p.group(10)))
    return out


if __name__ == "__main__":
    import sys
    for spec in sys.argv[1:]:
        lib, name = spec.split(":")
        try:
            b = lib_symbol(lib, name)
        except Exception as e:
            print(spec, "ERR", e)
            continue
        ps = pins(b)
        print(spec, len(ps), "pins:", " ".join("%s/%s/u%d@(%g,%g,%g)" % (p["number"], p["name"], p["unit"], p["x"], p["y"], p["angle"]) for p in ps))
