"""JLR-MOST-Amp schematic: root + 4 sheets, written for KiCad 10."""
import os, sys, json
import schgen as G
import symdefs

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
os.makedirs(OUT, exist_ok=True)

R0603 = "Resistor_SMD:R_0603_1608Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
C1206 = "Capacitor_SMD:C_1206_3216Metric"
C1210 = "Capacitor_SMD:C_1210_3225Metric"
FB0603 = "Inductor_SMD:L_0603_1608Metric"
L = "Device:L"


def R(sh, ref, val, x, y, a, b, o="v", fp=R0603):
    return sh.two("Device:R", ref, val, x, y, a, b, o, fp)


def C(sh, ref, val, x, y, a, b, o="v", fp=C0603):
    return sh.two("Device:C", ref, val, x, y, a, b, o, fp)


def FB(sh, ref, x, y, a, b):
    return sh.two("Device:FerriteBead", ref, "600R@100MHz", x, y, a, b, "v", FB0603)


def lbl(ref_at=None, val_at=None):
    d = {}
    if ref_at:
        d["_ref_at"] = ref_at
    if val_at:
        d["_val_at"] = val_at
    return d


# ---------------------------------------------------------------- ring
ring = G.Sheet("ring.kicad_sch", "Кільце: FOT і OS8104A", 2)
s = ring
s.text("Кільце. Штекер MOST 2+0 машини вставляється в J2.", 15.24, 22.86, 2.0)
s.text("J2: STATUS = 0, коли є світло (DS8104AFP4, рис. 20-2). Приймач живиться з +3V3_AON і живий у сні, передавач - з +3V3.", 15.24, 29.21)
s.text("R4 (RGAIN -> TXVCC) задає струм світлодіода передавача; AN 5344 дає 13.5..33 кОм.", 15.24, 33.02)
s.text("R1 і R5 - послідовне узгодження ліній RX і TX (33..150 Ом): R1 біля J2, R5 біля U1. R2 тримає RX у нулі, поки U1 без живлення.", 15.24, 36.83)

j2 = s.add("jlr_most_amp:TE_1-1394640-1", "J2", "TE 1-1394640-1", 59.69, 130.81,
           nets={"8": "FOT_RXD", "7": "FOT_STATUS", "4": "FOT_RGAIN", "1": "FOT_TXD",
                 "5": "FOT_RXVCC", "3": "FOT_TXVCC", "6": "GND", "2": "GND", "0": "GND"},
           props=lbl((49.53, 116.84), (30.48, 152.4)))
# receiver supply (always on)
FB(s, "FB1", 29.21, 69.85, "+3V3_AON", "FOT_RXVCC")
C(s, "C1", "100n", 49.53, 69.85, "FOT_RXVCC", "GND")
C(s, "C2", "10u", 69.85, 69.85, "FOT_RXVCC", "GND", fp=C0805)
s.flag("FOT_RXVCC", 30.48, 254.0)
# transmitter supply (switched)
FB(s, "FB2", 92.71, 69.85, "+3V3", "FOT_TXVCC")
C(s, "C3", "100n", 113.03, 69.85, "FOT_TXVCC", "GND")
C(s, "C4", "10u", 133.35, 69.85, "FOT_TXVCC", "GND", fp=C0805)
s.flag("FOT_TXVCC", 60.96, 254.0)
R(s, "R4", "15k", 153.67, 69.85, "FOT_TXVCC", "FOT_RGAIN")
R(s, "R3", "47k", 173.99, 69.85, "+3V3_AON", "FOT_STATUS")
# line terminations
R(s, "R1", "100", 101.6, 118.11, "FOT_RXD", "MOST_RX", "h")
R(s, "R2", "100k", 128.27, 125.73, "MOST_RX", "GND")
R(s, "R5", "100", 101.6, 143.51, "MOST_TX", "FOT_TXD", "h")

u1 = s.add("jlr_most_amp:OS8104A", "U1", "OS8104AAQ", 224.79, 148.59,
           fp="Package_QFP:LQFP-44_10x10mm_P0.8mm",
           nets={"42": "MOST_RX", "3": "MOST_TX", "31": "MOST_RST", "39": "I2C_SCL", "40": "I2C_SDA",
                 "41": "MOST_INT", "43": "MOST_AINT", "38": "+3V3", "32": "+3V3",
                 "44": "GND", "1": "GND", "34": "GND", "22": "GND", "2": "GND", "11": "GND",
                 "7": "+3V3", "8": "+3V3", "24": "+3V3", "25": "MOST_TEST3",
                 "4": "RMCK", "13": "FSY", "14": "SCK", "9": "SPDIF", "36": "MOST_SX1", "10": "MOST_SX2",
                 "12": None, "15": "GND", "35": "SPDIF_SRC", "16": "GND", "17": "GND",
                 "20": "XTI", "21": "XTO", "26": "FLT", "23": None, "37": None, "33": None,
                 "5": "+3V3", "18": "+3V3", "29": "+3V3_NICA", "30": "+3V3_NICA",
                 "6": "GND", "19": "GND", "27": "GND", "28": "GND"},
           props=lbl((209.55, 107.95), (231.14, 191.77)))
s.text("U1: I2C, адреса 0x46/0x47 (AD0 = AD1 = 1). PAR_CP, PAR_SRC, ASYNC, LEG = 0: послідовні порти, режим Enhanced.", 15.24, 40.64)
s.text("SX0 - S/PDIF (вихід), SX1 і SX2 - приймачі на ESP32, SR1 - потік для S/PDIF від ESP32. SR0, SR2, SR3 не плавають.", 15.24, 44.45)
# supplies
C(s, "C5", "100n", 196.85, 69.85, "+3V3", "GND")
C(s, "C6", "100n", 207.01, 69.85, "+3V3", "GND")
C(s, "C7", "10u", 217.17, 69.85, "+3V3", "GND", fp=C0805)
FB(s, "FB3", 232.41, 69.85, "+3V3", "+3V3_NICA")
C(s, "C8", "100n", 252.73, 69.85, "+3V3_NICA", "GND")
C(s, "C9", "100n", 273.05, 69.85, "+3V3_NICA", "GND")
C(s, "C10", "10u", 293.37, 69.85, "+3V3_NICA", "GND", fp=C0805)
s.flag("+3V3_NICA", 91.44, 254.0)
# pull-ups and reset
R(s, "R8", "4.7k", 113.03, 182.88, "+3V3", "I2C_SCL")
R(s, "R9", "4.7k", 133.35, 182.88, "+3V3", "I2C_SDA")
R(s, "R10", "47k", 153.67, 182.88, "+3V3", "MOST_INT")
R(s, "R11", "47k", 173.99, 182.88, "+3V3", "MOST_AINT")
R(s, "R12", "10k", 113.03, 218.44, "+3V3", "MOST_RST")
C(s, "C14", "1u", 133.35, 218.44, "MOST_RST", "GND")
R(s, "R7", "47k", 153.67, 218.44, "MOST_TEST3", "GND")
R(s, "R17", "100k", 173.99, 218.44, "SPDIF_SRC", "GND")
s.text("R12 + C14: скидання U1 після ввімкнення живлення, ESP32 може тримати MOST_RST у нулі (відкритий стік).", 101.6, 238.76)
# clocks out
R(s, "R13", "22", 285.75, 118.11, "RMCK", "MCLK", "h")
R(s, "R14", "22", 285.75, 128.27, "FSY", "LRCK", "h")
R(s, "R15", "22", 285.75, 138.43, "SCK", "BCK", "h")
R(s, "R16", "100k", 320.04, 125.73, "SCK", "GND")
s.text("R16: SCK не плаває після скидання (DS8104AFP4, табл. 19-1).", 274.32, 107.95)
# crystal and PLL filter
y1 = s.add("Device:Crystal_GND24", "Y1", "12.288MHz", 290.83, 175.26, fp="Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
           nets={"1": "XTI", "3": "XTO", "2": "GND", "4": "GND"}, props=lbl((288.29, 168.91), (295.91, 182.88)))
C(s, "C11", "18p", 271.78, 195.58, "XTI", "GND")
C(s, "C12", "18p", 309.88, 195.58, "XTO", "GND")
R(s, "R6", "2.2k", 342.9, 175.26, "FLT", "FLT_RC")
C(s, "C13", "100n C0G", 342.9, 200.66, "FLT_RC", "GND", fp=C1206)
s.text("R6 + C13: фільтр PLL біля GNDA; C13 - C0G, не X7R (DS8104AFP4, 20.2).", 274.32, 220.98)

# ---------------------------------------------------------------- host
host = G.Sheet("host.kicad_sch", "ESP32-WROVER-IE", 3)
s = host
s.text("ESP32-WROVER-IE. I2S0: SX1 -> IO18, потік для S/PDIF IO26 -> SR1. I2S1: SX2 -> IO19, DOUT IO25 -> PCM5122 і роз'єм I2S.", 15.24, 22.86, 2.0)
s.text("Усі чотири секції I2S ведені від BCK (IO34) і LRCK (IO35) з OS8104A.", 15.24, 29.21)
s.text("IO36 і IO39 - лише входи без підтяжок, тому тут клавіші перемикача з R22..R25. IO0, IO2, IO5, IO12, IO15 задають режим старту; IO12 лишається вільним.", 15.24, 33.02)
u2 = s.add("jlr_most_amp:ESP32-WROVER-IE", "U2", "ESP32-WROVER-IE-N16R8", 200.66, 133.35,
           nets={"3": "ESP_EN", "25": "ESP_IO0", "35": "ESP_TXD0", "34": "ESP_RXD0",
                 "6": "BCK", "7": "LRCK", "4": "MODE2", "5": "MODE3",
                 "8": "REM_EN", "9": "HOLD", "10": "DOUT", "11": "SPDIF_SRC", "12": "MOST_RST",
                 "24": "LED", "26": None, "29": None, "14": None, "16": "MODE0", "13": "MODE1", "23": None,
                 "30": "MOST_SX1", "31": "MOST_SX2", "33": "I2C_SDA", "36": "I2C_SCL", "37": "MOST_INT",
                 "17": None, "18": None, "19": None, "20": None, "21": None, "22": None, "27": None, "28": None, "32": None,
                 "2": "+3V3", "1": "GND", "15": "GND", "38": "GND", "39": "GND"},
           props=lbl((185.42, 92.71), (205.74, 176.53)))
C(s, "C20", "22u", 101.6, 69.85, "+3V3", "GND", fp=C1206)
C(s, "C21", "100n", 121.92, 69.85, "+3V3", "GND")
R(s, "R20", "10k", 142.24, 69.85, "+3V3", "ESP_EN")
C(s, "C22", "1u", 162.56, 69.85, "ESP_EN", "GND")
R(s, "R21", "10k", 182.88, 69.85, "+3V3", "ESP_IO0")
j3 = s.add("Connector_Generic:Conn_01x06", "J3", "Програмування", 60.96, 129.54,
           fp="Connector_PinHeader_2.54mm:PinHeader_1x06_P2.54mm_Vertical",
           nets={"1": "GND", "2": "ESP_TXD0", "3": "ESP_RXD0", "4": "ESP_EN", "5": "ESP_IO0", "6": "WAKE_FORCE"},
           props=lbl((58.42, 119.38), (58.42, 142.24)))
s.text("J3: 1 GND, 2 TXD0 (вихід ESP32), 3 RXD0, 4 EN, 5 IO0, 6 WAKE_FORCE: 3.3 В тут вмикає живлення без світла в кільці.", 15.24, 152.4)
sw = s.add("Switch:SW_DIP_x04", "SW1", "DP-04 Piano", 304.8, 129.54, fp="Button_Switch_THT:SW_DIP_SPSTx04_Piano_10.8x11.72mm_W7.62mm_P2.54mm",
           nets={"1": "MODE0", "2": "MODE1", "3": "MODE2", "4": "MODE3", "5": "GND", "6": "GND", "7": "GND", "8": "GND"},
           props=lbl((299.72, 119.38), (299.72, 139.7)))
s.text("SW1: клавіші 1-3 - тип підсилювача, 4 - зчитати номер. Замкнена клавіша = 0.", 279.4, 152.4)
for i, x in enumerate((289.56, 309.88, 330.2, 350.52)):
    R(s, "R%d" % (22 + i), "10k", x, 69.85, "+3V3", "MODE%d" % i)
R(s, "R26", "1k", 284.48, 182.88, "LED", "LED_A", "h")
s.two("Device:LED", "D20", "LED 3 mm", 309.88, 182.88, "GND", "LED_A", "h", "LED_THT:LED_D3.0mm", flip=True)
s.text("D20: підказка типу і «номер зчитано».", 279.4, 193.04)

# ---------------------------------------------------------------- outputs
outs = G.Sheet("outputs.kicad_sch", "Виходи: PCM5122, S/PDIF, I2S", 4)
s = outs
s.text("Виходи. Усі беруть той самий оброблений потік на тактах кільця.", 15.24, 22.86, 2.0)
s.text("U3 PCM5122: I2C (MODE1 = 0, MODE2 = 1), адреса 0x4C. SCK = MCLK 12.288 МГц (256Fs), PLL не потрібен. VCOM = 0: режим VREF.", 15.24, 29.21)
s.text("R32/C39 і R33/C40: 470 Ом + 2.2 нФ C0G, зріз ~153 кГц (рекомендація TI для навантаження 10 кОм).", 15.24, 33.02)
u3 = s.add("Audio:PCM5122PW", "U3", "PCM5122PW", 101.6, 125.73, fp="Package_SO:TSSOP-28_4.4x9.7mm_P0.65mm",
           nets={"1": "+3V3A", "8": "+3V3A", "28": "+3V3", "3": "GND", "9": "GND", "27": "GND",
                 "11": "I2C_SDA", "12": "I2C_SCL", "16": "GND", "24": "GND", "17": "GND", "18": "DAC_MODE2",
                 "20": "MCLK", "21": "BCK", "22": "DOUT", "23": "LRCK", "25": "DAC_XSMT",
                 "2": "DAC_CAPP", "4": "DAC_CAPM", "5": "DAC_VNEG", "6": "DAC_OUTL", "7": "DAC_OUTR",
                 "10": "GND", "13": None, "14": None, "15": None, "19": None, "26": "DAC_LDOO"},
           props=lbl((88.9, 97.79), (106.68, 153.67)))
C(s, "C30", "10u", 30.48, 69.85, "+3V3A", "GND", fp=C0805)
C(s, "C31", "100n", 45.72, 69.85, "+3V3A", "GND")
C(s, "C32", "10u", 60.96, 69.85, "+3V3A", "GND", fp=C0805)
C(s, "C33", "100n", 76.2, 69.85, "+3V3A", "GND")
C(s, "C34", "10u", 91.44, 69.85, "+3V3", "GND", fp=C0805)
C(s, "C35", "100n", 106.68, 69.85, "+3V3", "GND")
R(s, "R30", "10k", 121.92, 69.85, "+3V3", "DAC_MODE2")
R(s, "R31", "10k", 142.24, 69.85, "+3V3", "DAC_XSMT")
C(s, "C36", "2.2u", 152.4, 165.1, "DAC_CAPP", "DAC_CAPM", fp=C0805)
C(s, "C37", "2.2u", 172.72, 165.1, "DAC_VNEG", "GND", fp=C0805)
C(s, "C38", "100n", 193.04, 165.1, "DAC_LDOO", "GND")
R(s, "R32", "470", 157.48, 107.95, "DAC_OUTL", "LINE_L", "h")
C(s, "C39", "2.2n C0G", 182.88, 115.57, "LINE_L", "GND")
s.add("jlr_most_amp:RCA", "J4", "RCA L", 210.82, 107.95, nets={"1": "LINE_L", "2": "GND", "3": None},
      props=lbl((208.28, 101.6), (213.36, 101.6)))
R(s, "R33", "470", 157.48, 133.35, "DAC_OUTR", "LINE_R", "h")
C(s, "C40", "2.2n C0G", 182.88, 140.97, "LINE_R", "GND")
s.add("jlr_most_amp:RCA", "J5", "RCA R", 210.82, 133.35, nets={"1": "LINE_R", "2": "GND", "3": None},
      props=lbl((208.28, 127.0), (213.36, 127.0)))
# S/PDIF
s.text("S/PDIF з SX0 OS8104A: TOSLINK напряму, коаксіал через U4 і дільник 249/107 Ом: 0.5 В на 75 Ом.", 254.0, 60.96)
s.text("U5 PLT133 допускає Vcc 2.75..3.25 В, тому живиться через D30: ~3.0 В.", 254.0, 64.77)
s.add("74xGxx:74LVC1G125", "U4", "74LVC1G125", 289.56, 101.6, fp="Package_TO_SOT_SMD:SOT-23-5",
      nets={"1": "GND", "2": "SPDIF", "3": "GND", "4": "COAX_DRV", "5": "+3V3"}, props=lbl((294.64, 88.9), (294.64, 113.03)))
C(s, "C41", "100n", 264.16, 80.01, "+3V3", "GND")
R(s, "R34", "249", 322.58, 101.6, "COAX_DRV", "COAX_MID")
R(s, "R35", "107", 345.44, 116.84, "COAX_MID", "GND")
C(s, "C42", "100n", 368.3, 101.6, "COAX_MID", "COAX_OUT")
s.add("jlr_most_amp:RCA", "J6", "RCA S/PDIF", 398.78, 116.84, nets={"1": "COAX_OUT", "2": "GND", "3": None},
      props=lbl((396.24, 110.49), (378.46, 124.46)))
s.add("jlr_most_amp:PLT133", "U5", "PLT133/T10W", 299.72, 149.86, nets={"3": "SPDIF", "2": "+3V0_TOS", "1": "GND"},
      props=lbl((306.07, 143.51), (306.07, 156.21)))
s.two("Device:D_Schottky", "D30", "BAT54WS", 335.28, 139.7, "+3V0_TOS", "+3V3", "h", "Diode_SMD:D_SOD-323", flip=True)
C(s, "C43", "100n", 358.14, 147.32, "+3V0_TOS", "GND")
s.flag("+3V0_TOS", 375.92, 157.48)
# I2S out
s.text("Роз'єм I2S: буфер U6 і 33 Ом, щоб кабель не навантажував такти PCM5122.", 99.06, 182.88)
for k, (unit, a, y_, net_in, net_out) in enumerate(((1, "2", 195.58, "MCLK", "I2S_MCLK"), (2, "5", 215.9, "BCK", "I2S_BCK"),
                                                   (3, "9", 236.22, "LRCK", "I2S_LRCK"), (4, "12", 256.54, "DOUT", "I2S_DATA"))):
    oe, yy = {1: ("1", "3"), 2: ("4", "6"), 3: ("10", "8"), 4: ("13", "11")}[unit]
    s.add("74xx:74LS125", "U6", "74LVC125A", 165.1, y_, unit=unit, fp="Package_SO:TSSOP-14_4.4x5mm_P0.65mm",
          nets={a: net_in, oe: "GND", yy: net_out + "_B"}, props=lbl((170.18, y_ - 3.81), (170.18, y_ + 5.08)))
    R(s, "R%d" % (36 + k), "33", 212.09, y_, net_out + "_B", net_out, "h")
s.add("74xx:74LS125", "U6", "74LVC125A", 129.54, 223.52, unit=5, fp="Package_SO:TSSOP-14_4.4x5mm_P0.65mm",
      nets={"14": "+3V3", "7": "GND"}, props=lbl((133.35, 213.36), (133.35, 234.95)))
C(s, "C44", "100n", 114.3, 223.52, "+3V3", "GND")
s.add("Connector_Generic:Conn_02x05_Odd_Even", "J7", "I2S", 254.0, 226.06,
      fp="Connector_PinHeader_2.54mm:PinHeader_2x05_P2.54mm_Vertical",
      nets={"1": "I2S_MCLK", "2": "GND", "3": "I2S_BCK", "4": "GND", "5": "I2S_LRCK", "6": "GND",
            "7": "I2S_DATA", "8": "GND", "9": "GND", "10": "GND"}, props=lbl((252.73, 215.9), (252.73, 237.49)))

# ---------------------------------------------------------------- power
pw = G.Sheet("power.kicad_sch", "Живлення і REM", 5)
s = pw
s.text("Живлення. +12 В постійні з роз'єму штатного підсилювача; запобіжник - у джгуті, де він бере +12 В.", 15.24, 22.86, 2.0)
s.text("WAKE = світло (STATUS = 0 відкриває Q1) АБО HOLD від ESP32 АБО WAKE_FORCE з J3. Вмикає U8 (цифра) і U9 (аналог).", 15.24, 29.21)
s.text("У сні живі U7, приймач FOT і логіка WAKE; споживання U8 і U9 у вимкненні ~1 мкА.", 15.24, 33.02)
s.add("Connector_Generic:Conn_02x02_Odd_Even", "J1", "Micro-Fit 3.0 2x2", 33.02, 69.85,
      fp="Connector_Molex:Molex_Micro-Fit_3.0_43045-0412_2x02_P3.00mm_Vertical",
      nets={"1": "+12V_IN", "2": "GND", "3": "REM_OUT", "4": "GND"}, props=lbl((30.48, 62.23), (30.48, 77.47)))
s.text("J1: 1 +12 В, 2 маса, 3 REM, 4 маса.", 15.24, 88.9)
s.flag("+12V_IN", 33.02, 99.06)
s.flag("GND", 60.96, 99.06)
s.two("Device:D_Schottky", "D1", "SS34", 81.28, 69.85, "+12V_D", "+12V_IN", "h", "Diode_SMD:D_SMA", flip=True)
s.two("Device:D_Zener", "D2", "SM6T24A", 104.14, 82.55, "+12V_D", "GND", "v", "Diode_SMD:D_SMB", flip=True)
s.two(L, "L1", "10u 1A", 127.0, 69.85, "+12V_D", "+12V", "h", "Inductor_SMD:L_Bourns-SRN6028")
s.two("Device:C_Polarized", "C50", "47u 50V", 152.4, 80.01, "+12V", "GND", "v", "Capacitor_SMD:CP_Elec_8x10")
s.flag("+12V", 167.64, 99.06)
s.text("D1: Шотткі, тримає заряд C50 при просіданні під час пуску. D2: TVS 600 Вт, клемп ~33 В < 40 В U8, U7, U9, U10.", 15.24, 114.3)
# always-on LDO
s.add("Regulator_Linear:TPS7B8133DRV", "U7", "TPS7B8133DRV", 215.9, 73.66,
      nets={"1": "+12V", "2": "+12V", "3": "GND", "4": "GND", "7": "GND", "5": None, "6": "+3V3_AON"},
      props=lbl((213.36, 64.77), (223.52, 83.82)))
C(s, "C51", "1u 50V", 190.5, 82.55, "+12V", "GND", fp=C0805)
C(s, "C52", "2.2u", 241.3, 82.55, "+3V3_AON", "GND", fp=C0805)
# wake logic
s.add("Transistor_FET:TP0610T", "Q1", "BSS84", 289.56, 73.66, 180, fp="Package_TO_SOT_SMD:SOT-23",
      nets={"1": "FOT_STATUS", "2": "+3V3_AON", "3": "WAKE"}, props=lbl((294.64, 72.39), (294.64, 76.2)))
s.add("Diode:BAT54C", "D3", "BAT54C", 330.2, 69.85, fp="Package_TO_SOT_SMD:SOT-23",
      nets={"1": "HOLD", "2": "WAKE_FORCE", "3": "WAKE"}, props=lbl((327.66, 63.5), (327.66, 82.55)))
R(s, "R50", "100k", 360.68, 80.01, "WAKE", "GND")
R(s, "R51", "100k", 375.92, 80.01, "HOLD", "GND")
R(s, "R52", "100k", 391.16, 80.01, "WAKE_FORCE", "GND")
# buck
s.text("U8 AP64200Q: RT 47.5 кОм -> 2.1 МГц (RT[кОм] = 100000 / f[кГц]), з FSS +-6 % лишається вище 1.71 МГц. 3.3 В: 31.6k/10k, Vref 0.8 В.", 15.24, 121.92)
s.text("Компенсація для fc ~80 кГц: R5 = 4.67e3 * fc * Vout * Cout = 37.4 кОм, C5 = Vout * Cout / (2 А * R5) = 1.5 нФ (DS43577, 16).", 15.24, 125.73)
s.add("jlr_most_amp:AP64200Q", "U8", "AP64200Q", 99.06, 160.02,
      nets={"2": "+12V", "3": "WAKE", "4": "BUCK_RT", "1": "BUCK_BST", "8": "BUCK_SW", "5": "BUCK_FB", "6": "BUCK_COMP",
            "7": "GND", "9": "GND"}, props=lbl((91.44, 149.86), (107.95, 176.53)))
C(s, "C55", "10u 50V", 30.48, 167.64, "+12V", "GND", fp=C1210)
C(s, "C56", "10u 50V", 45.72, 167.64, "+12V", "GND", fp=C1210)
C(s, "C57", "100n 50V", 60.96, 167.64, "+12V", "GND")
R(s, "R53", "47.5k", 68.58, 190.5, "BUCK_RT", "GND")
C(s, "C53", "100n", 134.62, 147.32, "BUCK_BST", "BUCK_SW")
s.two(L, "L2", "4.7u 2A", 147.32, 165.1, "BUCK_SW", "+3V3", "h", "Inductor_SMD:L_Bourns-SRN4018")
s.flag("+3V3", 243.84, 195.58)
C(s, "C58", "22u", 180.34, 175.26, "+3V3", "GND", fp=C1206)
C(s, "C59", "22u", 195.58, 175.26, "+3V3", "GND", fp=C1206)
R(s, "R54", "31.6k", 210.82, 175.26, "+3V3", "BUCK_FB")
R(s, "R55", "10k", 226.06, 175.26, "BUCK_FB", "GND")
R(s, "R56", "37.4k", 134.62, 195.58, "BUCK_COMP", "BUCK_CC")
C(s, "C54", "1.5n", 134.62, 215.9, "BUCK_CC", "GND")
# analog LDO
s.add("Regulator_Linear:TPS7B8133DRV", "U9", "TPS7B8133DRV", 297.18, 165.1,
      nets={"1": "+12V", "2": "WAKE", "3": "GND", "4": "GND", "7": "GND", "5": None, "6": "+3V3A"},
      props=lbl((294.64, 156.21), (304.8, 175.26)))
C(s, "C61", "1u 50V", 271.78, 175.26, "+12V", "GND", fp=C0805)
C(s, "C62", "2.2u", 322.58, 175.26, "+3V3A", "GND", fp=C0805)
s.text("U9: окремий 3.3 В для AVDD і CPVDD PCM5122.", 266.7, 147.32)
# REM
s.text("U10: REM, обмеження 0.5 А: R58 = 0.8 В * 2500 / 0.5 А = 4.0 кОм. DELAY підтягнуто: при замиканні - повторні спроби.", 15.24, 233.68)
s.add("jlr_most_amp:TPS1H200A-Q1", "U10", "TPS1H200A-Q1", 99.06, 261.62,
      nets={"8": "+12V", "1": "REM_IN", "2": "GND", "5": "REM_DELAY", "4": "REM_CL", "7": "REM_OUT", "3": None,
            "6": "GND", "9": "GND"}, props=lbl((91.44, 246.38), (107.95, 276.86)))
R(s, "R57", "4.7k", 53.34, 254.0, "REM_EN", "REM_IN", "h")
R(s, "R58", "4.02k", 68.58, 279.4, "REM_CL", "GND")
R(s, "R59", "10k", 53.34, 271.78, "+3V3", "REM_DELAY")
C(s, "C63", "100n 50V", 134.62, 254.0, "+12V", "GND")

SHEETS = [ring, host, outs, pw]

# ---------------------------------------------------------------- write
ROOT_UUID = G.uid("root")


def title_block(title):
    return ('(title_block (title "JLR-MOST-Amp: %s") (rev "0.1") (company "github.com/Oleg-mk/JLR-MOST-Amp"))' % title)


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
    open(os.path.join(OUT, sh.fname), "w", encoding="utf-8", newline="\n").write(
        G.sheet_text(sh, ROOT_UUID, global_nets, title_block))

# root
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
open(os.path.join(OUT, "jlr_most_amp.kicad_sch"), "w", encoding="utf-8", newline="\n").write("\n".join(parts))

# custom symbol library + tables + project
lib = ["(kicad_symbol_lib (version 20251024) (generator \"jlr_most_amp_gen\") (generator_version \"10.0\")"]
for f in symdefs.ALL:
    lib.append(symdefs.lib_text(f()))
lib.append(")")
open(os.path.join(OUT, "jlr_most_amp.kicad_sym"), "w", encoding="utf-8", newline="\n").write("\n".join(lib))
open(os.path.join(OUT, "fp-lib-table"), "w", encoding="utf-8", newline="\n").write(
    '(fp_lib_table (version 7)\n  (lib (name "jlr_most_amp")(type "KiCad")(uri "${KIPRJMOD}/jlr_most_amp.pretty")(options "")(descr "JLR-MOST-Amp footprints"))\n)\n')
open(os.path.join(OUT, "sym-lib-table"), "w", encoding="utf-8", newline="\n").write(
    '(sym_lib_table (version 7)\n  (lib (name "jlr_most_amp")(type "KiCad")(uri "${KIPRJMOD}/jlr_most_amp.kicad_sym")(options "")(descr "JLR-MOST-Amp parts"))\n)\n')
pro = os.path.join(OUT, "jlr_most_amp.kicad_pro")
if not os.path.exists(pro):
    json.dump({"meta": {"filename": "jlr_most_amp.kicad_pro", "version": 3}, "schematic": {}, "boards": [], "sheets": [],
               "text_variables": {}}, open(pro, "w", encoding="utf-8"), indent=2)

# intended connections for the check
intent = {}
for sh in SHEETS:
    for (ref, pin), net in sh.conn.items():
        if ref.startswith("#"):
            continue
        intent["%s.%s" % (ref, pin)] = net
json.dump(intent, open(os.path.join(OUT, "gen", "intent.json"), "w"), indent=0, sort_keys=True)
print("sheets written; global nets:", len(global_nets), "pins:", len(intent))
