# Ordering the PCB at JLCPCB

These files were generated from the KiCad project in [`../`](../) (KiCad 10) and used for a real
order at JLCPCB.

| File | Purpose |
|---|---|
| `DRO_Gerber_JLCPCB.zip` | PCB: Gerber files and drill data (Excellon, mm) |
| `DRO_BOM_JLCPCB.csv` | Bill of materials with JLCPCB/LCSC part numbers (SMD parts only) |
| `DRO_CPL_JLCPCB.csv` | Pick-and-place list (position, side, rotation) |

## PCB

- 2 layers, 130 × 80 mm, 1.6 mm FR-4
- Smallest hole 0.35 mm (vias), narrowest track 0.3 mm
- Surface finish: lead-free HASL works well, because through-hole parts are soldered by hand

## Assembly

Upload the Gerber zip, enable **PCB Assembly** and choose
**PCBA type: Economic, Assembly side: Bottom side**. Then upload the BOM and CPL files.

JLCPCB assembles only the SMD parts on the bottom side:

| Parts | Value | JLCPCB/LCSC no. |
|---|---|---|
| R1–R8 | 10 kΩ 1 % 0805 (UNI-ROYAL 0805W8F1002T5E) | C17414 (basic) |
| C1, C2 | 100 nF X7R 50 V 0805 (YAGEO CC0805KRX7R9BB104) | C49678 (basic) |
| U1, U2 | SN74LVC14ADR, SOIC-14 | C133541 (extended) |

For 5 boards this cost about 23 € without shipping (quote from October 2026). Choosing "Standard"
and "Both sides" (to also have the JST sockets soldered) costs about four times as much.

### Through-hole parts – solder them yourself

| Parts | Part | LCSC no. |
|---|---|---|
| J1, J2, J3, J5 | JST B5B-XH-A(LF)(SN), 5 pins | C157991 |
| J4 | XKB DC-005-5A-2.0 barrel jack | C381116 |
| J6, J7 | TE 282837-3 screw terminal, 3 pins, 5.08 mm | C589616 |
| J8, J9 | TE 282837-4 screw terminal, 4 pins, 5.08 mm | C5172818 |
| J10 | TE 282837-2 screw terminal, 2 pins, 5.08 mm | C592976 |
| U3 | ESP32 DevKit V1, 30 pins (female headers recommended) | – |

H1–H8 are mounting holes only.

**On/off switch at J10:** pin 2 comes from the barrel jack J4, pin 1 supplies the board (+5 V).
Without a switch, fit a wire bridge – otherwise the board gets no power from J4. The +5 V
terminals at J6 (pin 3) and J9 (pin 1) are behind the switch.

## Hints for the JLCPCB upload

- After uploading the BOM, JLCPCB may report J1–J10 and U3 as "not in the BOM" – that is intended,
  you solder them yourself.
- The message "component may be offset from the PCB" refers to the barrel jack J4, which
  overhangs the board edge on purpose: choose **Cancel**, do not let it auto-align.
- Check the rotations in the assembly preview, especially pin 1 of U1/U2 (the dot/bevel on the
  chip must match the marking in the silkscreen). The CPL file was checked against the preview.
- Check the stock of U1/U2 on the day you order.
