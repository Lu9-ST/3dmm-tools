"""Converts a font and sanity-checks the result.  usage: selfcheck.py font.ttf"""

import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np

import chunky
import ttf23dmm as T
from preview import read_bmdl

font = sys.argv[1]
r = T.convert(font, os.path.join(tempfile.mkdtemp(), "check.vxp"))
n = bad = 0
for (c, cno), ch in r["scene"].chunks.items():
    if chunky.ctgs(c) != "BMDL":
        continue
    P, N, F = read_bmdl(ch.data)
    if len(P) == 0:
        continue
    n += 1
    assert len(P) < 65535, "too many vertices"
    assert np.allclose(np.linalg.norm(N, axis=1), 1, atol=2e-3), "normals not unit"
    tot = sum(
        np.dot(
            np.cross(P[f[1]] - P[f[0]], P[f[2]] - P[f[0]]),
            (P[f[0]] + P[f[1]] + P[f[2]]) / 3,
        )
        for f in F
    )
    bad += tot <= 0
print("glyphs checked:", n, " inward-wound:", bad)
sys.exit(1 if bad else 0)
