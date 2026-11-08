"""Net classes of the Specctra export rebuilt for FreeRouting: power widths, and nets that must stay on the top layer.

python dsn_classes.py <in.dsn> <out.dsn>
"""
import re, sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, encoding="utf-8").read()

POWER12 = {"+12V", "+12V_IN", "+12V_D", "REM_OUT"}
SUPPLY = {"+3V3", "+3V3_AON", "+3V0_TOS", "+3V3_NICA", "FOT_RXVCC", "FOT_TXVCC"}
TOP_SUPPLY = {"+3V3A", "BUCK_SW"}
TOP = {"DAC_OUTL", "DAC_OUTR", "LINE_L", "LINE_R", "DAC_CAPP", "DAC_CAPM", "DAC_VNEG", "DAC_LDOO",
       "XTI", "XTO", "FLT", "FLT_RC", "MOST_RX", "MOST_TX", "FOT_RXD", "FOT_TXD",
       "BUCK_BST", "BUCK_FB", "BUCK_COMP", "BUCK_CC", "BUCK_RT", "COAX_DRV", "COAX_MID", "COAX_OUT"}


def block_end(i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "(":
            depth += 1
        elif s[j] == ")":
            depth -= 1
            if depth == 0:
                return j + 1


# every class KiCad wrote (Default and the project's own), their nets pooled
spans, names = [], []
for m in re.finditer(r"\(class ", s):
    i = m.start()
    j = block_end(i)
    blk = s[i:j]
    head = blk[len("(class "):blk.index("(circuit")]
    toks = re.findall(r'"[^"]*"|[^\s"()]+', head)
    names += toks[1:]
    spans.append((i, j))

groups = {"kicad_default": [], "Power12": [], "Supply": [], "TopSupply": [], "TopOnly": []}
for n in names:
    base = n.strip('"').split("/")[-1]
    if base in POWER12:
        groups["Power12"].append(n)
    elif base in SUPPLY:
        groups["Supply"].append(n)
    elif base in TOP_SUPPLY:
        groups["TopSupply"].append(n)
    elif base in TOP:
        groups["TopOnly"].append(n)
    else:
        groups["kicad_default"].append(n)

VIA = '(use_via "Via[0-1]_600:300_um")'
RULE = {"kicad_default": (250, 150, False), "Power12": (800, 150, False), "Supply": (500, 150, False),
        "TopSupply": (500, 150, True), "TopOnly": (250, 150, True)}
out = []
for name, nets in groups.items():
    if not nets:
        continue
    w, clr, top = RULE[name]
    lay = " (use_layer F.Cu)" if top else ""
    out.append("(class %s %s\n      (circuit %s%s)\n      (rule (width %d) (clearance %d))\n    )" % (
        name, " ".join(nets), VIA, lay, w, clr))
for i, j in reversed(spans[1:]):
    s = s[:i] + s[j:]
i, j = spans[0]
s = s[:i] + "\n    ".join(out) + s[j:]
open(dst, "w", encoding="utf-8", newline="\n").write(s)
for g, v in groups.items():
    print(g, len(v))
