#!/usr/bin/env python3
"""Write smart-lock-mainboard.kicad_pro: net classes, JLCPCB-friendly rules."""
import json, os
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def nc(name, clearance, track, via_d, via_drill, prio):
    return {"name": name, "bus_width": 12, "clearance": clearance, "diff_pair_gap": 0.25,
            "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2, "line_style": 0, "microvia_diameter": 0.3,
            "microvia_drill": 0.1, "pcb_color": "rgba(0, 0, 0, 0.000)", "priority": prio,
            "schematic_color": "rgba(0, 0, 0, 0.000)", "track_width": track, "via_diameter": via_d,
            "via_drill": via_drill, "wire_width": 6}

pro = {
  "board": {
    "design_settings": {
      "defaults": {"board_outline_line_width": 0.05, "copper_line_width": 0.2, "copper_text_size_h": 1.0,
                   "copper_text_size_v": 1.0, "copper_text_thickness": 0.15, "silk_line_width": 0.12,
                   "silk_text_size_h": 0.8, "silk_text_size_v": 0.8, "silk_text_thickness": 0.12},
      "rules": {"min_clearance": 0.1, "min_track_width": 0.1, "min_via_diameter": 0.45,
                "min_through_hole_diameter": 0.2, "min_via_annular_width": 0.1, "min_hole_to_hole": 0.25,
                "min_copper_edge_clearance": 0.3, "min_hole_clearance": 0.2, "min_silk_clearance": 0.0,
                "min_microvia_diameter": 0.2, "min_microvia_drill": 0.1, "min_text_height": 0.8,
                "min_text_thickness": 0.08, "use_height_for_length_calcs": True, "allow_blind_buried_vias": False,
                "allow_microvias": False},
      "track_widths": [0.0, 0.15, 0.2, 0.3, 0.5, 0.8],
      "via_dimensions": [{"diameter": 0.0, "drill": 0.0}, {"diameter": 0.45, "drill": 0.2},
                         {"diameter": 0.6, "drill": 0.3}]
    }
  },
  "meta": {"filename": "smart-lock-mainboard.kicad_pro", "version": 1},
  "net_settings": {
    "classes": [nc("Default", 0.127, 0.15, 0.45, 0.2, 2147483647),
                nc("Power", 0.2, 0.5, 0.6, 0.3, 0),
                nc("RF", 0.2, 0.3, 0.45, 0.2, 1)],
    "meta": {"version": 3},
    "net_colors": None,
    "netclass_assignments": None,
    "netclass_patterns": [
      {"netclass": "Power", "pattern": p} for p in
      ["BAT", "VBAT_P", "USB_VBUS", "MOT_OUT1", "MOT_OUT2", "MOT_GND", "SW_3V3", "SW_5V", "5V_FACE"]
    ] + [{"netclass": "RF", "pattern": p} for p in
         ["SUBG_O1", "SUBG_I1", "RF_*", "ANT_*", "RF2G4_*", "RF24_*"]]
  },
  "pcbnew": {"last_paths": {"gencad": "", "idf": "", "netlist": "", "plot": "production/gerber/",
                            "pos_files": "production/", "specctra_dsn": "", "step": "", "svg": "", "vrml": ""},
             "page_layout_descr_file": ""},
  "schematic": {"drawing": {"default_line_thickness": 6.0, "default_text_size": 50.0,
                            "field_names": [{"name": "LCSC", "visible": False}, {"name": "MPN", "visible": False}]},
                "legacy_lib_dir": "", "legacy_lib_list": []},
  "sheets": [],
  "text_variables": {}
}
with open(os.path.join(HERE, "smart-lock-mainboard.kicad_pro"), "w") as f:
    json.dump(pro, f, indent=2)
