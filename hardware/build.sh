#!/usr/bin/env bash
# Rebuilds the schematic and the board, then checks both:
#   hardware/build.sh
set -euo pipefail
cd "$(dirname "$0")"
KICAD_PYTHON="${KICAD_PYTHON:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3}"
python3 make_schematic.py
kicad-cli sch erc --severity-error --exit-code-violations -o /tmp/fddsynthpro_erc.rpt fddsynthpro_carrier.kicad_sch
kicad-cli sch export netlist --format kicadsexpr -o /tmp/fddsynthpro_carrier.net fddsynthpro_carrier.kicad_sch
"$KICAD_PYTHON" make_pcb.py /tmp/fddsynthpro_carrier.net 2> >(grep -v "stdpbase\|traits" >&2)
# KiCad refills the ground pours (pcbnew's filler from a script leaves them
# stale) and saves the board with them, then checks it all.
kicad-cli pcb drc --refill-zones --save-board --schematic-parity --severity-all --format report \
    -o /tmp/fddsynthpro_drc.rpt fddsynthpro_carrier.kicad_pcb
grep -E "^\*\* Found" /tmp/fddsynthpro_drc.rpt

# Fabrication files: Gerbers and drill files, zipped, for JLCPCB and the like.
rm -rf fab && mkdir -p fab
kicad-cli pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts \
    --subtract-soldermask -o fab/ fddsynthpro_carrier.kicad_pcb >/dev/null
kicad-cli pcb export drill --format excellon --excellon-separate-th --generate-map --map-format gerberx2 \
    -o fab/ fddsynthpro_carrier.kicad_pcb >/dev/null
(cd fab && zip -q ../fddsynthpro_carrier_fab.zip ./*)
echo "fab: hardware/fddsynthpro_carrier_fab.zip ($(ls fab | wc -l | tr -d ' ') files)"

# JLCPCB assembly: the BOM and the placement (CPL), from KiCad's own
# position export (through-hole parts included: J1 is assembled too).
kicad-cli pcb export pos --format csv --units mm --side both -o /tmp/fddsynthpro_pos.csv \
    fddsynthpro_carrier.kicad_pcb >/dev/null
python3 make_jlc.py /tmp/fddsynthpro_carrier.net /tmp/fddsynthpro_pos.csv jlc  # production/ is the Fabrication Toolkit's
