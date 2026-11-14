# First PCB version (Fusion Electronics) – legacy

**For new builds, use the KiCad version in [`../kicad/`](../kicad/).**

This is the first version of the DRO board, designed in Autodesk Fusion Electronics and
manufactured at JLCPCB in May 2026. The KiCad board was derived from it with the same layout and
connections, and then extended.

**Missing on this version:**

- **No pull-up resistors** on the A/B lines. An input without a connected scale floats, picks up
  noise and the display can count steps that do not exist. (KiCad version: R1–R8, 10 kΩ to +5 V.)
- **No terminal for an on/off switch.** (KiCad version: J10 between the barrel jack and +5 V.)
- All parts are soldered by hand (no assembly files).

| File | Content |
|---|---|
| `DRO_Fusion_Electronics.f3z` | Fusion archive: schematic, board and 3D model of the board |
| `DRO_V2_Gerber_Fusion.zip` | Gerber files of this version as ordered |

---

**Deutsch:** Erste Platinenversion aus Fusion (gefertigt im Mai 2026). Ihr fehlen die
**Pull-up-Widerstände** an den A/B-Leitungen und die **Klemme für den Ein/Aus-Schalter**. Für einen
Nachbau bitte die KiCad-Version in [`../kicad/`](../kicad/) nehmen.
