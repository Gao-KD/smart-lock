#!/usr/bin/env bash
# Regenerate and verify the KiCad project. Needs KiCad 8 (kicad-cli + pcbnew python).
# Easiest on any machine with Docker:
#   docker run --rm -v "$PWD/../..":/repo -w /repo/hardware/mainboard kicad/kicad:8.0 tools/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."
SYMDIR=${KICAD8_SYMBOL_DIR:-/usr/share/kicad/symbols}
mkdir -p ~/.config/kicad/8.0
[ -f ~/.config/kicad/8.0/sym-lib-table ] || cp /usr/share/kicad/template/sym-lib-table ~/.config/kicad/8.0/
[ -f ~/.config/kicad/8.0/fp-lib-table ] || cp /usr/share/kicad/template/fp-lib-table ~/.config/kicad/8.0/
TMP=$(mktemp -d)
python3 tools/make_pro.py
python3 tools/gen_project.py "$SYMDIR"
kicad-cli sch erc --severity-all --exit-code-violations -o production/erc.rpt smart-lock-mainboard.kicad_sch
kicad-cli sch export netlist -o "$TMP/board.net" smart-lock-mainboard.kicad_sch
python3 tools/check_netlist.py "$TMP/board.net"
python3 tools/make_pcb.py "$TMP/board.net"
kicad-cli pcb drc --schematic-parity --severity-error -o "$TMP/drc.rpt" smart-lock-mainboard.kicad_pcb | grep -i parity
kicad-cli sch export pdf -o production/schematic.pdf smart-lock-mainboard.kicad_sch
rm -rf "$TMP"
echo "OK"
