"""Compare the netlist KiCad exported with the intended pin -> net map."""
import json, re, sys

net_file, intent_file = sys.argv[1], sys.argv[2]
s = open(net_file, encoding="utf-8").read()
intent = json.load(open(intent_file))

actual = {}  # "REF.PIN" -> net name
i = s.index("(nets")
for blk in re.split(r"\(net\s*\(code", s[i:])[1:]:
    m = re.search(r'\(name "([^"]*)"\)', blk)
    name = m.group(1)
    for n in re.finditer(r'\(node\s*\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', blk):
        actual["%s.%s" % (n.group(1), n.group(2))] = name


def norm(n):
    n = n.lstrip("/")
    return n.split("/")[-1]


bad = []
nets_seen = {}
for key, want in sorted(intent.items()):
    got = actual.get(key)
    if want is None:
        if got is not None and not got.startswith("unconnected-"):
            bad.append((key, "NC", got))
        continue
    if got is None:
        bad.append((key, want, "(missing)"))
        continue
    g = norm(got)
    if g != want:
        bad.append((key, want, got))
    nets_seen.setdefault(want, set()).add(g)

# nets that merged: two intended nets on one actual net
by_actual = {}
for key, want in intent.items():
    if want is None:
        continue
    got = actual.get(key)
    if got:
        by_actual.setdefault(got, set()).add(want)
merged = {a: w for a, w in by_actual.items() if len(w) > 1}

single = {}
for key, got in actual.items():
    single.setdefault(got, []).append(key)
lonely = [(n, k) for n, k in single.items() if len(k) == 1 and not n.startswith("unconnected-")]

print("pins in netlist:", len(actual), "intended:", len(intent))
print("mismatches:", len(bad))
for b in bad[:40]:
    print("  ", b)
print("merged nets:", merged if merged else "none")
print("single-pin nets:", lonely if lonely else "none")
