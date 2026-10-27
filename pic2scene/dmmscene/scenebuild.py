"""Assemble a 3DMM scene (.3cn + .3th) and VXP from camera data."""

import struct
import zipfile

import numpy as np

from . import bkgdwrite as B
from .chunky import Chunk, Chunky, ctg

GOKD_CONST = bytes.fromhex(
    "01000303000000000000000000000000000000000a0000000000000000000000"
    "ffffffffffffffffffffffff61c30000ffffffff"
)
assert len(GOKD_CONST) == 52
MUSIC_NONE = None
MUSIC_IMAGINOPOLIS = (2, "MSND", 20769)  # sid 2 = stock product content


def shared_palette(bkgds_path):
    """The 146 colours (palette idx 10..155) every stock background shares."""
    c = Chunky.read(bkgds_path)
    g = c.get("GLCR", sorted(k[1] for k in c.chunks if k[0] == ctg("GLCR"))[0]).data
    n = struct.unpack_from("<i", g, 8)[0]
    e = np.frombuffer(g[12 : 12 + 4 * n], np.uint8).reshape(n, 4)
    return e[:146, [2, 1, 0]].copy()


def thumb_image(idx, shared_n=146):
    """Downscale an index image (h,w) to 128x72 using only indices 10..155 (nearest-colour by caller)."""
    h, w = idx.shape
    ys = np.arange(72) * h // 72
    xs = np.arange(128) * w // 128
    return idx[np.ix_(ys, xs)]


_song_cache = {}
last_song_notes = []


def _find_ci(directory, name):
    """Case-insensitive file lookup (3DMM installs use upper/lower case freely)."""
    import os

    try:
        for f in os.listdir(directory):
            if f.lower() == name.lower():
                return os.path.join(directory, f)
    except OSError:
        pass
    return None


def _find_ci_path(base, *parts):
    import os

    cur = base
    for i, p in enumerate(parts):
        nxt = _find_ci(cur, p)
        if nxt is None:
            return None
        cur = nxt
    return cur


def song_sources(snds_path):
    """[(label, sid, path)] for the product and the two official expansions (if installed)."""
    import os

    out = [("3D Movie Maker", 2, snds_path)]
    kids = os.path.dirname(os.path.dirname(os.path.abspath(snds_path)))
    for label, sid, parts in (
        ("Nickelodeon", 3, ("N3DMM EXPANSION", "NICKSNDS.3CN")),
        ("Doraemon", 4, ("DORAEMON", "DORASNDS.3CN")),
    ):
        p = _find_ci_path(kids, *parts)
        if p:
            out.append((label, sid, p))
    return out


def stock_songs(snds_path):
    """{display name: (sid, cno)} of the MIDI songs in snds.3cn and the official expansions' sound files.
    Names that exist in more than one source get the source in brackets. Cached by path+mtime."""
    import os

    srcs = [x for x in song_sources(snds_path) if os.path.isfile(x[2])]
    key = tuple((p, os.path.getmtime(p)) for _l, _s, p in srcs)
    if key not in _song_cache:
        out = {}
        notes = []
        for label, sid, path in srcs:
            c = Chunky.read(path)
            n = 0
            for (ct, cno), ch in sorted(c.chunks.items()):
                if (
                    ct == ctg("MSND")
                    and len(ch.data) >= 8
                    and struct.unpack_from("<i", ch.data, 4)[0] == 4
                    and ch.name
                ):
                    nm = ch.name if ch.name not in out else "%s (%s)" % (ch.name, label)
                    out[nm] = (sid, cno)
                    n += 1
            notes.append("%d from %s" % (n, label))
        _song_cache.clear()
        _song_cache[key] = out
        last_song_notes[:] = notes
    return _song_cache[key]


def build(
    name,
    cams,
    palette236,
    cno_base,
    music=MUSIC_IMAGINOPOLIS,
    lights=None,
    thumbs=None,
    midi=None,
):
    """midi: optional (mids_bytes, song_name): stored in this file as MSND+MIDS (source 5) and used as the music."""
    """cams: list of dict(M, fov_deg, idx, z, placements). thumbs: list of (72,128) index images.
    Returns (scene Chunky, thumb Chunky)."""
    sc = Chunky("3DMM")
    th = Chunky("3DMM")
    n = len(cams)
    bk = sc.add(Chunk("BKGD", cno_base, B.enc_bkgd(), name=name))
    for ct in ("BDS ", "GLCR", "GLLT"):
        pass
    if midi:
        mids, sname = midi
        sc.add(
            Chunk(
                "MSND",
                cno_base,
                struct.pack("<hhiii", 1, 0x0303, 4, 65536, 0),
                name=sname[:60],
            )
        )
        sc.add(Chunk("MIDS", cno_base, mids))
        sc.get("MSND", cno_base).add_kid(0, "MIDS", cno_base)
        music = (5, "MSND", cno_base)
    if music:
        sc.add(Chunk("BDS ", cno_base, B.enc_bds(*music)))
        bk.add_kid(0, "BDS ", cno_base)
    else:
        sc.add(Chunk("BDS ", cno_base, B.enc_bds(0, "MSND", 0, vlm=0, loop=0)))
        bk.add_kid(0, "BDS ", cno_base)
    sc.add(Chunk("GLCR", cno_base, B.enc_pal(palette236)))
    bk.add_kid(0, "GLCR", cno_base)
    if lights is None:
        lights = [(np.array([[1, 0, 0], [0, 0.8, -0.6], [0, 0.6, 0.8]]), 1.0, 1)]
    sc.add(Chunk("GLLT", cno_base, B.enc_lights(lights)))
    bk.add_kid(0, "GLLT", cno_base)
    for i, c in enumerate(cams):
        cn = cno_base + i
        cam = sc.add(
            Chunk(
                "CAM ",
                cn,
                B.enc_cam(c["M"], c["fov_deg"], placements=c.get("placements", ())),
            )
        )
        sc.add(Chunk("MBMP", cn, B.enc_mbmp(c["idx"])))
        cam.add_kid(0, "MBMP", cn)
        sc.add(Chunk("ZBMP", cn, B.enc_zbmp(c["z"])))
        cam.add_kid(0, "ZBMP", cn)
        bk.add_kid(i, "CAM ", cn)
    # thumbnails
    bt = th.add(
        Chunk(
            "BKTH",
            cno_base,
            struct.pack("<hh", 1, 0x0303)
            + struct.pack(">I", ctg("BKGD"))[::-1][::-1][:0]
            + struct.pack("<II", ctg("BKGD"), cno_base),
        )
    )
    for i in range(n):
        cn = cno_base + i
        t = thumbs[i] if thumbs is not None else thumb_image(cams[i]["idx"])
        th.add(Chunk("MBMP", cn, B.enc_mbmp(t, 0, 0)))
        g = th.add(Chunk("GOKD", cn, GOKD_CONST))
        g.add_kid(65536, "MBMP", cn)
        ca = th.add(
            Chunk(
                "CATH",
                cn,
                struct.pack("<hh", 1, 0x0303) + struct.pack("<II", ctg("CAM "), i),
            )
        )
        ca.add_kid(0, "GOKD", cn)
        bt.add_kid(i, "CATH", cn)
    bt.add_kid(0, "GOKD", cno_base)
    if midi:
        th.add(
            Chunk(
                "SMTH",
                cno_base,
                struct.pack("<hh", 1, 0x0303)
                + struct.pack("<II", ctg("MSND"), cno_base),
                name=midi[1][:60],
            )
        )
    return sc, th


def vxp_cfg(name, author, content, generator):
    """the .cfg of a VXP, with every required field"""
    import time

    author = author or "Unknown"
    return (
        "Name=%s\nAuthor=%s\nOriginal Author=%s\nType=Portable\nContent=%s\nDate=%d\nGenerator=%s\n"
        % (name, author, author, content, int(time.time()), generator)
    )


def write_vxp(path, base, sc, th, title, author=""):
    import io
    import os
    import tempfile

    d = tempfile.mkdtemp()
    sc.write(os.path.join(d, base + ".3cn"))
    th.write(os.path.join(d, base + ".3th"))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(d, base + ".3cn"), base + ".3cn")
        z.write(os.path.join(d, base + ".3th"), base + ".3th")
        z.writestr(
            base + ".cfg", vxp_cfg(title, author, "Backgrounds", "3DMM Pic2Scene")
        )
