"""Custom symbols for JLR-MOST-Amp, drawn from the parts' data sheets.

Each symbol: dict(name, ref, value, desc, footprint, body=(x1,y1,x2,y2), pins=[(number, name, type, x, y, angle)])
Pin (x, y) is the connection point in library coordinates (y up); angle points from the
connection point into the body (0 = pin on the left side, 180 = right, 270 = top, 90 = bottom).
"""

PL = 5.08  # pin length


def side(pins_spec, x, y0, angle, step=2.54):
    out = []
    y = y0
    for p in pins_spec:
        if p is not None:
            num, name, typ = p
            out.append((num, name, typ, x, y, angle))
        y -= step
    return out


def topbot(pins_spec, y, x0, angle, step=2.54):
    out = []
    x = x0
    for p in pins_spec:
        if p is not None:
            num, name, typ = p
            out.append((num, name, typ, x, y, angle))
        x += step
    return out


def os8104a():
    left = [("42", "RX", "input"), ("3", "TX", "output"), None,
            ("31", "~{RS}", "input"), ("39", "SCL", "bidirectional"), ("40", "SDA", "bidirectional"),
            ("41", "~{INT}", "open_collector"), ("43", "~{AINT}", "open_collector"),
            ("38", "AD0", "input"), ("32", "AD1", "input"), None,
            ("44", "PAR_CP", "input"), ("1", "PAR_SRC", "input"), ("34", "ASYNC", "input"),
            ("22", "LEG", "input"), ("2", "PAD0", "input"), ("11", "PAD1", "input"),
            ("7", "~{RD}", "input"), ("8", "~{WR}", "input"), ("24", "TEST2", "input"), ("25", "TEST3", "input")]
    right = [("4", "RMCK", "output"), ("13", "FSY", "bidirectional"), ("14", "SCK", "bidirectional"), None,
             ("9", "SX0", "output"), ("36", "SX1", "output"), ("10", "SX2", "output"), ("12", "SX3", "output"),
             ("15", "SR0", "input"), ("35", "SR1", "input"), ("16", "SR2", "input"), ("17", "SR3", "input"), None,
             ("20", "XTI", "passive"), ("21", "XTO", "passive"), ("26", "FLT", "passive"), None,
             ("23", "STATUS", "output"), ("37", "ERROR", "output"), ("33", "CP_FLOW", "output")]
    top = [("5", "VDDD", "power_in"), ("18", "VDDP", "power_in"), ("29", "VDDA", "power_in"), ("30", "VDDA", "power_in")]
    bot = [("6", "GNDD", "power_in"), ("19", "GNDP", "power_in"), ("27", "GNDA", "power_in"), ("28", "GNDA", "power_in")]
    h = 27.94
    pins = side(left, -20.32, 25.4, 0) + side(right, 20.32, 25.4, 180)
    pins += topbot(top, h + 2.54 + PL, -3.81, 270) + topbot(bot, -(h + 2.54 + PL), -3.81, 90)
    return dict(name="OS8104A", ref="U", value="OS8104A",
                desc="MOST25 network transceiver, 44-pin QFP (DS8104AFP4)",
                footprint="Package_QFP:LQFP-44_10x10mm_P0.8mm",
                body=(-15.24, h + 2.54, 15.24, -(h + 2.54)), pins=pins)


def esp32_wrover_ie():
    left = [("3", "EN", "input"), ("25", "IO0", "bidirectional"), ("35", "TXD0/IO1", "output"),
            ("34", "RXD0/IO3", "input"), None,
            ("6", "IO34", "input"), ("7", "IO35", "input"), ("4", "SENSOR_VP/IO36", "input"),
            ("5", "SENSOR_VN/IO39", "input"), None,
            ("8", "IO32", "bidirectional"), ("9", "IO33", "bidirectional"), ("10", "IO25", "bidirectional"),
            ("11", "IO26", "bidirectional"), ("12", "IO27", "bidirectional")]
    right = [("24", "IO2", "bidirectional"), ("26", "IO4", "bidirectional"), ("29", "IO5", "bidirectional"),
             ("14", "IO12", "bidirectional"), ("16", "IO13", "bidirectional"), ("13", "IO14", "bidirectional"),
             ("23", "IO15", "bidirectional"), ("30", "IO18", "bidirectional"), ("31", "IO19", "bidirectional"),
             ("33", "IO21", "bidirectional"), ("36", "IO22", "bidirectional"), ("37", "IO23", "bidirectional"), None,
             ("17", "NC", "no_connect"), ("18", "NC", "no_connect"), ("19", "NC", "no_connect"),
             ("20", "NC", "no_connect"), ("21", "NC", "no_connect"), ("22", "NC", "no_connect"),
             ("27", "NC", "no_connect"), ("28", "NC", "no_connect"), ("32", "NC", "no_connect")]
    top = [("2", "3V3", "power_in")]
    bot = [("1", "GND", "power_in"), ("15", "GND", "power_in"), ("38", "GND", "power_in"), ("39", "GND", "power_in")]
    h = 27.94
    pins = side(left, -20.32, 25.4, 0) + side(right, 20.32, 25.4, 180)
    pins += topbot(top, h + 2.54 + PL, 0, 270) + topbot(bot, -(h + 2.54 + PL), -3.81, 90)
    return dict(name="ESP32-WROVER-IE", ref="U", value="ESP32-WROVER-IE-N16R8",
                desc="Wi-Fi + Bluetooth module, ESP32-D0WD-V3, 16 MB flash, 8 MB PSRAM, U.FL antenna connector",
                footprint="jlr_most_amp:ESP32-WROVER-IE", body=(-15.24, h + 2.54, 15.24, -(h + 2.54)), pins=pins)


def fot_header():
    right = [("8", "RXD", "output"), ("7", "STATUS", "output"), None, ("4", "RGAIN", "passive"), ("1", "TXD", "input")]
    top = [("5", "RXVCC", "power_in"), None, ("3", "TXVCC", "power_in")]
    bot = [("6", "RXGND", "power_in"), ("2", "TXGND", "power_in"), ("0", "SHIELD", "passive")]
    pins = side(right, 15.24, 5.08, 180) + topbot(top, 12.7 + PL - 2.54, -2.54, 270) + topbot(bot, -(10.16 + PL), -2.54, 90)
    return dict(name="TE_1-1394640-1", ref="J", value="TE 1-1394640-1",
                desc="MOST25 2+0 POF header with FOT receiver (STATUS low = light) and transmitter",
                footprint="jlr_most_amp:TE_MOST_Pigtail_FOT_2+0", body=(-10.16, 10.16, 10.16, -10.16), pins=pins)


def lm65625q1():
    left = [("11", "VIN1", "power_in"), ("17", "VIN2", "power_in"), None, ("7", "EN/UVLO", "input"),
            ("5", "MODE/SYNC", "input"), ("6", "RT", "input"), ("3", "FB", "input")]
    right = [("15", "BOOT", "passive"), ("13", "SW1", "passive"), ("14", "SW2", "passive"), None,
             ("20", "BIAS", "input"), ("4", "VCC", "power_out"), ("2", "PG", "open_collector")]
    top = [("1", "NC", "no_connect"), ("8", "NC", "no_connect"), ("10", "NC", "no_connect"),
           ("12", "NC", "no_connect"), ("16", "NC", "no_connect"), ("18", "NC", "no_connect")]
    bot = [("9", "PGND1", "power_in"), ("19", "PGND2", "power_in"), ("21", "DAP", "passive")]
    pins = side(left, -15.24, 7.62, 0) + side(right, 15.24, 7.62, 180)
    pins += topbot(top, 10.16 + PL, -6.35, 270) + topbot(bot, -(10.16 + PL), -2.54, 90)
    return dict(name="LM65625-Q1", ref="U", value="LM65625-Q1",
                desc="65 V 2.5 A synchronous buck, fixed 3.3/5 V or adjustable, 0.3..2.2 MHz, tON min 40 ns, WQFN-FCRLF-20 (SNVSCH2A)",
                footprint="jlr_most_amp:TI_RZT0020A", body=(-10.16, 10.16, 10.16, -10.16), pins=pins)


def tps1h200a():
    left = [("1", "IN", "input"), ("2", "DIAG_EN", "input"), ("5", "DELAY", "bidirectional"), ("4", "CL", "passive")]
    right = [("7", "OUT", "power_out"), None, ("3", "~{FAULT}", "open_collector")]
    top = [("8", "VS", "power_in")]
    bot = [("6", "GND", "power_in"), ("9", "EP", "passive")]
    pins = side(left, -12.7, 3.81, 0) + side(right, 12.7, 3.81, 180)
    pins += topbot(top, 7.62 + PL, 0, 270) + topbot(bot, -(7.62 + PL), -1.27, 90)
    return dict(name="TPS1H200A-Q1", ref="U", value="TPS1H200A-Q1",
                desc="40 V 200 mOhm high-side switch, adjustable current limit, HVSSOP-8",
                footprint="Package_SO:HVSSOP-8-1EP_3x3mm_P0.65mm_EP1.57x1.89mm",
                body=(-7.62, 7.62, 7.62, -7.62), pins=pins)


def plt133():
    left = [("1", "Vin", "input")]
    top = [("2", "Vcc", "power_in")]
    bot = [("3", "GND", "power_in")]
    pins = side(left, -10.16, 0, 0) + topbot(top, 5.08 + PL, 0, 270) + topbot(bot, -(5.08 + PL), 0, 90)
    return dict(name="PLT133", ref="U", value="PLT133/T10W",
                desc="TOSLINK transmitter, 660 nm, Vcc 2.7..5.5 V (Everlight DPL-0000049 Rev.5): 1 Vin, 2 Vcc, 3 GND, left to right when facing the port",
                footprint="jlr_most_amp:Everlight_TOSLINK_T10", body=(-5.08, 5.08, 5.08, -5.08), pins=pins)


def rca():
    pins = [("1", "In", "passive", -7.62, 0, 0), ("2", "Ext", "passive", 0, -7.62, 90), ("3", "Aux", "passive", 7.62, 0, 180)]
    return dict(name="RCA", ref="J", value="RCA", desc="RCA jack, right angle: 1 centre, 3 the thin pin beside it (the outer contact), 2 the thick legs",
                footprint="jlr_most_amp:BKL_0101153_RCA_RA", body=(-2.54, 2.54, 2.54, -2.54), pins=pins)


def ch340c():
    # V3 is the output of the internal 3.3 V regulator; with VCC = 3.3 V it is tied to VCC (CH340 data sheet), so here
    # it is a power input: the library symbol's power output would clash with the LDO output in ERC
    left = [("5", "UD+", "bidirectional"), ("6", "UD-", "bidirectional"), None,
            ("7", "NC", "no_connect"), ("8", "NC", "no_connect")]
    right = [("2", "TXD", "output"), ("3", "RXD", "input"), None, ("13", "~{DTR}", "output"), ("14", "~{RTS}", "output"),
             None, ("9", "~{CTS}", "input"), ("10", "~{DSR}", "input"), ("11", "~{RI}", "input"),
             ("12", "~{DCD}", "input"), ("15", "R232", "input")]
    top = [("16", "VCC", "power_in"), ("4", "V3", "power_in")]
    bot = [("1", "GND", "power_in")]
    pins = side(left, -15.24, 2.54, 0) + side(right, 15.24, 12.7, 180)
    pins += topbot(top, 15.24 + PL, -1.27, 270) + topbot(bot, -(15.24 + PL), 0, 90)
    return dict(name="CH340C", ref="U", value="CH340C",
                desc="USB 2.0 full-speed to UART bridge, internal oscillator, SOP-16; V3 tied to VCC for a 3.3 V supply",
                footprint="Package_SO:SOIC-16_3.9x9.9mm_P1.27mm", body=(-10.16, 15.24, 10.16, -15.24), pins=pins)


ALL = [os8104a, esp32_wrover_ie, fot_header, lm65625q1, tps1h200a, plt133, rca, ch340c]


def lib_text(sym, libname=None):
    """kicad_sym block; libname='jlr_most_amp' gives the embedded name 'jlr_most_amp:NAME'"""
    n = sym["name"]
    full = (libname + ":" + n) if libname else n
    x1, y1, x2, y2 = sym["body"]
    f = '(effects (font (size 1.27 1.27)))'
    fh = '(effects (font (size 1.27 1.27)) (hide yes))'
    out = ['(symbol "%s" (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)' % full]
    out.append('(property "Reference" "%s" (at %g %g 0) %s)' % (sym["ref"], x1, y1 + 1.27, '(effects (font (size 1.27 1.27)) (justify left bottom))'))
    out.append('(property "Value" "%s" (at %g %g 0) %s)' % (sym["value"], x1, y2 - 1.27, '(effects (font (size 1.27 1.27)) (justify left top))'))
    out.append('(property "Footprint" "%s" (at 0 0 0) %s)' % (sym["footprint"], fh))
    out.append('(property "Datasheet" "" (at 0 0 0) %s)' % fh)
    out.append('(property "Description" "%s" (at 0 0 0) %s)' % (sym["desc"], fh))
    out.append('(symbol "%s_0_1" (rectangle (start %g %g) (end %g %g) (stroke (width 0.254) (type default)) (fill (type background))))' % (n, x1, y1, x2, y2))
    ps = []
    for num, name, typ, x, y, a in sym["pins"]:
        ps.append('(pin %s line (at %g %g %g) (length %g) (name "%s" %s) (number "%s" %s))' % (typ, x, y, a, PL, name, f, num, f))
    out.append('(symbol "%s_1_1" %s)' % (n, " ".join(ps)))
    out.append(')')
    return " ".join(out)


def pins_as_lib(sym):
    return [dict(unit=1, type=t, x=x, y=y, angle=a, name=nm, number=num) for num, nm, t, x, y, a in sym["pins"]]
