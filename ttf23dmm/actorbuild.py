"""3DMM actor/prop (TMPL) writer prototype. Format reference: 3DMMEx inc/tmpl.h, mtrl.h, modl.h, body.h,
7gen's md2_to_vxp.py, and dumps of the stock tmpls.3cn / actor.3th."""

import os
import struct
import tempfile
import zipfile

import numpy as np

from chunky import Chunk, Chunky, ctg

BO, OSK = 1, 0x0303


def fx(v):
    return int(round(v * 65536.0))


# ---------------------------------------------------------------- primitive encoders
def enc_gl(cb_entry, entries):
    return struct.pack("<hhii", BO, OSK, cb_entry, len(entries)) + b"".join(entries)


def enc_gl16(vals):
    return enc_gl(2, [struct.pack("<h", v) for v in vals])


def enc_gg(cb_fixed, entries):
    """entries: list of (fixed_bytes, var_bytes)."""
    data = b""
    locs = []
    for f, v in entries:
        assert len(f) == cb_fixed
        locs.append((len(data), cb_fixed + len(v)))
        data += f + v
    return (
        struct.pack("<hhiiii", BO, OSK, len(entries), len(data), -1, cb_fixed)
        + data
        + b"".join(struct.pack("<ii", a, b) for a, b in locs)
    )


def enc_bmdl(verts, faces, smooth=1):
    """verts: [(x,y,z,u,v)] in 3DMM units / 0..1 uv; faces: [(a,b,c)]. 'Unprepared' model (rRadius 0): BRender builds
    normals/bounds on load, exactly like 7gen's output."""
    out = struct.pack("<hhhh", BO, OSK, len(verts), len(faces)) + b"\0" * 40
    for x, y, z, u, v in verts:
        out += (
            struct.pack(
                "<5i", fx(x), fx(y), fx(z), int(round(u * 65535)), int(round(v * 65535))
            )
            + b"\0" * 12
        )
    for a, b, c in faces:
        out += (
            struct.pack("<3H", a, b, c)
            + b"\0" * 6
            + b"\0" * 4
            + struct.pack("<H", smooth)
            + b"\0" * 14
        )
    return out


def enc_tmap(idx):
    """idx: (h,w) uint8 palette indices. Rows are stored bottom-up."""
    h, w = idx.shape
    return (
        struct.pack("<4BH2BL2HL", 1, 0, 3, 3, w, 3, 2, 0, w, h, 0)
        + np.ascontiguousarray(idx[::-1]).astype(np.uint8).tobytes()
    )


def enc_mtrl(base=15, rng=7, color=0):
    return struct.pack("<hhIHHHBBi", BO, OSK, color, 0, 65535, 0, base, rng, 3276800)


def enc_cmtl(ibset):
    return struct.pack("<hhi", BO, OSK, ibset)


def enc_tmpl(rest=(0, 0, 0), prop=False):
    return struct.pack("<hhhhhhI", BO, OSK, *rest, 0x0014, 4 if prop else 1)


def enc_actn(flags=0x0A):
    return struct.pack("<hhi", BO, OSK, flags)


def enc_matrix(R, t):
    m = np.vstack(
        [np.asarray(R, float).reshape(3, 3), np.asarray(t, float).reshape(1, 3)]
    )
    return struct.pack("<12i", *[fx(v) for v in m.reshape(12)])


def enc_mbmp(img, transparent=-1):
    h, w = img.shape
    rows = []
    for y in range(h):
        row = img[y]
        opaque = np.nonzero(row >= 0)[0]
        if len(opaque) == 0:
            rows.append(b"")
            continue
        last = opaque[-1] + 1
        x = 0
        out = bytearray()
        while x < last:
            skip = 0
            while x < last and row[x] < 0 and skip < 255:
                x += 1
                skip += 1
            n = 0
            s = x
            while x < last and row[x] >= 0 and n < 255:
                x += 1
                n += 1
            out += bytes([skip, n]) + bytes(row[s : s + n].astype(np.uint8).tolist())
        rows.append(bytes(out))
    body = struct.pack("<%dh" % h, *[len(r) for r in rows]) + b"".join(rows)
    return (
        struct.pack("<hhBBh", BO, OSK, 0, 0, 0)
        + struct.pack("<4i", 0, 0, w, h)
        + struct.pack("<i", 28 + len(body))
        + body
    )


GOKD_CONST = bytes.fromhex(
    "01000303000000000000000000000000000000000a0000000000000000000000"
    "ffffffffffffffffffffffff61c30000ffffffff"
)


# ---------------------------------------------------------------- spec
class Part:
    def __init__(s, name, parent, bset=0):
        s.name, s.parent, s.bset = name, parent, bset


class Spec:
    """parts: [Part]; models: [(verts, faces)]  (index = BMDL chid under the TMPL)
    costumes: {bset: [ {part_index: texture(h,w uint8) or ('solid', base)} , ...]}   (first = default)
    actions: [ (name, [ {part_index: (model_idx, R(3x3), t(3))} ... one dict per cel ]) ]  first must be 'At Rest'
    thumb: (72,72) palette-index array."""

    def __init__(
        s,
        name,
        parts,
        models,
        costumes,
        actions,
        thumb,
        prop=False,
        rest=(0, 0, 0),
        acc_models=(),
    ):
        s.acc_models = list(acc_models)
        s.name, s.parts, s.models, s.costumes, s.actions, s.thumb, s.prop, s.rest = (
            name,
            parts,
            models,
            costumes,
            actions,
            thumb,
            prop,
            rest,
        )


def build_actor(spec, cno_base):
    sc = Chunky("3DMM")
    th = Chunky("3DMM")
    counters = {}

    def nxt(c):
        n = counters.get(c, cno_base)
        counters[c] = n + 1
        return n

    tm = sc.add(Chunk("TMPL", cno_base, enc_tmpl(spec.rest, spec.prop), name=spec.name))
    nparts = len(spec.parts)
    # body part tree and sets
    c = sc.add(Chunk("GLPI", nxt("GLPI"), enc_gl16([p.parent for p in spec.parts])))
    tm.add_kid(0, "GLPI", c.cno)
    c = sc.add(Chunk("GLBS", nxt("GLBS"), enc_gl16([p.bset for p in spec.parts])))
    tm.add_kid(0, "GLBS", c.cno)
    # costumes
    nsets = max(p.bset for p in spec.parts) + 1
    ggcm = []
    cmid = 0
    # accessory models live under CMTLs (not the TMPL); texture variants of one accessory share one BMDL chunk
    accb = [
        sc.add(Chunk("BMDL", nxt("BMDL"), enc_bmdl(v, f))) for (v, f) in spec.acc_models
    ]
    for s in range(nsets):
        ids = []
        members = [i for i, p in enumerate(spec.parts) if p.bset == s]
        for costume in spec.costumes[s]:
            cm = sc.add(Chunk("CMTL", nxt("CMTL"), enc_cmtl(s)))
            tm.add_kid(cmid, "CMTL", cm.cno)
            for k, pi in enumerate(members):
                tex = costume.get(pi)
                amdl = None
                if isinstance(tex, tuple) and tex and tex[0] == "model":
                    amdl, tex = tex[1], tex[2]
                mt = sc.add(Chunk("MTRL", nxt("MTRL"), enc_mtrl()))
                if isinstance(tex, np.ndarray):
                    tmap = sc.add(Chunk("TMAP", nxt("TMAP"), enc_tmap(tex)))
                    mt.add_kid(0, "TMAP", tmap.cno)
                elif isinstance(tex, tuple):
                    mt.data = enc_mtrl(base=tex[1], rng=7)
                else:
                    mt.data = enc_mtrl(base=15, rng=7)
                cm.add_kid(k, "MTRL", mt.cno)
                if amdl is not None:
                    cm.add_kid(k, "BMDL", accb[amdl].cno)
            ids.append(cmid)
            cmid += 1
        ggcm.append((struct.pack("<i", len(ids)), struct.pack("<%di" % len(ids), *ids)))
    c = sc.add(Chunk("GGCM", nxt("GGCM"), enc_gg(4, ggcm)))
    tm.add_kid(0, "GGCM", c.cno)
    # models
    for i, (verts, faces) in enumerate(spec.models):
        b = sc.add(Chunk("BMDL", nxt("BMDL"), enc_bmdl(verts, faces)))
        tm.add_kid(i, "BMDL", b.cno)
    # actions
    for ai, (aname, cels) in enumerate(spec.actions):
        an = sc.add(Chunk("ACTN", nxt("ACTN"), enc_actn(), name=aname))
        tm.add_kid(ai, "ACTN", an.cno)
        mats = []
        mindex = {}

        def mat(R, t):
            d = enc_matrix(R, t)
            if d not in mindex:
                mindex[d] = len(mats)
                mats.append(d)
            return mindex[d]

        entries = []
        for cel in cels:
            cps = b""
            for pi in range(nparts):
                if pi in cel:
                    mi, R, t = cel[pi]
                    cps += struct.pack("<hh", mi, mat(R, t))
                else:
                    cps += struct.pack("<hh", -1, mat(np.eye(3), (0, 0, 0)))
            entries.append((struct.pack("<ii", 0, 163840), cps))
        gcl = sc.add(Chunk("GGCL", nxt("GGCL"), enc_gg(8, entries)))
        an.add_kid(0, "GGCL", gcl.cno)
        gx = sc.add(Chunk("GLXF", nxt("GLXF"), enc_gl(48, mats)))
        an.add_kid(0, "GLXF", gx.cno)
    # thumbnail file
    tfc = struct.pack("<hh", BO, OSK) + struct.pack("<II", ctg("TMPL"), cno_base)
    tt = th.add(Chunk("PRTH" if spec.prop else "TMTH", cno_base, tfc))
    g = th.add(Chunk("GOKD", cno_base, GOKD_CONST))
    tt.add_kid(0, "GOKD", cno_base)
    mb = th.add(Chunk("MBMP", cno_base, enc_mbmp(spec.thumb)))
    g.add_kid(65536, "MBMP", cno_base)
    return sc, th


def write_vxp(path, base, sc, th, title, author="", content="Actors"):
    d = tempfile.mkdtemp()
    sc.write(os.path.join(d, base + ".3cn"))
    th.write(os.path.join(d, base + ".3th"))
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(os.path.join(d, base + ".3cn"), base + ".3cn")
        z.write(os.path.join(d, base + ".3th"), base + ".3th")
        cfg = (
            ["Name=%s" % title]
            + (["Author=%s" % author] if author else [])
            + ["Content=" + content, "Generator=3DMM Actor Prototype"]
        )
        z.writestr(base + ".cfg", "\r\n".join(cfg) + "\r\n")
