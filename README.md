# Bitline

A stand-in for the stock JLR MOST25 amplifier. It takes the digital sound off the ring, carries out the head unit's
commands with no loss of quality and sends the sound on to your own DAC or amplifier. A phone joins over Bluetooth or
AirPlay.

Українською: [README.uk.md](README.uk.md).

Jaguar, Land Rover and JLR are trademarks of their owners. This project is not affiliated with them.

## What it is for

- Fit your own DAC and amplifier and keep the stock head unit with all its controls: volume, tone, mute, navigation,
  phone, parking sensors and chimes.
- Play a phone through the same system, with no cables.

## Cars

- JLR MOST25 stock amplifiers of two generations. The pre-facelift Hi-Line and Premium are the same amplifier in Jaguar
  and Land Rover, set up for the car. AUD3-8, -12 and -16 (Meridian) look the same on the bus. SW1 selects the type.
- First car: Jaguar XF X250, 2009.
- Component protection (Land Rover, Range Rover): the device reads the stock amplifier's serial number and the rest of
  how it answers the head unit, and then answers the same way.

## What it does

- **Sound from the ring.** 48 kHz, 16 bit. Pre-facelift: sinks 1-4 carry stereo music; phone, navigation and voice;
  parking sensors; chimes. AUD3: main or video (stereo), call, parking sensors, navigation. They are mixed down to
  stereo, as the stock amplifier does.
- **Head unit commands.** Levels per sink (volume), balance, mute with the ramps from MuteParameters, bass, treble.
  Processing in float, 24-bit out; at neutral settings the sound passes bit for bit.
- **Outputs**, all at once, one stream:
  - line RCA, 2.1 Vrms, straight from a PCM5122 DAC, no op-amp;
  - I2S with MCLK over HDMI for an external DAC: LVDS, PS Audio pinout;
  - S/PDIF: TOSLINK and 0.5 V coax into 75 ohm;
  - REM +12 V for an external amplifier.
- **Phone.** Bluetooth A2DP from any phone, with pause and volume over AVRCP; lossless AirPlay from an iPhone over the
  phone's hotspot. The phone's sound follows the ring's clock.
- **Switching.** The phone plays while the head unit is on a quiet source (AUX or paused). When the head unit plays, the
  phone pauses; when it goes quiet, the phone plays again. Of two phones, the newer one plays.
- **Updates over Wi-Fi** with a trial start and a return to the previous firmware if the new one does not take: it
  crashes, does not keep the network, the page or the sound path, or the ring chip does not start with it.
- **Start and sleep.** Joins the ring as the stock amplifier would. Sleeps on the network master's command, when the
  ring's light goes or when the ring chip stops answering; wakes on light.

## Hardware

- ESP32-WROVER-IE: 16 MB flash, 8 MB PSRAM, Bluetooth, Wi-Fi, external antenna connector.
- OS8104A, the MOST node: control, ring sound, clocks, S/PDIF.
- PCM5122, the line-out DAC.
- Optics: TE 1-1394640-1; the ring's plug goes straight into it.
- Power: +12 V from the stock amplifier's connector. Asleep, only the standby LDO and the optical receiver are on.
- An optional tube board stacks on the main one on standoffs and takes the DAC's line out: four 6N28B-V or 6N16B-V as an
  amplifier stage, a concertina and followers, no transformer; XLR and RCA out. Its anode supply comes about 12 s after
  REM, and about 9 s later it switches on the external amplifier's REM; on the way down that goes first.
- **Bitline Lite** is the same main board without the DAC, RCA L/R, HDMI, REM and USB (51 parts not fitted): the car
  keeps its ring, the sound leaves over TOSLINK and coax. It is flashed once through the J3 header, then over Wi-Fi.
  `hw/gen/lite.py` makes its package, LCSC numbers included.

## Status

- **Firmware.** The amplifier emulation, processing, phone, Wi-Fi, AirPlay, switching, profiles, the serial number
  read, updates and sleep are checked on a virtual model (QEMU) by test scripts, and what depends on the ring chip by
  tests on a PC with stubs for the hardware. The emulation has been reconciled function by function with usb2most's
  code, and the self-tests take their reference bytes from its firmware
  ([docs/usb2most/check_amp.md](docs/usb2most/check_amp.md)). The board on the bench, the ring in a car, ring sound to
  the outputs and REM are still to be checked on a real board.
- **Boards.** The 2- and 4-layer main boards and the tube board are ready to order; none has been made or tested yet.
  The main board has been checked pin by pin against the datasheets, its ring part also against usb2most's schematic
  ([docs/board_check/check.md](docs/board_check/check.md)). What only a live board can show is listed there as well.

## Repository map

| Path | What |
|---|---|
| `fw/` | firmware, ESP-IDF v5.5.6, C |
| `fw/tools/` | PC scripts that play the ring, a phone, an iPhone and the head unit for the firmware in QEMU; `host/` has tests that run on a PC without an ESP32 |
| `hw/` | main board, 2 layers, KiCad 10: schematic and board; `gen/` holds the generators and checks, `production/` the files for JLCPCB |
| `hw4/` | the same board on 4 layers |
| `tube/` | the tube board: `6n28b/` (board and package), `6n16b/` (schematic and BOM for 6N16B-V on the same board), `gen/` |
| `docs/` | concept, block diagram, stock amplifiers, firmware plan and log, the reconciliation with usb2most, the datasheet check of the main board |
| `ref/usb2most/` | reference files from [usb2most](https://github.com/smartgauges/usb2most), GPL-3.0 |

## Build

Firmware:

1. Install ESP-IDF v5.5.6. The path to `fw` must be in Latin letters, without spaces.
2. In `fw`: `idf.py build`. The first build fetches espressif/mdns 1.14.1 from the Espressif registry.
3. Flash with `idf.py -p COM5 flash monitor` over the board's USB-C. On the bench without a ring, hold SW2 for the whole
   write: it powers the board without light on the ring. Bitline Lite has no USB: a 3.3 V USB-UART adapter goes on J3
   (pin 1 IO0 to ground for the write, 2 GND, 3 RXD0, 4 TXD0) and esptool runs with `--before no_reset --after no_reset`.

Virtual model: `idf.py -B build_qemu -DQEMU=1 build`, then `idf.py -B build_qemu qemu`. PC tests:
`python tools/host/run.py`, which needs GCC MinGW-w64. `tools/host/golden.py` makes `main/u2m_golden.h` from usb2most's
firmware; that binary is not in this repository ([ref/usb2most/README.md](ref/usb2most/README.md)), and without it
`run.py` says the header was not compared and goes on.

Boards: KiCad 10. Every `production/` folder has a README with the JLCPCB order fields; `hw/gen/fab.py` regenerates
the files.

## Licence

Copyright 2026 Oleg-mk.

- **Hardware, under CERN-OHL-S-2.0** ([LICENSE](LICENSE)): `hw/`, `hw4/` and `tube/`: the KiCad designs and the
  scripts that generate them, footprints, 3D models, fabrication files.
- **Software and its documentation, under GPL-3.0-or-later** ([COPYING](COPYING)): `fw/` and `docs/`.
- **Other people's parts keep their licences:** Apple's ALAC decoder in `fw/components/alac/` (Apache-2.0,
  `APPLE_LICENSE.txt`); the AirPort key in `fw/main/raop_key.c` as shairport-sync carries it (MIT); `ref/usb2most/`
  (GPL-3.0); footprints and 3D models from other sources as `hw/SOURCES.md` lists them.

If you build or change the hardware and share it, share your sources under the same licence.

## How it was made

The author made the decisions, placed the parts and routed much of the copper. Claude (Anthropic's AI model, in
Claude Code) wrote the firmware, the schematic generators, the checks and the rest of the copper, as a co-author.
