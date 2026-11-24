"""Minimal KiCad 8 schematic writer used by gen_project.py.

It embeds symbols from the KiCad standard library (flattening `extends`)
or from symbols built here, places every part on a grid and connects each
pin with a short wire stub plus a global label, a power symbol or a
no-connect flag. The result opens in KiCad 8 and passes ERC; the user is
expected to tidy the drawing, not the connectivity.
"""
import re
import uuid as _uuid

GRID = 2.54
NS = _uuid.UUID("6f1c2d3e-0000-4000-8000-5ec0de5a11ce")


def uid(*key):
    """Stable UUID so that regenerating does not churn the files."""
    return str(_uuid.uuid5(NS, "/".join(str(k) for k in key)))


def q(s):
    s = str(s).replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"' + s + '"'


def fmt(v):
    v = round(v, 4)
    if v == int(v):
        return str(int(v))
    return ("%.4f" % v).rstrip("0").rstrip(".")


def snap(v):
    return round(v / GRID) * GRID


# --------------------------------------------------------------------------
# s-expression helpers on raw text
# --------------------------------------------------------------------------

def block_end(s, i):
    depth, instr = 0, False
    while True:
        c = s[i]
        if instr:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1


def children(block):
    """Top-level child spans of a block '(head ... )'."""
    out, i, n = [], 1, len(block) - 1
    instr = False
    while i < n:
        c = block[i]
        if c == "(":
            j = block_end(block, i)
            out.append(block[i:j])
            i = j
        else:
            i += 1
    return out


class StdLib:
    def __init__(self, symdir):
        self.symdir = symdir
        self.cache = {}

    def _text(self, lib):
        if lib not in self.cache:
            with open(f"{self.symdir}/{lib}.kicad_sym", encoding="utf-8") as f:
                self.cache[lib] = f.read()
        return self.cache[lib]

    def raw(self, lib, name):
        s = self._text(lib)
        m = re.search(r"\n\t\(symbol " + re.escape(q(name)) + r"\s", s)
        if not m:
            raise KeyError(f"{lib}:{name}")
        i = m.start() + 2
        return s[i:block_end(s, i)]

    def flat(self, lib, name):
        b = self.raw(lib, name)
        m = re.search(r'\(extends "([^"]+)"\)', b)
        if not m:
            return b
        parent = self.flat(lib, m.group(1))
        child_props = [c for c in children(b) if c.startswith("(property ")]
        pchildren = children(parent)
        keep = [c for c in pchildren if not c.startswith("(property ")]
        keep = [c.replace(f'(symbol "{m.group(1)}_', f'(symbol "{name}_') for c in keep]
        flags = [c for c in keep if not c.startswith("(symbol ")]
        units = [c for c in keep if c.startswith("(symbol ")]
        return "(symbol " + q(name) + "\n" + "\n".join(flags + child_props + units) + ")"


PIN_RE = re.compile(
    r'\(pin (\w+) (\w+)\s*\(at ([-\d.]+) ([-\d.]+) ([-\d.]+)\)\s*\(length ([-\d.]+)\)'
    r'.*?\(name "((?:[^"\\]|\\.)*)".*?\(number "((?:[^"\\]|\\.)*)"', re.S)


class Symbol:
    """A library symbol as embedded in lib_symbols, plus its pins."""

    def __init__(self, lib_id, body):
        self.lib_id = lib_id
        name = lib_id.split(":", 1)[1]
        # rename the top-level symbol to its lib_id
        self.body = re.sub(r'^\(symbol "[^"]+"', "(symbol " + q(lib_id), body, count=1)
        self.pins = {}
        for t, style, x, y, a, ln, nm, num in PIN_RE.findall(body):
            self.pins.setdefault(num, []).append(
                dict(type=t, x=float(x), y=float(y), a=float(a) % 360, name=nm))
        self.is_power = "(power)" in body.split("(symbol", 2)[1] if body.count("(symbol") > 1 else False
        xs = [p["x"] for ps in self.pins.values() for p in ps] or [0]
        ys = [p["y"] for ps in self.pins.values() for p in ps] or [0]
        self.bbox = (min(xs), min(ys), max(xs), max(ys))


def box_symbol(lib_id, ref, value, left=(), right=(), top=(), bottom=(), fp="",
               desc="", pin_len=2.54, min_w=10.16):
    """Rectangular IC symbol. Each side is a list of (number, name, type) or
    None for a gap."""
    name = lib_id.split(":", 1)[1]
    maxl = max([len(p[1]) for p in list(left) + list(right) if p] + [1])
    maxt = max([len(p[1]) for p in list(top) + list(bottom) if p] + [1])
    w = max(min_w, snap((maxl * 2 * 1.0 + 4) / 2) * 2, len(top) * GRID + 5.08, len(bottom) * GRID + 5.08)
    w = snap(w / 2) * 2
    h = max(len(left), len(right), 2) * GRID + GRID
    h = max(h, maxt * 1.0 * 2 + 5.08 if (top or bottom) else 0)
    h = snap(h / 2) * 2
    x0, x1, y0, y1 = -w / 2, w / 2, -h / 2, h / 2
    pins = []

    def side(lst, sx, sy, dx, dy, ang):
        for i, p in enumerate(lst):
            if p is None:
                continue
            num, nm, t = p
            px = sx + dx * i
            py = sy + dy * i
            pins.append((t, px, py, ang, nm, num))
    side(left, x0 - pin_len, y1 - GRID, 0, -GRID, 0)
    side(right, x1 + pin_len, y1 - GRID, 0, -GRID, 180)
    side(top, x0 + GRID * 2, y1 + pin_len, GRID, 0, 270)
    side(bottom, x0 + GRID * 2, y0 - pin_len, GRID, 0, 90)

    def prop(k, v, x, y, hide=False):
        h = " (hide yes)" if hide else ""
        return f'(property {q(k)} {q(v)} (at {fmt(x)} {fmt(y)} 0) (effects (font (size 1.27 1.27)){h}))'
    out = [f"(symbol {q(name)} (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes)",
           prop("Reference", ref, x0, y1 + 1.27 + (pin_len if top else 0)),
           prop("Value", value, x0, y0 - 1.27 - (pin_len if bottom else 0)),
           prop("Footprint", fp, 0, 0, True), prop("Datasheet", "", 0, 0, True),
           prop("Description", desc, 0, 0, True),
           f'(symbol "{name}_0_1" (rectangle (start {fmt(x0)} {fmt(y1)}) (end {fmt(x1)} {fmt(y0)}) '
           '(stroke (width 0.254) (type default)) (fill (type background))))',
           f'(symbol "{name}_1_1"']
    for t, px, py, ang, nm, num in pins:
        out.append(f'(pin {t} line (at {fmt(px)} {fmt(py)} {fmt(ang)}) (length {fmt(pin_len)}) '
                   f'(name {q(nm)} (effects (font (size 1.27 1.27)))) '
                   f'(number {q(num)} (effects (font (size 1.27 1.27)))))')
    out.append("))")
    return Symbol(lib_id, "\n".join(out))


def power_symbol(stdlib, name, template="+3V3"):
    body = stdlib.flat("power", template)
    body = body.replace(f'(symbol "{template}_', f'(symbol "{name}_')
    body = re.sub(r'\(property "Value" "[^"]*"', f'(property "Value" {q(name)}', body, count=1)
    body = re.sub(r'\(property "Description" "(?:[^"\\]|\\.)*"',
                  f'(property "Description" {q("Power symbol creates a global label with name " + name)}',
                  body, count=1)
    return Symbol("smartlock:" + name, body)


# --------------------------------------------------------------------------
# schematic sheet
# --------------------------------------------------------------------------

OUT = {0: (-1, 0), 180: (1, 0), 90: (0, 1), 270: (0, -1)}  # outward dir in sheet coords


class Sheet:
    def __init__(self, project, name, filename, title, paper="A3", page=1):
        self.project, self.name, self.filename = project, name, filename
        self.title, self.paper, self.page = title, paper, page
        self.uuid = uid("sheet", filename)
        self.items = []
        self.symbols = {}
        self.texts = []
        self.parts = []
        self.pwr_n = 0
        self.width = 380 if paper == "A3" else 270
        self.height = 260 if paper == "A3" else 180
        self.cur_x, self.cur_y, self.row_h = 20.32, 30.48, 0

    def use(self, sym):
        self.symbols[sym.lib_id] = sym

    def text(self, s, x, y, size=1.5):
        self.texts.append((s, x, y, size))


class Project:
    def __init__(self, name, stdlib):
        self.name = name
        self.stdlib = stdlib
        self.root_uuid = uid("root", name)
        self.sheets = []
        self.syms = {}
        self.power_nets = set()
        self.power_syms = {}
        self.pwr_counter = 0
        self.flg_counter = 0
        self.parts = []          # all real parts (for BOM)

    def sym(self, lib_id, custom=None):
        if lib_id not in self.syms:
            if custom is not None:
                self.syms[lib_id] = custom
            else:
                lib, name = lib_id.split(":", 1)
                self.syms[lib_id] = Symbol(lib_id, self.stdlib.flat(lib, name))
        return self.syms[lib_id]

    def add_power(self, net, template="+3V3"):
        if net == "GND":
            self.power_syms[net] = self.sym("power:GND")
        else:
            self.power_syms[net] = power_symbol(self.stdlib, net, template)
            self.syms["smartlock:" + net] = self.power_syms[net]
        self.power_nets.add(net)

    def sheet(self, *a, **k):
        s = Sheet(self, *a, **k)
        s.page = len(self.sheets) + 2
        self.sheets.append(s)
        return s

    # ---------------------------------------------------------------- parts
    def part(self, sheet, ref, lib_id, value, footprint, pins, lcsc="", mpn="",
             dnp=False, note="", custom=None, bom=True, newrow=False):
        sym = self.sym(lib_id, custom)
        sheet.use(sym)
        for num in pins:
            if num not in sym.pins:
                raise ValueError(f"{ref}: pin {num} not in {lib_id}")
        for num in sym.pins:
            if num not in pins:
                raise ValueError(f"{ref}: pin {num} of {lib_id} not assigned")
        p = dict(ref=ref, lib_id=lib_id, value=value, footprint=footprint, pins=pins,
                 lcsc=lcsc, mpn=mpn, dnp=dnp, note=note, bom=bom, sheet=sheet,
                 sym=sym, newrow=newrow)
        sheet.parts.append(p)
        if bom:
            self.parts.append(p)
        return p

    # --------------------------------------------------------------- layout
    def _label_len(self, net):
        return len(net) * 1.0 + 4

    def _extent(self, p):
        sym = p["sym"]
        x0, y0, x1, y1 = sym.bbox
        # symbol body is between pins; extend by stub + label
        l = r = u = d = 0
        for num, plist in sym.pins.items():
            net = p["pins"][num]
            ln = 0 if net is None else (GRID * 1 + (4 if net in self.power_nets else self._label_len(net)))
            for pin in plist:
                a = pin["a"]
                if a == 0:
                    l = max(l, ln)
                elif a == 180:
                    r = max(r, ln)
                elif a == 90:
                    d = max(d, ln)
                else:
                    u = max(u, ln)
        # sheet coords: y inverted
        left = x0 - l - 2
        right = x1 + r + 2
        top = -y1 - u - 3
        bottom = -y0 + d + 3
        return left, right, top, bottom

    def layout(self, sheet):
        lines = sum(t[0].count("\n") + 1 for t in sheet.texts)
        x, y, row_h = 15.24, snap(20 + lines * 2.8), 0
        maxw = sheet.width - 15
        for p in sheet.parts:
            l, r, t, b = self._extent(p)
            w, h = r - l, b - t
            if p["newrow"] or x + w > maxw:
                x = 15.24
                y += row_h + 5.08
                row_h = 0
            p["x"] = snap(x - l)
            p["y"] = snap(y - t)
            x += w + 5.08
            row_h = max(row_h, h)
        sizes = [("A4", 297, 210), ("A3", 420, 297), ("A2", 594, 420)]
        need_h = y + row_h + 15
        for name, W, H in sizes:
            if W >= sheet.width + 10 and H >= need_h:
                sheet.paper = name
                break
        else:
            raise RuntimeError(f"sheet {sheet.filename} overflows: {need_h:.0f} mm")

    # ---------------------------------------------------------------- write
    def _symbol_instance(self, sheet, lib_id, ref, value, x, y, angle, props, pins, unit=1,
                         in_bom=True, on_board=True, dnp=False, key=None):
        u = uid("sym", sheet.filename, key or ref)
        sym = self.syms[lib_id]
        out = [f"(symbol (lib_id {q(lib_id)}) (at {fmt(x)} {fmt(y)} {fmt(angle)}) (unit {unit})",
               f"(exclude_from_sim no) (in_bom {'yes' if in_bom else 'no'}) "
               f"(on_board {'yes' if on_board else 'no'}) (dnp {'yes' if dnp else 'no'})",
               f"(uuid {q(u)})"]
        hide_ref = ref.startswith("#")
        bx0, by0, bx1, by1 = sym.bbox
        if hide_ref:
            rx, ry, rj = x, y, ""
            vx = x
            vy = y + (3.81 if lib_id == "power:GND" else -3.81)
            if angle == 180:
                vy = y - (vy - y)
            vj = ""
        elif bx1 - bx0 > 6:          # IC / connector: above and below the body
            rx, ry, rj = x + bx0 + 2.54, y - by1 - 1.27, " (justify left)"
            vx, vy, vj = x + bx0 + 2.54, y - by0 + 1.27, " (justify left)"
        else:                         # two-pin parts: to the right
            rx, ry, rj = x + 2.54, y - 1.27, " (justify left)"
            vx, vy, vj = x + 2.54, y + 1.27, " (justify left)"
        out.append(f'(property "Reference" {q(ref)} (at {fmt(rx)} {fmt(ry)} 0) '
                   f'(effects (font (size 1.27 1.27)){rj}{" (hide yes)" if hide_ref else ""}))')
        out.append(f'(property "Value" {q(value)} (at {fmt(vx)} {fmt(vy)} 0) '
                   f'(effects (font (size 1.27 1.27)){vj}))')
        for k, v in props:
            out.append(f'(property {q(k)} {q(v)} (at {fmt(x)} {fmt(y)} 0) '
                       f'(effects (font (size 1.27 1.27)) (hide yes)))')
        for num in sym.pins:
            out.append(f"(pin {q(num)} (uuid {q(uid('pin', sheet.filename, key or ref, num))}))")
        out.append(f"(instances (project {q(self.name)} (path {q('/' + self.root_uuid + '/' + sheet.uuid)} "
                   f"(reference {q(ref)}) (unit {unit}))))")
        out.append(")")
        return "\n".join(out), u

    def write_sheet(self, sheet, path):
        self.layout(sheet)
        items = []
        used = {}
        for p in sheet.parts:
            sym = p["sym"]
            props = [("Footprint", p["footprint"]), ("Datasheet", ""), ("Description", p["note"]),
                     ("LCSC", p["lcsc"]), ("MPN", p["mpn"])]
            txt, u = self._symbol_instance(sheet, p["lib_id"], p["ref"], p["value"], p["x"], p["y"], 0,
                                           props, p["pins"], in_bom=p["bom"], dnp=p["dnp"])
            p["uuid"] = u
            p["path"] = "/" + sheet.uuid + "/" + u
            items.append(txt)
            used[p["lib_id"]] = sym
            for num, net in p["pins"].items():
                for k, pin in enumerate(sym.pins[num]):
                    px, py = p["x"] + pin["x"], p["y"] - pin["y"]
                    key = f"{p['ref']}.{num}.{k}"
                    if net is None:
                        items.append(f"(no_connect (at {fmt(px)} {fmt(py)}) (uuid {q(uid('nc', sheet.filename, key))}))")
                        continue
                    dx, dy = OUT[int(pin["a"])]
                    ex, ey = px + dx * GRID, py + dy * GRID
                    items.append(f"(wire (pts (xy {fmt(px)} {fmt(py)}) (xy {fmt(ex)} {fmt(ey)})) "
                                 f"(stroke (width 0) (type default)) (uuid {q(uid('w', sheet.filename, key))}))")
                    if net in self.power_nets and dy != 0:
                        psym = self.power_syms[net]
                        used[psym.lib_id] = psym
                        gnd = net == "GND"
                        if gnd:
                            ang = {(0, 1): 0, (1, 0): 90, (0, -1): 180, (-1, 0): 270}[(dx, dy)]
                        else:
                            ang = {(0, -1): 0, (-1, 0): 90, (0, 1): 180, (1, 0): 270}[(dx, dy)]
                        self.pwr_counter += 1
                        ref = "#PWR%03d" % self.pwr_counter
                        txt, _ = self._symbol_instance(sheet, psym.lib_id, ref, net, ex, ey, ang,
                                                       [("Footprint", ""), ("Datasheet", ""),
                                                        ("Description", "")], {"1": net},
                                                       in_bom=False, key="pwr." + key)
                        items.append(txt)
                    else:
                        ang = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[(dx, dy)]
                        just = "left" if ang in (0, 90) else "right"
                        items.append(
                            f"(global_label {q(net)} (shape passive) (at {fmt(ex)} {fmt(ey)} {ang}) "
                            f"(fields_autoplaced yes) (effects (font (size 1.27 1.27)) (justify {just})) "
                            f"(uuid {q(uid('gl', sheet.filename, key))}) "
                            f'(property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {fmt(ex)} {fmt(ey)} 0) '
                            f"(effects (font (size 1.27 1.27)) (justify {just}) (hide yes))))")
        ty = 12.7
        for i, (s, x, y, size) in enumerate(sheet.texts):
            x, y = 15.24, ty
            ty += (s.count("\n") + 1) * 2.8
            items.append(f"(text {q(s)} (exclude_from_sim no) (at {fmt(x)} {fmt(y)} 0) "
                         f"(effects (font (size {size} {size})) (justify left top)) "
                         f"(uuid {q(uid('txt', sheet.filename, i))}))")
        hdr = [f'(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")',
               f"(uuid {q(sheet.uuid)}) (paper {q(sheet.paper)})",
               f"(title_block (title {q(sheet.title)}) (date \"2026-11-24\") (rev \"A\") "
               f"(company \"smart-lock\") (comment 1 \"引脚与 docs/hardware/board-spec.md 第 3 节、firmware/lock/port/efr32/board.h 一一对应\"))",
               "(lib_symbols"]
        for lib_id in sorted(used):
            hdr.append(used[lib_id].body)
        hdr.append(")")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(hdr + items + [")"]) + "\n")

    def write_root(self, path, title, notes=()):
        items = []
        x, y = 25.4, 40.64
        for i, s in enumerate(self.sheets):
            w, h = 60.96, 25.4
            sx = x + (i % 3) * (w + 20.32)
            sy = y + (i // 3) * (h + 20.32)
            items.append(
                f"(sheet (at {fmt(sx)} {fmt(sy)}) (size {fmt(w)} {fmt(h)}) (fields_autoplaced yes) "
                f"(stroke (width 0.1524) (type solid)) (fill (color 0 0 0 0.0000)) (uuid {q(s.uuid)}) "
                f'(property "Sheetname" {q(s.name)} (at {fmt(sx)} {fmt(sy - 0.7)} 0) '
                f"(effects (font (size 1.27 1.27)) (justify left bottom))) "
                f'(property "Sheetfile" {q(s.filename)} (at {fmt(sx)} {fmt(sy + h + 0.6)} 0) '
                f"(effects (font (size 1.27 1.27)) (justify left top))) "
                f"(instances (project {q(self.name)} (path {q('/' + self.root_uuid)} (page {q(str(s.page))})))))")
        for i, n in enumerate(notes):
            items.append(f"(text {q(n)} (exclude_from_sim no) (at 25.4 {fmt(120 + i * 0)} 0) "
                         f"(effects (font (size 1.5 1.5)) (justify left top)) (uuid {q(uid('roottxt', i))}))")
        hdr = [f'(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")',
               f"(uuid {q(self.root_uuid)}) (paper \"A4\")",
               f"(title_block (title {q(title)}) (date \"2026-11-24\") (rev \"A\") (company \"smart-lock\"))",
               "(lib_symbols)"]
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(hdr + items + ['(sheet_instances (path "/" (page "1"))))']) + "\n")

    def write_symlib(self, path):
        out = ["(kicad_symbol_lib (version 20231120) (generator \"smartlock_gen\") (generator_version \"8.0\")"]
        for lib_id, s in sorted(self.syms.items()):
            if lib_id.startswith("smartlock:"):
                name = lib_id.split(":", 1)[1]
                out.append(s.body.replace("(symbol " + q(lib_id), "(symbol " + q(name), 1))
        out.append(")")
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
