# Hardware

KiCad 10 project of the clock, board revision B.

| File | Content |
|------|---------|
| `binary_clock.kicad_pro` | project file, open this one |
| `binary_clock.kicad_sch` | schematic |
| `binary_clock.kicad_pcb` | PCB |
| `binary_clock.kicad_sym`, `binary_clock.pretty/` | project libraries: symbol and footprint of the DCF77 module |
| `sym-lib-table`, `fp-lib-table` | project library tables |
| [`bom.csv`](bom.csv) | bill of materials |

Round board, Ø 180 mm, two layers. All parts go on the front; they are
through-hole parts, except the DCF77 module. The circuit is described in
[docs/hardware.md](../docs/hardware.md); fabrication files and assembly
are covered in [docs/assembly.md](../docs/assembly.md).
