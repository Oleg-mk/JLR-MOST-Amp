"""JLR-MOST-Amp tube board schematic: root + 3 sheets, written for KiCad 10 with the hw/gen writer."""
import os, sys, re, json, uuid, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "hw", "gen"))
import schgen as G
import symdefs
import kilib

VAR = sys.argv[1] if len(sys.argv) > 1 else "6n28b"
VARIANTS = {
    "6n28b": dict(sym="6N28B-V", cyr="6Н28Б-В", role="основна", fp="jlr_tube:Valve_6N28B-V_Flat",
                  desc="Double triode 6N28B-V, subminiature, 9 wire leads: a1 1, k1 2, h 3, g1 4, screen 5, a2 6, k2 7, h 8, g2 9",
                  renum={1: {"1": "1", "2": "4", "3": "2"}, 2: {"6": "6", "7": "9", "8": "7"}, 3: {"4": "3", "5": "8"}},
                  pins=dict(a1="1", g1="4", k1="2", a2="6", g2="9", k2="7", h1="3", h2="8", s="5"),
                  Rbot="4.7k", Rk="820", Rk1="680", Rk2="14.3k", ih="0.247", heat="1 А", hw="6.2", bw="6.8", iin="1.2", ib="32",
                  mode="Режим (B+L ~200 В): V1a підсилювач Ra 27k / Rk 820 без шунта - 4.1 мА, 85 В, Ку 10.5, THD 0.29 % (H2) при 2.6 В. "
                       "V1b концертина 15k/15k (Rk 680 + 14.3k, витік на їхню точку) - 4.1 мА, катод 62 В, x0.89 на обидві ноги. "
                       "V2 повторювачі: сітки на ~100 В, CCS 4 мА, катоди ~104 В, Zout ~270 Ом.",
                  inp="Вхід: дільник 36k/4.7k (x0.115): 2.1 В ЦАП -> 0.24 В -> 2.5 В на концертині -> 2.0 В на ногу: 4.1 В диференційно на XLR, "
                      "2 В на RCA; THD тракту ~0.25 % (H2, H3 0.005 %)."),
    "6n16b": dict(sym="6N16B-V", cyr="6Н16Б-В", role="варіант", fp="jlr_tube:Valve_6N16B-V_Flat",
                  desc="Double triode 6N16B-V, subminiature, 8 wire leads: triode 1 a5 g3 k2, triode 2 a1 g7 k6, heater 4-8",
                  renum={1: {"1": "5", "2": "3", "3": "2"}, 2: {"6": "1", "7": "7", "8": "6"}, 3: {"4": "4", "5": "8"}},
                  pins=dict(a1="5", g1="3", k1="2", a2="1", g2="7", k2="6", h1="4", h2="8", s=None),
                  Rbot="4.02k", Rk="1k", Rk1="1k", Rk2="14k", ih="0.4", heat="1.6 А", hw="10", bw="6.0", iin="1.6", ib="28",
                  mode="Режим (B+L ~200 В): V1a підсилювач Ra 27k / Rk 1k без шунта - 3.2 мА, 113 В, Ку 10.9, THD 0.17 % (H2) при 2.2 В. "
                       "V1b концертина 15k/15k (Rk 1k + 14k) - 3.1 мА, катод 46 В, x0.91 на обидві ноги. "
                       "V2 повторювачі: сітки на ~100 В, CCS 4 мА, катоди ~103 В, Zout ~240 Ом.",
                  inp="Вхід: дільник 36k/4.02k (x0.10): 2.1 В ЦАП -> 0.21 В -> 2.3 В на концертині -> 1.9 В на ногу: 3.9 В диференційно на XLR, "
                      "1.9 В на RCA; THD тракту ~0.17 %."),
}
V = VARIANTS[VAR]
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", VAR)
os.makedirs(OUT, exist_ok=True)

G.NS = uuid.UUID("2b7c9f41-58d0-4e1a-b3c6-7a0d9e4f1c25")
G.PROJECT = "jlr_tube_" + VAR
G.CUSTOM_LIB = "jlr_tube"
G.POWER = {
    "GND": ("power:GND", "GND"),
    "+12V": ("power:+12V", "+12V"),
    "+12V_IN": ("power:+12V", "+12V_IN"),
    "+3V3_REM": ("power:+3V3", "+3V3_REM"),
    "+6V3": ("power:+6V", "+6V3"),
    "B+": ("power:HT", "B+"),
    "B+L": ("power:HT", "B+L"),
    "B+R": ("power:HT", "B+R"),
}


# ---------------------------------------------------------------- symbols
def derive(lib, name, new, ref, value, fp, desc, renum, passive=False, add=None, val_y=None):
    """KiCad library symbol copied under a new name: pins renumbered per unit {unit: {old: new}},
    optional extra items appended to a unit's body {unit: text}."""
    blk = kilib.lib_symbol(lib, name)
    blk = blk.replace('(symbol "%s_' % name, '(symbol "%s_' % new)
    blk = blk.replace('(symbol "%s"' % name, '(symbol "@@NAME@@"', 1)
    for key, v in (("Reference", ref), ("Value", value), ("Footprint", fp), ("Datasheet", ""), ("Description", desc)):
        blk = re.sub(r'\(property "%s" "[^"]*"' % key, '(property "%s" "%s"' % (key, v), blk, count=1)
    blk = re.sub(r'\(property "ki_fp_filters" "[^"]*"', '(property "ki_fp_filters" "*"', blk, count=1)
    if val_y is not None:
        blk = re.sub(r'(\(property "Value" "[^"]*"\s*\(at [-\d.]+ )[-\d.]+', r'\g<1>%s' % val_y, blk, count=1)
    out, pos = [], 0
    for m in re.finditer(r'\(symbol "%s_(\d+)_(\d+)"' % re.escape(new), blk):
        if m.start() < pos:
            continue
        sub = kilib.block(blk, m.start())
        unit = int(m.group(1))
        mp = renum.get(unit, {})

        def pin_fix(pm):
            p = pm.group(0)
            p = re.sub(r'\(number\s+"([^"]*)"', lambda n: '(number "%s"' % mp.get(n.group(1), n.group(1)), p, count=1)
            if passive:
                p = re.sub(r'^\(pin\s+\w+', '(pin passive', p)
            return p
        new_sub = re.sub(r'\(pin\s+\w+\s+\w+\s*\(at [^()]*\)\s*\(length [^()]*\).*?\(number\s+"[^"]*"', pin_fix, sub, flags=re.S)
        if add and unit in add and int(m.group(2)) == 1:
            new_sub = new_sub[:-1] + " " + add[unit] + ")"
        out.append(blk[pos:m.start()])
        out.append(new_sub)
        pos = m.start() + len(sub)
    out.append(blk[pos:])
    raw = "".join(out)
    return dict(name=new, ref=ref, value=value, footprint=fp, desc=desc, raw=raw, pins_lib=kilib.pins(raw))


def pin_text(num, name, x, y, ang, length=2.54):
    f = '(effects (font (size 1.27 1.27)))'
    return '(pin passive line (at %g %g %g) (length %g) (name "%s" %s) (number "%s" %s))' % (x, y, ang, length, name, f, num, f)


SCREEN = ('(polyline (pts (xy 0 7.62) (xy 0 3.81)) (stroke (width 0) (type default)) (fill (type none))) '
          '(polyline (pts (xy -3.81 3.81) (xy 3.81 3.81)) (stroke (width 0.254) (type dash)) (fill (type none))) '
          + pin_text("5", "S", 0, 10.16, 270))
tube = derive("Valve", "ECC88", V["sym"], "V", V["sym"], V["fp"], V["desc"], V["renum"],
              passive=True, add={3: SCREEN} if V["pins"]["s"] else None)
TUBE = "jlr_tube:" + V["sym"]
TUBE_FP = {"6n28b": {"V1": "jlr_tube:Valve_6N28B-V_Flat_458", "V3": "jlr_tube:Valve_6N28B-V_Flat_458"}}.get(VAR, {})   # tubes with their own lead layout; the right channel is the left one's copy
HEAT_SWAP = {"6n28b": {"V1", "V3"}, "6n16b": {"V1", "V3"}}.get(VAR, set())   # V1/V3 heater fed at 8 (its short lead), 3 to ground: no foil-side jumper under the channel; 6N16B: 8 fed, 4 to ground, same holes
ccs = derive("Device", "Q_NMOS", "DN2540_SOT89", "Q", "DN2540N8", "Package_TO_SOT_SMD:SOT-89-3",
             "Depletion N-MOSFET as a current source, SOT-89 (DN2540 B060313): 1 G, 2 D (tab), 3 S", {1: {"G": "1", "D": "2", "S": "3"}})
nmos = derive("Device", "Q_NMOS", "NMOS_GDS", "Q", "NMOS", "Package_TO_SOT_SMD:TO-252-2",
              "N-MOSFET, pins 1 G, 2 D (tab), 3 S", {1: {"G": "1", "D": "2", "S": "3"}})
lm = symdefs.lm65625q1(); lm["footprint"] = "jlr_tube:TI_RZT0020A"
hs = symdefs.tps1h200a()
rca = symdefs.rca(); rca["footprint"] = "jlr_tube:BKL_0101153_RCA_RA"
clip = dict(name="TubeClip", ref="MP", value="Keystone 3566", footprint="",
            desc="Clip holding a tube bulb (13/32 in fuse clip); its holes are in the tube footprint, so the symbol stays off the board",
            body=(-2.54, 2.54, 2.54, -2.54), pins=[])
CUSTOMS = [tube, ccs, nmos, lm, hs, rca, clip]
G.CUSTOM = {c["name"]: c for c in CUSTOMS}

_pins, _text = symdefs.pins_as_lib, symdefs.lib_text
symdefs.pins_as_lib = lambda s: s["pins_lib"] if "pins_lib" in s else _pins(s)
symdefs.lib_text = lambda s, libname=None: (s["raw"].replace("@@NAME@@", (libname + ":" if libname else "") + s["name"])
                                             if "raw" in s else _text(s, libname))

_sym_text = G.sym_text
G.sym_text = lambda sy, path: (_sym_text(sy, path).replace("(on_board yes)", "(on_board no)", 1)
                               if sy.lib_id == "jlr_tube:TubeClip" else _sym_text(sy, path))

# ---------------------------------------------------------------- helpers
R0805 = "Resistor_SMD:R_0805_2012Metric"
R1206 = "Resistor_SMD:R_1206_3216Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C1206 = "Capacitor_SMD:C_1206_3216Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C1210 = "Capacitor_SMD:C_1210_3225Metric"
C1812 = "Capacitor_SMD:C_1812_4532Metric"


def R(sh, ref, val, x, y, a, b, o="v", fp=R0805):
    return sh.two("Device:R", ref, val, x, y, a, b, o, fp)


def C(sh, ref, val, x, y, a, b, o="v", fp=C0805):
    return sh.two("Device:C", ref, val, x, y, a, b, o, fp)


def CP(sh, ref, val, x, y, a, b, fp):
    return sh.two("Device:C_Polarized", ref, val, x, y, a, b, "v", fp)


def lbl(ref_at=None, val_at=None):
    d = {}
    if ref_at:
        d["_ref_at"] = ref_at
    if val_at:
        d["_val_at"] = val_at
    return d


# ---------------------------------------------------------------- power
pw = G.Sheet("power.kicad_sch", "Живлення: вхід, розжарення, B+", 2)
s = pw
s.text("Живлення. +12 В постійні з основної плати через стопку J1 (її J9 над її J1, до її F1); F1 одразу за гніздом.", 15.24, 22.86, 2.0)
s.text("REM_IN (вивід 2 J1) - REM_OUT основної плати. Від нього вмикається розжарення і живиться контролер B+; сам B+ стартує, коли аркуш REM відпустить BP_EN (~12 с, D4 на UVLO).", 15.24, 29.21)
s.text("Без REM плата бере ~14 мкА: U1 0.85 мкА у вимкненні, дільник B+ 11 мкА (через L3 і D3 на B+ лишається ~11 В), D2, U5.", 15.24, 33.02)
s.add("Connector_Generic:Conn_01x05", "J1", "гніздо 1x5", 33.02, 69.85,
      fp="Connector_PinSocket_2.54mm:PinSocket_1x05_P2.54mm_Vertical",
      nets={"1": "GND", "2": "REM_IN", "3": "+12V_IN", "4": "+12V_IN", "5": "GND"}, props=lbl((30.48, 62.23), (30.48, 77.47)))
s.text("J1: гніздо 1x5 2.54 на боці B під штирі J9 основної плати (над її J1): 1 маса, 2 REM_IN (REM_OUT основної, +12 В), 3 і 4 +12 В до F1 (по 0.6 А), 5 маса. "
       "Стопка штирі + гніздо 8.5 мм = 11 мм між платами, стійки M2.5 x 11.", 15.24, 88.9)
s.flag("+12V_IN", 33.02, 99.06)
s.flag("GND", 45.72, 99.06)
s.flag("REM_IN", 58.42, 99.06)
s.two("Device:Fuse", "F1", "3A T 0452003", 66.04, 69.85, "+12V_IN", "+12V_F", "h", "Fuse:Fuse_Littelfuse-NANO2-451_453")
C(s, "C1", "100n 50V", 78.74, 80.01, "+12V_F", "GND")
s.two("Device:D_Schottky", "D1", "SS54", 93.98, 69.85, "+12V_D", "+12V_F", "h", "Diode_SMD:D_SMC", flip=True)
s.two("Device:D_Zener", "D2", "SM6T30A", 116.84, 82.55, "+12V_D", "GND", "v", "Diode_SMD:D_SMB", flip=True)
s.two("Device:L", "L1", "10u NRSE8040-100M", 139.7, 69.85, "+12V_D", "+12V", "h", "Inductor_SMD:L_Bourns-SRN8040_8x8.15mm")
CP(s, "C2", "47u 50V", 165.1, 80.01, "+12V", "GND", "Capacitor_SMD:CP_Elec_8x10")
# a second 47u + 100n pair on +12V, on the board by the boost controller's parts
CP(s, "C61", "47u 50V", 177.8, 80.01, "+12V", "GND", "Capacitor_SMD:CP_Elec_8x10")
C(s, "C62", "100n 50V", 190.5, 80.01, "+12V", "GND")
s.flag("+12V", 190.5, 99.06)
s.text("Вхід як на основній: D1 тримає заряд C2 при просіданні, D2 - TVS 600 Вт, клемп ~33 В (U1 до 65 В, U2 до 60 В, U5 до 40 В). Один 47 мкФ: перетворювачі мають свої вхідні ємності.", 15.24, 114.3)
s.text("F1 3 А з затримкою (NANO2 452): розжарення %s Вт + B+ %s Вт ~ %s А від 12 В;" % (V["hw"], V["bw"], V["iin"]) + " поки нитки холодні, U1 у межі струму - до ~2.5 А на 1-2 с.", 15.24, 118.11)

# heater buck
s.text("U1 LM65625-Q1 (SNVSCH2A): нитки 4 x %s по %s А = %s." % (V["cyr"], V["ih"], V["heat"]) + " FB: 0.8 В x (1 + 100k/14.3k) = 6.39 В, на лампах ~6.34 В (5.7..6.9).", 15.24, 125.73)
s.text("BIAS від виходу (3.3..30 В). RT на GND - 2.2 МГц, MODE на VCC - FPWM з DRSS +-5 %, як U8 основної. L2 CKCSA4020-2.2uH/M: Isat 6.1 А при -30 % > 4.9 А межі U1.", 15.24, 129.54)
s.text("Холодні нитки: U1 тримає струм 3.75..4.9 А; гикавка лише при виході < 40 %, тоді прогрів трохи довший. EN: R3/R4 100k/18.7k - старт при REM > 7.9 В, стоп < 6.35 В.", 15.24, 133.35)
s.add("jlr_tube:LM65625-Q1", "U1", "LM65625-Q1", 99.06, 167.64,
      nets={"11": "+12V", "17": "+12V", "9": "GND", "19": "GND", "21": "GND",
            "13": "HB_SW", "14": "HB_SW", "15": "HB_BST", "20": "+6V3",
            "7": "HB_EN", "5": "HB_VCC", "4": "HB_VCC", "6": "GND", "3": "HB_FB", "2": None,
            "1": None, "8": None, "10": None, "12": None, "16": None, "18": None},
      props=lbl((88.9, 151.13), (109.22, 184.15)))
C(s, "C4", "10u 50V", 30.48, 175.26, "+12V", "GND", fp=C1210)
C(s, "C5", "10u 50V", 43.18, 175.26, "+12V", "GND", fp=C1210)
C(s, "C6", "100n 50V", 55.88, 175.26, "+12V", "GND", fp=C0603)
C(s, "C7", "100n 50V", 68.58, 175.26, "+12V", "GND", fp=C0603)
C(s, "C8", "100n", 134.62, 154.94, "HB_BST", "HB_SW", fp=C0603)
C(s, "C9", "1u", 134.62, 193.04, "HB_VCC", "GND", fp=C0603)
s.two("Device:L", "L2", "2.2u CKCSA4020-2.2uH/M", 147.32, 172.72, "HB_SW", "+6V3", "h", "Inductor_SMD:L_Coilcraft_XxL4030")
C(s, "C10", "22u 16V", 180.34, 182.88, "+6V3", "GND", fp=C1206)
C(s, "C11", "22u 16V", 195.58, 182.88, "+6V3", "GND", fp=C1206)
R(s, "R1", "100k", 215.9, 172.72, "+6V3", "HB_FB")
R(s, "R2", "14.3k", 215.9, 193.04, "HB_FB", "GND")
R(s, "R3", "100k", 236.22, 172.72, "REM_IN", "HB_EN")
R(s, "R4", "18.7k", 236.22, 193.04, "HB_EN", "GND")
s.flag("+6V3", 254.0, 203.2)

# B+ boost
s.text("U2 LM5022-Q1 (SNVSAG9): boost у розривному струмі, 12 -> 211 В, ~%s мА (2 канали: підсилювач, концертина і два повторювачі по ~4 мА + дільник)." % V["ib"] + " RT 115k: 149 кГц.", 15.24, 213.36)
s.text("L3 33 мкГн CDRH129RNP-330MC (Isat 4 А при -10 %%), стає на майданчики MSS1278: пік ~1.8 А при 12 В і %s Вт; при 9 В - упор у 90 %% заповнення, B+ ~190 В. R10 0.18 Ом: межа 0.434..0.55 В -> 2.4..3.1 А. Q1 і D3 - 600 В." % V["bw"], 15.24, 217.17)
s.text("Дільник R12-R14 3 x 330k (по 70 В на 1206) + R15 5.9k: 1.25 В -> 211 В. Він же розряджає B+ без ламп (~1.5 хв); з лампами B+ спадає за ~0.5 с.", 15.24, 220.98)
s.text("R16/C21/C22 47k/220n/100p: петля ~100 Гц, навантаження ламп стале. UVLO R6/R7 100k/18.7k: старт 7.9 В, гістерезис 20 мкА x 100k = 2 В; D4 (аркуш REM) тримає UVLO < 0.6 В до прогріву ниток.", 15.24, 224.79)
s.text("R9 47k тримає затвор Q1, поки U2 без живлення (REM знято, а +12 В на L3 є). R11/C15 100 Ом/1 нФ - фільтр CS з прикладу паспорта.", 15.24, 228.6)
s.add("Regulator_Switching:LM5022MM", "U2", "LM5022-Q1", 299.72, 104.14,
      nets={"1": "REM_IN", "7": "BP_UVLO", "9": "BP_RT", "10": "BP_SS", "5": "BP_GATE", "8": "BP_CS",
            "2": "BP_FB", "3": "BP_COMP", "4": "BP_VCC", "6": "GND"},
      props=lbl((292.1, 91.44), (304.8, 121.92)))
R(s, "R6", "100k", 261.62, 137.16, "REM_IN", "BP_UVLO")
C(s, "C12", "1u 50V", 276.86, 137.16, "REM_IN", "GND")
R(s, "R7", "18.7k", 261.62, 157.48, "BP_UVLO", "GND")
R(s, "R5", "115k", 276.86, 157.48, "BP_RT", "GND")
C(s, "C14", "100n", 292.1, 157.48, "BP_SS", "GND")
C(s, "C13", "1u 25V", 320.04, 137.16, "BP_VCC", "GND")
R(s, "R16", "47k", 325.12, 73.66, "BP_COMP", "BP_CZ", "h")
C(s, "C21", "220n", 355.6, 73.66, "BP_CZ", "BP_FB", "h")
C(s, "C22", "100p", 337.82, 86.36, "BP_COMP", "BP_FB", "h")
R(s, "R15", "5.9k", 378.46, 96.52, "BP_FB", "GND")
R(s, "R12", "330k", 340.36, 137.16, "B+", "BP_FBA", fp=R1206)
R(s, "R13", "330k", 355.6, 137.16, "BP_FBA", "BP_FBB", fp=R1206)
R(s, "R14", "330k", 370.84, 137.16, "BP_FBB", "BP_FB", fp=R1206)
C(s, "C16", "10u 50V", 261.62, 187.96, "+12V", "GND", fp=C1210)
C(s, "C17", "100n 50V", 276.86, 187.96, "+12V", "GND")
# the boost's own input caps right at L3 (its DCM current is a 0..1.8 A triangle); C16/C17 stay at the input
C(s, "C58", "10u 50V", 289.56, 187.96, "+12V", "GND", fp=C1210)
C(s, "C59", "100n 50V", 302.26, 187.96, "+12V", "GND")
s.two("Device:L", "L3", "33u CDRH129RNP-330MC", 294.64, 172.72, "+12V", "BP_SW", "h", "Inductor_SMD:L_Coilcraft_MSS1278-XXX")
R(s, "R8", "10", 314.96, 162.56, "BP_GATE", "BP_G", "h")
s.add("jlr_tube:NMOS_GDS", "Q1", "IPD60R600P7", 332.74, 185.42, nets={"1": "BP_G", "2": "BP_SW", "3": "BP_SNS"},
      props=lbl((337.82, 182.88), (337.82, 187.96)))
R(s, "R9", "47k", 317.5, 195.58, "BP_G", "GND")
R(s, "R10", "0.18 1%", 350.52, 198.12, "BP_SNS", "GND", fp=R1206)
R(s, "R11", "100", 365.76, 177.8, "BP_SNS", "BP_CS", "h")
C(s, "C15", "1n", 383.54, 190.5, "BP_CS", "GND")
s.two("Device:D", "D3", "ES1J", 355.6, 162.56, "B+", "BP_SW", "h", "Diode_SMD:D_SMA", flip=True)
C(s, "C18", "100n 630V", 360.68, 220.98, "B+", "GND", fp=C1812)
C(s, "C19", "100n 630V", 373.38, 220.98, "B+", "GND", fp=C1812)
CP(s, "C20", "10u 350V", 386.08, 220.98, "B+", "GND", "Capacitor_THT:CP_Radial_D10.0mm_P5.00mm")
# a second 10u 350V on B+, next to C18/C19 on the board
CP(s, "C60", "10u 350V", 347.98, 220.98, "B+", "GND", "Capacitor_THT:CP_Radial_D10.0mm_P5.00mm")
# a third 100n 630V on B+, on the board at the far end of the B+ run next to C60
C(s, "C63", "100n 630V", 335.28, 220.98, "B+", "GND", fp=C1812)
s.flag("B+", 398.78, 213.36)

# ---------------------------------------------------------------- REM
rm = G.Sheet("rem.kicad_sch", "REM: нитки, B+, підсилювач", 3)
s = rm
s.text("REM. Нитки вмикаються з REM_IN одразу; B+ - через ~12 с (U4 відпускає BP_EN, D4 звільняє UVLO бустера); REM_OUT на підсилювач - "
       "ще через ~9 с (U6, відлік від BP_EN). Вимкнення - одразу.", 15.24, 22.86, 2.0)
s.text("Коли REM_IN зникає: обидва RESET у нулі за 20 мкс, U5 закривається за ~0.1 мс, REM_OUT спадає через C64 і вхід підсилювача (100 кОм: ~15 мс до 3 В), B+ сідає за ~0.5 с - лампи гаснуть з уже вимкненим підсилювачем.", 15.24, 29.21)
s.text("U3 живиться від REM_IN: без REM уся логіка без струму.", 15.24, 33.02)
s.add("Regulator_Linear:TPS7B8133DRV", "U3", "TPS7B8133DRV", 99.06, 76.2,
      nets={"1": "REM_IN", "2": "REM_IN", "3": "GND", "4": "GND", "7": "GND", "5": None, "6": "+3V3_REM"},
      props=lbl((96.52, 67.31), (106.68, 86.36)))
C(s, "C23", "1u 50V", 73.66, 85.09, "REM_IN", "GND")
C(s, "C24", "2.2u", 137.16, 85.09, "+3V3_REM", "GND")
s.text("U4 TPS3808G01-Q1 (SBVS085J): SENSE 0.405 В, R17/R18 1M/52.3k -> REM_IN > 8.15 В. RESET відпускається через tD = CT / 175 нФ: C26 2.2 мкФ -> ~12.6 с, у межах паспорта.", 165.1, 60.96)
s.text("BP_EN - відкритий стік U4 з підтяжкою R66 100k до +3V3_REM. Поки він у нулі, D4 тримає UVLO U2 на ~0.5 В (< 1.25 В: бустер у standby, SS розряджений);", 165.1, 64.77)
s.text("після відпускання D4 закритий (UVLO 1.9..2.2 В < 3.3 В). SENSE нижче порогу - RESET у нулі за 20 мкс і до VDD 0.8 В. MR U4 вільний (90k до VDD всередині).", 165.1, 68.58)
s.add("Power_Supervisor:TPS3808DBV", "U4", "TPS3808G01-Q1", 238.76, 101.6,
      nets={"6": "+3V3_REM", "2": "GND", "3": None, "4": "REM_CT", "5": "REM_SENSE", "1": "BP_EN"},
      props=lbl((243.84, 91.44), (243.84, 111.76)))
C(s, "C25", "100n", 190.5, 86.36, "+3V3_REM", "GND")
R(s, "R17", "1M", 182.88, 124.46, "REM_IN", "REM_SENSE")
R(s, "R18", "52.3k", 198.12, 124.46, "REM_SENSE", "GND")
C(s, "C26", "2.2u 10V", 213.36, 124.46, "REM_CT", "GND")
R(s, "R66", "100k", 271.78, 86.36, "+3V3_REM", "BP_EN")
s.two("Device:D_Schottky", "D4", "BAT54W", 302.26, 101.6, "BP_EN", "BP_UVLO", "h", "Diode_SMD:D_SOD-123", flip=True)
s.text("U6 - другий TPS3808: MR від BP_EN, SENSE спільний з U4; відлік C56 1.5 мкФ -> ~8.6 с іде, коли B+ уже стоїть і вихідні конденсатори заряджені. RESET -> REM_GO -> U5.", 165.1, 139.7)
s.add("Power_Supervisor:TPS3808DBV", "U6", "TPS3808G01-Q1", 238.76, 170.18,
      nets={"6": "+3V3_REM", "2": "GND", "3": "BP_EN", "4": "REM_CT2", "5": "REM_SENSE", "1": "REM_GO"},
      props=lbl((243.84, 160.02), (243.84, 180.34)))
C(s, "C57", "100n", 190.5, 154.94, "+3V3_REM", "GND")
C(s, "C56", "1.5u 10V", 213.36, 193.04, "REM_CT2", "GND")
R(s, "R19", "10k", 271.78, 154.94, "+3V3_REM", "REM_GO")
s.text("U5 TPS1H200A-Q1, як U10 основної: межа 0.5 А (R21 4.02k), DELAY підтягнуто - при замиканні повторні спроби. J2 - REM на підсилювач.", 15.24, 146.05)
s.add("jlr_tube:TPS1H200A-Q1", "U5", "TPS1H200A-Q1", 99.06, 177.8,
      nets={"8": "+12V", "1": "REM_GO", "2": "GND", "5": "REM_DLY", "4": "REM_CL", "7": "REM_OUT", "3": None,
            "6": "GND", "9": "GND"}, props=lbl((91.44, 162.56), (107.95, 193.04)))
R(s, "R20", "10k", 50.8, 195.58, "+3V3_REM", "REM_DLY")
R(s, "R21", "4.02k", 66.04, 195.58, "REM_CL", "GND")
C(s, "C27", "100n 50V", 137.16, 167.64, "+12V", "GND")
s.add("Connector_Generic:Conn_02x01", "J2", "43045-0221", 167.64, 177.8,
      fp="Connector_Molex:Molex_Micro-Fit_3.0_43045-0221_2x01-1MP_P3.00mm_Horizontal",
      nets={"1": "REM_OUT", "2": "GND"}, props=lbl((162.56, 172.72), (162.56, 185.42)))
C(s, "C64", "100n 50V", 167.64, 198.12, "REM_OUT", "GND")
s.text("J2: Micro-Fit 3.0 2x1 кутовий (43045-0221): 1 REM_OUT, 2 маса.", 15.24, 210.82)
s.text("C64 при J2: ВЧ із кабелю REM на масу роз'єму. Вихід U5 наростає до 0.6 В/мкс, C64 бере до 60 мА - далеко до межі 0.5 А.", 15.24, 214.63)

# ---------------------------------------------------------------- tubes
tb = G.Sheet("tubes.kicad_sch", "Лампи і виходи", 4)
s = tb
R2512 = "Resistor_SMD:R_2512_6332Metric"
CFILM = "Capacitor_THT:C_Rect_L7.2mm_W4.5mm_P5.00mm_FKS2_FKP2_MKS2_MKP2"
s.text("Канал на двох %s: V1a - тріод підсилення, V1b - концертина (фазоінвертор з рівними опорами), V2a/V2b - катодні повторювачі "
       "з CCS у катодах, по одному на ногу XLR; те саме V3/V4 для правого каналу. Без трансформатора." % V["cyr"], 15.24, 22.86, 2.0)
s.text(V["mode"], 15.24, 29.21)
s.text(V["inp"], 15.24, 33.02)
if VAR == "6n16b":
    s.text("Плата та сама, що під 6Н28Б-В: виводи гнучкі, кожен іде в отвір своєї функції, таблиця в production/README.md. Посадкове місце V1-V4 у цій схемі на плату не йде.", 15.24, 52.07)
s.text("Фази: підсилювач інвертує, анодна гілка концертини інвертує ще раз - вона в фазі зі входом і йде на XLR 2 і RCA; катодна гілка - на XLR 3. "
       "Вихід impedance-balanced (по ~270 Ом на ногу), не плаваючий; XLR 1 на масу. 47k після конденсаторів: вихід без навантаження в нулі.", 15.24, 36.83)
s.text("CCS DN2540N8 (SOT-89, паспорт B060313: 1 G, 2 D = tab, 3 S): Id = |Vgs| / Rs, між екземплярами 2.5..6 мА. "
       "Катод повторювача тримає опора ~100 В (1M/1M + 1 мкФ), тому режим від CCS не залежить. 10k (2512) між катодом і стоком бере 25..60 В (до 0.36 Вт): "
       "на DN2540 лишається 40..78 В і 0.19..0.25 Вт замість 0.41..0.61 Вт; плюс мідь під tab.", 15.24, 40.64)
s.text("Нитки на масі: Uнк повторювачів ~104 В, концертини ~62 В (ліміт 150). Вихідні 4.7 мкФ 250 В MKS4 (31.5 x 11, крок 27.5): 3.4 Гц на 10 кОм.", 15.24, 44.45)


def channel(x0, ch, refs):
    """one channel: gain triode, concertina, two cathode followers; refs: dict of designators"""
    r = refs
    B = "B+" + ch
    n = lambda name: name + "_" + ch
    s.text("Канал %s" % ch, x0 + 12.7, 55.88, 2.0)
    # B+ filter
    R(s, r["Rbp"], "470", x0 + 121.92, 50.8, "B+", B, "h", fp=R1206)
    CP(s, r["Cbp"], "47u 250V", x0 + 137.16, 58.42, B, "GND", "Capacitor_THT:CP_Radial_D12.5mm_P5.00mm")
    C(s, r["Cbh"], "100n 250V", x0 + 149.86, 58.42, B, "GND", fp=C1206)
    s.flag(B, x0 + 160.02, 45.72)
    # input and divider
    R(s, r["Rtop"], "36k", x0 + 50.8, 81.28, n("IN"), n("DIV"), "h")
    R(s, r["Rbot"], V["Rbot"], x0 + 63.5, 99.06, n("DIV"), "GND")
    C(s, r["Cg"], "100p C0G", x0 + 76.2, 99.06, n("DIV"), "GND")
    R(s, r["Rg1"], "1k", x0 + 91.44, 81.28, n("DIV"), n("G1"), "h")
    # V1a gain stage
    P = V["pins"]
    vg, vf = r["Vgain"], r["Vcf"]
    s.add(TUBE, vg, V["sym"], x0 + 116.84, 86.36, unit=1, nets={P["a1"]: n("A"), P["g1"]: n("G1"), P["k1"]: n("K1")},
          props=lbl((x0 + 121.92, 81.28), (x0 + 121.92, 85.09)), fp=TUBE_FP.get(vg, ""))
    R(s, r["Ra"], "27k", x0 + 116.84, 66.04, B, n("A"), fp=R2512)
    R(s, r["Rk"], V["Rk"], x0 + 116.84, 106.68, n("K1"), "GND")
    # V1b concertina, cap coupled, grid leak to the Rk1/Rk2 junction
    C(s, r["Cc1"], "100n 250V", x0 + 132.08, 73.66, n("A"), n("G2"), "h", fp=CFILM)
    R(s, r["Rg2"], "1M", x0 + 139.7, 101.6, n("G2"), n("KJ"), fp=R1206)
    s.add(TUBE, vg, V["sym"], x0 + 154.94, 86.36, unit=2, nets={P["a2"]: n("P"), P["g2"]: n("G2"), P["k2"]: n("K2")},
          props=lbl((x0 + 160.02, 81.28), (x0 + 160.02, 85.09)), fp=TUBE_FP.get(vg, ""))
    R(s, r["Rpa"], "15k", x0 + 154.94, 66.04, B, n("P"), fp=R2512)
    R(s, r["Rk1"], V["Rk1"], x0 + 154.94, 104.14, n("K2"), n("KJ"))
    R(s, r["Rk2"], V["Rk2"], x0 + 154.94, 119.38, n("KJ"), "GND", fp=R2512)
    # grid reference of the followers
    R(s, r["Rv1"], "1M", x0 + 50.8, 152.4, B, n("VREF"), fp=R1206)
    R(s, r["Rv2"], "1M", x0 + 50.8, 170.18, n("VREF"), "GND", fp=R1206)
    C(s, r["Cv"], "470n 250V", x0 + 60.96, 170.18, n("VREF"), "GND", fp=C1210)
    # V2a hot follower (from the concertina's anode), V2b cold (from its cathode)
    C(s, r["Cc2"], "100n 250V", x0 + 73.66, 144.78, n("P"), n("GH"), "h", fp=CFILM)
    R(s, r["Rgh"], "1M", x0 + 83.82, 160.02, n("GH"), n("VREF"), fp=R1206)
    s.add(TUBE, vf, V["sym"], x0 + 99.06, 152.4, unit=1, nets={P["a1"]: B, P["g1"]: n("GH"), P["k1"]: n("OH")},
          props=lbl((x0 + 104.14, 147.32), (x0 + 104.14, 151.13)), fp=TUBE_FP.get(vf, ""))
    R(s, r["Rdh"], "10k", x0 + 124.46, 152.4, n("OH"), n("DH"), fp=R2512)       # takes part of the CCS's voltage and heat
    s.add("jlr_tube:DN2540_SOT89", r["Qh"], "DN2540N8", x0 + 124.46, 165.1, nets={"1": "GND", "2": n("DH"), "3": n("S1")},
          props=lbl((x0 + 129.54, 162.56), (x0 + 129.54, 167.64)))
    R(s, r["Rs1"], "620", x0 + 124.46, 180.34, n("S1"), "GND")
    C(s, r["Cc3"], "100n 250V", x0 + 73.66, 193.04, n("K2"), n("GC"), "h", fp=CFILM)
    R(s, r["Rgc"], "1M", x0 + 83.82, 208.28, n("GC"), n("VREF"), fp=R1206)
    s.add(TUBE, vf, V["sym"], x0 + 99.06, 200.66, unit=2, nets={P["a2"]: B, P["g2"]: n("GC"), P["k2"]: n("OC")},
          props=lbl((x0 + 104.14, 195.58), (x0 + 104.14, 199.39)), fp=TUBE_FP.get(vf, ""))
    R(s, r["Rdc"], "10k", x0 + 124.46, 200.66, n("OC"), n("DC"), fp=R2512)
    s.add("jlr_tube:DN2540_SOT89", r["Qc"], "DN2540N8", x0 + 124.46, 213.36, nets={"1": "GND", "2": n("DC"), "3": n("S2")},
          props=lbl((x0 + 129.54, 210.82), (x0 + 129.54, 215.9)))
    R(s, r["Rs2"], "620", x0 + 124.46, 228.6, n("S2"), "GND")
    # outputs
    MKS = "Capacitor_THT:C_Rect_L31.5mm_W11.0mm_P27.50mm_MKS4"
    s.two("Device:C", r["Coh"], "4.7u 250V MKS4", x0 + 142.24, 144.78, n("OH"), n("XH"), "h", MKS)
    R(s, r["Rbh"], "47k", x0 + 152.4, 160.02, n("XH"), "GND", fp=R1206)
    R(s, r["Rxh"], "100", x0 + 165.1, 144.78, n("XH"), n("XLRH"), "h")
    R(s, r["Rrca"], "100", x0 + 165.1, 132.08, n("XH"), n("RCAT"), "h")
    C(s, r["Cx1"], "1n C0G", x0 + 177.8, 160.02, n("XLRH"), "GND")
    s.two("Device:C", r["Coc"], "4.7u 250V MKS4", x0 + 142.24, 193.04, n("OC"), n("XC"), "h", MKS)
    R(s, r["Rbc"], "47k", x0 + 152.4, 208.28, n("XC"), "GND", fp=R1206)
    R(s, r["Rxc"], "100", x0 + 165.1, 193.04, n("XC"), n("XLRC"), "h")
    C(s, r["Cx2"], "1n C0G", x0 + 177.8, 208.28, n("XLRC"), "GND")
    s.add("Connector_Audio:XLR3_Ground", r["Jx"], "NC3MAH", x0 + 190.5, 177.8, fp="jlr_tube:Neutrik_NC3MAH", hl=True,
          nets={"1": "GND", "2": n("XLRH"), "3": n("XLRC"), "G": "GND"}, props=lbl((x0 + 187.96, 170.18), (x0 + 194.31, 170.18)))
    s.add("jlr_tube:RCA", r["Jo"], "RCA OUT " + ch, x0 + 190.5, 124.46, hl=True,
          nets={"1": n("RCAT"), "2": n("RCAS"), "3": n("RCAS")}, props=lbl((x0 + 187.96, 118.11), (x0 + 194.31, 118.11)))
    C(s, r["Cr1"], "1n C0G", x0 + 177.8, 116.84, n("RCAT"), "GND")
    R(s, r["Rrs"], "10", x0 + 185.42, 142.24, n("RCAS"), "GND")
    C(s, r["Cr2"], "10n 100V", x0 + 193.04, 142.24, n("RCAS"), "GND")
    # heaters and screens
    hn = {P["h1"]: "HEAT", P["h2"]: "GND"}
    if P["s"]:
        hn[P["s"]] = "GND"
    hs = {**hn, P["h1"]: "GND", P["h2"]: "HEAT"}     # fed at the other end of the heater
    s.add(TUBE, vg, V["sym"], x0 + 22.86, 233.68, unit=3, nets=dict(hs if vg in HEAT_SWAP else hn), props=lbl((x0 + 27.94, 228.6), (x0 + 27.94, 232.41)), fp=TUBE_FP.get(vg, ""))
    s.add(TUBE, vf, V["sym"], x0 + 45.72, 233.68, unit=3, nets=dict(hs if vf in HEAT_SWAP else hn), props=lbl((x0 + 50.8, 228.6), (x0 + 50.8, 232.41)), fp=TUBE_FP.get(vf, ""))


def refs(base_r, base_c, base_q, v, jx, jo, rd):
    names_r = ["Rtop", "Rbot", "Rg1", "Rbp", "Ra", "Rk", "Rg2", "Rpa", "Rk1", "Rk2", "Rgh", "Rgc", "Rv1", "Rv2",
               "Rs1", "Rs2", "Rbh", "Rbc", "Rxh", "Rxc", "Rrca", "Rrs"]
    names_c = ["Cg", "Cbp", "Cbh", "Cc1", "Cc2", "Cc3", "Cv", "Coh", "Coc", "Cx1", "Cx2", "Cr1", "Cr2"]
    d = {n: "R%d" % (base_r + i) for i, n in enumerate(names_r)}
    d.update({n: "C%d" % (base_c + i) for i, n in enumerate(names_c)})
    d.update(Vgain="V%d" % v, Vcf="V%d" % (v + 1), Qh="Q%d" % base_q, Qc="Q%d" % (base_q + 1), Jx="J%d" % jx, Jo="J%d" % jo)
    d.update(Rdh="R%d" % rd, Rdc="R%d" % (rd + 1))      # added later, numbered after the rest
    return d


channel(0.0, "L", refs(22, 28, 2, 1, 5, 7, 67))
channel(198.12, "R", refs(44, 41, 4, 3, 6, 8, 69))
s.add("Connector_Generic:Conn_01x04", "J3", "гніздо 1x4", 25.4, 81.28, fp="Connector_PinSocket_2.54mm:PinSocket_1x04_P2.54mm_Vertical",
      nets={"1": "IN_L", "2": "GND", "3": "IN_R", "4": "GND"}, props=lbl((22.86, 71.12), (22.86, 93.98)))
s.text("J3: гніздо 1x4 2.54 на боці B під штирі J8 основної плати (між її R32/R33 і RCA): 1 LINE_L, 2 маса, 3 LINE_R, 4 маса - сигнал ЦАП після 470 Ом / 2.2 нФ.", 15.24, 48.26)
s.two("Device:FerriteBead", "FB1", "BLM41PG600SN1", 50.8, 259.08, "+6V3", "HEAT", "h", "Inductor_SMD:L_1806_4516Metric")
C(s, "C54", "22u 16V", 66.04, 269.24, "HEAT", "GND", fp=C1206)
C(s, "C55", "22u 16V", 78.74, 269.24, "HEAT", "GND", fp=C1206)
for i, x in enumerate((96.52, 116.84, 137.16, 157.48)):
    s.add("jlr_tube:TubeClip", "MP%d" % (i + 1), "Keystone 3566", x, 271.78, props=lbl((x + 3.81, 269.24), (x + 3.81, 274.32)))
s.text("MP1-MP4: кліпси Keystone 3566 під колби V1-V4, по одній на 6..17 мм від цоколя, з краплею нейтрального силікону (>= 150 °C) всередині; "
       "їхні отвори - у посадковому місці лампи, символи на плату не йдуть.", 91.44, 281.94)
s.text("FB1 6 А і C54/C55 біля ламп: нитки на постійному струмі, пульсації U1 (2.2 МГц) не йдуть дротами на уступ.", 15.24, 285.75)

SHEETS = [pw, rm, tb]

# ---------------------------------------------------------------- write
ROOT_UUID = G.uid("root")


def title_block(title):
    return '(title_block (title "Лампова %s, %s: %s") (rev "0.1") (company "github.com/Oleg-mk/JLR-MOST-Amp"))' % (V["cyr"], V["role"], title)


net_sheets = {}
for sh in SHEETS:
    for (_, net) in sh.conn.items():
        if net is None or net in G.POWER:
            continue
        net_sheets.setdefault(net, set()).add(sh.fname)
    for (net, x, y, a) in sh.labels:
        net_sheets.setdefault(net, set()).add(sh.fname)
global_nets = {n for n, ss in net_sheets.items() if len(ss) > 1}

for sh in SHEETS:
    open(os.path.join(OUT, sh.fname), "w", encoding="utf-8", newline="\n").write(G.sheet_text(sh, ROOT_UUID, global_nets, title_block))

parts = ['(kicad_sch (version 20250610) (generator "jlr_most_amp_gen") (generator_version "10.0") (uuid "%s") (paper "A4")' % ROOT_UUID,
         title_block("аркуші"), "(lib_symbols)"]
for i, sh in enumerate(SHEETS):
    x, y = 30.48 + (i % 2) * 101.6, 50.8 + (i // 2) * 60.96
    parts.append('(sheet (at %s %s) (size 76.2 38.1) (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no) (fields_autoplaced yes) '
                 '(stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000)) (uuid "%s") '
                 '(property "Sheetname" "%s" (at %s %s 0) (effects (font (size 1.524 1.524) (bold yes)) (justify left bottom))) '
                 '(property "Sheetfile" "%s" (at %s %s 0) (effects (font (size 1.27 1.27)) (justify left top))) '
                 '(instances (project "%s" (path "/%s" (page "%d")))))'
                 % (G.fmt(x), G.fmt(y), sh.uuid, sh.title, G.fmt(x), G.fmt(y - 0.7), sh.fname, G.fmt(x), G.fmt(y + 38.8),
                    G.PROJECT, ROOT_UUID, sh.page))
parts.append('(sheet_instances (path "/" (page "1")))')
parts.append("(embedded_fonts no)\n)")
open(os.path.join(OUT, G.PROJECT + ".kicad_sch"), "w", encoding="utf-8", newline="\n").write("\n".join(parts))

lib = ['(kicad_symbol_lib (version 20251024) (generator "jlr_most_amp_gen") (generator_version "10.0")']
for c in CUSTOMS:
    lib.append(symdefs.lib_text(c))
lib.append(")")
open(os.path.join(OUT, "jlr_tube.kicad_sym"), "w", encoding="utf-8", newline="\n").write("\n".join(lib))
open(os.path.join(OUT, "fp-lib-table"), "w", encoding="utf-8", newline="\n").write(
    '(fp_lib_table (version 7)\n  (lib (name "jlr_tube")(type "KiCad")(uri "${KIPRJMOD}/jlr_tube.pretty")(options "")(descr "JLR-MOST-Amp tube board footprints"))\n)\n')
open(os.path.join(OUT, "sym-lib-table"), "w", encoding="utf-8", newline="\n").write(
    '(sym_lib_table (version 7)\n  (lib (name "jlr_tube")(type "KiCad")(uri "${KIPRJMOD}/jlr_tube.kicad_sym")(options "")(descr "JLR-MOST-Amp tube board parts"))\n)\n')
pretty = os.path.join(OUT, "jlr_tube.pretty")
os.makedirs(pretty, exist_ok=True)
for f in ("BKL_0101153_RCA_RA.kicad_mod", "TI_RZT0020A.kicad_mod"):
    dst = os.path.join(pretty, f)
    if not os.path.exists(dst):
        shutil.copy(os.path.join(HERE, "..", "..", "hw", "jlr_most_amp.pretty", f), dst)
pro = os.path.join(OUT, G.PROJECT + ".kicad_pro")
if not os.path.exists(pro):
    json.dump({"meta": {"filename": G.PROJECT + ".kicad_pro", "version": 3}, "schematic": {}, "boards": [], "sheets": [],
               "text_variables": {}}, open(pro, "w", encoding="utf-8"), indent=2)

intent = {}
for sh in SHEETS:
    for (ref, pin), net in sh.conn.items():
        if ref.startswith("#"):
            continue
        intent["%s.%s" % (ref, pin)] = net
json.dump(intent, open(os.path.join(HERE, "intent_%s.json" % VAR), "w"), indent=0, sort_keys=True)
print("sheets written; global nets:", len(global_nets), "pins:", len(intent))
