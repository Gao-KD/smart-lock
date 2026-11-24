#!/usr/bin/env python3
"""Compare a KiCad netlist export with the intended connections (parts.json)
and with the board-spec pin table. Usage: check_netlist.py <file.net>"""
import json, os, re, sys
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
s = open(sys.argv[1], encoding="utf-8").read()
nets = {}
for m in re.finditer(r'\(net \(code "\d+"\) \(name "([^"]*)"\)(.*?)(?=\(net \(code|\Z)', s, re.S):
    nets[m.group(1).lstrip("/")] = set(re.findall(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', m.group(2)))
want = {}
for p in json.load(open(os.path.join(HERE, "tools", "parts.json"), encoding="utf-8")):
    for pin, net in p["pins"].items():
        if net is not None:
            want.setdefault(net, set()).add((p["ref"], pin))
bad = 0
for net, nodes in sorted(want.items()):
    got = nets.get(net, set())
    if not nodes <= got:
        bad += 1
        print("MISSING", net, sorted(nodes - got))
    extra = {n for n in got - nodes if not n[0].startswith("#")}
    if extra:
        bad += 1
        print("EXTRA", net, sorted(extra))
# board-spec section 3 (GPIO -> signal) via U1 pads
spec = {"KEY_INT": "32", "SWCLK": "34", "SWDIO": "35", "SWV": "36", "NFC_IRQ": "37", "SETUP_KEY": "38",
        "FP_TOUCH": "39", "DOOR_HALL": "40", "VCOM_TX": "41", "VCOM_RX": "42", "PROX_INT": "43",
        "MECH_KEY": "44", "KEY_SCL": "45", "KEY_SDA": "46", "BUZZER": "47", "DBL_LOCK": "31", "TAMPER": "30",
        "VBAT_ADC": "29", "SPARE_WAKE": "28", "MOT_ISENSE": "27", "LED_R": "26", "NFC_SCK": "3",
        "NFC_MOSI": "4", "NFC_MISO": "5", "NFC_CS": "7", "NFC_NRST": "8", "NFC_PWR_EN": "9", "MOT_IN1": "10",
        "MOT_IN2": "11", "MOT_NSLEEP": "12", "VBAT_EN": "13", "LED_G": "14", "LED_B": "15", "LFXTAL_O": "2",
        "LFXTAL_I": "1", "FP_TX": "68", "FP_RX": "67", "FP_PWR_EN": "66", "FACE_PWR_EN": "65", "FACE_TX": "64",
        "FACE_RX": "63", "KEY_BL_EN": "62"}
for sig, pad in spec.items():
    if ("U1", pad) not in nets.get(sig, set()):
        bad += 1
        print("PINMAP", sig, "U1 pad", pad)
print("nets:", len(nets), "checked:", len(want), "pin-table signals:", len(spec), "problems:", bad)
sys.exit(1 if bad else 0)
