"""Encoders for 3DMM background chunks (BKGD/CAM/MBMP/ZBMP/GLCR/GLLT/BDS). Python + numpy."""

import math
import struct

import numpy as np

from .chunky import Chunk, Chunky, ctg

BO, OSK = 1, 0x0303
W, H = 544, 306
ASPECT = W / H


def fx(v):
    return int(round(v * 65536.0))


def enc_bkgd(bIndexBase=10):
    return struct.pack("<hhBBBB", BO, OSK, bIndexBase, 0, 0, 0)


def _glhdr(n, cbEntry):
    return struct.pack("<hhii", BO, OSK, cbEntry, n)


def enc_pal(rgb):
    """rgb: (n,3) uint8 -> GLCR. Entry i is palette index 10+i. Stored B,G,R,0."""
    rgb = np.asarray(rgb, dtype=np.uint8)
    n = len(rgb)
    ent = np.zeros((n, 4), np.uint8)
    ent[:, 0] = rgb[:, 2]
    ent[:, 1] = rgb[:, 1]
    ent[:, 2] = rgb[:, 0]
    return _glhdr(n, 4) + ent.tobytes()


def enc_cam(M, fov_deg, hither=1.0, yon=5000.0, placements=()):
    """M: 4x3 BRender matrix (rows R0 right, R1 up, R2 back, row3 = position).
    placements: first -> CAM's APOS, others appended (12-byte APOS each)."""
    pl = list(placements) or [(0.0, 0.0, 0.0)]
    d = struct.pack(
        "<hhiiHh",
        BO,
        OSK,
        fx(hither),
        fx(yon),
        int(round(fov_deg / 360.0 * 65536)) & 0xFFFF,
        0,
    )
    d += struct.pack("<3i", *[fx(v) for v in pl[0]])
    d += struct.pack("<12i", *[fx(v) for v in np.asarray(M).reshape(12)])
    for p in pl[1:]:
        d += struct.pack("<3i", *[fx(v) for v in p])
    return d


def enc_zbmp(z):
    z = np.ascontiguousarray(z, dtype="<u2")
    h, w = z.shape
    return struct.pack("<hhhhhh", BO, OSK, 0, 0, w, h) + z.tobytes()


def enc_mbmp(img, left=None, top=None, transparent=-1):
    """img: (h,w) int array of palette indices; transparent value = -1. rc default (-w,-h,0,0) as stock."""
    h, w = img.shape
    if left is None:
        left, top = -w, -h
    rows = []
    for y in range(h):
        row = img[y]
        out = bytearray()
        x = 0
        opaque = np.nonzero(row >= 0)[0]
        if len(opaque) == 0:
            rows.append(b"")
            continue
        last = opaque[-1] + 1
        while x < last:
            skip = 0
            while x < last and row[x] < 0 and skip < 255:
                x += 1
                skip += 1
            if x >= last:
                break  # can't happen (row never ends transparent)
            n = 0
            s = x
            while x < last and row[x] >= 0 and n < 255:
                x += 1
                n += 1
            out += bytes([skip, n]) + bytes(row[s : s + n].astype(np.uint8).tolist())
        rows.append(bytes(out))
    body = struct.pack("<%dh" % h, *[len(r) for r in rows]) + b"".join(rows)
    cb = 28 + len(body)  # stock: cb is the whole chunk size
    return (
        struct.pack("<hhBBh", BO, OSK, 0, 0, 0)
        + struct.pack("<4i", left, top, left + w, top + h)
        + struct.pack("<i", cb)
        + body
    )


def enc_lights(lights):
    """lights: list of (3x3 rotation rows, intensity, type=1 directional). -> GLLT."""
    ent = b""
    for R, inten, typ in lights:
        M = np.zeros((4, 3))
        M[:3] = R
        ent += struct.pack("<12i", *[fx(v) for v in M.reshape(12)]) + struct.pack(
            "<ii", fx(inten), typ
        )
    return _glhdr(len(lights), 56) + ent


def enc_bds(sid, ctg_, cno, vlm=65536, loop=1):
    c = ctg(ctg_) if isinstance(ctg_, str) else ctg_
    return struct.pack("<hhii", BO, OSK, vlm, loop) + struct.pack(
        "<iiII", sid, 0, c, cno
    )


# ---- projection helpers -------------------------------------------------
def depth_to_z16(d, hither=1.0, yon=5000.0):
    """d: distance along view axis (>0). inf/None -> 65535."""
    A = (yon + hither) / (yon - hither)
    B = 2 * yon * hither / (yon - hither)
    with np.errstate(divide="ignore", invalid="ignore"):
        z = 32767.0 * (A - B / d)
    z = np.where(np.isfinite(d) & (d > 0), z, 65535.0)
    return np.clip(np.rint(z), 0, 65535).astype("<u2")
