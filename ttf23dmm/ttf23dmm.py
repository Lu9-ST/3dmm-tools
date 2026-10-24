#!/usr/bin/env python3
"""TrueType/OpenType font -> 3D Movie Maker 3D font (.vxp).

Every glyph for character codes 32..255 becomes one extruded BMDL; the codes -> characters through a
code page (cp1252 by default, like all of 3dmm's fonts).
Format reference: 3DMMEx src/engine/tdf.cpp, modl.cpp, and dumps of 3dmm tdfs.3cn / actresl.3th.
"""

import argparse
import math
import os
import struct
import sys
import tempfile
import zipfile
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(1, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from actorbuild import GOKD_CONST, enc_mbmp
from chunky import Chunk, Chunky

BO, OSK = 1, 0x0303
FIRST, LAST = 32, 255


def fx(v):
    return int(round(v * 65536.0))


# ------------------------------------------------------------------------------------------ outlines
def _flatten_pts(p0, pts, verb, tol):
    """pts after p0 -> list of points (excluding p0)."""
    out = []
    if verb == "lineTo":
        return [pts[0]]
    if verb == "qCurveTo":
        from fontTools.pens.basePen import decomposeQuadraticSegment

        cur = p0
        if (
            pts[-1] is None
        ):  # closed all-off-curve contour (not produced by pathops, kept for safety)
            return []
        for c, e in decomposeQuadraticSegment(pts):
            dev = math.hypot(cur[0] - 2 * c[0] + e[0], cur[1] - 2 * c[1] + e[1]) / 4.0
            n = max(1, min(48, int(math.ceil(math.sqrt(dev / tol)))))
            for i in range(1, n + 1):
                t = i / n
                a = (1 - t) ** 2
                b = 2 * (1 - t) * t
                d = t * t
                out.append(
                    (a * cur[0] + b * c[0] + d * e[0], a * cur[1] + b * c[1] + d * e[1])
                )
            cur = e
        return out
    if verb == "curveTo":
        c1, c2, e = pts
        dev = (
            max(
                math.hypot(p0[0] - 2 * c1[0] + c2[0], p0[1] - 2 * c1[1] + c2[1]),
                math.hypot(c1[0] - 2 * c2[0] + e[0], c1[1] - 2 * c2[1] + e[1]),
            )
            * 0.75
            / 8.0
            * 2.0
        )
        n = max(1, min(64, int(math.ceil(math.sqrt(dev / tol)))))
        for i in range(1, n + 1):
            t = i / n
            a = (1 - t) ** 3
            b = 3 * (1 - t) ** 2 * t
            c = 3 * (1 - t) * t * t
            d = t**3
            out.append(
                (
                    a * p0[0] + b * c1[0] + c * c2[0] + d * e[0],
                    a * p0[1] + b * c1[1] + c * c2[1] + d * e[1],
                )
            )
        return out
    return []


def glyph_rings(font, gname, tol_units):
    """Outline of a glyph as simple, non-overlapping closed rings in font units (list of Nx2 arrays)."""
    import pathops

    gs = font.getGlyphSet()
    path = pathops.Path()
    pen = path.getPen(glyphSet=gs)
    gs[gname].draw(pen)
    if not list(path.segments):
        return []
    path.simplify(fix_winding=True, keep_starting_points=False)
    rings, cur, start = [], None, None
    for verb, pts in path.segments:
        if verb == "moveTo":
            cur = [pts[0]]
        elif verb == "closePath" or verb == "endPath":
            if cur and len(cur) >= 3:
                rings.append(cur)
            cur = None
        elif cur is not None:
            cur += _flatten_pts(cur[-1], pts, verb, tol_units)
    out = []
    for r in rings:
        a = np.array(r, float)
        keep = np.ones(len(a), bool)
        keep[1:] = np.hypot(*(a[1:] - a[:-1]).T) > 1e-9
        a = a[keep]
        if len(a) > 1 and np.hypot(*(a[0] - a[-1])) < 1e-9:
            a = a[:-1]
        if len(a) >= 3:
            out.append(a)
    return out


def rings_to_polygons(rings, simplify_tol):
    """Rings -> shapely polygons (shell CCW, holes CW) by nesting depth (even = shell, odd = hole)."""
    from shapely.geometry import Polygon
    from shapely.geometry.polygon import orient

    polys = []
    for r in rings:
        p = Polygon(r)
        if simplify_tol > 0:
            ps = p.simplify(simplify_tol, preserve_topology=True)
            if not ps.is_empty and ps.geom_type == "Polygon":
                p = ps
        if p.area > 1e-10 and p.geom_type == "Polygon":
            polys.append(p)
    polys.sort(key=lambda p: -p.area)
    depth, parent = [], []
    for i, p in enumerate(polys):
        par, d = -1, 0
        for j in range(i - 1, -1, -1):
            if polys[j].contains(p.representative_point()) and depth[j] >= d:
                if par < 0 or polys[j].area < polys[par].area:
                    par = j
        depth.append(depth[par] + 1 if par >= 0 else 0)
        parent.append(par)
    shells = {}
    result = []
    for i, p in enumerate(polys):
        if depth[i] % 2 == 0:
            shells[i] = [list(p.exterior.coords)[:-1], []]
            result.append(shells[i])
    for i, p in enumerate(polys):
        if depth[i] % 2 == 1 and parent[i] in shells:
            shells[parent[i]][1].append(list(p.exterior.coords)[:-1])
    out = []
    for shell, holes in result:
        poly = orient(Polygon(shell, holes), 1.0)
        out.append(
            [np.array(poly.exterior.coords)[:-1]]
            + [np.array(h.coords)[:-1] for h in poly.interiors]
        )
    return out  # each: [shell CCW, hole CW, ...]


# ------------------------------------------------------------------------------------------ mesh
def _inset(ring, off):
    """Move a ring inwards (solid on the left of travel) by off, mitre joins (clamped)."""
    n = len(ring)
    if off == 0:
        return ring.copy()
    prev = ring - np.roll(ring, 1, axis=0)
    nxt = np.roll(ring, -1, axis=0) - ring

    def nrm(e):
        l = np.hypot(e[:, 0], e[:, 1])
        l[l == 0] = 1
        return np.stack([-e[:, 1] / l, e[:, 0] / l], 1)

    n1, n2 = nrm(prev), nrm(nxt)
    s = n1 + n2
    dot = np.maximum(1 + (n1 * n2).sum(1), 0.25)
    return ring + s / dot[:, None] * off


def _layers_valid(base_rings, off):
    from shapely.geometry import Polygon

    shell = _inset(base_rings[0], off)
    holes = [_inset(h, off) for h in base_rings[1:]]
    p = Polygon(shell, holes)
    orig = Polygon(base_rings[0], base_rings[1:])
    if not p.is_valid or p.is_empty or p.area <= 1e-9 or p.area > orig.area:
        return False

    # a ring must keep its orientation and not collapse (thin strokes)
    def sa(r):
        x, y = r[:, 0], r[:, 1]
        return 0.5 * np.sum(x * np.roll(y, -1) - np.roll(x, -1) * y)

    if sa(shell) <= 0:
        return False
    return all(sa(h) < 0 for h in holes)


def build_glyph_mesh(polys, depth, bevel, steps, smooth, crease_deg):
    """polys: list of [shell, hole...] rings (glyph units).  Returns (positions Nx3, normals Nx3, faces Mx3) deduped
    per (position, normal)."""
    import mapbox_earcut as earcut

    h = depth / 2.0
    tris = []  # (3x3 array) triangles, CCW from outside
    b = min(bevel, h * 0.98) if bevel > 0 else 0.0

    # profile: (inset offset, z) from the straight wall to the cap
    def profile(bb):
        if bb <= 0:
            return [(0.0, h)]
        if steps <= 1:
            return [(0.0, h - bb), (bb, h)]
        return [
            (
                bb * (1 - math.cos(math.pi / 2 * j / steps)),
                (h - bb) + bb * math.sin(math.pi / 2 * j / steps),
            )
            for j in range(steps + 1)
        ]

    for rings in polys:
        bb = b
        while bb > 0 and not all(_layers_valid(rings, o) for o, _ in profile(bb)):
            bb *= 0.5
            if bb < 0.01:
                bb = 0.0
        prof = profile(bb)
        layers = [[_inset(r, o) for r in rings] for o, _ in prof]
        zs = [z for _, z in prof]
        # cap triangulation on the innermost layer
        inner = layers[-1]
        verts = np.concatenate(inner)
        ends = np.cumsum([len(r) for r in inner]).astype(np.uint32)
        idx = earcut.triangulate_float64(verts, ends).reshape(-1, 3)
        for t in idx:
            p = verts[t]
            ar = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (
                p[1][1] - p[0][1]
            )
            if abs(ar) < 1e-14:
                continue
            tt = t if ar > 0 else t[::-1]
            tris.append(
                np.array([[verts[k][0], verts[k][1], zs[-1]] for k in tt])
            )  # front
            tris.append(
                np.array([[verts[k][0], verts[k][1], -zs[-1]] for k in tt[::-1]])
            )  # back

        # walls
        def wall(ra, za, rb, zb):  # ra at z za (lower), rb at z zb, same vertex counts
            n = len(ra)
            for i in range(n):
                j = (i + 1) % n
                a0 = (ra[i][0], ra[i][1], za)
                b0 = (ra[j][0], ra[j][1], za)
                a1 = (rb[i][0], rb[i][1], zb)
                b1 = (rb[j][0], rb[j][1], zb)
                yield np.array([a0, b0, b1])
                yield np.array([a0, b1, a1])

        for ri in range(len(rings)):
            if 2 * zs[0] > 1e-6:
                for t in wall(layers[0][ri], -zs[0], layers[0][ri], zs[0]):
                    tris.append(t)
            for k in range(len(prof) - 1):
                for t in wall(
                    layers[k][ri], zs[k], layers[k + 1][ri], zs[k + 1]
                ):  # front bevel
                    tris.append(t)
                for t in wall(
                    layers[k][ri], -zs[k], layers[k + 1][ri], -zs[k + 1]
                ):  # back bevel (mirror)
                    tris.append(t[[0, 2, 1]])
    tris = np.array(tris) if tris else np.zeros((0, 3, 3))
    # drop degenerate triangles, face normals
    e1, e2 = tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]
    cr = np.cross(e1, e2)
    area2 = np.linalg.norm(cr, axis=1)
    ok = area2 > 1e-10
    tris, cr, area2 = tris[ok], cr[ok], area2[ok]
    fn = cr / area2[:, None]
    # corner normals
    corner = np.repeat(fn[:, None, :], 3, axis=1)
    if smooth:
        cosc = math.cos(math.radians(crease_deg))
        key = {}
        for f in range(len(tris)):
            for c in range(3):
                key.setdefault(
                    tuple(np.round(tris[f, c] * 1e4).astype(np.int64)), []
                ).append((f, c))
        for lst in key.values():
            if len(lst) < 2:
                continue
            for f, c in lst:
                acc = np.zeros(3)
                for g, cg in lst:
                    if np.dot(fn[f], fn[g]) >= cosc:
                        u = tris[g, (cg + 1) % 3] - tris[g, cg]
                        v = tris[g, (cg + 2) % 3] - tris[g, cg]
                        ang = math.acos(
                            max(
                                -1,
                                min(
                                    1,
                                    np.dot(u, v)
                                    / (np.linalg.norm(u) * np.linalg.norm(v) + 1e-30),
                                ),
                            )
                        )
                        acc += fn[g] * ang
                nn = np.linalg.norm(acc)
                if nn > 1e-12:
                    corner[f, c] = acc / nn
    # dedupe (position, normal) as the default models do
    vmap, P, N, F = {}, [], [], []
    for f in range(len(tris)):
        ids = []
        for c in range(3):
            pk = tuple(np.round(tris[f, c] * 65536).astype(np.int64))
            nk = tuple(np.round(corner[f, c] * 32767).astype(np.int64))
            k = (pk, nk)
            if k not in vmap:
                vmap[k] = len(P)
                P.append(tris[f, c])
                N.append(corner[f, c])
            ids.append(vmap[k])
        F.append(ids)
    return (
        np.array(P).reshape(-1, 3),
        np.array(N).reshape(-1, 3),
        np.array(F, int).reshape(-1, 3),
    )


# ------------------------------------------------------------------------------------------ BMDL
def enc_bmdl_prepared(P, N, F, bounds):
    """P,N: Nx3 (final model coordinates); F: Mx3.  bounds = (min3, max3)."""
    if len(P) == 0 or len(F) == 0:
        return struct.pack("<hhhhi", BO, OSK, 0, 0, 0) + b"\0" * 36
    mn, mx = bounds
    radius = float(np.max(np.linalg.norm(P, axis=1))) + 0.001
    out = struct.pack("<hhhhi", BO, OSK, len(P), len(F), fx(radius))
    out += struct.pack("<6i", *[fx(v) for v in list(mn) + list(mx)])
    out += struct.pack("<3i", *[fx(v) for v in mn])
    xmin, xmax = P[:, 0].min(), P[:, 0].max()
    ymin, ymax = P[:, 1].min(), P[:, 1].max()
    w, hgt = max(xmax - xmin, 1e-6), max(ymax - ymin, 1e-6)
    for p, n in zip(P, N):
        u = (p[0] - xmin) / w
        v = (p[1] - ymin) / hgt
        out += struct.pack("<3i", fx(p[0]), fx(p[1]), fx(p[2]))
        out += struct.pack(
            "<2i",
            int(round(min(max(u, 0), 1) * 65535)),
            int(round(min(max(v, 0), 1) * 65535)),
        )
        out += b"\0" * 6 + struct.pack(
            "<3h", *[int(round(max(-1.0, min(1.0, c)) * 32767)) for c in n]
        )  # index/rgb/pad, normal
    for f in F:
        p0, p1, p2 = P[f[0]], P[f[1]], P[f[2]]
        nn = np.cross(p1 - p0, p2 - p0)
        nn /= np.linalg.norm(nn)
        out += (
            struct.pack("<3H", *[int(i) for i in f]) + b"\0" * 6 + b"\0" * 4
        )  # vertices, edges, material
        out += struct.pack("<HBB", 0, 0, 0)  # smoothing, flags, pad
        out += struct.pack("<3h", *[int(round(c * 32767)) for c in nn]) + b"\0\0"
        out += struct.pack("<i", fx(float(np.dot(nn, p0))))
    return out


# ------------------------------------------------------------------------------------------ font
class FontSource:
    def __init__(self, path, face_index=0, axes=None):
        from fontTools.ttLib import TTCollection, TTFont

        if path.lower().endswith(".ttc"):
            self.font = TTCollection(path).fonts[face_index]
        else:
            self.font = (
                TTFont(path, fontNumber=face_index) if face_index else TTFont(path)
            )
        if "fvar" in self.font and axes is not None:
            from fontTools.varLib import instancer

            self.font = instancer.instantiateVariableFont(
                self.font, axes, inplace=False
            )
        self.path = path
        self.cmap = self.font.getBestCmap() or {}
        self.upem = self.font["head"].unitsPerEm
        self.hmtx = self.font["hmtx"]

    def name(self):
        n = self.font["name"]
        for nid in (4, 1):
            r = n.getBestFamilyName() if nid == 1 else n.getDebugName(4)
            if r:
                return r
        return os.path.splitext(os.path.basename(self.path))[0]

    def cap_height(self):
        os2 = self.font.get("OS/2")
        ch = getattr(os2, "sCapHeight", 0) if os2 else 0
        if ch and ch > 0:
            return ch
        gn = self.cmap.get(ord("H"))
        if gn:
            from fontTools.pens.boundsPen import BoundsPen

            bp = BoundsPen(self.font.getGlyphSet())
            self.font.getGlyphSet()[gn].draw(bp)
            if bp.bounds:
                return bp.bounds[3]
        return self.upem * 0.7


def make_codes(charset):
    """code (32..255) -> unicode character or None"""
    out = {}
    for c in range(FIRST, LAST + 1):
        try:
            ch = bytes([c]).decode(charset)
        except UnicodeDecodeError:
            ch = None
        out[c] = ch if ch and ch != "�" and (ch.isprintable() or ch in "  ­") else None
    return out


def thumbnail_index(font_path, text, face_index=0, size=(136, 16)):
    from PIL import Image, ImageDraw, ImageFont

    W, H = size
    try:
        best = None
        for px in range(30, 5, -1):
            f = ImageFont.truetype(font_path, px, index=face_index)
            l, t, r, b = f.getbbox(text)
            if r - l <= W - 4 and b - t <= H - 2:
                best = (f, l, t, r, b)
                break
        if best is None:
            raise ValueError
        f, l, t, r, b = best
        im = Image.new("L", (W, H), 255)
        ImageDraw.Draw(im).text(
            ((W - (r - l)) // 2 - l, (H - (b - t)) // 2 - t), text, font=f, fill=0
        )
    except Exception:
        from PIL import ImageFont

        f = ImageFont.load_default()
        im = Image.new("L", (W, H), 255)
        ImageDraw.Draw(im).text((2, 2), text[:20], font=f, fill=0)
    a = np.asarray(im, float)
    return (15 + np.round(a / 255.0 * 15)).astype(
        np.int16
    )  # grey ramp, palette indices 15 (ink) .. 30 (paper)


# ------------------------------------------------------------------------------------------ build
def convert(
    font_path,
    out_path,
    name=None,
    author="",
    charset="cp1252",
    cap_height=5.4,
    depth=2.5,
    shading="flat",
    crease=40.0,
    bevel=0.0,
    bevel_steps=1,
    detail=0.03,
    spacing=0.0,
    face_index=0,
    axes=None,
    log=print,
):
    src = FontSource(font_path, face_index, axes)
    font = src.font
    name = name or src.name()
    if len(name) > 60:
        name = name[:60]
    scale = cap_height / src.cap_height()
    tol = detail / scale  # in font units
    codes = make_codes(charset)
    gs = font.getGlyphSet()
    base = 0x20000000 + (zlib.crc32(name.encode("utf8")) & 0x0FFFFF00)
    sc = Chunky()
    kids = []
    dx, dy = [0.0] * 256, [0.0] * 256
    nmiss, glyph_cache, empty = 0, {}, []
    smooth = shading == "smooth"
    total_verts = 0
    for code in range(FIRST, LAST + 1):
        ch = codes[code]
        gname = src.cmap.get(ord(ch)) if ch else None
        if ch and gname is None:
            nmiss += 1
        P = N = F = None
        adv = None
        if gname is not None:
            adv_raw = src.hmtx[gname][0] * scale
            adv = adv_raw + spacing
            if gname in glyph_cache:
                P, N, F = glyph_cache[gname]
            else:
                tt = 1.0
                while True:
                    rings = glyph_rings(font, gname, tol * tt)
                    polys = rings_to_polygons(rings, tol * 0.1 * tt) if rings else []
                    if not polys:
                        break
                    scaled = [[r * scale for r in rs] for rs in polys]
                    P, N, F = build_glyph_mesh(
                        scaled, depth, bevel, bevel_steps, smooth, crease
                    )
                    if len(P) < 60000:
                        break
                    tt *= 2  # far too detailed for 16-bit indices: flatten coarser
                if polys:
                    P = P.copy()
                    P[:, 0] -= (
                        adv_raw / 2.0
                    )  # centre on the advance box (spacing is split to both sides)
                else:
                    P = N = F = None
                glyph_cache[gname] = (P, N, F)
        cno = base + 1 + (code - FIRST)
        if P is not None and len(P):
            ink = (
                P[:, 0].min(),
                P[:, 1].min(),
                P[:, 2].min(),
                P[:, 0].max(),
                P[:, 1].max(),
                P[:, 2].max(),
            )
            half = adv / 2.0
            mn = (-half, ink[1], -depth / 2.0)
            mx = (half, ink[4], depth / 2.0)
            sc.add(Chunk("BMDL", cno, enc_bmdl_prepared(P, N, F, (mn, mx))))
            dx[code] = adv
            dy[code] = (ink[4] - ink[1]) + 0.5
            total_verts += len(P)
        else:
            sc.add(
                Chunk(
                    "BMDL",
                    cno,
                    enc_bmdl_prepared(
                        np.zeros((0, 3)), None, np.zeros((0, 3), int), None
                    ),
                )
            )
            dx[code] = adv if adv is not None else 0.0
            empty.append(code)
        kids.append((code, cno))
    # like the 3dmm source's authoring tool: models without geometry (space ...) get the height of an "i"
    ref = dy[ord("i")] if dy[ord("i")] > 0 else (max(dy) if max(dy) > 0 else 6.0)
    refw = dx[ord("i")] if dx[ord("i")] > 0 else 2.0
    for code in empty:
        dy[code] = ref
        if dx[code] <= 0:
            dx[code] = refw
    dyr_max = max(dy)
    tdf_data = (
        struct.pack("<hhii", BO, OSK, 256, fx(dyr_max))
        + struct.pack("<256i", *[fx(v) for v in dx])
        + struct.pack("<256i", *[fx(v) for v in dy])
    )
    tdf = sc.add(Chunk("TDF ", base, tdf_data, name=name))
    for code, cno in kids:
        tdf.add_kid(code, "BMDL", cno)
    th = Chunky()
    from chunky import ctg

    thumb = thumbnail_index(font_path, name, face_index)
    th.add(Chunk("MBMP", base, enc_mbmp(thumb)))
    g = th.add(Chunk("GOKD", base, GOKD_CONST))
    g.add_kid(65536, "MBMP", base)
    tf = th.add(Chunk("TFTH", base, struct.pack("<hhIi", BO, OSK, ctg("TDF "), base)))
    tf.add_kid(0, "GOKD", base)
    d = tempfile.mkdtemp()
    bn = (
        "".join(c if c.isalnum() else "_" for c in name).strip("_").lower()[:32]
        or "font3d"
    )
    sc.write(os.path.join(d, bn + ".3cn"))
    th.write(os.path.join(d, bn + ".3th"))
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(d, bn + ".3cn"), bn + ".3cn")
        z.write(os.path.join(d, bn + ".3th"), bn + ".3th")
        cfg = (
            ["Name=%s" % name]
            + (["Author=%s" % author] if author else [])
            + ["Content=3D Words", "Generator=ttf23dmm"]
        )
        z.writestr(bn + ".cfg", "\r\n".join(cfg) + "\r\n")
    log(
        "%s: %d glyph models (%d empty), %d vertices, %d chars missing from the font, cap height %.2f, depth %.2f, %s%s"
        % (
            name,
            224 - len(empty),
            len(empty),
            total_verts,
            nmiss,
            cap_height,
            depth,
            shading,
            (", bevel %.2f x%d" % (bevel, bevel_steps)) if bevel > 0 else "",
        )
    )
    return dict(
        name=name,
        base=base,
        cno=base,
        dx=dx,
        dy=dy,
        scene=sc,
        glyphs=glyph_cache,
        codes=codes,
        src=src,
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("font")
    ap.add_argument("-o", "--output")
    ap.add_argument("--name")
    ap.add_argument("--author", default="")
    ap.add_argument(
        "--charset",
        default="cp1252",
        help="Python codec that maps codes 32..255 to characters",
    )
    ap.add_argument(
        "--cap-height",
        type=float,
        default=5.4,
        help="cap height in 3DMM units (default fonts: 5.4)",
    )
    ap.add_argument(
        "--depth", type=float, default=2.5, help="extrusion depth (default: 2.5)"
    )
    ap.add_argument("--shading", choices=["flat", "smooth"], default="flat")
    ap.add_argument(
        "--crease",
        type=float,
        default=40.0,
        help="smooth shading: edges sharper than this stay hard (degrees)",
    )
    ap.add_argument("--bevel", type=float, default=0.0, help="bevel size (0 = none)")
    ap.add_argument(
        "--bevel-steps", type=int, default=1, help="1 = chamfer, more = rounded"
    )
    ap.add_argument(
        "--detail",
        type=float,
        default=0.03,
        help="curve flattening tolerance (3DMM units)",
    )
    ap.add_argument(
        "--spacing",
        type=float,
        default=0.0,
        help="extra advance added to every character",
    )
    ap.add_argument("--face-index", type=int, default=0)
    ap.add_argument(
        "--axis",
        action="append",
        default=[],
        help="variable fonts: TAG=VALUE (e.g. wght=700)",
    )
    a = ap.parse_args(argv)
    axes = {k: float(v) for k, v in (s.split("=") for s in a.axis)} if a.axis else None
    out = a.output or os.path.splitext(os.path.basename(a.font))[0] + ".vxp"
    convert(
        a.font,
        out,
        a.name,
        a.author,
        a.charset,
        a.cap_height,
        a.depth,
        a.shading,
        a.crease,
        a.bevel,
        a.bevel_steps,
        a.detail,
        a.spacing,
        a.face_index,
        axes,
    )
    print("wrote", out)


if __name__ == "__main__":
    main()
