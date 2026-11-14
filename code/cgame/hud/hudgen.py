#!/usr/bin/env python3
"""
hudgen.py: makes code/cgame/cg_hud_data.h, the Competitive HUD's built-in
vector data, from the files beside it:

  fonts/Oswald-Medium.ttf, fonts/Oswald-SemiBold.ttf (SIL OFL 1.1, fonts/OFL.txt):
      the outlines of the characters the game's text uses (Windows-1252,
      0x20 to 0xFF) and the faces' metrics. No kerning (the Omaha HUD had none).
  silhouettes/*.png: the weapon, grenade and kill-feed pictures (white with
      alpha, from Project: Omaha), traced once into polygons: the outline of
      alpha 0.5 between the texel centers (marching squares), simplified.

Everything is a path of stb_truetype vertexes (move, line, quadratic curve),
y down, that cgame rasterizes into its atlas at the exact pixel size it draws
it at (cg_hud_atlas.cpp). The header is committed: the build needs neither
this script nor its inputs. Run it again after changing them:

    python3 code/cgame/hud/hudgen.py

Needs Python 3 with Pillow and numpy.
"""

import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "cg_hud_data.h")

# 0x80..0x9F of Windows-1252; None: no character
CP1252_HIGH = [
    0x20AC, None, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, None, 0x017D, None,
    None, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, None, 0x017E, 0x0178,
]

SILHOUETTE_UNITS = 8  # a silhouette's coordinates: 1/8 of its picture's texels
SIMPLIFY = 0.35       # the most a traced outline is moved by simplifying it, texels


def cp1252_to_unicode(byte):
    if byte < 0x80:
        return None if byte == 0x7F else byte
    if byte < 0xA0:
        return CP1252_HIGH[byte - 0x80]
    return byte


# ---------------------------------------------------------------------------
# TrueType
# ---------------------------------------------------------------------------

class Font:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        numTables = struct.unpack(">H", d[4:6])[0]
        self.tables = {}
        for i in range(numTables):
            tag, _, off, length = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
            self.tables[tag.decode("latin-1")] = (off, length)
        head = self.table("head")
        self.unitsPerEm = struct.unpack(">H", head[18:20])[0]
        self.longLoca = struct.unpack(">h", head[50:52])[0] == 1
        hhea = self.table("hhea")
        self.ascent, self.descent, self.lineGap = struct.unpack(">hhh", hhea[4:10])
        self.numHMetrics = struct.unpack(">H", hhea[34:36])[0]
        self.numGlyphs = struct.unpack(">H", self.table("maxp")[4:6])[0]
        os2 = self.table("OS/2")
        version = struct.unpack(">H", os2[0:2])[0]
        if version >= 2:
            self.xHeight, self.capHeight = struct.unpack(">hh", os2[86:90])
        else:
            self.xHeight, self.capHeight = 0, 0
        self.cmap = self.read_cmap()
        loca = self.table("loca")
        if self.longLoca:
            self.loca = struct.unpack(">%dI" % (self.numGlyphs + 1), loca[:4 * (self.numGlyphs + 1)])
        else:
            self.loca = [2 * x for x in struct.unpack(">%dH" % (self.numGlyphs + 1), loca[:2 * (self.numGlyphs + 1)])]
        self.glyf = self.table("glyf")
        hmtx = self.table("hmtx")
        self.advances = []
        for g in range(self.numGlyphs):
            i = min(g, self.numHMetrics - 1)
            self.advances.append(struct.unpack(">H", hmtx[4 * i:4 * i + 2])[0])

    def table(self, tag):
        off, length = self.tables[tag]
        return self.data[off:off + length]

    def read_cmap(self):
        cmap = self.table("cmap")
        n = struct.unpack(">H", cmap[2:4])[0]
        for i in range(n):
            platform, encoding, off = struct.unpack(">HHI", cmap[4 + 8 * i:12 + 8 * i])
            if platform == 3 and encoding == 1 and struct.unpack(">H", cmap[off:off + 2])[0] == 4:
                return self.read_cmap4(cmap[off:])
        raise SystemExit("no Windows Unicode BMP character map (format 4)")

    @staticmethod
    def read_cmap4(sub):
        segX2 = struct.unpack(">H", sub[6:8])[0]
        seg = segX2 // 2
        ends = struct.unpack(">%dH" % seg, sub[14:14 + segX2])
        starts = struct.unpack(">%dH" % seg, sub[16 + segX2:16 + 2 * segX2])
        deltas = struct.unpack(">%dh" % seg, sub[16 + 2 * segX2:16 + 3 * segX2])
        rangeOff = 16 + 3 * segX2
        ranges = struct.unpack(">%dH" % seg, sub[rangeOff:rangeOff + segX2])
        result = {}
        for s in range(seg):
            for c in range(starts[s], ends[s] + 1):
                if c == 0xFFFF:
                    continue
                if ranges[s] == 0:
                    g = (c + deltas[s]) & 0xFFFF
                else:
                    at = rangeOff + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", sub[at:at + 2])[0]
                    if g:
                        g = (g + deltas[s]) & 0xFFFF
                if g:
                    result[c] = g
        return result

    def glyph_contours(self, gid, depth=0):
        """The glyph's contours: lists of (x, y, on curve), font units, y up."""
        if depth > 8:
            raise SystemExit("composite glyphs nested too deep")
        start, end = self.loca[gid], self.loca[gid + 1]
        if start == end:
            return []
        g = self.glyf[start:end]
        numContours = struct.unpack(">h", g[0:2])[0]
        if numContours >= 0:
            return self.simple_contours(g, numContours)
        contours = []
        p = 10
        while True:
            flags, glyph = struct.unpack(">HH", g[p:p + 4])
            p += 4
            if flags & 0x0001:
                a, b = struct.unpack(">hh", g[p:p + 4])
                p += 4
            else:
                a, b = struct.unpack(">bb", g[p:p + 2])
                p += 2
            if not flags & 0x0002:
                raise SystemExit("composite glyph %d places a part by points" % gid)
            m = [1.0, 0.0, 0.0, 1.0]
            if flags & 0x0008:
                s = struct.unpack(">h", g[p:p + 2])[0] / 16384.0
                p += 2
                m = [s, 0.0, 0.0, s]
            elif flags & 0x0040:
                sx, sy = struct.unpack(">hh", g[p:p + 4])
                p += 4
                m = [sx / 16384.0, 0.0, 0.0, sy / 16384.0]
            elif flags & 0x0080:
                v = struct.unpack(">hhhh", g[p:p + 8])
                p += 8
                m = [x / 16384.0 for x in v]
            for c in self.glyph_contours(glyph, depth + 1):
                contours.append([(m[0] * x + m[2] * y + a, m[1] * x + m[3] * y + b, on) for x, y, on in c])
            if not flags & 0x0020:
                break
        return contours

    @staticmethod
    def simple_contours(g, numContours):
        ends = struct.unpack(">%dH" % numContours, g[10:10 + 2 * numContours])
        numPoints = ends[-1] + 1 if numContours else 0
        p = 10 + 2 * numContours
        insLen = struct.unpack(">H", g[p:p + 2])[0]
        p += 2 + insLen
        flags = []
        while len(flags) < numPoints:
            f = g[p]
            p += 1
            flags.append(f)
            if f & 8:
                r = g[p]
                p += 1
                flags.extend([f] * r)
        flags = flags[:numPoints]
        xs, ys = [], []
        for coords, short, same in ((xs, 0x02, 0x10), (ys, 0x04, 0x20)):
            v = 0
            for f in flags:
                if f & short:
                    d = g[p]
                    p += 1
                    v += d if f & same else -d
                elif not f & same:
                    v += struct.unpack(">h", g[p:p + 2])[0]
                    p += 2
                coords.append(v)
        contours = []
        first = 0
        for e in ends:
            contours.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(first, e + 1)])
            first = e + 1
        return contours


def contour_to_path(pts):
    """A TrueType contour as move/line/curve commands, closed."""
    def mid(a, b):
        return ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0, True)

    if len(pts) < 2:
        return []
    if pts[0][2]:
        start = pts[0]
        seq = pts[1:] + [pts[0]]
    elif pts[-1][2]:
        start = pts[-1]
        seq = list(pts)
    else:
        start = mid(pts[-1], pts[0])
        seq = list(pts) + [start]
    cmds = [("M", start[0], start[1])]
    ctrl = None
    for p in seq:
        if p[2]:
            if ctrl is not None:
                cmds.append(("Q", p[0], p[1], ctrl[0], ctrl[1]))
                ctrl = None
            else:
                cmds.append(("L", p[0], p[1]))
        else:
            if ctrl is not None:
                m = mid(ctrl, p)
                cmds.append(("Q", m[0], m[1], ctrl[0], ctrl[1]))
            ctrl = p
    if ctrl is not None:
        cmds.append(("Q", start[0], start[1], ctrl[0], ctrl[1]))
    return cmds


# ---------------------------------------------------------------------------
# Tracing
# ---------------------------------------------------------------------------

# the edges of a cell joined by its outline, by its corners inside (top left 8,
# top right 4, bottom right 2, bottom left 1); a saddle by whether its middle is
# in: T top, R right, B bottom, L left
SEGMENTS = {
    1: [("L", "B")], 2: [("B", "R")], 3: [("L", "R")], 4: [("T", "R")],
    6: [("T", "B")], 7: [("T", "L")], 8: [("T", "L")], 9: [("T", "B")],
    11: [("T", "R")], 12: [("L", "R")], 13: [("B", "R")], 14: [("L", "B")],
}
SADDLES = {
    (5, True): [("T", "L"), ("B", "R")], (5, False): [("T", "R"), ("L", "B")],
    (10, True): [("T", "R"), ("L", "B")], (10, False): [("T", "L"), ("B", "R")],
}
# the corner two adjacent edges cut off
CUT = {frozenset("TL"): "a", frozenset("TR"): "b", frozenset("BR"): "c", frozenset("BL"): "d"}


def trace(alpha):
    """Closed polylines around alpha >= 0.5 (texel centers at x + 0.5, y + 0.5),
    the inside to the left of their way in y-down coordinates; holes go the
    other way round."""
    v = np.pad(alpha.astype(np.float64), 1)
    t = 0.5
    rows, cols = v.shape
    inside = v >= t
    case = (inside[:-1, :-1] * 8 + inside[:-1, 1:] * 4 + inside[1:, 1:] * 2 + inside[1:, :-1] * 1).astype(np.int32)
    segs = {}
    for i, j in zip(*np.nonzero((case > 0) & (case < 15))):
        k = int(case[i, j])
        va, vb, vc, vd = v[i, j], v[i, j + 1], v[i + 1, j + 1], v[i + 1, j]

        def point(edge):
            if edge == "T":
                return ("h", i, j), (j + (t - va) / (vb - va), i)
            if edge == "B":
                return ("h", i + 1, j), (j + (t - vd) / (vc - vd), i + 1)
            if edge == "L":
                return ("v", i, j), (j, i + (t - va) / (vd - va))
            return ("v", i, j + 1), (j + 1, i + (t - vb) / (vc - vb))

        corners = {"a": ((j, i), va >= t), "b": ((j + 1, i), vb >= t), "c": ((j + 1, i + 1), vc >= t),
                   "d": ((j, i + 1), vd >= t)}
        if k in (5, 10):
            pairs = SADDLES[(k, (va + vb + vc + vd) / 4.0 >= t)]
        else:
            pairs = SEGMENTS[k]
        for e1, e2 in pairs:
            k1, p1 = point(e1)
            k2, p2 = point(e2)
            ref = CUT.get(frozenset(e1 + e2), "a")
            (cx, cy), cin = corners[ref]
            cross = (p2[0] - p1[0]) * (cy - p1[1]) - (p2[1] - p1[1]) * (cx - p1[0])
            # y down: cross < 0 puts the corner on the left of p1 -> p2
            if (cross < 0) != cin:
                k1, p1, k2, p2 = k2, p2, k1, p1
            if k1 in segs:
                raise SystemExit("tracing: an edge starts two segments")
            segs[k1] = (k2, p1)
    loops = []
    while segs:
        startKey = next(iter(segs))
        loop = []
        key = startKey
        while True:
            nxt, p = segs.pop(key)
            # texel centers: the padded sample (i, j) is texel (i - 1, j - 1)
            loop.append((p[0] - 0.5, p[1] - 0.5))
            key = nxt
            if key == startKey:
                break
        if len(loop) >= 3:
            loops.append(loop)
    return loops


def simplify(points, tol):
    """Douglas-Peucker on an open polyline (the ends kept)."""
    pts = np.asarray(points)
    keep = np.zeros(len(pts), dtype=bool)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        p, q = pts[a], pts[b]
        d = q - p
        seg = pts[a + 1:b]
        n = np.hypot(d[0], d[1])
        if n < 1e-9:
            dist = np.hypot(seg[:, 0] - p[0], seg[:, 1] - p[1])
        else:
            dist = np.abs(d[0] * (seg[:, 1] - p[1]) - d[1] * (seg[:, 0] - p[0])) / n
        m = int(np.argmax(dist))
        if dist[m] > tol:
            keep[a + 1 + m] = True
            stack.append((a, a + 1 + m))
            stack.append((a + 1 + m, b))
    return [tuple(x) for x in pts[keep]]


def simplify_loop(loop, tol):
    pts = np.asarray(loop)
    far = int(np.argmax(np.hypot(pts[:, 0] - pts[0, 0], pts[:, 1] - pts[0, 1])))
    if far == 0:
        return []
    first = simplify(loop[:far + 1], tol)
    second = simplify(loop[far:] + [loop[0]], tol)
    out = first[:-1] + second[:-1]
    return out if len(out) >= 3 else []


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def emit_vertexes(out, name, verts):
    out.append("static const stbtt_vertex %s[%d] = {" % (name, max(1, len(verts))))
    line = []
    for x, y, cx, cy, kind in verts:
        line.append("{%d,%d,%d,%d,%d,0}" % (x, y, cx, cy, kind))
        if len(line) == 8:
            out.append("\t" + ",".join(line) + ",")
            line = []
    if line:
        out.append("\t" + ",".join(line) + ",")
    if not verts:
        out.append("\t{0,0,0,0,0,0},")
    out.append("};")


def face(out, ident, path, label):
    f = Font(path)
    glyphs = []
    verts = []
    for byte in range(0x20, 0x100):
        u = cp1252_to_unicode(byte)
        gid = f.cmap.get(u) if u is not None else None
        if gid is None:
            glyphs.append(None)
            continue
        first = len(verts)
        xs, ys = [], []
        for contour in f.glyph_contours(gid):
            for cmd in contour_to_path(contour):
                if cmd[0] == "M":
                    verts.append((round(cmd[1]), -round(cmd[2]), 0, 0, 1))
                elif cmd[0] == "L":
                    verts.append((round(cmd[1]), -round(cmd[2]), 0, 0, 2))
                else:
                    verts.append((round(cmd[1]), -round(cmd[2]), round(cmd[3]), -round(cmd[4]), 3))
                    xs.append(round(cmd[3]))
                    ys.append(-round(cmd[4]))
                xs.append(round(cmd[1]))
                ys.append(-round(cmd[2]))
        bbox = (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0)
        glyphs.append((f.advances[gid], bbox, first, len(verts) - first))
    out.append("")
    out.append("// %s: %d glyphs, %d vertexes" % (label, sum(1 for g in glyphs if g), len(verts)))
    emit_vertexes(out, "hud_%s_vertexes" % ident, verts)
    out.append("static const hudGlyphDef_t hud_%s_glyphs[224] = {" % ident)
    for i, g in enumerate(glyphs):
        if g is None:
            out.append("\t{0, 0, 0, 0, 0, 0, 0}, // 0x%02X: none" % (0x20 + i))
        else:
            adv, (x0, y0, x1, y1), first, count = g
            out.append("\t{%d, %d, %d, %d, %d, %d, %d}, // 0x%02X" % (adv, x0, y0, x1, y1, first, count, 0x20 + i))
    out.append("};")
    out.append(
        "static const hudFontDef_t hud_%s = {%d, %d, %d, %d, %d, %d, hud_%s_glyphs, hud_%s_vertexes};"
        % (ident, f.unitsPerEm, f.ascent, f.descent, f.lineGap, f.capHeight, f.xHeight, ident, ident)
    )


def silhouettes(out):
    names = sorted(n for n in os.listdir(os.path.join(HERE, "silhouettes")) if n.endswith(".png"))
    verts = []
    defs = []
    for n in names:
        im = Image.open(os.path.join(HERE, "silhouettes", n)).convert("RGBA")
        alpha = np.asarray(im)[:, :, 3] / 255.0
        first = len(verts)
        for loop in trace(alpha):
            pts = simplify_loop(loop, SIMPLIFY)
            if not pts:
                continue
            for k, (x, y) in enumerate(pts):
                verts.append((round(x * SILHOUETTE_UNITS), round(y * SILHOUETTE_UNITS), 0, 0, 1 if k == 0 else 2))
        defs.append((os.path.splitext(n)[0], im.width, im.height, first, len(verts) - first))
    out.append("")
    out.append("// the silhouettes: %d pictures, %d vertexes, in 1/%d texels" % (len(defs), len(verts), SILHOUETTE_UNITS))
    emit_vertexes(out, "hud_silhouette_vertexes", verts)
    out.append("#define HUD_SILHOUETTE_UNITS %d" % SILHOUETTE_UNITS)
    out.append("#define HUD_NUM_SILHOUETTES %d" % len(defs))
    out.append("static const hudSilhouetteDef_t hud_silhouettes[HUD_NUM_SILHOUETTES] = {")
    for name, w, h, first, count in defs:
        out.append('\t{"%s", %d, %d, %d, %d},' % (name, w, h, first, count))
    out.append("};")


def main():
    out = [
        "// Generated by code/cgame/hud/hudgen.py from the files beside it. Don't edit:",
        "// run the script again.",
        "//",
        "// The Competitive HUD's text faces (Oswald Medium and SemiBold, Copyright 2016",
        "// The Oswald Project Authors, SIL Open Font License 1.1: code/cgame/hud/fonts/OFL.txt),",
        "// cut down to the Windows-1252 characters, and its silhouettes (Project: Omaha),",
        "// as stb_truetype paths, y down. Include stb_truetype.h first.",
        "",
        "#pragma once",
        "",
        "typedef struct {",
        "\tshort advance;",
        "\tshort x0, y0, x1, y1; // the outline's bounds, font units, y down from the baseline",
        "\tint   first, count;    // its vertexes; count 0: no outline (a space, or no character)",
        "} hudGlyphDef_t;",
        "",
        "typedef struct {",
        "\tint unitsPerEm;",
        "\tint ascent, descent, lineGap; // hhea, y up",
        "\tint capHeight, xHeight;       // OS/2",
        "\tconst hudGlyphDef_t *glyphs;  // characters 0x20 to 0xFF",
        "\tconst stbtt_vertex  *vertexes;",
        "} hudFontDef_t;",
        "",
        "typedef struct {",
        "\tconst char *name;  // its picture's name in code/cgame/hud/silhouettes",
        "\tint width, height; // that picture's size",
        "\tint first, count;",
        "} hudSilhouetteDef_t;",
    ]
    face(out, "oswald_medium", os.path.join(HERE, "fonts", "Oswald-Medium.ttf"), "Oswald Medium")
    face(out, "oswald_semibold", os.path.join(HERE, "fonts", "Oswald-SemiBold.ttf"), "Oswald SemiBold")
    silhouettes(out)
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s (%d bytes)" % (os.path.normpath(OUT), os.path.getsize(OUT)))


if __name__ == "__main__":
    main()
