#!/usr/bin/env bash
# Rebuilds rev C from its generators and checks it: schematic, ERC, placement, routing, DRC.
# Placement overwrites the board and routing takes about 8 minutes. Set:
#   KICAD_SHARE, KICAD_CLI, KICAD_PYTHON  KiCad 10 (the portable copy's env.sh sets all three)
#   FREEROUTING, JAVA                     freerouting-2.5.0.jar and a Java 25 runtime
# Pass --silkscreen to redo only the labels on the routed board.
set -euo pipefail
cd "$(dirname "$0")"
if [[ "${1:-}" == "--silkscreen" ]]; then
    "$KICAD_PYTHON" tools/layout_pcb.py --silkscreen fddsynthpro_revc.kicad_pcb
else
    python tools/gen_schematic.py
    "$KICAD_PYTHON" tools/make_logo.py
    "$KICAD_CLI" sch erc --exit-code-violations fddsynthpro_revc.kicad_sch
    "$KICAD_PYTHON" tools/layout_pcb.py
    "$KICAD_PYTHON" tools/autoroute.py
    "$KICAD_PYTHON" tools/layout_pcb.py --silkscreen fddsynthpro_revc.kicad_pcb
fi
# Expect 0 unconnected, 0 parity issues, and 7 warnings: J1, J8 and J9's outlines overhang the edge.
"$KICAD_CLI" pcb drc --refill-zones --save-board --schematic-parity fddsynthpro_revc.kicad_pcb
"$KICAD_CLI" pcb render --side top --width 2000 --height 1400 --quality basic --zoom 1.3 -o revc-top.png fddsynthpro_revc.kicad_pcb
"$KICAD_CLI" pcb render --side bottom --width 2000 --height 1400 --quality basic --zoom 1.3 -o revc-bottom.png fddsynthpro_revc.kicad_pcb
