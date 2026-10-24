"""Camera / depth / palette math for the 3DMM scene maker. numpy only (no bpy) so it can be unit tested."""
import numpy as np
W, H = 544, 306
ASPECT = W / H

def bl_to_3dmm_vec(v, scale=1.0, ground_z=0.0, point=True):
    v = np.asarray(v, dtype=float)
    if point:
        return np.array([v[0] * scale, (v[2] - ground_z) * scale, -v[1] * scale])
    return np.array([v[0], v[2], -v[1]])

def camera_matrix(world4, scale=1.0, ground_z=0.0):
    """world4: 4x4 Blender matrix_world (column-vector convention, camera looks down local -Z).
    Returns 4x3 BRender matrix: rows right, up, back, position."""
    m = np.asarray(world4, dtype=float)
    out = np.zeros((4, 3))
    for r in range(3):
        a = m[:3, r]
        a = a / np.linalg.norm(a)          # strip any object scale
        out[r] = bl_to_3dmm_vec(a, point=False)
    out[3] = bl_to_3dmm_vec(m[:3, 3], scale, ground_z)
    return out

def fov_y_from_projection(P):
    """P: 4x4 projection matrix rendered at 544x306 (P[1][1] = cot(fovy/2)). Degrees."""
    return float(np.degrees(2 * np.arctan(1.0 / P[1][1])))

def pixel_dirs(M, fov_deg, w=W, h=H):
    """World-space ray directions (h,w,3) with unit depth along the view axis (so t == view-axis depth)."""
    M = np.asarray(M); t = np.tan(np.radians(fov_deg) / 2)
    x = (2 * (np.arange(w) + 0.5) / w - 1) * t * (w / h)
    y = (1 - 2 * (np.arange(h) + 0.5) / h) * t
    X, Y = np.meshgrid(x, y)
    return X[..., None] * M[0] + Y[..., None] * M[1] - M[2]

def plane_depth(M, fov_deg, y0=0.0):
    d = pixel_dirs(M, fov_deg); o = np.asarray(M)[3]
    with np.errstate(divide='ignore', invalid='ignore'):
        t = (y0 - o[1]) / d[..., 1]
    return np.where(t > 0, t, np.inf)

# ---- palette ------------------------------------------------------------
def nearest_index(rgb, pal, chunk=40000):
    """rgb (n,3) uint8/float, pal (k,3) -> (n,) index of nearest palette colour (squared RGB distance)."""
    rgb = np.asarray(rgb, dtype=np.float32); pal = np.asarray(pal, dtype=np.float32)
    out = np.empty(len(rgb), np.int32); pn = (pal ** 2).sum(1)
    for i in range(0, len(rgb), chunk):
        a = rgb[i:i + chunk]
        d = (a ** 2).sum(1)[:, None] - 2 * a @ pal.T + pn[None]
        out[i:i + chunk] = d.argmin(1)
    return out

def kmeans(px, k, iters=12, seed=1):
    px = np.asarray(px, dtype=np.float32); rng = np.random.default_rng(seed)
    if len(px) <= k:
        c = np.zeros((k, 3), np.float32); c[:len(px)] = px; return c
    c = px[rng.choice(len(px), k, replace=False)].copy()
    for _ in range(iters):
        a = nearest_index(px, c)
        for j in range(k):
            m = a == j
            if m.any(): c[j] = px[m].mean(0)
    return c

def build_palette(images_rgb, shared, nfree=90, sample=60000, seed=1):
    """images_rgb: list of (h,w,3) uint8. shared: (146,3). Returns (236,3) uint8: shared + nfree tuned colours."""
    rng = np.random.default_rng(seed)
    px = np.concatenate([i.reshape(-1, 3) for i in images_rgb]).astype(np.float32)
    if len(px) > 400000: px = px[rng.choice(len(px), 400000, replace=False)]
    idx = nearest_index(px, shared)
    err = ((px - np.asarray(shared, np.float32)[idx]) ** 2).sum(1)
    # k-means on the pixels the shared palette serves worst (top half by error)
    hard = px[err >= np.median(err)]
    if len(hard) > sample: hard = hard[rng.choice(len(hard), sample, replace=False)]
    free = kmeans(hard, nfree, seed=seed)
    return np.concatenate([np.asarray(shared, np.uint8), np.clip(np.rint(free), 0, 255).astype(np.uint8)])

def quantize(img_rgb, pal236, dither=False):
    """img (h,w,3) uint8 -> (h,w) int16 palette indices (entry + 10)."""
    h, w, _ = img_rgb.shape
    if not dither:
        return (nearest_index(img_rgb.reshape(-1, 3), pal236).reshape(h, w) + 10).astype(np.int16)
    f = img_rgb.astype(np.float32).copy(); out = np.empty((h, w), np.int16); pal = pal236.astype(np.float32)
    for y in range(h):
        row = f[y]
        for x in range(w):
            i = int(nearest_index(row[x:x+1], pal)[0]); out[y, x] = i + 10
            e = row[x] - pal[i]
            if x + 1 < w: row[x+1] += e * 7/16
            if y + 1 < h:
                if x > 0: f[y+1, x-1] += e * 3/16
                f[y+1, x] += e * 5/16
                if x + 1 < w: f[y+1, x+1] += e * 1/16
    return out
