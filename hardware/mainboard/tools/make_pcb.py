#!/usr/bin/env python3
"""Create smart-lock-mainboard.kicad_pcb: 4-layer empty board with every
footprint loaded and every pad on its net (same result as "Update PCB from
Schematic"). Parts are parked beside a placeholder outline, grouped by sheet;
layout and routing are left to the designer.

Run with KiCad 8's python (needs pcbnew), after gen_project.py:
    python3 tools/make_pcb.py <netlist.net>
"""
import json
import os
import re
import sys

import pcbnew

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FPDIR = os.environ.get("KICAD8_FOOTPRINT_DIR", "/usr/share/kicad/footprints")
OUT = os.path.join(HERE, "smart-lock-mainboard.kicad_pcb")
MM = pcbnew.FromMM

parts = json.load(open(os.path.join(HERE, "tools", "parts.json"), encoding="utf-8"))
netlist = open(sys.argv[1], encoding="utf-8").read()

# net names exactly as the schematic exports them (incl. unconnected-(...) nets)
pad_net = {}
for m in re.finditer(r'\(net \(code "\d+"\) \(name "((?:[^"\\]|\\.)*)"\)(.*?)(?=\(net \(code|\Z)', netlist, re.S):
    for ref, pin in re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', m.group(2)):
        pad_net[(ref, pin)] = m.group(1).replace('\\"', '"')

board = pcbnew.NewBoard(OUT)
board.SetCopperLayerCount(4)
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(4)
ds.SetBoardThickness(MM(1.6))

nets = {}


def net(name):
    if name not in nets:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        nets[name] = ni
    return nets[name]


# placeholder outline 70 x 50 mm (replace with the measured lock housing)
W, H = 70.0, 50.0
corners = [(0, 0), (W, 0), (W, H), (0, H)]
for i in range(4):
    seg = pcbnew.PCB_SHAPE(board)
    seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
    seg.SetLayer(pcbnew.Edge_Cuts)
    seg.SetWidth(MM(0.05))
    seg.SetStart(pcbnew.VECTOR2I(MM(corners[i][0]), MM(corners[i][1])))
    seg.SetEnd(pcbnew.VECTOR2I(MM(corners[(i + 1) % 4][0]), MM(corners[(i + 1) % 4][1])))
    board.Add(seg)

txt = pcbnew.PCB_TEXT(board)
txt.SetText("PLACEHOLDER OUTLINE 70x50 - replace with lock housing outline")
txt.SetLayer(pcbnew.Cmts_User)
txt.SetPosition(pcbnew.VECTOR2I(MM(0), MM(-4)))
txt.SetTextSize(pcbnew.VECTOR2I(MM(1.5), MM(1.5)))
board.Add(txt)

order = ["mcu.kicad_sch", "rf.kicad_sch", "power.kicad_sch", "motor.kicad_sch", "io.kicad_sch"]
x0 = W + 10
col_x = x0
for sheet in order:
    group = [p for p in parts if p["sheet"] == sheet]
    x, y, row_h, col_w = col_x, 0.0, 0.0, 0.0
    maxw = 70.0
    for p in group:
        lib, name = p["footprint"].split(":", 1)
        path = os.path.join(HERE, "lib", lib + ".pretty") if lib == "smartlock" else os.path.join(FPDIR, lib + ".pretty")
        fp = pcbnew.FootprintLoad(path, name)
        if fp is None:
            raise SystemExit(f"footprint not found: {p['footprint']}")
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(p["ref"])
        fp.SetValue(p["value"])
        fp.SetPath(pcbnew.KIID_PATH(p["path"]))
        fp.SetSheetname(p["sheetname"])
        fp.SetSheetfile(p["sheet"])
        for k in ("LCSC", "MPN"):
            if p[k.lower()]:
                f = pcbnew.PCB_FIELD(fp, fp.GetFieldCount(), k)
                f.SetText(p[k.lower()])
                f.SetVisible(False)
                f.SetLayer(pcbnew.F_Fab)
                fp.AddField(f)
        attrs = fp.GetAttributes()
        if p["dnp"]:
            attrs |= pcbnew.FP_DNP
        if p["ref"].startswith("TP") or p["ref"].startswith("ANT2"):
            attrs |= pcbnew.FP_EXCLUDE_FROM_BOM
        fp.SetAttributes(attrs)
        for pad in fp.Pads():
            n = pad_net.get((p["ref"], pad.GetNumber()))
            if n:
                pad.SetNet(net(n))
        bb = fp.GetBoundingBox(False, False)
        try:
            cy = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
            if cy.GetWidth() > 0:
                bb.Merge(pcbnew.BOX2I(pcbnew.VECTOR2I(cy.GetX(), cy.GetY()),
                                      pcbnew.VECTOR2L(cy.GetWidth(), cy.GetHeight())))
        except Exception:
            pass
        w, h = pcbnew.ToMM(bb.GetWidth()) + 1.5, pcbnew.ToMM(bb.GetHeight()) + 1.5
        if x + w > col_x + maxw:
            x = col_x
            y += row_h
            row_h = 0
        # place so that the bounding box (not the anchor) lands in the slot
        ox = pcbnew.ToMM(fp.GetPosition().x - bb.GetX())
        oy = pcbnew.ToMM(fp.GetPosition().y - bb.GetY())
        fp.SetPosition(pcbnew.VECTOR2I(MM(x + 0.75 + ox), MM(y + 0.75 + oy)))
        board.Add(fp)
        # keep the footprint's bbox origin consistent after placement
        x += w
        row_h = max(row_h, h)
        col_w = max(col_w, x - col_x)
    lbl = pcbnew.PCB_TEXT(board)
    lbl.SetText(sheet.replace(".kicad_sch", ""))
    lbl.SetLayer(pcbnew.Cmts_User)
    lbl.SetPosition(pcbnew.VECTOR2I(MM(col_x), MM(-4)))
    lbl.SetTextSize(pcbnew.VECTOR2I(MM(2), MM(2)))
    board.Add(lbl)
    col_x += max(col_w, 20) + 10

board.BuildConnectivity()
pcbnew.SaveBoard(OUT, board)
print("footprints:", len(board.GetFootprints()), "nets:", len(nets))
