"""JLR-MOST-Amp schematic: root + 4 sheets, written for KiCad 10."""
import os, sys, json
import schgen as G
import symdefs

OUT = sys.argv[1] if len(sys.argv) > 1 else "out"
os.makedirs(OUT, exist_ok=True)

# H2 is a hole, not a part: on the board, out of the BOM
_sym_text = G.sym_text
G.sym_text = lambda sy, path: (_sym_text(sy, path).replace("(in_bom yes)", "(in_bom no)", 1)
                               if sy.lib_id == "Mechanical:MountingHole_Pad" else _sym_text(sy, path))

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
                 "4": "RMCK", "13": "FSY", "14": "SCK", "9": "MOST_SPDIF", "36": "MOST_SX1", "10": None,
                 "12": "MOST_SX3", "15": "GND", "35": "SPDIF_SRC", "16": "GND", "17": "GND",
                 "20": "XTI", "21": "XTO", "26": "FLT", "23": None, "37": None, "33": None,
                 "5": "+3V3", "18": "+3V3", "29": "+3V3_NICA", "30": "+3V3_NICA",
                 "6": "GND", "19": "GND", "27": "GND", "28": "GND"},
           props=lbl((209.55, 107.95), (231.14, 191.77)))
s.text("U1: I2C, адреса 0x46/0x47 (AD0 = AD1 = 1). PAR_CP, PAR_SRC, ASYNC, LEG = 0: послідовні порти, режим Enhanced.", 15.24, 40.64)
s.text("SX0 - S/PDIF (вихід, через R18), SX1 і SX3 - приймачі на ESP32, SR1 - потік для S/PDIF від ESP32. SR0, SR2, SR3 не плавають.", 15.24, 44.45)
s.text("C15-C17 і C69-C71 - по 100 нФ під U1 (бік F) на кожному виводі +3V3: 24, 32, 38 і 5, 7/8, 18. C5-C7 - з боку B.", 15.24, 48.26)
# supplies
C(s, "C5", "100n", 196.85, 69.85, "+3V3", "GND")
C(s, "C6", "100n", 207.01, 69.85, "+3V3", "GND")
C(s, "C7", "10u", 217.17, 69.85, "+3V3", "GND", fp=C0805)
FB(s, "FB3", 232.41, 69.85, "+3V3", "+3V3_NICA")
C(s, "C8", "100n", 252.73, 69.85, "+3V3_NICA", "GND")
C(s, "C9", "100n", 273.05, 69.85, "+3V3_NICA", "GND")
C(s, "C10", "10u", 293.37, 69.85, "+3V3_NICA", "GND", fp=C0805)
C(s, "C15", "100n", 313.69, 69.85, "+3V3", "GND")
C(s, "C16", "100n", 334.01, 69.85, "+3V3", "GND")
C(s, "C17", "100n", 354.33, 69.85, "+3V3", "GND")
C(s, "C69", "100n", 313.69, 90.17, "+3V3", "GND")
C(s, "C70", "100n", 334.01, 90.17, "+3V3", "GND")
C(s, "C71", "100n", 354.33, 90.17, "+3V3", "GND")
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
R(s, "R13", "68", 285.75, 118.11, "RMCK", "MCLK", "h")
R(s, "R14", "47", 285.75, 128.27, "FSY", "LRCK", "h")
R(s, "R15", "47", 285.75, 138.43, "SCK", "BCK", "h")
R(s, "R18", "68", 285.75, 148.59, "MOST_SPDIF", "SPDIF", "h")
R(s, "R16", "10k", 320.04, 125.73, "SCK", "GND")
s.text("R13 68, R14 і R15 47, R18 68 Ом: узгодження біля U1 довгих ліній (MCLK 102 мм, LRCK 86, BCK 80, S/PDIF 65) при Z0 ~130 Ом.", 274.32, 104.14)
s.text("R16 10 кОм: SCK не плаває після скидання (DS8104AFP4, табл. 19-1); проти ~44 кОм підтяжки MTDO (GPIO15) дає ~0.6 В - чіткий 0 і для strapping MTDO.", 274.32, 107.95)
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
s.text("ESP32-WROVER-IE. I2S0: SX1 -> IO14, потік для S/PDIF IO12 -> SR1. I2S1: SX3 -> IO5, DOUT IO2 -> PCM5122 і HDMI.", 15.24, 22.86, 2.0)
s.text("Усі чотири секції I2S ведені від BCK (IO15) і LRCK (IO4) з OS8104A. I2C0 (IO26, IO27) - OS8104A, I2C1 (IO21, IO19) - PCM5122.", 15.24, 29.21)
s.text("IO12 = SPDIF_SRC, вихід ESP32: при старті R17 тримає його низьким, flash 3.3 В. Виводи - за порядком ліній OS8104A на платі.", 15.24, 33.02)
s.text("IO2 = DOUT (лише вихід на PCM5122 і U6): при старті ніщо не тягне його вгору, завантаження через USB (U11) працює. HOLD = IO33, REM_EN = IO32 (обидва RTC).", 15.24, 36.83)
s.text("Прошивка: сила виходів ESP32 (gpio drive capability) 0..1 для DOUT і SPDIF_SRC, 0 для решти виходів - повільніші фронти, менше випромінювання.", 15.24, 40.64)
u2 = s.add("jlr_most_amp:ESP32-WROVER-IE", "U2", "ESP32-WROVER-IE-N16R8", 200.66, 133.35,
           nets={"3": "ESP_EN", "25": "ESP_IO0", "35": "ESP_TXD0", "34": "ESP_RXD0",
                 "4": "MODE0", "5": "MODE1", "6": "MODE2", "7": "MODE3", "8": "REM_EN", "9": "HOLD",
                 "10": "MOST_INT", "11": "I2C_SDA", "12": "I2C_SCL", "13": "MOST_SX1", "14": "SPDIF_SRC", "16": "MOST_RST",
                 "23": "BCK", "24": "DOUT", "26": "LRCK", "29": "MOST_SX3", "31": "DAC_SCL", "33": "DAC_SDA", "36": "LED",
                 "17": None, "18": None, "19": None, "20": None, "21": None, "22": None, "30": None, "27": None, "28": None,
                 "32": None, "37": None,
                 "2": "+3V3", "1": "GND", "15": "GND", "38": "GND", "39": "GND"},
           props=lbl((185.42, 92.71), (205.74, 176.53)))
C(s, "C20", "22u", 101.6, 69.85, "+3V3", "GND", fp=C1206)
C(s, "C21", "100n", 121.92, 69.85, "+3V3", "GND")
R(s, "R20", "10k", 142.24, 69.85, "+3V3", "ESP_EN")
C(s, "C22", "1u", 162.56, 69.85, "ESP_EN", "GND")
R(s, "R21", "10k", 182.88, 69.85, "+3V3", "ESP_IO0")
sw = s.add("Switch:SW_DIP_x04", "SW1", "DP-04 Piano", 304.8, 129.54, fp="Button_Switch_THT:SW_DIP_SPSTx04_Piano_10.8x11.72mm_W7.62mm_P2.54mm",
           nets={"5": "MODE0", "6": "MODE1", "7": "MODE2", "8": "MODE3", "1": "GND", "2": "GND", "3": "GND", "4": "GND"},
           props=lbl((299.72, 119.38), (299.72, 139.7)))
s.text("SW1: клавіші 1-3 (MODE3, MODE2, MODE1) - тип підсилювача, 4 (MODE0) - зчитати номер. Замкнена клавіша = 0. Бік 5-8 - до ESP32, 1-4 - земля.", 279.4, 152.4)
for i, x in enumerate((289.56, 309.88, 330.2, 350.52)):
    R(s, "R%d" % (22 + i), "10k", x, 69.85, "+3V3", "MODE%d" % i)
R(s, "R26", "1k", 284.48, 182.88, "LED", "LED_A", "h")
s.two("Device:LED", "D20", "LED 3 mm", 309.88, 182.88, "GND", "LED_A", "h", "LED_THT:LED_D3.0mm", flip=True)
s.text("D20: підказка типу і «номер зчитано».", 279.4, 193.04)
# USB-UART: the log and the flashing
s.add("Connector:USB_C_Receptacle_USB2.0_14P", "J10", "USB-C", 40.64, 228.6,
      fp="jlr_most_amp:USB_C_Receptacle_14P_SideInsert_THT",
      nets={"A4": "USB_VBUS", "A9": "USB_VBUS", "B4": "USB_VBUS", "B9": "USB_VBUS", "A5": "USB_CC1", "B5": "USB_CC2",
            "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM",
            "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND"},
      props=lbl((30.48, 207.01), (40.64, 207.01)))
R(s, "R60", "5.1k", 71.12, 248.92, "USB_CC1", "GND")
R(s, "R61", "5.1k", 81.28, 248.92, "USB_CC2", "GND")
s.add("Power_Protection:USBLC6-2P6", "U13", "USBLC6-2SC6", 106.68, 241.3, fp="Package_TO_SOT_SMD:SOT-23-6",
      nets={"1": "USB_DP", "6": "USB_DP", "3": "USB_DM", "4": "USB_DM", "5": "USB_VBUS", "2": "GND"},
      props=lbl((111.76, 252.73), (111.76, 255.27)))
s.add("Regulator_Linear:AP2204K-1.5", "U12", "AP2112K-3.3", 106.68, 205.74, fp="Package_TO_SOT_SMD:SOT-23-5",
      nets={"1": "USB_VBUS", "3": "USB_VBUS", "4": None, "5": "USB_3V3", "2": "GND"},
      props=lbl((116.84, 205.74), (116.84, 208.28)))
s.flag("USB_VBUS", 78.74, 205.74)
C(s, "C66", "1u", 91.44, 220.98, "USB_VBUS", "GND")
C(s, "C67", "1u", 127.0, 220.98, "USB_3V3", "GND")
C(s, "C68", "100n", 137.16, 220.98, "USB_3V3", "GND")
s.add("jlr_most_amp:CH340C", "U11", "CH340C", 165.1, 233.68,
      nets={"5": "USB_DP", "6": "USB_DM", "7": None, "8": None, "2": "USB_TXD", "3": "USB_RXD", "13": "USB_DTR",
            "14": "USB_RTS", "9": None, "10": None, "11": None, "12": None, "15": None,
            "16": "USB_3V3", "4": "USB_3V3", "1": "GND"},
      props=lbl((177.8, 252.73), (177.8, 255.27)))
R(s, "R62", "4.7k", 198.12, 215.9, "USB_TXD", "ESP_RXD0")
R(s, "R63", "4.7k", 213.36, 215.9, "ESP_TXD0", "USB_RXD")
R(s, "R64", "10k", 228.6, 215.9, "USB_DTR", "USB_Q2B")
R(s, "R65", "10k", 241.3, 215.9, "USB_RTS", "USB_Q3B")
s.add("Transistor_BJT:Q_NPN_BEC", "Q2", "MMBT3904", 228.6, 243.84, fp="Package_TO_SOT_SMD:SOT-23",
      nets={"1": "USB_Q2B", "2": "USB_RTS", "3": "ESP_EN"})
s.add("Transistor_BJT:Q_NPN_BEC", "Q3", "MMBT3904", 254.0, 243.84, fp="Package_TO_SOT_SMD:SOT-23",
      nets={"1": "USB_Q3B", "2": "USB_DTR", "3": "ESP_IO0"})
# flash and log without the USB bridge (Bitline Lite)
s.add("Connector_Generic:Conn_01x04", "J3", "штирі 1x4", 302.26, 233.68, fp="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
      nets={"1": "ESP_IO0", "2": "GND", "3": "ESP_RXD0", "4": "ESP_TXD0"}, props=lbl((304.8, 228.6), (304.8, 241.3)), dnp=True)
s.text("J3: штирі для запису й журналу без USB (Bitline Lite), адаптер USB-UART 3.3 В. Завод не ставить, паяти вручну за потреби.", 279.4, 213.36)
s.text("3 RXD0 на TX адаптера, 4 TXD0 на його RX, 2 маса. +3.3 В адаптера не підключати: плата живиться від J1.", 279.4, 217.17)
s.text("Запис: перемичка на виводи 1 і 2 (IO0 на масу), на столі без кільця тримати SW2 увесь запис.", 279.4, 220.98)
s.text("U11 CH340C: USB - UART0 для журналу й прошивки без адаптера. Живиться від USB через U12, не від плати: COM-порт не зникає, коли плата засинає й прокидається з кільцем.", 15.24, 264.16)
s.text("R62, R63 4.7 кОм: міст і вимкнена плата не живлять одне одного через TX/RX. На платі кожен біля свого передавача (R62 - біля U11, R63 - біля U2), тож довгі лінії між ними мають пологі фронти.", 15.24, 267.97)
s.text("Q2, Q3: перезапуск у завантажувач від DTR/RTS, як на ESP32 DevKitC. Термінал, що смикає DTR/RTS при відкритті порту, перезапускає ESP32.", 15.24, 271.78)
s.text("J10 USB-C: R60, R61 5.1 кОм на CC - пристрій USB 2.0. U13 - захист D+/D- і VBUS від статики. AP2112K-3.3 і USBLC6-2SC6 - на символах AP2204K і USBLC6-2P6 з тими самими виводами.", 15.24, 275.59)

# ---------------------------------------------------------------- outputs
outs = G.Sheet("outputs.kicad_sch", "Виходи: PCM5122, S/PDIF, I2S", 4)
s = outs
s.text("Виходи. Усі беруть той самий оброблений потік на тактах кільця.", 15.24, 22.86, 2.0)
s.text("U3 PCM5122: власна шина I2C від ESP32 (MODE1 = 0, MODE2 = 1), адреса 0x4C. SCK = MCLK 12.288 МГц (256Fs), PLL не потрібен. VCOM = 0: режим VREF.", 15.24, 29.21)
s.text("R32/C39 і R33/C40: 470 Ом + 2.2 нФ C0G, зріз ~153 кГц (рекомендація TI для навантаження 10 кОм).", 15.24, 33.02)
u3 = s.add("Audio:PCM5122PW", "U3", "PCM5122PW", 101.6, 125.73, fp="Package_SO:TSSOP-28_4.4x9.7mm_P0.65mm",
           nets={"1": "+3V3A", "8": "+3V3A", "28": "+3V3", "3": "GND", "9": "GND", "27": "GND",
                 "11": "DAC_SDA", "12": "DAC_SCL", "16": "GND", "24": "GND", "17": "GND", "18": "DAC_MODE2",
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
R(s, "R27", "4.7k", 162.56, 69.85, "+3V3", "DAC_SDA")
R(s, "R28", "4.7k", 182.88, 69.85, "+3V3", "DAC_SCL")
C(s, "C36", "2.2u", 152.4, 165.1, "DAC_CAPP", "DAC_CAPM", fp=C0805)
C(s, "C37", "2.2u", 172.72, 165.1, "DAC_VNEG", "GND", fp=C0805)
C(s, "C38", "100n", 193.04, 165.1, "DAC_LDOO", "GND")
R(s, "R32", "470", 157.48, 107.95, "DAC_OUTL", "LINE_L", "h")
C(s, "C39", "2.2n C0G", 182.88, 115.57, "LINE_L", "GND")
s.add("jlr_most_amp:RCA", "J4", "RCA L", 210.82, 107.95, nets={"1": "LINE_L", "2": "GND", "3": "GND"},
      props=lbl((208.28, 101.6), (213.36, 101.6)))
R(s, "R33", "470", 157.48, 133.35, "DAC_OUTR", "LINE_R", "h")
C(s, "C40", "2.2n C0G", 182.88, 140.97, "LINE_R", "GND")
s.add("jlr_most_amp:RCA", "J5", "RCA R", 210.82, 133.35, nets={"1": "LINE_R", "2": "GND", "3": "GND"},
      props=lbl((208.28, 127.0), (213.36, 127.0)))
s.add("Connector_Generic:Conn_01x04", "J8", "штирі 1x4", 236.22, 120.65, fp="Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
      nets={"1": "LINE_L", "2": "GND", "3": "LINE_R", "4": "GND"}, props=lbl((233.68, 110.49), (233.68, 133.35)))
s.text("J8: штирі 1x4 2.54 на боці B між R32/R33 і гніздами, стопкою в гніздо лампової плати: 1 LINE_L, 2 маса, 3 LINE_R, 4 маса. Без лампової плати не ставиться.", 15.24, 36.83)
# S/PDIF
s.text("S/PDIF з SX0 OS8104A через R18: TOSLINK напряму, коаксіал через U4 і дільник 249/107 Ом: 0.5 В на 75 Ом.", 254.0, 60.96)
s.text("U5 PLT133/T10W: Vcc 2.7..5.5 В (DPL-0000049 Rev.5), живлення прямо від +3V3, C43 біля виводу Vcc.", 254.0, 64.77)
s.text("Виводи U5 за даташитом: 1 Vin, 2 Vcc, 3 GND; дивлячись на порт, зліва направо (стор. 4: рис. Using Method і нумерований вид знизу). Звірено 11-05, docs/board_check.", 254.0, 68.58)
s.text("J4, J5, J6 BKL 0101153: зовнішній контакт - тонкий вивід 3 біля центрального, товсті ніжки 2 механічні (креслення, 2 полюси в каталозі). На масі обидва.", 15.24, 40.64)
s.add("74xGxx:74LVC1G125", "U4", "74LVC1G125", 289.56, 101.6, fp="Package_TO_SOT_SMD:SOT-23-5",
      nets={"1": "GND", "2": "SPDIF", "3": "GND", "4": "COAX_DRV", "5": "+3V3"}, props=lbl((294.64, 88.9), (294.64, 113.03)))
C(s, "C41", "100n", 264.16, 80.01, "+3V3", "GND")
R(s, "R34", "249", 322.58, 101.6, "COAX_DRV", "COAX_MID")
R(s, "R35", "107", 345.44, 116.84, "COAX_MID", "GND")
C(s, "C42", "100n", 368.3, 101.6, "COAX_MID", "COAX_OUT")
s.add("jlr_most_amp:RCA", "J6", "RCA S/PDIF", 398.78, 116.84, nets={"1": "COAX_OUT", "2": "GND", "3": "GND"},
      props=lbl((396.24, 110.49), (378.46, 124.46)))
s.add("jlr_most_amp:PLT133", "U5", "PLT133/T10W", 299.72, 149.86, nets={"1": "SPDIF", "2": "+3V3", "3": "GND"},
      props=lbl((306.07, 143.51), (306.07, 156.21)))
C(s, "C43", "100n", 358.14, 147.32, "+3V3", "GND")
# I2S out: HDMI, PS Audio pinout, LVDS
s.text("I2S на HDMI, розпіновка PS Audio: 3/1 DATA+/-, 4/6 BCK+/-, 9/7 LRCK+/-, 10/12 MCLK+/-, 2 5 8 11 17 19 - GND.", 99.06, 182.88)
s.text("U6 переводить чотири лінії в LVDS, кабель не навантажує такти PCM5122. EN = 1, EN* = 0 (SNLS044D, Pin Functions). DS90LV047A: виводи такі самі, як у SN65LVDS047 цього символу.", 99.06, 186.69)
s.text("Канали U6: 4 - MCLK, 3 - BCK, 2 - DATA, 1 - LRCK. MCLK і BCK на дальньому ряду HDMI, туди лише в обхід краю роз'єму.", 99.06, 190.5)
s.text("Полярність виходів U6 чергується, у HDMI ні: пари BCK і LRCK на платі один раз перехрещені (перемичка на фользі).", 99.06, 194.31)
s.add("Interface:SN65LVDS047PW", "U6", "DS90LV047ATMTCX", 165.1, 223.52, fp="Package_SO:TSSOP-16_4.4x5mm_P0.65mm",
      nets={"1": "+3V3", "2": "LRCK", "3": "DOUT", "4": "+3V3", "5": "GND", "6": "BCK", "7": "MCLK", "8": "GND",
            "9": "I2S_MCLK_N", "10": "I2S_MCLK_P", "11": "I2S_BCK_P", "12": "I2S_BCK_N",
            "13": "I2S_DATA_N", "14": "I2S_DATA_P", "15": "I2S_LRCK_P", "16": "I2S_LRCK_N"},
      props=lbl((170.18, 205.74), (170.18, 241.3)))
C(s, "C44", "100n", 129.54, 223.52, "+3V3", "GND")
s.add("Connector:HDMI_A", "J7", "HDMI I2S", 254.0, 223.52,
      fp="Connector_Video:HDMI_A_Kycon_KDMIX-SL1-NS-WS-B15_VerticalRightAngle",
      nets={"1": "I2S_DATA_N", "3": "I2S_DATA_P", "4": "I2S_BCK_P", "6": "I2S_BCK_N",
            "7": "I2S_LRCK_N", "9": "I2S_LRCK_P", "10": "I2S_MCLK_P", "12": "I2S_MCLK_N",
            "2": "GND", "5": "GND", "8": "GND", "11": "GND", "17": "GND", "19": "GND", "SH": "GND",
            "13": None, "14": None, "15": None, "16": None, "18": None},
      props=lbl((254.0, 195.58), (254.0, 251.46)))

# ---------------------------------------------------------------- power
pw = G.Sheet("power.kicad_sch", "Живлення і REM", 5)
s = pw
s.text("Живлення. +12 В постійні з роз'єму штатного підсилювача; F1 одразу за роз'ємом, за ним 100 нФ кераміки.", 15.24, 22.86, 2.0)
s.text("WAKE = світло (STATUS = 0 відкриває Q1) АБО HOLD від ESP32 АБО кнопка SW2 (WAKE_FORCE). Вмикає U8 (цифра); U9 (аналог) вмикається від його +3V3.", 15.24, 29.21)
s.text("У сні живі U7, приймач FOT і логіка WAKE; споживання U8 і U9 у вимкненні ~1 мкА.", 15.24, 33.02)
s.text("SW2: натиснути на столі без кільця - плата вмикається, далі живлення тримає HOLD. Прошивка через USB без кільця - тримати SW2. R52 тримає WAKE_FORCE у нулі.", 15.24, 36.83)
s.add("Connector_Generic:Conn_02x02_Odd_Even", "J1", "43045-0421", 33.02, 69.85,
      fp="Connector_Molex:Molex_Micro-Fit_3.0_43045-0421_2x02-1MP_P3.00mm_Horizontal",
      nets={"1": "+12V_IN", "2": "GND", "3": "REM_OUT", "4": "GND"}, props=lbl((30.48, 62.23), (30.48, 77.47)))
s.text("J1: Micro-Fit 3.0 2x2 кутовий з лапками (43045-0421), фронтом у стінку з RCA і HDMI; штекер 43025-0400. 1 +12 В, 2 маса, 3 REM, 4 маса.", 15.24, 88.9)
s.add("Connector_Generic:Conn_01x05", "J9", "штирі 1x5", 380.0, 106.68, fp="Connector_PinHeader_2.54mm:PinHeader_1x05_P2.54mm_Vertical",
      nets={"1": "GND", "2": "REM_OUT", "3": "+12V_IN", "4": "+12V_IN", "5": "GND"}, props=lbl((377.46, 96.52), (377.46, 119.38)))
s.text("J9: штирі 1x5 2.54 на боці B над J1, стопкою в гніздо лампової плати: 1 маса, 2 REM_OUT (її REM_IN), 3 і 4 +12 В до F1 (1.2 А, пуск ниток до 2.9 А; лампова має свій F1 3 А), 5 маса. "
       "Без лампової плати не ставиться; стопка штирі + гніздо 8.5 мм = 11 мм, стійки M2.5 x 11 на H1-H5.", 15.24, 140.97)
s.flag("+12V_IN", 33.02, 99.06)
s.flag("GND", 60.96, 99.06)
s.two("Device:Fuse", "F1", "1.5A T", 53.34, 69.85, "+12V_IN", "+12V_F", "h", "Fuse:Fuse_1206_3216Metric")
C(s, "C64", "100n 50V", 66.04, 80.01, "+12V_F", "GND")
s.two("Device:D_Schottky", "D1", "SS34", 81.28, 69.85, "+12V_D", "+12V_F", "h", "Diode_SMD:D_SMA", flip=True)
s.two("Device:D_Zener", "D2", "SM6T30A", 104.14, 82.55, "+12V_D", "GND", "v", "Diode_SMD:D_SMB", flip=True)
s.two(L, "L1", "10u NRSE6028-100M", 127.0, 69.85, "+12V_D", "+12V", "h", "Inductor_SMD:L_Bourns-SRN6028")
s.two("Device:C_Polarized", "C50", "47u 50V", 152.4, 80.01, "+12V", "GND", "v", "Capacitor_SMD:CP_Elec_8x10")
s.two("Device:C_Polarized", "C65", "47u 50V", 160.02, 80.01, "+12V", "GND", "v", "Capacitor_SMD:CP_Elec_8x10")
C(s, "C75", "100n 50V", 172.72, 80.01, "+12V", "GND")
C(s, "C76", "100n 50V", 180.34, 80.01, "+12V", "GND")
s.flag("+12V", 167.64, 99.06)
s.add("Mechanical:MountingHole_Pad", "H2", "M2.5", 88.9, 96.52, fp="jlr_most_amp:MountingHole_2.7mm_M2.5_Standoff_GND", nets={"1": "GND"})
s.text("H2 - зв'язок маси з металевим корпусом у кута J1/J9 (J9.1 за 5.5 мм, J1 за 11 мм): різниця кузов - маса джгута не тече через звукову частину. Маса лампової - через J9, її кріпильні отвори не зв'язані.", 15.24, 109.22)
s.text("C65 паралельно C50 (2 x 47 мкФ). C75, C76 - кераміка при C65 і C50: ВЧ, яку електроліти не беруть. D1: Шотткі, тримає заряд C50/C65 при просіданні під час пуску. D2: TVS 600 Вт, клемп ~33 В < 40 В U7, U9, U10 (U8 - до 65 В).", 15.24, 114.3)
s.text("F1 1.5 А з затримкою: плата до ~0.3 А плюс REM до 0.5 А. Стоїть перед D2, тож тривалий пробій TVS розриває F1.", 15.24, 118.11)
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
R(s, "R52", "10k", 391.16, 80.01, "WAKE_FORCE", "GND")
s.add("Switch:SW_Push", "SW2", "TL3305A", 375.92, 57.15, fp="Button_Switch_SMD:SW_SPST_TL3305A",
      nets={"1": "+3V3_AON", "2": "WAKE_FORCE"}, props=lbl((375.92, 52.07), (375.92, 60.96)))
# buck
s.text("U8 LM65625-Q1 (SNVSCH2A): FB на GND - фіксовані 3.3 В, BIAS - вхід зворотного зв'язку з виходу; RT на GND - 2.2 МГц (1.98..2.42).", 15.24, 121.92)
s.text("MODE на VCC - FPWM з DRSS +-5 %: частота не нижче 1.88 МГц, над AM. tON min 40 нс: при 16 В на 2.54 МГц треба 81 нс. Компенсація внутрішня.", 15.24, 125.73)
s.text("L2: Isat вище межі струму U8 (4.9 А макс): CKCSA4020-2.2uH/M - 6.1 А при -30 %, 48 мОм, стає на майданчики XGL4030. C57 і C60 - кожен впритул до своєї пари VIN/PGND.", 15.24, 129.54)
s.add("jlr_most_amp:LM65625-Q1", "U8", "LM65625-Q1", 99.06, 160.02,
      nets={"11": "+12V", "17": "+12V", "9": "GND", "19": "GND", "21": "GND",
            "13": "BUCK_SW", "14": "BUCK_SW", "15": "BUCK_BST", "20": "+3V3",
            "7": "WAKE", "5": "BUCK_VCC", "4": "BUCK_VCC", "6": "GND", "3": "GND", "2": None,
            "1": None, "8": None, "10": None, "12": None, "16": None, "18": None},
      props=lbl((88.9, 143.51), (109.22, 176.53)))
C(s, "C55", "10u 50V", 30.48, 167.64, "+12V", "GND", fp=C1210)
C(s, "C56", "10u 50V", 43.18, 167.64, "+12V", "GND", fp=C1210)
C(s, "C57", "100n 50V", 55.88, 167.64, "+12V", "GND")
C(s, "C60", "100n 50V", 68.58, 167.64, "+12V", "GND")
C(s, "C53", "100n", 134.62, 147.32, "BUCK_BST", "BUCK_SW")
C(s, "C54", "1u", 134.62, 185.42, "BUCK_VCC", "GND")
s.two(L, "L2", "2.2u CKCSA4020-2.2uH/M", 147.32, 165.1, "BUCK_SW", "+3V3", "h", "Inductor_SMD:L_Coilcraft_XxL4030")
s.flag("+3V3", 243.84, 195.58)
C(s, "C58", "22u", 180.34, 175.26, "+3V3", "GND", fp=C1206)
C(s, "C59", "22u", 195.58, 175.26, "+3V3", "GND", fp=C1206)
# analog LDO
s.add("Regulator_Linear:TPS7B8133DRV", "U9", "TPS7B8133DRV", 297.18, 165.1,
      nets={"1": "+12V", "2": "+3V3", "3": "GND", "4": "GND", "7": "GND", "5": None, "6": "+3V3A"},
      props=lbl((294.64, 156.21), (304.8, 175.26)))
C(s, "C61", "1u 50V", 271.78, 175.26, "+12V", "GND", fp=C0805)
C(s, "C62", "2.2u", 322.58, 175.26, "+3V3A", "GND", fp=C0805)
C(s, "C72", "1u 50V", 256.54, 175.26, "+3V3", "GND", fp=C0805)
C(s, "C73", "100n", 241.3, 175.26, "+3V3", "GND")
C(s, "C74", "100n", 337.82, 175.26, "+3V3A", "GND")
s.text("U9: окремий 3.3 В для AVDD і CPVDD PCM5122. EN від +3V3: AVDD іде слідом за DVDD.", 266.7, 147.32)
s.text("C72, C73 - +3V3 там, де він розходиться на EN U9, DVDD U3 і підтяжки I2C. C74 - ВЧ на виході U9.", 266.7, 151.13)
# REM
s.text("U10: REM, обмеження 0.5 А: R58 = 0.8 В * 2500 / 0.5 А = 4.0 кОм. DELAY підтягнуто: при замиканні - повторні спроби.", 15.24, 233.68)
s.add("jlr_most_amp:TPS1H200A-Q1", "U10", "TPS1H200A-Q1", 99.06, 261.62,
      nets={"8": "+12V", "1": "REM_IN", "2": "GND", "5": "REM_DELAY", "4": "REM_CL", "7": "REM_OUT", "3": None,
            "6": "GND", "9": "GND"}, props=lbl((91.44, 246.38), (107.95, 276.86)))
R(s, "R57", "4.7k", 53.34, 254.0, "REM_EN", "REM_IN", "h")
R(s, "R58", "4.02k", 68.58, 279.4, "REM_CL", "GND")
R(s, "R59", "10k", 53.34, 271.78, "+3V3", "REM_DELAY")
C(s, "C63", "100n 50V", 134.62, 254.0, "+12V", "GND")
C(s, "C77", "100n 50V", 152.4, 254.0, "REM_OUT", "GND")
s.text("C77 при J9.2 і J1.3: ВЧ із кабелю REM одразу на масу. Вихід U10 наростає до 0.6 В/мкс, C77 бере до 60 мА - далеко до межі 0.5 А.", 15.24, 237.49)

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
