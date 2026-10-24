"""Blender-independent export pipeline: camera shots -> palette/quantise -> 3DMM scene -> .vxp."""
import os, re, zlib
from dataclasses import dataclass, field
import numpy as np
from . import coremath as C, bkgdwrite as B, scenebuild as S

# stock Street light 0 (rotation rows) used when the scene has no sun lamp
DEFAULT_LIGHT = (np.array([[0.034, 0.0, -0.9993], [-0.5662, 0.8238, -0.0193], [0.8232, 0.5666, 0.028]]), 1.0, 1)

@dataclass
class Shot:
    M: np.ndarray            # 4x3 BRender matrix (already in 3DMM units/axes)
    fov_deg: float
    rgb: np.ndarray          # (306,544,3) uint8
    depth: np.ndarray        # (306,544) float, view-axis distance in 3DMM units, inf = nothing
    placements: list = field(default_factory=list)   # (x,0,z) points

@dataclass
class Settings:
    name: str = 'ZZ-My Scene'
    bkgds_path: str = ''
    music: object = S.MUSIC_IMAGINOPOLIS     # None, or (sid, 'MSND', cno)
    hither: float = 1.0
    yon: float = 5000.0
    dither: bool = False
    author: str = ''
    addon_name: str = ''                     # name of the .vxp / expansion (cfg Name=)
    cno_override: int = 0                    # 0 = derive from the scene name
    midi: object = None                      # (mids_bytes, song_name) for a custom song
    lights: list = None                      # list of (3x3, intensity, type) or None for default

def cno_base_for(name):
    return 0x20000000 + (zlib.crc32(name.encode('utf8')) & 0x0FFFFF00)

def safe_name(name):
    return re.sub(r'[^A-Za-z0-9_]+', '_', name).strip('_').lower()[:32] or 'scene'

def box_downscale(rgb, w=128, h=72):
    H, W, _ = rgb.shape
    ys = np.linspace(0, H, h + 1).astype(int); xs = np.linspace(0, W, w + 1).astype(int)
    out = np.empty((h, w, 3), np.uint8)
    for j in range(h):
        row = rgb[ys[j]:max(ys[j + 1], ys[j] + 1)].astype(np.float32)
        for i in range(w):
            out[j, i] = row[:, xs[i]:max(xs[i + 1], xs[i] + 1)].reshape(-1, 3).mean(0)
    return out

def export_vxp(shots, settings, out_dir, log=print):
    assert shots, 'no cameras'
    shared = S.shared_palette(settings.bkgds_path)
    log('building palette from %d image(s)...' % len(shots))
    pal = C.build_palette([s.rgb for s in shots], shared)
    cams, thumbs = [], []
    for i, s in enumerate(shots):
        idx = C.quantize(s.rgb, pal, settings.dither)
        z = B.depth_to_z16(s.depth, settings.hither, settings.yon)
        cams.append(dict(M=s.M, fov_deg=s.fov_deg, idx=idx, z=z, placements=s.placements or [(0.0, 0.0, 0.0)]))
        small = box_downscale(s.rgb)
        thumbs.append((C.nearest_index(small.reshape(-1, 3), shared).reshape(72, 128) + 10).astype(np.int16))
        log('camera %d quantised' % i)
    base = settings.cno_override or cno_base_for(settings.name)
    lights = settings.lights or [DEFAULT_LIGHT]
    sc, th = S.build(settings.name, cams, pal, base, music=settings.music, lights=lights, thumbs=thumbs, midi=settings.midi)
    fname = safe_name(settings.addon_name or settings.name)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, fname + '.vxp')
    S.write_vxp(path, fname, sc, th, settings.addon_name or settings.name, settings.author)
    log('wrote %s (chunk numbers start at %d)' % (path, base))
    return path
