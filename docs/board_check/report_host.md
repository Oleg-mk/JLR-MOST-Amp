# ESP32 and USB bridge check against manufacturers' documents

**Result:** I found no wrong pin and no mirrored or renumbered footprint on U2, U11, U12, U13, Q2, Q3, SW1, SW2 or D20. J10's contact map is right, but its geometry has no drawing. There are 2 findings with a settled outcome and 8 unsettled doubts. Neither finding stops the board in the car. No files were written.

Documents:
- Module = ESP32-WROVER-E_IE v2.4
- Series = ESP32 Series v5.3
- OS = OS8104A DS8104AFP4

## 1. PROBLEMS

**P1. The power latch drops on any ESP32 reset and in download mode (U2.9 GPIO33/HOLD, R51, D3, R50, U8.7, Q2).**
- Document says: all pins are output-disabled during reset (Series p.62 note 9). GPIO33 is oe=0, ie=0 at and after reset (Series p.69).
- My inference:
  - HOLD falls through R51. WAKE then falls through R50 unless Q1 conducts (light present) or SW2 is pressed. U8 stops and +3V3 collapses.
  - On a bench with no MOST light, any of these switches the board off: esptool's reset pulse, a terminal open or close that passes through DTR=H/RTS=L, or a panic, WDT or restart. Firmware pad-hold was not assessed.
  - USB flashing needs SW2 held for the whole session, because download mode never sets HOLD.
  - A cold start by SW2 needs it held until firmware sets HOLD. That is U8 delay 3 ms + soft start 2.9–8.1 ms (LM656x5 p.8) + EN 15–18 ms + ROM, bootloader and app start.
- In the car with light present, Q1 holds WAKE and there is no effect.

**P2. R16 straps MTDO low, so the ROM log is off at every cold boot (U2.23, R15, R16).**
- Document says: MTDO defaults to pull-up, and MTDO=0 disables U0TXD printing (Module p.13, p.16). OS asks for this SCK pull-down (p.47, p.179 note). VIL max is 0.25·VDD = 0.825 V (Series p.52).
- Calculation: 45k up against 10.05k down gives 0.60 V. With U6.6's maximum 10 µA (DS90LV047A p.4) it is 0.68 V. It reads 0.
- My inference: at first bring-up the USB-UART shows no ROM lines (boot mode, flash errors), so a board failing in ROM looks silent. Bootloader and app logs are unaffected. Boot is not stopped.

### CHECK (doubts I could not settle, ranked)

**C1. CH340C is parasitically powered with nothing on USB (U2.35, R63, U11.3, USB_3V3).**
- TXD0 idle high feeds U11's clamp through 4.7k, up to 0.57 mA. USB_3V3 can sit at up to about 2.7 V.
- Q2 conducts if the DTR pin is about 0.6 V above the RTS pin. With no CH340C datasheet here, the DTR/RTS state at low VCC is unknown.
- This is the only path by which "nothing on USB" could reset or hold the module, in the car too.
- On the first board, measure USB_3V3, U11.13, U11.14 and EN with USB absent.

**C2. Fast re-power gives no valid reset (U2.3, R20, C22, about 88 µF on +3V3).**
- Document says: CHIP_PU must stay below 0.6 V for at least 50 µs (Module p.17, p.29). Series p.19 warns that a large rail capacitor plus CHIP_PU tied to the rail can give an inadequate reset on quick re-power.
- My inference: EN follows +3V3 through R20, and I found no output discharge function in the LM656x5 text. A new WAKE before +3V3 falls below 0.6 V gives no valid reset. If the chip hangs, there is no reset path while light keeps WAKE high.
- Outcome is unknown. Test: drop HOLD, then re-wake at varying delays.

**C3. ROM activity on GPIO5, GPIO14 and GPIO15.**
- IO_MUX says oe=0 (Series p.69).
- From memory only, third-party pinout guides claim these pins output a signal at boot.
- If true, GPIO5 and GPIO14 fight U1.12 and U1.36 driving low with no series resistor. OS limit is ±10 mA per pin (p.167).
- Scope this on the first board.

**C4. USB attached with the board off pre-biases EN (Q2, R64, C22).**
- With DTR high, base-collector conducts and charges C22 to about 0.6 V above the rail, or up to 1.3–2.1 V if CHIP_PU has no clamp.
- When 12 V then arrives, EN starts above VIL_nRST and above 0.75·VDD early in the ramp, against tST BL and tRST (Module p.17).
- Bench only. Test: USB plugged first, then 12 V, then SW2.

**C5. RS edge rate (U1.31, C14, R12, GPIO13).**
- OS p.167 gives 8 ns/V maximum for all digital inputs unless otherwise specified, and p.171 gives RS no exception.
- The actual rate is about 5 ms/V at power-up, and about 10 µs/V when GPIO13 pushes C14.
- The effect of a slow edge through the threshold is unknown.

**C6. GPIO13 drives 1 µF with no series resistor.**
- IOL is 28 mA at 0.495 V at maximum drive (Series p.52), about 18 Ω. That means peaks of order 100 mA for tens of µs.
- The documents give only a cumulative limit of 1200 mA (Series p.51).

**C7. SW1 numbering.**
- With standard DIP pairing (pad k to pad 9−k), switch 1 = GPIO35, switch 2 = GPIO34, switch 3 = GPIO39, switch 4 = GPIO36. The net labels run opposite to the switch numbers.
- Check against silkscreen and firmware.

**C8. U1.35 is SR1.**
- S/PDIF decode exists only on SR0/SX0 (OS p.47, p.56), and SR0 (U1.15) is tied to GND. The net label "SPDIF_SRC" suggests otherwise.
- SR1 works as a serial or transparent input, and the direction from GPIO12 is right. This matters only if the net is meant to carry S/PDIF.

## 2. Boot and back-feed

### Timing, nothing on USB

Computed for tSS 2.9–8.1 ms:
- RS crosses 2.0 V at 11–14 ms.
- EN crosses 0.75·VDD at 15–18 ms.
- The rail is stable by about 9 ms or earlier, so tST BL is met by several ms.
- U1 leaves reset about 5 ms before the ESP32.

### Strapping pins

| Strap (pad) | Sources | Case 1: nothing on USB | Case 2: USB and terminal | Case 3: other chip in reset, running or dead | Result |
|---|---|---|---|---|---|
| GPIO0 (25) | R21 10k up, wpu, Q3.C | High | High, unless DTR=L and RTS=H (then low) | U11 dead: Q3 off, high | SPI boot if high |
| GPIO2 (24) | wpd; U3.22 and U6.3 inputs | Low, ≤0.45 V | Same | Same; U3 and U6 digital supplies share U2's rail | Don't-care in SPI boot; 0 allows download |
| MTDI GPIO12 (14) | wpd ∥ R17; U1.35 Hi-Z | Low, ≤0.31 V including ±10 µA leakage | Same | Same in reset, running or warm reset | VDD_SDIO = 3.3 V, always |
| MTDO GPIO15 (23) | wpu vs R16 + R15; U1.14 Hi-Z or input | Low, 0.60–0.68 V | Same | Warm reset with U1 SCK output: random | ROM print off (P2) or random |
| GPIO5 (29) | wpu vs U1.12 output | Low: driven low in reset (OS p.183), forced low while MT=0 (OS p.50) | Same | Warm reset with MT=1: random | SDIO slave timing only; unused |

- **Boot mode:** SPI in all cases, unless Q3 is on.
- **Flash voltage:** 3.3 V in all cases. This is what N16R8 needs (Module p.2, p.39).
- **What U1 can do:** U1 cannot stop boot in any state. RD is tied high, so D6 is never driven.
- **What can stop boot:** only the terminal states below, P1, and doubts C1, C2 and C4.

### Auto-reset (pin levels)

Pins are active-low per KiCad symbol names.

| DTR | RTS | Q2 | Q3 | EN | IO0 | Result |
|---|---|---|---|---|---|---|
| H | H | off | off | H | H | SPI boot |
| L | L | off | off | H | H | SPI boot |
| H | L (RTS asserted only) | on | off | low, about 0.1–0.2 V | H | Held in reset while this state lasts |
| L (DTR asserted only) | H | off | on | H | low | Running firmware unaffected; next reset or power-up enters download mode |
| U11 dead | | off | off | H | H | C-B reverse-biased, leakage only; cannot hold reset. Back-fed state: see C1 |

- **Glitches:** EN needs about 100–300 µs of Q2 conduction to fall below 0.6 V (1–1.1 µF). Shorter glitches do not reset. The current limit is not verified.
- **esptool classic sequence (timing from memory):**
  - (H,L) for 100 ms.
  - (L,H) for 50 ms.
  - EN crosses at about 14–15 ms inside that IO0-low window, so download entry works, subject to P1.

### Back-feed

There is no direct link between USB_3V3 and +3V3; only GND is shared. Clamps on the unpowered side are assumed, as the documents are silent.

| Link | USB off, board alive | USB alive, board off |
|---|---|---|
| R63 (U2.35–U11.3) | ≤0.57 mA into USB_3V3 (≤54 µA while TXD0 has only wpu) | None of note |
| R62 (U2.34–U11.2) | ≤54 µA via GPIO3 wpu | ≤0.57 mA into +3V3 via GPIO3 clamp |
| Q2 (EN–DTR/RTS) | None; C-B reverse-biased | DTR high: ≤0.26 mA base current, plus inverse-β current from RTS, into EN and R20 (C4) |
| Q3 (IO0–RTS/DTR) | None; C-B reverse-biased | RTS high: ≤0.26 mA base current, plus inverse-β current from DTR, into IO0 and R21 |

- Total into the main side is about 1–3 mA (inverse β undocumented here). That cannot start anything, and HOLD stays low.
- VIH max is VDD+0.3 V (Series p.52). This is formally exceeded on GPIO3, GPIO0 and CHIP_PU when USB is alive and the board is off. No injection limit is documented.

## 3. Per-part tables

### U2: ESP32-WROVER-IE-N16R8

**Identification and variant**
- Module p.2: 16 MB flash, 8 MB PSRAM, 3.3 V VDD_SDIO. Value agrees.
- Reserved pins: flash on GPIO6–11, PSRAM on GPIO16/17. None are led out (pads 17–22, 27, 28 are NC) and the board uses none.

**Footprint**
- I read Fig 15's vector data directly. All 38 pads and the 3×3 EPAD match within 0.008 mm.
- Pads are 1.5×0.9 at x=±8.75, pitch 1.27.
- Pad 1 is top-left, 19 bottom-left, 20 bottom-right, 38 top-right.
- Connector end is up (7.44 mm from pad 1 centre to body edge, 1.1 mm below pad 19).
- EPAD squares are 0.9 mm on 1.4 mm pitch, centred at (−1.5, −0.46).
- Not mirrored.

**Pin table** (also item 6). I2C and I2S route to any GPIO via the matrix (Series p.37–38, p.47–48). There is no ESP32 MCLK on this board; MCLK comes from U1.4.

| Pad | Function per doc | Special | Net and other end | Need vs capability | Verdict |
|---|---|---|---|---|---|
| 1, 15, 38 | GND | | GND | | ok |
| 2 | 3V3 | | +3V3; C20 22 µF, C21 100 nF (= Fig 11) | | ok |
| 3 | EN / CHIP_PU | must not float | ESP_EN; R20 10k, C22 1 µF (= p.39), Q2.C | | ok; C2, C4 |
| 4 | GPIO36 | input-only, no pulls, RTC | MODE0; R22 up, SW1.5 | input, external pull | ok |
| 5 | GPIO39 | input-only, no pulls, RTC | MODE1; R23 up, SW1.6 | input, external pull | ok |
| 6 | GPIO34 | input-only, no pulls, RTC | MODE2; R24 up, SW1.7 | input, external pull | ok |
| 7 | GPIO35 | input-only, no pulls, RTC | MODE3; R25 up, SW1.8 | input, external pull | ok |
| 8 | GPIO32 | RTC, 32K_P, no pull | REM_EN; R57 to U10.1 | output; VIH 1.8 V, internal 150–400k pull-down holds off (TPS1H200A p.6) | ok |
| 9 | GPIO33 | RTC, 32K_N, no pull | HOLD; R51 down, D3.1 | output | ok; P1 |
| 10 | GPIO25 | RTC | MOST_INT; U1.41 open-drain, R10 47k up | input | ok |
| 11 | GPIO26 | RTC | I2C_SDA; U1.40, R9 | I2C | ok |
| 12 | GPIO27 | RTC, wpu after reset | I2C_SCL; U1.39, R8 | I2C; high at RS rise selects I2C (OS p.198) | ok |
| 13 | GPIO14 | MTMS, RTC, wpu after reset | MOST_SX1; U1.36 output | input, I2S data in | ok; C3 |
| 14 | GPIO12 | MTDI strap, RTC, wpd | SPDIF_SRC; U1.35 input, R17 down | output, I2S data out | ok; C8 |
| 16 | GPIO13 | MTCK, RTC, wpd after reset | MOST_RST; U1.31, R12 up, C14 | output; wpd leaves 2.55–2.7 V, above VIH 2.0 | ok; C5, C6 |
| 17–22 | NC | GPIO6–11 not led out | unconnected | | ok |
| 23 | GPIO15 | MTDO strap, RTC, wpu | BCK; R15 to U1.14 (+R16), U3.21, U6.6 | bidirectional clock, I2S | ok; P2, C3 |
| 24 | GPIO2 | strap, RTC, wpd | DOUT; U3.22, U6.3 | output, I2S data | ok |
| 25 | GPIO0 | strap, RTC, wpu | ESP_IO0; R21 up, Q3.C | | ok |
| 26 | GPIO4 | RTC, wpd | LRCK; R14 to U1.13, U3.23, U6.2 | bidirectional, I2S WS | ok |
| 27, 28 | NC | | unconnected | | ok |
| 29 | GPIO5 | strap, wpu | MOST_SX3; U1.12 output | input, I2S data in | ok; C3 |
| 30, 37 | GPIO18, GPIO23 | | unconnected | | ok |
| 31 | GPIO19 | | DAC_SCL; U3.12, R28 | I2C | ok |
| 32 | NC | | unconnected | | ok |
| 33 | GPIO21 | | DAC_SDA; U3.11, R27 | I2C | ok |
| 34 | GPIO3 U0RXD | wpu | R62 to U11.2 TXD | input | ok |
| 35 | GPIO1 U0TXD | wpu | R63 to U11.3 RXD | output | ok; C1 |
| 36 | GPIO22 | | LED; R26 to D20.2 | output; off in reset | ok |
| 39 | EPAD | soldering optional (p.39) | GND | | ok |

**Two outputs on one net**
- None at reset.
- BCK and LRCK can be driven by U1, U2 or U3. Direction is firmware's. R14 and R15 alone would not keep U1 under ±10 mA if both sides drive.

### U1 pins facing the ESP32 (OS p.179–184, p.49–50)

| Pad | Function | Reset state | After reset | Verdict |
|---|---|---|---|---|
| 12 | SX3 | driven low | forced low while MT=0 | ok |
| 36 | SX1 | driven low | forced low while MT=0 | ok |
| 13 | FSY | Hi-Z | input (IO=0) | ok; GPIO4 wpd keeps it defined |
| 14 | SCK | Hi-Z | input (IO=0) | ok; R16 = recommended pull-down |
| 31 | RS | input, must be driven low | | ok; C5 |
| 35 | SR1 | Hi-Z | input | ok; R17 keeps it defined |
| 39 | SCL | Hi-Z | | ok; R8 |
| 40 | SDA | Hi-Z | | ok; R9 |
| 41 | INT | Hi-Z | open-drain | ok; R10 |

Levels: U1 VOH ≥2.8 V vs ESP32 VIH 2.475 V; ESP32 VOH ≥2.64 V vs U1 VIH 2.0 V. ok.

### U3 pins facing the ESP32 (PCM512x p.5–6)

SDA 11, SCL 12, BCK 21, DIN 22, LRCK 23; MODE1 low and MODE2 high select I2C. ok.

### U11: CH340C SOIC-16

KiCad symbol, not the datasheet. Footprint is standard: pad 1 top-left, counter-clockwise, pitch 1.27, span 4.95.

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | GND | GND | ok |
| 2 | TXD out | USB_TXD | ok |
| 3 | RXD in | USB_RXD | ok |
| 4 | V3 | USB_3V3, tied to VCC | ok; tie rule at 3.3 V from memory |
| 5 | UD+ | USB_DP | ok |
| 6 | UD− | USB_DM | ok |
| 7, 8 | NC | none | ok |
| 9–12 | ~CTS, ~DSR, ~RI, ~DCD inputs | open | CHECK; internal pull-ups from memory only |
| 13 | ~DTR | USB_DTR | ok |
| 14 | ~RTS | USB_RTS | ok |
| 15 | R232 input | open | CHECK; internal pull-down from memory only |
| 16 | VCC | USB_3V3 | ok |

### U12: AP2112K-3.3 SOT-23-5 (KiCad symbol, not the datasheet)

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1 | VIN | USB_VBUS | ok |
| 2 | GND | GND | ok |
| 3 | EN | USB_VBUS | ok |
| 4 | NC | none | ok |
| 5 | VOUT | USB_3V3 | ok |

Input and output capacitors: C66 1 µF, C67 1 µF, C68 100 nF. Value requirement is from memory.

### U13: USBLC6-2SC6 SOT-23-6 (KiCad symbol, not the datasheet)

| Pad | Function | Net | Verdict |
|---|---|---|---|
| 1, 6 | I/O1 | USB_DP | ok |
| 3, 4 | I/O2 | USB_DM | ok |
| 2 | GND | GND | ok |
| 5 | VBUS | USB_VBUS | ok |

### Q2 and Q3: MMBT3904 SOT-23 (KiCad symbol, not the datasheet)

Pad 1 = B, 2 = E, 3 = C. This is what the circuit needs.

| Part | B | E | C | Verdict |
|---|---|---|---|---|
| Q2 | R64 from DTR | RTS | ESP_EN | ok |
| Q3 | R65 from RTS | DTR | ESP_IO0 | ok |

### Passives

| Ref | Value | Connection | Verdict |
|---|---|---|---|
| R60, R61 | 5.1k | CC1, CC2 to GND (public standard) | ok |
| R62 | 4.7k | U11.2 to U2.34 | ok |
| R63 | 4.7k | U2.35 to U11.3 | ok |
| R64, R65 | 10k | base resistors | ok |
| C66 | 1 µF | VBUS to GND | ok |
| C67, C68 | 1 µF, 100 nF | USB_3V3 to GND | ok |
| R16 | 10k | SCK to GND | ok; P2 |
| R17 | 100k | SR1 to GND | ok |
| R20, R21 | 10k | EN and IO0 up | ok |
| R22–R25 | 10k | MODE lines up | ok |
| C20, C21, C22 | 22 µF, 100 nF, 1 µF | per Fig 11 and p.39 | ok |
| R26 | 1k | LED to LED_A | ok |

### J10: USB-C 14P

Contact names are from my knowledge of the public standard. There is no drawing.

| Pads | Function | Net | Verdict |
|---|---|---|---|
| A1, A12, B1, B12 | GND | GND | ok |
| A4, A9, B4, B9 | VBUS | USB_VBUS | ok |
| A5 | CC1 | R60 | ok |
| B5 | CC2 | R61 | ok |
| A6, B6 | D+ | USB_DP | ok |
| A7, B7 | D− | USB_DM | ok |
| SH | shell | GND | ok |

Pad order along the row (A5, B7, A6, B6, A7, B5) follows the standard contact sequence. The map is 180° symmetric, so nets agree in either orientation that physically fits.

### SW1: DP-04 Piano

No datasheet; KiCad stock footprint.
- Pads 1–4 are GND, pads 5–8 are MODE0–3.
- Closed = low; external pull-ups present.
- ok; C7.

### SW2: TL3305A

No datasheet; KiCad stock footprint, which matches the board.
- Pads 1,1 = +3V3_AON. Pads 2,2 = WAKE_FORCE, with R52 10k down and D3.2.
- D3 is common cathode per its KiCad symbol: pads 1 and 2 are the anodes, pad 3 the cathode.
- ok.

### D20 with R26

KiCad symbol and footprint.
- Pad 1 = K to GND, pad 2 = A to LED_A. R26 runs from GPIO22.
- About 1–2 mA.
- ok.

## 4. Not verified

- **No datasheet on this PC:**
  - CH340C: IOL during C22 discharge, power-up DTR/RTS levels, internal pulls, behaviour at low VCC.
  - AP2112K: reverse current, capacitor requirements.
  - MMBT3904: inverse β, leakage.
- **J10:** real part geometry and hole sizes (0.4 mm drill, 0.6 mm pad, 0.5×1.7 slots).
- **SW1, SW2, D20:** no drawings. TL3305 terminal pairing is taken from KiCad.
- **ESP32:**
  - Behaviour with CHIP_PU between 0.6 V and 0.75·VDD.
  - Per-pin injection and sink limits are not given.
- **Boot flow figure (Module p.15):** a raster image, and no PDF renderer is installed. Whether the ROM's SDIO slave in download mode reacts to U1 running on GPIO14/15 (SD_CLK, SD_CMD) is unsettled.
- **Module C4 (0.1 µF, schematic p.38):** drawn near EN; I did not trace it. If it is on EN, the RC rises by 10% at most. That is harmless.
- **Module ambient rating:** −40 to 85 °C (p.2). The installed temperature was not assessed.
