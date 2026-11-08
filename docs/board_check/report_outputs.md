# Audio outputs: independent check against manufacturers' documents

Nothing was changed. I used in-memory Python only (`python -I`). The harness saved one long output to its own tool-results folder.

## 1. PROBLEMS

### P1. U5 pads 1 and 3: Vin and GND are reversed (highest consequence)

**Document says** (PLT133/T10W Rev.5, p.4):
- The numbered plan view has pegs 4 and 5 above the pins, and pins 1, 2, 3 left to right.
- The functions are 1 Vin, 2 Vcc, 3 GND.
- That view has no caption.

**My inference: that view is from the pin side.**
- It shows the pin tips (0.5 x 0.4) and the peg ends.
- It shows the 1.0 mm notch that the side view places on the pin side, 2.35 mm from the front.
- The other plan view has no pins. It carries the "PLT T10" mark and the circle and two windows of the top face in the photo (clearest on the PLT237 sheet, p.1).
- So, seen from above with pegs above pins, pin 1 is on the right.
- Facing the port, the order left to right is Vin, Vcc, GND.

**Cross-check, same handedness** (other documents on this PC):
- PLT133 Rev.2, p.2: front view toward the light output reads 3 2 1, with 3 = Vin and 1 = GND.
- Cliff FCR684205T, p.3: front view reads 3 2 1, with 3 = Vin and 1 = GND.

**Board** (footprint frame, top side, not mirrored): pegs are at y = +2.63, so the port faces +y and -x is on your left when facing the port. U5 is placed at -90 degrees with the port toward the x = 100 edge; rotation does not change handedness.

| Pad | Position | Gets per document | Net on board | Verdict |
|---|---|---|---|---|
| 1 | -2.54, 0 (left) | Vin | GND | PROBLEM |
| 2 | 0, 0 (middle) | Vcc | +3V3 | ok |
| 3 | +2.54, 0 (right) | GND | SPDIF | PROBLEM |

Likely origin (my inference): the symbol numbering (1 GND, 2 Vcc, 3 Vin) is the one used by Rev.2 and Cliff, but the footprint's pad numbers sit where T10W's numbers are.

**Real-board effect, document:**
- Vin is limited to -0.5 V..Vcc+0.5 V (p.2).
- SX0 low: the part is powered with Vin low, so the LED is off (p.3).
- SX0 high: Vcc-GND = 0 and Vin is 3.3 V below U5's GND pin, so the rating is broken and there is no light.
- Result: no optical output in any state.

**Real-board effect, my inference:**
- An internal input clamp conducts from SPDIF through U5 to GND and holds the net near one diode drop.
- U4.2 then never reaches logic high (VIH 2 V from memory), so coax J6 is likely dead too.
- U1.9 SX0 is specified at 2.4 mA (OS8104A p.168) but sources roughly 10 mA through R18, and also sinks U5's 3-10 mA when low.
- DAC and HDMI are unaffected.

**Correct assignment:** pad 1 SPDIF, pad 2 +3V3, pad 3 GND. `hw4/jlr_most_amp.kicad_pcb` has the same wrong nets.

**To settle it on a loose part:** put +3.3 V on the centre pin, GND on the right-hand pin facing the port, and leave the left pin open. The LED should be on (p.3).

**Conflict with prior notes:** the memory index attached to this session says the TOSLINK pins are right and Codex's are swapped. I did not open that file and did not use it as evidence. This finding contradicts it.

### P2. U6 bypass, minor

**Document says** (DS90LV047A p.16): 0.1 uF and 0.001 uF in parallel at the supply pin.

**Board:** C44 100 nF only.

**My inference:** more high-frequency noise on +3V3 at U6, no functional failure.

### CHECKs (not settled, ranked)

**C1. J4, J5, J6 return path.**
- The BKL 0101153 drawing (`C:\Users\geniy\Downloads\TDS-BKL-BKL-Electronic-DE-EN-1565619614199.pdf`) has balloons with no legend.
- Geometry matches the footprint: Ø1 holes 2.5 apart, Ø2 holes 10.1 apart, rows 5.1.
- GND goes only to the thick legs (balloon 6). The thin pin (balloon 9, pad 3) is open.
- My inference: if pin 9 is the shell contact and the legs are plastic or not bonded to the shell, all three shells float and RCA L, RCA R and coax have no return.
- A continuity meter on a sample settles it.

**C2. J7 polarity.**
- D2 and D0 are reversed against HDMI +/- naming; BCK and MCLK are straight (table in section 2).
- No document here defines I2S over HDMI.
- From memory only: PS Audio-style pinouts are 1/3 DATA+/-, 4/6 BCK+/-, 7/9 LRCK+/-, 10/12 MCLK+/-. Against such a receiver, data would be polarity-inverted and L/R swapped.
- Pad 19 (HPD) is tied to GND and pad 18 (+5 V) is open. Compare both with the target receiver.

**C3. U6 unterminated when J7 is empty.**
- DS90LV047A p.11 and p.16 say unterminated use is not allowed. EN and EN* are strapped, so the driver cannot be disabled.
- My inference: no damage. No-load ICC is specified (4 mA typ, 8 mA max, p.5) and shorts are continuous (p.3).

**C4. U3.19 GPIO6 floats.**
- PCM512x p.6 lists an internal pull-down for GPIO5, 4, 3 only. GPIO6 is an input by default (Page 0 Reg 8).
- Pad 19 is open.

**C5. XSMT is not controllable.**
- R31 holds it high. Planned shutdown (p.61) then has only the stop-clocks option.
- There is no pre-regulated rail sense, so a pop on unplanned power loss is possible (p.60-62).

**C6. U9 EN from +3V3.**
- VEN must stay at or below VIN (TPS7B81 p.4). This is fine in operation.
- It could be breached briefly if +12V falls below +3V3 at power-off. The main converter is out of my scope.

**C7. U9 land pattern.**
- Stock pads are 0.375 x 0.40 at span 1.775; TI's DRV0006A example is 0.45 x 0.30 at (1.95) (p.28).
- Pitch, thermal pad 1 x 1.6 and numbering match. The lead lands fully, with less toe.
- My inference: solderable.

**C8. U5 holes.**
- NPTH Ø1.2 against 1.10 ±0.05; plated Ø0.9 against 0.80 ±0.05 (p.4).
- Peg-to-pin distance 2.63 against 2.62. Slightly loose.

**C9. Temperature and automotive use, for a car device.**
- U3 is rated -25..85 C (addendum).
- U6 is rated -40..85 C.
- U5 p.6 note 6 says it is not intended for automotive use.

## 2. Derived configuration

**U3 PCM5122PW**
- **Control:** I2C. Pad 17 MODE1 is low; pad 18 MODE2 is high through R30 (p.6, p.51).
- **Address:** 1001 1 ADR2 ADR1 with both at GND gives 0x4C 7-bit, 0x98 write (p.55). 4.7 k pull-ups go to +3V3; bus is U2.33 (IO21) SDA, U2.31 (IO19) SCL.
- **Clocks:**
  - SCK pad 20 from U1.4 RMCK via 68 ohm. RMCK is active from reset, driven low in reset, default 384 Fs: 16.9344 / 18.432 MHz at 44.1 / 48 kHz, both in Table 32 p.46 (OS8104A p.38-39, p.96, p.183).
  - BCK pad 21 from U1.14 SCK via 47 ohm. LRCK pad 23 from U1.13 FSY via 47 ohm.
  - DIN pad 22 from U2.24 (IO2).
- **Default drive states:**
  - U1 FSY and SCK are inputs until bSDC1.IO or SPD is set (p.49-50).
  - Until then BCK is held low by R16 10 k (U1 p.47, p.179), and LRCK by ESP32 IO4's internal pull-down.
  - U3 BCK and LRCK are inputs at reset (Page 0 Reg 9).
  - No two outputs share a net by default.
- **Supplies:**
  - AVDD and CPVDD on +3V3A. DVDD on +3V3 (3.3 V I/O, internal LDO active).
  - C38 100 nF on LDOO (p.60). C36 2.2 uF flying, C37 2.2 uF on VNEG (-3.3 V).
- **Reference:** VCOM at GND means VREF mode: 2.1 Vrms full scale, AVDD min 3.2 V (p.7, p.42).
- **Mute and standby:**
  - No standby pin. XSMT is high, so hardware is unmuted.
  - POR above 2.8 V, 4 ms, ramps only after valid data (p.64).
  - Auto standby on clock error; auto power-down with BCK and LRCK low for over 1 s (p.65).
- **Sequence:** +3V3 first, then U9. The document sets no order.
- **Filter:** fc = 1/(2π x 470 x 2.2n) = 153.9 kHz (document approx. 153 kHz, p.42). Source 470 ohm; 2 Vrms into 10 k. Pad 6 goes to J4 and J8.1; pad 7 goes to J5 and J8.3.

**U9 TPS7B8133**
- Fixed 3.3 V ±1.5% (3.25-3.35 V). IN is +12V.
- EN on +3V3 (VIH 2 V), so AVDD exists only while +3V3 is up.
- COUT nominal 22.5 uF (C62, C30, C32, C31, C33, C74), within 1-200 uF (effective value, p.4).
- CIN C61 1 uF. TI advises 10-22 uF but none is required for stability (p.18). The +12V net carries more.
- At 32 mA max load (PCM512x p.9-10), about 0.28 W and about 20 C rise.

**U4, R34, R35**
- U4 is always enabled: OE on GND (KiCad symbol, not the datasheet).

| Driver Ro | Open-circuit swing | Into 75 ohm | Source impedance |
|---|---|---|---|
| 0 ohm | 3.3 x 107/356 = 0.992 Vpp | 0.497 Vpp | 74.8 ohm |
| 20 ohm (assumed, from memory) | 0.939 Vpp | 0.465 Vpp | 76.6 ohm |

- C42 corner is 10.6 kHz.
- IEC 60958 limits are not in these documents. From memory, 0.5 Vpp ±20% into 75 ohm ±20%: within limits.
- SX0 is low in reset and until bSDC1.MT is set; S/PDIF requires SPD (OS8104A p.50, p.183).

**U6 and J7**
- EN high, EN* low: always enabled. All four channels are used. No termination on the board.
- HDMI names below are from my knowledge of the standard.

| Input | Signal | DOUT+ pad to J7 | DOUT- pad to J7 | Polarity vs HDMI naming |
|---|---|---|---|---|
| DIN1 pad 2 | LRCK | 15 to 9 (D0-) | 16 to 7 (D0+) | reversed |
| DIN2 pad 3 | DOUT | 14 to 3 (D2-) | 13 to 1 (D2+) | reversed |
| DIN3 pad 6 | BCK | 11 to 4 (D1+) | 12 to 6 (D1-) | straight |
| DIN4 pad 7 | MCLK | 10 to 10 (CK+) | 9 to 12 (CK-) | straight |

- SN65LVDS047: identical pinout (SLLS416 p.1), so the wiring is right for both.

## 3. Pin tables

**U3.** PCM5122PW, TSSOP-28. The document's figures are captioned "RHB", but the addendum lists PW only.
Footprint: pin 1 top-left, counter-clockwise, 0.65 pitch, span 5.725, no exposed pad. It agrees with the PW outline, PDF p.120 (body 4.30-4.50, span 6.20-6.60, leads 0.19-0.30, pin 28 opposite pin 1). No land pattern in that document; PW0016A gives (5.8), 1.5 x 0.45.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | CPVDD | +3V3A | ok |
| 2 | CAPP | DAC_CAPP | ok |
| 3 | CPGND | GND | ok |
| 4 | CAPM | DAC_CAPM | ok |
| 5 | VNEG | DAC_VNEG | ok |
| 6 | OUTL | DAC_OUTL | ok |
| 7 | OUTR | DAC_OUTR | ok |
| 8 | AVDD | +3V3A | ok |
| 9 | AGND | GND | ok |
| 10 | VCOM, to GND in VREF mode | GND | ok |
| 11 | SDA | DAC_SDA | ok |
| 12 | SCL | DAC_SCL | ok |
| 13 | GPIO5, pull-down | open | ok |
| 14 | GPIO4, pull-down | open | ok |
| 15 | GPIO3, pull-down | open | ok |
| 16 | ADR2 | GND | ok |
| 17 | MODE1 | GND | ok |
| 18 | MODE2 | DAC_MODE2 | ok |
| 19 | GPIO6 | open | CHECK C4 |
| 20 | SCK | MCLK | ok |
| 21 | BCK | BCK | ok |
| 22 | DIN | DOUT | ok |
| 23 | LRCK | LRCK | ok |
| 24 | ADR1 | GND | ok |
| 25 | XSMT | DAC_XSMT | ok, see C5 |
| 26 | LDOO | DAC_LDOO | ok |
| 27 | DGND | GND | ok |
| 28 | DVDD | +3V3 | ok |

**U3 passives**, all ok:
- R27 4.7k +3V3 to DAC_SDA; R28 4.7k +3V3 to DAC_SCL.
- R30 10k +3V3 to DAC_MODE2; R31 10k +3V3 to DAC_XSMT.
- R32 470 OUTL to LINE_L; R33 470 OUTR to LINE_R.
- C30 10u, C31 100n, C32 10u, C33 100n on +3V3A; C34 10u, C35 100n on +3V3.
- C36 2.2u CAPP-CAPM (about 3.3 V across); C37 2.2u VNEG-GND (top side).
- C38 100n LDOO-GND; C39, C40 2.2n C0G on LINE_L, LINE_R (±3 V peak).
- No unrated capacitor sits above 5 V. No polarised parts in scope.

**U9.** TPS7B8133DRV (orderable TPS7B8133QDRVRQ1 or the wettable-flank TPS7B8133QWDRVRQ1), WSON-6. Numbering matches p.3. Land pattern: see C7.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | IN | +12V | ok |
| 2 | EN | +3V3 | ok, see C6 |
| 3 | GND | GND | ok |
| 4 | GND | GND | ok |
| 5 | DNC | open | ok |
| 6 | OUT | +3V3A | ok |
| 7 | Thermal pad | GND | ok |
| two unnumbered pads | no number in parts.txt, no net; I took them as paste apertures, not confirmed | none | ok |

C61 1u 50V on +12V: ok. C62 2.2u on +3V3A: ok.

**U6.** DS90LV047ATMTCX, TSSOP-16 PW0016A. Value and footprint agree (p.19). Pin 1 top-left, counter-clockwise. Span 5.725 against (5.8); pad 1.475 x 0.40 against 1.5 x 0.45 (p.26). Pinout from Fig. 25, p.17.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | EN | +3V3 | ok |
| 2 | DIN1 | LRCK | ok |
| 3 | DIN2 | DOUT | ok |
| 4 | VCC | +3V3 | ok |
| 5 | GND | GND | ok |
| 6 | DIN3 | BCK | ok |
| 7 | DIN4 | MCLK | ok |
| 8 | EN* | GND | ok |
| 9 | DOUT4- | I2S_MCLK_N | ok |
| 10 | DOUT4+ | I2S_MCLK_P | ok |
| 11 | DOUT3+ | I2S_BCK_P | ok |
| 12 | DOUT3- | I2S_BCK_N | ok |
| 13 | DOUT2- | I2S_DATA_N | ok |
| 14 | DOUT2+ | I2S_DATA_P | ok |
| 15 | DOUT1+ | I2S_LRCK_P | ok |
| 16 | DOUT1- | I2S_LRCK_N | ok |

C44 100n on +3V3: PROBLEM P2.

**J7.** Function per HDMI type A from my knowledge. Stock footprint, pad positions identical to the library.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | D2+ | I2S_DATA_N | CHECK C2 |
| 2 | D2 shield | GND | ok |
| 3 | D2- | I2S_DATA_P | CHECK C2 |
| 4 | D1+ | I2S_BCK_P | ok |
| 5 | D1 shield | GND | ok |
| 6 | D1- | I2S_BCK_N | ok |
| 7 | D0+ | I2S_LRCK_N | CHECK C2 |
| 8 | D0 shield | GND | ok |
| 9 | D0- | I2S_LRCK_P | CHECK C2 |
| 10 | CK+ | I2S_MCLK_P | ok |
| 11 | CK shield | GND | ok |
| 12 | CK- | I2S_MCLK_N | ok |
| 13 | CEC | open | ok |
| 14 | Utility | open | ok |
| 15 | SCL | open | ok |
| 16 | SDA | open | ok |
| 17 | DDC/CEC GND | GND | ok |
| 18 | +5 V | open | CHECK C2 |
| 19 | HPD | GND | CHECK C2 |
| SH x4 | Shell | GND | ok |

**U4.** 74LVC1G125, SOT-23-5 (KiCad symbol, not the datasheet). Pads 1-3 left top to bottom, 4 bottom right, 5 top right.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | OE, active low | GND | ok |
| 2 | A | SPDIF | ok |
| 3 | GND | GND | ok |
| 4 | Y | COAX_DRV | ok |
| 5 | VCC | +3V3 | ok |

R34 249, R35 107, C41 100n (+3V3), C42 100n (COAX_MID to COAX_OUT, under 1 V): ok.

**U5.** See P1. Pegs (document's 4 and 5, NC) at ±2.54, 2.63: no net, ok. See C8. C43 100n on +3V3: ok by value.

**Series resistors.** OS8104A p.193 gives 33-150 ohm. U1 pin functions are from Table 19-1.

| Part | Value | From | To | Verdict |
|---|---|---|---|---|
| R13 | 68 | U1.4 RMCK | MCLK | ok |
| R14 | 47 | U1.13 FSY | LRCK | ok |
| R15 | 47 | U1.14 SCK | BCK | ok |
| R18 | 68 | U1.9 SX0 | SPDIF | ok |

**J4, J5, J6.** All three pads: function by balloon only, see C1.

| Pad | Item | Net | Verdict |
|---|---|---|---|
| 1 | Thin lead on axis (8) | LINE_L / LINE_R / COAX_OUT | CHECK |
| 2 x2 | Thick legs (6) | GND | CHECK |
| 3 | Thin lead beside axis (9) | open | CHECK |

**J8.** No document: pad 1 LINE_L, 2 GND, 3 LINE_R, 4 GND.

## 4. Not verified, and why

- **Not on this PC:** 74LVC1G125 datasheet (symbol only; VIH and Ro from memory) and the Kycon HDMI drawing (stock footprint only).
- **No document on this PC:** I2S-over-HDMI convention, and IEC 60958 levels.
- **BKL drawing:** no legend, so pads 2 and 3 are unresolved.
- **U5 clamp behaviour:** undocumented. U5 characteristics are specified at Vcc = 5 V, only the supply range covers 3.3 V, and no real part was measured.
- **Drawings:** page rendering was unavailable (no pdftoppm). The PLT133 pages came through the Read tool as images. Other drawings were read from the text layer or plotted from PDF vector paths in memory.
- **Outside your document list (not on your forbidden list):**
  - PLT133 Rev.2, PLT237-T10WH and Cliff FCR684205T (cross-checks).
  - The BKL TDS and the ESP32 datasheets (pin numbers and reset pulls).
  - Pads and description of `hw\jlr_most_amp.pretty\Everlight_TOSLINK_T10.kicad_mod` and `BKL_0101153_RCA_RA.kicad_mod`.
  - U5, J4-J7 placement in `hw\jlr_most_amp.kicad_pcb` and `hw4\jlr_most_amp.kicad_pcb`.
