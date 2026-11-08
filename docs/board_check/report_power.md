# Power and sleep check: independent report

**Result in one line:** I found no wrong pin function, mirrored or renumbered footprint, reversed diode or transistor, forbidden float, or exceeded voltage rating in scope. What remains is two deviations from the documents and a list of doubts I could not settle.

**Sources beyond your list.** The TE documents do not define the receiver outputs, so I also read these. None is a designer file.
- `C:\Users\geniy\Desktop\MOST-specifications\specifications\MOST_version_2V5\MOST_2V5_Application_Notes.zip`: "AAR THM 3V2.pdf" (below "AAR", the MOST automotive recommendation for through-hole FOTs; TE 108-94198 §1.1.2 cites an earlier version) and "App Note - oPhy_1V0.pdf" (below "AN").
- `MOST_2V5_Physical_Layer.zip` in the same folder: Physical Layer Rev 1.1 and Addendum E.
- `Bitline-info\most-spec\txt\MOST_Specification_2V5.txt`.
- `Bitline-info\datasheets\on-board\TE_2-2141577-1_drawing_dxf.zip`.
- Third-party product: `Bitline-info\products\usb2most\hardware-v3`.
- For facts only: `hw\jlr_most_amp.pretty\TI_RZT0020A.kicad_mod`, the J2 footprint, and `Bitline-2L-BOM.csv`.

Nothing was written or changed.

Note on `parts.txt`: U8.21 shows 0.20x0.20, but that is only the anchor of a custom polygon. The real pad is 0.95 x 2.1 mm.

---

## 1. PROBLEMS

**PROBLEM 1. R4 = 15 kΩ between J2.4 (CONT/RGAIN) and J2.3 (TX VCC).**
- Document says: TE 108-94198 §2.3.4 p.5 states the output power is valid only with Rext 13.5 kΩ. AAR p.24 puts the resistor between supply and gain pin (the board does this), with 13.5 kΩ giving at least -10 dBm at SP2 and 27 kΩ about 3 dB less. AN p.7 says full power is required in normal use. MOST Physical Layer Table 2-2 p.19 sets the SP2 minimum at -10 dBm.
- Inference: power goes as 1/R, so 15 kΩ loses about 0.46 dB. TE's -9.0 dBm (Micro) or -9.5 dBm (Flex) becomes about -9.5 or -10.0 dBm. AAR's worst case becomes about -10.5 dBm, below the SP2 minimum.
- On the board: about 0.5 dB less of a 13 dB link budget (AN p.11). usb2most `most.sch` has 33 kΩ in the same place, so this is unlikely to be fatal.
- Also: AN §3 p.7 wants a -3 dB optical power switch; this board has none. AAR p.24 calls that diagnosis OEM-specific.

**PROBLEM 2. Paste on exposed pads is below TI's figures.**
- U10.9: 64.5 % (4 x 0.63 x 0.76 on 1.57 x 1.89). TPS1H200A §7.4.1 p.23 says coverage must be at least 85 %; the stencil example is 100 % (p.29).
- U8.21: 58 % (3 x 0.85 x 0.43 on a 1.88 mm² polygon). TI's example is 85 % (LM656x5 PDF p.62).
- U7.7 and U9.7: 65 %. TI's example is 88 % (TPS7B81 PDF p.29).
- Inference: higher thermal resistance and less solder on the main heat path. U8 matters most. U10 and the LDOs dissipate little.

---

## CHECK (could not settle, ranked by consequence)

**CHECK 1. D2 SM6T24A against jump start and load dump.**
- KiCad symbol, not the datasheet: description says 24 Vrwm, unidirectional 600 W.
- From memory only, unverified: ST's table gives Vrwm 20.5 V, Vbr 22.8-25.2 V, Vcl 33 V at 18 A.
- Everything behind it tolerates more:
  - U7 and U9: 40 V operating, 45 V for 200 ms.
  - U10: 40 V operating, 42 V for less than 400 ms.
  - U8: 65 V (70 V for less than 100 ms).
  - Capacitors: 50 V.
- Inference: a sustained 24-28 V jump start or a long load dump puts D2 in conduction with only F1 limiting it. The likely result is a blown F1 and possibly a shorted D2. The board goes dead through its own clamp, not from overvoltage.

**CHECK 2. J9.3 and J9.4 are on +12V_IN, upstream of F1, D1, D2 and L1.**
- Inference: whatever is on J9 gets raw battery, including reverse polarity, jump start and load dump. A short there is not fused on this board.
- J1.1 is one pin carrying both this board and J9. The J9 load is unknown to me. No document covers this.

**CHECK 3. J2's four legs at (0, 4.45), (-12.85, 6.2), (-4.47, 9.15), (7.01, 9.15) go into unplated 1.1 mm holes with no net.**
- Document says: AN p.8 says the optical connector's shielding box must be soldered to the ground plane.
- usb2most (same part number) plates these holes as pad 0, which its schematic ties to GND.
- DXF (for 2-2141577-1, marked not controlled): blades are about 1.0 x 0.4 mm, diagonal 1.08 mm, against a 1.1 mm hole. That is 0.02 mm nominal clearance.
- Inference: the action pins (pad 0, on GND) lie in the same side-wall planes in the DXF, so the shield probably is grounded, but only at the front corners. A hole that comes out undersize would stop the part seating.

**CHECK 4. Capacitor ratings are not fixed in the BOM; the LCSC column is empty.**
- C53 needs at least 16 V (LM656x5 §8.2.2.7 p.32) and shares the generic "100n" line.
- C54: TI recommends 10 V X7R (§7.3.6 p.18).
- C55 and C56 must each give at least 4.7 µF effective at 12-16 V bias (§8.2.2.6 p.31, §8.1 p.26). Dielectric is not stated.
- C58, C59, C20: voltage and dielectric not stated, though the nominal bank has margin.

**CHECK 5. Inductor land patterns are unverified.**
- L2 is CENKER CKCSA4020 on a Coilcraft XxL4030 footprint. The maker PDF pages are empty (web pages 121, 123, 124).
- L1 is NRSE6028-100M on a Bourns SRN6028 footprint, with no data on this PC.
- A wrong terminal span would mean a poor or open joint on a power path.

**CHECK 6. FOT_STATUS reaches only Q1 and R3, not U2.**
- OS8104A §20.2 p.195 and AN Fig 3-1 p.7 show status going to the controller as well.
- Inference: this is not a wiring error, but firmware holding HOLD high can learn of light loss only through U1.

**CHECK 7. U10.5 DELAY pulled to switched +3V3 through R59.**
- Table 4-1 p.3 says DELAY has an internal pull-up. The standby test conditions (p.5) do not state DELAY.
- Inference: if the pull-up is live in standby, a small current flows into the dead +3V3 rail. The only figure in the document is 4.5 µA, and that is the latch-off charging current.

**CHECK 8. U9.2 EN on +3V3 against abs max VEN ≤ VIN (TPS7B81 p.4).**
- Inference: if +12V falls faster than +3V3 (battery removed while REM is loaded), EN can sit about one diode drop above IN, because U8's VIN-SW diode clamps +3V3 to VIN plus a drop.
- That same case is LM656x5 §8.3-8.4 p.49. D1 prevents the hard-short version of it from J1.

**CHECK 9. F1 "1.5A T" states no voltage rating.** D2's clamp can exceed 32 V if the fuse is a 32 V type.

---

## 2. Derived configuration

**U8 LM65625-Q1** (orderable LM65625SRZTRQ1, RZT package)
- Output: fixed 3.3 V, 3.267-3.333 V (FB to GND, p.8).
- Frequency: 2.2 MHz, 1.98-2.42 MHz (RT to GND, p.9).
- Mode: FPWM with ±5 % DRSS (MODE to VCC, Table 7-2 p.15).
- Current limits, 2.5 A trim: high-side 3.75/4.1/4.9 A, low-side valley 2.4/3.0/3.4 A, negative -6/-4.3/-2.8 A. Hiccup after 64 cycles below 0.4 x setpoint, off 45 ms.
- Input thresholds: VIN UVLO rising 3.25-3.65 V, falling 2.5/2.6 V. EN rising 1.15-1.35 V, falling 0.9-1.1 V.
- Regulation:
  - On-time foldback starts at 34 V worst case, 50 V typical.
  - Off-time foldback starts below about 4.5 V.
  - Below about 3.4 V the output tracks VIN.

**L2 and output capacitors against the document**
- Eq 7 gives 1.45 µH at 12 V, 30 % ripple; Table 8-1 lists 1.5 µH. The board has 2.2 µH, the value TI's own BOM uses (Table 8-6 p.47, LM6x625 3.3 V 2200 kHz).
- Ripple 0.43 A at 9 V, 0.49 A at 12 V, 0.54 A at 16 V (0.41-0.62 A at 12 V with ±20 % L). That is 17-22 % of 2.5 A: at the low edge of the 20-40 % range, above the 10 % floor (p.30-31).
- Lmin (Eq 9) is 0.705 µH: ok.
- Isat 6.1 A is above 4.9 A, Irated 4.3 A, DCR 40-56 mΩ. These come from the LCSC listing (page 120) and a search summary (`searches.md`), not the maker.
- COUT: 88.4 µF nominal directly on +3V3, plus 20 µF behind FB2 and FB3. Needed: 20 µF effective, up to about 200 µF (Table 8-1, p.31). Ok.
- CIN: 2 x 10 µF plus 2 x 100 nF plus 94 µF aluminium (20-100 µF, p.49). Ok on value.

**U7 and U9 TPS7B8133, DRV package**
- 3.3 V ±1.5 %, 150 mA.
- U7 always on (EN = IN). U9 enabled by +3V3 (VIH 2 V, VIL 0.7 V).
- UVLO: off at or below 2.7 V, hysteresis 200 mV.
- Output capacitance 1-200 µF required: C52 plus C2 is ok. Input capacitor is advisory; bulk is on the net.

**U10 TPS1H200A-Q1, DGN package**
- Current limit = 0.8 x 2500 / 4020 = 0.4975 A, in the ±20 % band (0.40-0.60 A), p.6.
- Auto-retry: on 35-45 ms, off 0.8-1.2 s (DELAY pulled up, 3.3 V above Vdl(th) 2.7 V, p.6-7, §6.3.2.3).
- IN: VIH 1.8 V, VIL 0.8 V, pull-down 150-400 kΩ. R57 = TI's 4.7 kΩ (p.20).
- DIAG_EN low: no diagnostics, full protection when on (Table 6-3). VS UV off 3-3.4 V, on 3.5-4 V.

**Optical**
- TX and RX at 3.3 V (TE §2.3.1: typical 5 V and 3.3 V).
- RX on continuous supply, TX on switched supply. AN p.6 asks for exactly this.
- R1 and R5 = 100 Ω, within OS8104A's 33-150 Ω (p.195). R2 = 100 kΩ, above the 50 kΩ SP4 load (Physical Layer Table 2-4 p.22).

**Receiver outputs**
- With or without light, per TE: neither TE document says. TE gives only pin names and optical thresholds: data recovery down to -24.5 dBm (Micro) or -24 dBm (Flex), "light off" -24.5 to -40 dBm (108-94198 §2.3.1).
- Per other documents, all agreeing:
  - Light: status low (AAR p.24, AN p.6, OS8104A p.195, Addendum E p.7).
  - Dark: status high.
  - Dark, data output: low for MOST25 (AAR p.24 only).
  - Avago's own datasheet is not on this PC.

**Turn-on and turn-off**
- Light gives STATUS low, Q1 on, WAKE at 3.3 V. tEN_HIGH 3 ms plus tSS 2.9-8.1 ms puts +3V3 at 90 % in about 6-11 ms, within OS8104A's 50 ms (p.195).
- Light lost gives Q1 off, and R50 takes EN low unless HOLD or WAKE_FORCE holds it through D3.

**Sleep: what stays powered**
- U7, the FOT receiver (via FB1), R3, Q1 source, SW2.
- U8 in shutdown, U9 in shutdown, U10 in standby.
- The input network, and J9.3/J9.4.

**Sleep: every crossing into the off side**

| Connection | Dark state | Current |
|---|---|---|
| J2.8 → R1 → U1.42, R2 to GND | data low (AAR p.24) | 0. If a part sat high: 33 µA in R2. OS8104A App C.1 p.208 says RX is "5 V tolerant even when not powered", so nothing enters U1 |
| J2.7 → Q1 gate, R3 | high | 0 |
| Q1 drain → WAKE, R50 | off | Q1 leakage x 100 kΩ; margin 11.5 µA before 1.15 V (my arithmetic; BSS84 leakage unverified) |
| WAKE → U8.7 | low | at most 1 µA, spec'd at VEN = VIN |
| D3 → HOLD → U2.9, R51 | 0 V across | 0 |
| D3 → WAKE_FORCE, R52, SW2 open | 0 V across | 0 |
| U8.20 BIAS, U8.13/14 via L2 → +3V3 | shutdown | inside IQ_SD |
| U9.2 EN, U9.6 OUT | off | 10 nA EN (typ, p.5) |
| U2.8 → R57 → U10.1 | IN pulled low inside U10 | 0 |
| +3V3 → R59 → U10.5 | see CHECK 7 | unknown, small |
| U10.7 → J1.3, J9.2 | off | at most 3 µA leakage |
| U1.3 → R5 → J2.1; FB2, R4 | both sides off | 0 |

**Sleep: battery budget at 25 °C**

| Item | Maximum | Source |
|---|---|---|
| U7 | 4.5 µA | TPS7B81 p.5 |
| FOT receiver | 30 µA | AAR p.8, a frame not an Avago figure |
| U8 | 0.85 µA | LM656x5 p.7 |
| U9 | 1 µA | TPS7B81 p.5 |
| U10 | 0.5 µA (3 µA at 125 °C) | TPS1H200A p.5 |

That totals about 37 µA, excluding D2 and electrolytic leakage (no data) and J9. MOST 2V5 p.202 gives a typical quiescent value below 100 µA.

---

## 3. Pin tables

**U8** (footprint matches TI 4228518/C p.61 within 0.03 mm, pin 1 top-left, counter-clockwise).
- Two pads numbered 7 is correct: pins 1, 7, 11, 17 are L-shaped corners, 24 lands for 20 pins (§7.1 p.12), and both legs share a net.
- Pad 21: 0.95 x 2.1 mm, 0.075 off centre toward pins 1-7, same as TI. The footprint omits TI's bottom-right notch; clearance is still at least 0.35 mm, harmless.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | NC, leave floating | none | ok |
| 2 | PG open drain | none | ok (TI asks for a pull-up; floating harmless, my inference) |
| 3 | FB | GND | ok, 3.3 V |
| 4 | VCC | BUCK_VCC (C54) | ok |
| 5 | MODE/SYNC | BUCK_VCC | ok, FPWM |
| 6 | RT | GND | ok, 2.2 MHz |
| 7 | EN/UVLO | WAKE | ok |
| 8, 10, 12, 16, 18 | NC | none | ok |
| 9 | PGND1 | GND | ok |
| 11 | VIN1 | +12V | ok |
| 13, 14 | SW1, SW2 | BUCK_SW | ok |
| 15 | BOOT | BUCK_BST | ok |
| 17 | VIN2 | +12V | ok |
| 19 | PGND2 | GND | ok |
| 20 | BIAS | +3V3 | ok (fixed mode) |
| 21 | DAP | GND | ok on net; PROBLEM 2 on paste |

**U7** (DRV, stock WSON footprint: numbering right; EP 1 x 1.6 same as TI; lands slightly differ from TI's, within lead). U9 has the same pinout: U9.1 +12V, U9.2 +3V3 (CHECK 8), U9.3/4/7 GND, U9.5 floating, U9.6 +3V3A.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | IN | +12V | ok |
| 2 | EN | +12V | ok |
| 3, 4 | GND | GND | ok |
| 5 | DNC | none | ok |
| 6 | OUT | +3V3_AON | ok |
| 7 | thermal pad | GND | ok on net; PROBLEM 2 on paste |

**U10** (stock HVSSOP footprint: numbering right; EP 1.57 x 1.89 same as TI p.28).

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | IN | REM_IN | ok |
| 2 | DIAG_EN | GND | ok |
| 3 | FAULT, may float | none | ok |
| 4 | CL | REM_CL | ok |
| 5 | DELAY | REM_DELAY | ok; CHECK 7 |
| 6 | GND | GND | ok |
| 7 | OUT | REM_OUT | ok |
| 8 | VS | +12V | ok |
| 9 | thermal pad | GND | ok on net; PROBLEM 2 on paste |

**J2** (TE 114-18997 §4.1 p.6, same mapping in AN Fig 3-1 p.7).
- Numbering checked: TE's view onto the optical face with pins down has pin 1 on the left. The DXF front and side views give the pins at x = ±1.93, ±4.47, ±7.01, ±9.55, y = 7.07, with the optical face toward -y.
- That puts pin 1 at +x, not mirrored. usb2most has identical coordinates and numbering. Pegs and action pins also match the DXF.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | DATA In | FOT_TXD | ok |
| 2 | TX GND | GND | ok |
| 3 | TX VCC | FOT_TXVCC | ok |
| 4 | CONT (gain) | FOT_RGAIN | ok on topology; PROBLEM 1 on value |
| 5 | RX VCC | FOT_RXVCC | ok |
| 6 | RX GND | GND | ok |
| 7 | MODEout (status) | FOT_STATUS | ok |
| 8 | Vout (data) | FOT_RXD | ok |
| 0 (x2) | action pins | GND | ok |
| unnumbered x6 | 2 pegs (hole 2.1 for 2.0 mm) | none | ok |
| | 4 legs | none | CHECK 3 |

**J1, J9** (no Molex drawing on PC; the page in the folder is empty; J9 has no document).

| Pad | Net | Verdict |
|---|---|---|
| J1.1 | +12V_IN | CHECK 2 |
| J1.2, J1.4 | GND | numbering unverified |
| J1.3 | REM_OUT | numbering unverified |
| J1 MP x2 | none | unverified |
| J9.1, J9.5 | GND | no doc |
| J9.2 | REM_OUT | no doc |
| J9.3, J9.4 | +12V_IN | CHECK 2 |

**Diodes and transistor** (KiCad symbol, not the datasheet, for all four).

| Part.pad | Terminal | Net | Verdict |
|---|---|---|---|
| D1.1 | K | +12V_D | ok |
| D1.2 | A | +12V_F | ok; 40 V, 3 A per description, reverse battery blocked |
| D2.1 | K | +12V_D | ok polarity |
| D2.2 | A | GND | ok polarity; CHECK 1 |
| D3.1 | A1 | HOLD | ok |
| D3.2 | A2 | WAKE_FORCE | ok |
| D3.3 | common K | WAKE | ok |
| Q1.1 | G | FOT_STATUS | ok |
| Q1.2 | S | +3V3_AON | ok |
| Q1.3 | D | WAKE | ok (P-FET high side, polarity right) |

**Passives** (pad 1 / pad 2)

| Part | Value | Nets | Verdict |
|---|---|---|---|
| F1 | 1.5A T | +12V_IN / +12V_F | CHECK 9 |
| L1 | 10u | +12V_D / +12V | CHECK 5 |
| L2 | 2.2u | BUCK_SW / +3V3 | CHECK 5 |
| C50, C65 | 47u 50V | +12V / GND | ok; pad 1 is + |
| C51 | 1u 50V | +12V / GND | ok |
| C52 | 2.2u | +3V3_AON / GND | ok |
| C53 | 100n | BUCK_BST / BUCK_SW | CHECK 4 |
| C54 | 1u | BUCK_VCC / GND | CHECK 4 |
| C55, C56 | 10u 50V | +12V / GND | CHECK 4 |
| C57, C60, C63, C75, C76 | 100n 50V | +12V / GND | ok |
| C58, C59 | 22u | +3V3 / GND | CHECK 4 |
| C64 | 100n 50V | +12V_F / GND | ok |
| C77 | 100n 50V | REM_OUT / GND | ok |
| C1, C2 | 100n, 10u | FOT_RXVCC / GND | ok |
| C3, C4 | 100n, 10u | FOT_TXVCC / GND | ok |
| FB1 | 600R@100MHz | +3V3_AON / FOT_RXVCC | ok (RX at most 30 mA, AAR p.8; DC rating unknown) |
| FB2 | 600R@100MHz | +3V3 / FOT_TXVCC | ok (TX at most 50 mA, AAR p.8; DC rating unknown) |
| R1 | 100 | FOT_RXD / MOST_RX | ok |
| R2 | 100k | MOST_RX / GND | ok |
| R3 | 47k | +3V3_AON / FOT_STATUS | ok |
| R4 | 15k | FOT_TXVCC / FOT_RGAIN | PROBLEM 1 |
| R5 | 100 | MOST_TX / FOT_TXD | ok |
| R50 | 100k | WAKE / GND | ok (EN must not float) |
| R51 | 100k | HOLD / GND | ok |
| R52 | 10k | WAKE_FORCE / GND | ok |
| R57 | 4.7k | REM_EN / REM_IN | ok |
| R58 | 4.02k | REM_CL / GND | ok |
| R59 | 10k | +3V3 / REM_DELAY | ok; CHECK 7 |

---

## 4. Not verified, and why

- **Avago AFBR-1012/1013/2012/2013 and AN 5497 are not on this PC.** So these are unverified:
  - status output type and data output in the dark for this exact part;
  - real receiver sleep current (30 µA is the AAR frame);
  - permitted Rext range;
  - supply tolerance;
  - whether TX and RX share anything internally.
- **Part identity:** neither TE document names 1-1394640-1, and the DXF is for 2-2141577-1 and marked not controlled. Only usb2most links that number to this geometry. Reflow against non-reflow FOT, and TE's 110 °C fibre limit for PC fibre (114-18997 §6.2), depend on a product drawing I do not have.
- **No datasheets for:**
  - SS34, SM6T24A, BAT54C, BSS84: pinouts come from KiCad symbols. Vf, leakage, Vth, TVS clamp and power are unverified; the SM6T figures in CHECK 1 are from memory.
  - F1, L1, and J1's drawing and contact rating.
  - L2: distributor numbers only.
- **Not assessed:** copper, placement, or the JLC rotations of polarised parts. Total FPWM no-load input current is not in the document. The J9 load is out of scope.
