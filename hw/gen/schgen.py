"""Small KiCad 10 schematic writer: symbols, short pin stubs, labels, power symbols."""
import uuid
import kilib
import symdefs

NS = uuid.UUID("6f1d2a8e-3c4b-4b8e-9a51-0b7e5d2c9a11")
PROJECT = "jlr_most_amp"
CUSTOM_LIB = "jlr_most_amp"
STUB = 2.54

POWER = {  # net -> (lib symbol, value)
    "GND": ("power:GND", "GND"),
    "+3V3": ("power:+3V3", "+3V3"),
    "+3V3_AON": ("power:+3V3", "+3V3_AON"),
    "+3V3A": ("power:+3V3", "+3V3A"),
    "+3V0_TOS": ("power:+3V3", "+3V0_TOS"),
    "+12V": ("power:+12V", "+12V"),
    "+12V_IN": ("power:+12V", "+12V_IN"),
}

CUSTOM = {f().get("name"): f() for f in symdefs.ALL}


def uid(*key):
    return str(uuid.uuid5(NS, "/".join(str(k) for k in key)))


def g(v):
    return round(v / 1.27) * 1.27


def fmt(v):
    s = "%.4f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def rot(dx, dy, angle):
    """library offset (y up) -> screen offset (y down) for a symbol rotated by angle (CCW on screen)"""
    x, y = dx, -dy
    a = int(angle) % 360
    if a == 0:
        return x, y
    if a == 90:
        return y, -x
    if a == 180:
        return -x, -y
    if a == 270:
        return -y, x
    raise ValueError(angle)


DIRS = {0: (-1, 0), 180: (1, 0), 90: (0, -1), 270: (0, 1)}  # lib pin angle -> outward direction (lib coords, y up)


class Sym:
    def __init__(self, sheet, lib_id, ref, value, x, y, angle=0, unit=1, fp="", props=None, dnp=False):
        self.sheet, self.lib_id, self.ref, self.value = sheet, lib_id, ref, value
        self.x, self.y, self.angle, self.unit, self.fp = x, y, angle, unit, fp
        self.props = props or {}
        self.dnp = dnp
        lib, name = lib_id.split(":")
        if lib == CUSTOM_LIB:
            self.pins = symdefs.pins_as_lib(CUSTOM[name])
        else:
            self.pins = kilib.pins(kilib.lib_symbol(lib, name))
        self.pins = [p for p in self.pins if p["unit"] in (0, unit)]
        sheet.lib_ids.add(lib_id)

    def pin(self, number):
        for p in self.pins:
            if p["number"] == number:
                dx, dy = rot(p["x"], p["y"], self.angle)
                ox, oy = DIRS[int(p["angle"]) % 360]
                odx, ody = rot(ox, oy, self.angle)
                return (self.x + dx, self.y + dy), (odx, ody), p
        raise KeyError("%s pin %s" % (self.ref, number))

    def numbers(self):
        return [p["number"] for p in self.pins]


class Sheet:
    def __init__(self, fname, title, page):
        self.fname, self.title, self.page = fname, title, page
        self.uuid = uid("sheet", fname)
        self.syms, self.wires, self.labels, self.ncs, self.texts = [], [], [], [], []
        self.junctions = []
        self.lib_ids = set()
        self.conn = {}  # (ref, pin) -> net
        self.pwr_n = 0

    def add(self, lib_id, ref, value, x, y, angle=0, unit=1, fp="", nets=None, props=None, dnp=False, hl=False):
        s = Sym(self, lib_id, ref, value, g(x), g(y), angle, unit, fp, props, dnp)
        self.syms.append(s)
        done = {}
        items = []
        for num, net in (nets or {}).items():
            (px, py), (ox, oy), p = s.pin(num)
            key = (round(px, 3), round(py, 3))
            self.conn[(s.ref, num)] = net
            if key in done:
                if done[key] != net:
                    raise ValueError("%s pins share a point but differ: %s" % (s.ref, num))
                continue
            done[key] = net
            if net is None:
                self.ncs.append((px, py))
                continue
            items.append((num, net, px, py, int(round(ox)), int(round(oy))))
        # power pins of one net side by side at 2.54 pitch: one symbol per run
        runs = {}
        for it in items:
            num, net, px, py, ox, oy = it
            if net not in POWER:
                continue
            line = round(px, 3) if ox != 0 else round(py, 3)
            runs.setdefault((net, ox, oy, line), []).append(it)
        grouped = set()
        for (net, ox, oy, line), its in runs.items():
            along = (lambda it: it[3]) if ox != 0 else (lambda it: it[2])
            its.sort(key=along)
            chain = [its[0]]
            chains = []
            for it in its[1:]:
                if abs(along(it) - along(chain[-1]) - 2.54) < 0.01:
                    chain.append(it)
                else:
                    chains.append(chain)
                    chain = [it]
            chains.append(chain)
            for ch in chains:
                if len(ch) < 2:
                    continue
                ends = []
                for num, net_, px, py, ox_, oy_ in ch:
                    ex, ey = px + ox_ * STUB, py + oy_ * STUB
                    self.wires.append(((px, py), (ex, ey)))
                    ends.append((ex, ey))
                    grouped.add(num)
                for k in range(len(ends) - 1):
                    self.wires.append((ends[k], ends[k + 1]))
                for k in range(1, len(ends) - 1):
                    self.junctions.append(ends[k])
                if ox != 0:
                    at = ends[-1] if net == "GND" else ends[0]
                else:
                    at = ends[0]
                self.netpoint(net, at[0], at[1], (ox, oy), hl)
        # lone power pins on the top or bottom side next to another one: stagger the stubs
        extra = {}
        tb = [it for it in items if it[0] not in grouped and it[1] in POWER and it[4] == 0]
        for line in set((it[5], round(it[3], 3)) for it in tb):
            row = sorted([it for it in tb if (it[5], round(it[3], 3)) == line], key=lambda it: it[2])
            level = 0
            for k in range(1, len(row)):
                level = level + 1 if row[k][2] - row[k - 1][2] < 2.55 else 0
                if level:
                    extra[row[k][0]] = 5.08 * (level % 3)
        for num, net, px, py, ox, oy in items:
            if num in grouped:
                continue
            ln = STUB + extra.get(num, 0)
            ex, ey = px + ox * ln, py + oy * ln
            self.wires.append(((px, py), (ex, ey)))
            self.netpoint(net, ex, ey, (ox, oy), hl)
        return s

    def netpoint(self, net, x, y, d, hl=False):
        """attach net at (x, y); d = outward direction of the stub"""
        if net in POWER:
            lib_id, value = POWER[net]
            if net == "GND":
                ang = {(0, 1): 0, (0, -1): 180, (1, 0): 90, (-1, 0): 270}[d]
            else:
                ang = {(0, -1): 0, (0, 1): 180, (1, 0): 270, (-1, 0): 90}[d]
            self.pwr_n += 1
            ref = "#PWR%s%02d" % (self.page, self.pwr_n)
            s = Sym(self, lib_id, ref, value, x, y, ang)
            self.syms.append(s)
        else:
            ang = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[d]
            if hl and ang in (90, 270):
                ang = 0
            self.labels.append((net, x, y, ang))

    def flag(self, net, x, y):
        """PWR_FLAG at (x, y) wired down to the net"""
        x, y = g(x), g(y)
        self.pwr_n += 1
        s = Sym(self, "power:PWR_FLAG", "#FLG%s%02d" % (self.page, self.pwr_n), "PWR_FLAG", x, y, 0)
        self.syms.append(s)
        self.wires.append(((x, y), (x, y + STUB)))
        self.netpoint(net, x, y + STUB, (0, 1), True)

    def text(self, t, x, y, size=1.27):
        self.texts.append((t, g(x), g(y), size))

    # ---- two-pin parts ----
    def two(self, lib_id, ref, value, x, y, a, b, orient="v", fp="", props=None, flip=False, dnp=False):
        """pin 1 -> net a, pin 2 -> net b. 'v': pin 1 on top; 'h': pin 1 on the left.
        For diodes (pin 1 = K on the left) flip=True puts pin 1 on the right."""
        if orient == "v":
            angle = 0
        else:
            angle = 90
        lib, name = lib_id.split(":")
        if name in ("D_Schottky", "D_TVS", "D_Zener", "LED", "D"):
            angle = 180 if flip else 0
            if orient == "v":
                angle = 270 if flip else 90
        return self.add(lib_id, ref, value, x, y, angle, 1, fp, {"1": a, "2": b}, props, dnp, hl=True)


def lib_symbols_block(lib_ids):
    out = []
    for lid in sorted(lib_ids):
        lib, name = lid.split(":")
        if lib == CUSTOM_LIB:
            out.append(symdefs.lib_text(CUSTOM[name], CUSTOM_LIB))
        else:
            blk = kilib.lib_symbol(lib, name)
            out.append(blk.replace('(symbol "%s"' % name, '(symbol "%s"' % lid, 1))
    return "(lib_symbols\n" + "\n".join(out) + "\n)"


def eff(just=None, hide=False, size=1.27):
    s = "(effects (font (size %s %s))" % (fmt(size), fmt(size))
    if just:
        s += " (justify %s)" % just
    if hide:
        s += " (hide yes)"
    return s + ")"


def sym_text(s, path):
    lines = []
    dnp = "yes" if s.dnp else "no"
    power = s.ref.startswith("#")
    lines.append('(symbol (lib_id "%s") (at %s %s %d) (unit %d) (exclude_from_sim no) (in_bom %s) (on_board %s) (dnp %s) (uuid "%s")'
                 % (s.lib_id, fmt(s.x), fmt(s.y), s.angle, s.unit, "no" if power else "yes", "no" if power else "yes", dnp,
                    uid("sym", path, s.ref, s.unit, s.x, s.y)))
    # field angles are stored in the symbol's frame: 90 for symbols turned by 90/270 keeps the text horizontal.
    # Justification is left out for turned symbols (KiCad mirrors it), so text is centred at a computed spot.
    fa = 90 if s.angle in (90, 270) else 0
    if power:
        lines.append('(property "Reference" "%s" (at %s %s %d) %s)' % (s.ref, fmt(s.x), fmt(s.y), fa, eff(hide=True)))
        gnd = s.lib_id == "power:GND"
        hide_v = s.lib_id == "power:PWR_FLAG" or (gnd and s.angle != 0)
        off = 3.81 + 0.6 * len(s.value)
        if s.angle == 0:
            vx, vy = s.x, (s.y + 3.81 if gnd else s.y - 3.81)
        elif s.angle == 180:
            vx, vy = s.x, (s.y - 3.81 if gnd else s.y + 3.81)
        elif s.angle == 90:
            vx, vy = (s.x + off, s.y) if gnd else (s.x - off, s.y)
        else:
            vx, vy = (s.x - off, s.y) if gnd else (s.x + off, s.y)
        lines.append('(property "Value" "%s" (at %s %s %d) %s)' % (s.value, fmt(vx), fmt(vy), fa, eff(hide=hide_v)))
    else:
        horiz = False
        if len(s.pins) == 2:
            (p1, _, _), (p2, _, _) = s.pin(s.pins[0]["number"]), s.pin(s.pins[1]["number"])
            horiz = abs(p1[1] - p2[1]) < 0.01
        turned = s.angle in (180, 270)
        if horiz:
            dr, dv = (s.x, s.y - 2.54), (s.x, s.y + 2.54)
            rj = vj = None
        elif turned:
            dr = (s.x + 2.54 + 0.6 * len(s.ref), s.y - 1.27)
            dv = (s.x + 2.54 + 0.6 * len(s.value), s.y + 1.27)
            rj = vj = None
        else:
            dr, dv, rj, vj = (s.x + 2.54, s.y - 1.27), (s.x + 2.54, s.y + 1.27), "left", "left"
        rx, ry = s.props.get("_ref_at", dr)
        vx, vy = s.props.get("_val_at", dv)
        if "_ref_at" in s.props:
            rj = None if turned else "left"
        if "_val_at" in s.props:
            vj = None if turned else "left"
        lines.append('(property "Reference" "%s" (at %s %s %d) %s)' % (s.ref, fmt(rx), fmt(ry), fa, eff(rj)))
        lines.append('(property "Value" "%s" (at %s %s %d) %s)' % (s.value, fmt(vx), fmt(vy), fa, eff(vj)))
    lines.append('(property "Footprint" "%s" (at %s %s 0) %s)' % (s.fp, fmt(s.x), fmt(s.y), eff(hide=True)))
    lines.append('(property "Datasheet" "" (at %s %s 0) %s)' % (fmt(s.x), fmt(s.y), eff(hide=True)))
    for k, v in s.props.items():
        if k.startswith("_"):
            continue
        lines.append('(property "%s" "%s" (at %s %s 0) %s)' % (k, v, fmt(s.x), fmt(s.y), eff(hide=True)))
    for num in s.numbers():
        lines.append('(pin "%s" (uuid "%s"))' % (num, uid("pin", path, s.ref, s.unit, num)))
    lines.append('(instances (project "%s" (path "%s" (reference "%s") (unit %d))))' % (PROJECT, path, s.ref, s.unit))
    lines.append(')')
    return " ".join(lines)


def sheet_text(sh, root_uuid, global_nets, title_block):
    path = "/%s/%s" % (root_uuid, sh.uuid)
    out = ['(kicad_sch (version 20250610) (generator "jlr_most_amp_gen") (generator_version "10.0") (uuid "%s") (paper "A3")' % sh.uuid]
    out.append(title_block(sh.title))
    out.append(lib_symbols_block(sh.lib_ids))
    for (a, b) in sh.wires:
        out.append('(wire (pts (xy %s %s) (xy %s %s)) (stroke (width 0) (type default)) (uuid "%s"))'
                   % (fmt(a[0]), fmt(a[1]), fmt(b[0]), fmt(b[1]), uid("w", sh.fname, a, b)))
    for (x, y) in sh.junctions:
        out.append('(junction (at %s %s) (diameter 0) (color 0 0 0 0) (uuid "%s"))' % (fmt(x), fmt(y), uid("j", sh.fname, x, y)))
    for (x, y) in sh.ncs:
        out.append('(no_connect (at %s %s) (uuid "%s"))' % (fmt(x), fmt(y), uid("nc", sh.fname, x, y)))
    for (net, x, y, ang) in sh.labels:
        just = "left" if ang in (0, 90) else "right"
        if net in global_nets:
            out.append('(global_label "%s" (shape passive) (at %s %s %d) (fields_autoplaced yes) %s (uuid "%s") '
                       '(property "Intersheetrefs" "${INTERSHEET_REFS}" (at %s %s 0) %s))'
                       % (net, fmt(x), fmt(y), ang, eff(just), uid("gl", sh.fname, net, x, y), fmt(x), fmt(y), eff(just, hide=True)))
        else:
            out.append('(label "%s" (at %s %s %d) %s (uuid "%s"))'
                       % (net, fmt(x), fmt(y), ang, eff(just + " bottom"), uid("l", sh.fname, net, x, y)))
    for (t, x, y, size) in sh.texts:
        out.append('(text "%s" (exclude_from_sim no) (at %s %s 0) %s (uuid "%s"))'
                   % (t.replace('"', "'"), fmt(x), fmt(y), eff("left bottom", size=size), uid("t", sh.fname, t, x, y)))
    for s in sh.syms:
        out.append(sym_text(s, path))
    out.append("(embedded_fonts no)\n)")
    return "\n".join(out)
