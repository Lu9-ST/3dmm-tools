"""Tiny software renderer (z-buffer, Gouraud, one light) to look at 3D font glyphs.  Works on anything made of
(positions, normals, faces) per glyph, so the stock fonts and converted fonts can be compared side by side."""
import struct
import numpy as np


def read_bmdl(raw):
    """prepared/unprepared BMDL bytes -> (P, N, F) (N None for unprepared)"""
    bo, osk, nv, nf, rad = struct.unpack('<hhHHi', raw[:12])
    if nv == 0:
        return np.zeros((0, 3)), np.zeros((0, 3)), np.zeros((0, 3), int)
    P, N = [], []
    for k in range(nv):
        e = raw[48 + k * 32:80 + k * 32]
        P.append([x / 65536.0 for x in struct.unpack('<3i', e[:12])])
        N.append([x / 32767.0 for x in struct.unpack('<3h', e[26:32])])
    F = [struct.unpack('<3H', raw[48 + nv * 32 + k * 32:48 + nv * 32 + k * 32 + 6]) for k in range(nf)]
    return np.array(P), np.array(N), np.array(F)


def render_text(glyph_of, text, dx_of, size=(900, 220), yaw=-22.0, pitch=14.0, scale=None, bg=(24, 26, 32),
                color=(235, 180, 60)):
    """glyph_of(ch) -> (P,N,F) or None; dx_of(ch) -> advance.  Returns an RGB uint8 image."""
    W, H = size
    xs, items = 0.0, []
    for ch in text:
        adv = dx_of(ch)
        g = glyph_of(ch)
        if g is not None and len(g[0]):
            items.append((g, xs + adv / 2.0))
        xs += adv
    total = xs
    cy, cp = np.cos(np.radians(yaw)), np.sin(np.radians(yaw))
    cx_, sx_ = np.cos(np.radians(pitch)), np.sin(np.radians(pitch))
    Ry = np.array([[cy, 0, cp], [0, 1, 0], [-cp, 0, cy]])
    Rx = np.array([[1, 0, 0], [0, cx_, -sx_], [0, sx_, cx_]])
    R = Rx @ Ry
    if scale is None:
        scale = (W * 0.92) / max(total, 1e-6) / 1.05
    light = np.array([-0.4, 0.6, 0.7]); light /= np.linalg.norm(light)
    img = np.zeros((H, W, 3), float); img[:] = bg
    zb = np.full((H, W), -1e9)
    yoff = H * 0.62
    for (P, N, F), cxm in items:
        Pw = P.copy(); Pw[:, 0] += cxm - total / 2.0
        Pc = Pw @ R.T
        Nc = N @ R.T
        sxp = Pc[:, 0] * scale + W / 2.0
        syp = yoff - Pc[:, 1] * scale
        zz = Pc[:, 2]
        shade = np.clip(Nc @ light, 0, 1) * 0.8 + 0.2
        for f in F:
            a, b, c = f
            xa, xb, xc = sxp[a], sxp[b], sxp[c]; ya, yb, yc = syp[a], syp[b], syp[c]
            area = (xb - xa) * (yc - ya) - (xc - xa) * (yb - ya)
            if area >= 0:        # back face (y flipped screen space: front faces are negative)
                continue
            x0 = int(max(0, np.floor(min(xa, xb, xc)))); x1 = int(min(W - 1, np.ceil(max(xa, xb, xc))))
            y0 = int(max(0, np.floor(min(ya, yb, yc)))); y1 = int(min(H - 1, np.ceil(max(ya, yb, yc))))
            if x1 < x0 or y1 < y0:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            w0 = ((xb - gx) * (yc - gy) - (xc - gx) * (yb - gy)) / area
            w1 = ((xc - gx) * (ya - gy) - (xa - gx) * (yc - gy)) / area
            w2 = 1 - w0 - w1
            m = (w0 >= -1e-9) & (w1 >= -1e-9) & (w2 >= -1e-9)
            if not m.any():
                continue
            z = w0 * zz[a] + w1 * zz[b] + w2 * zz[c]
            sh = w0 * shade[a] + w1 * shade[b] + w2 * shade[c]
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            upd = m & (z > sub)
            sub[upd] = z[upd]
            for ch in range(3):
                img[y0:y1 + 1, x0:x1 + 1, ch][upd] = (color[ch] * sh)[upd]
    return np.clip(img, 0, 255).astype(np.uint8)
