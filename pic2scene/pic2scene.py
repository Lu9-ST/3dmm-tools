#!/usr/bin/env python3
"""Picture (+ depth map) -> 3D Movie Maker scene (.vxp) with one camera angle.

The picture becomes the backdrop, the depth map becomes the z-buffer (so actors walk behind and in front of things in
the picture), and the camera angle is worked out from the picture: field of view (from EXIF metadata or the
--fov flag), tilt (--horizon, or estimated from a metric depth map with --auto-camera) and the camera height
above the floor.

    python3 pic2scene.py photo.jpg --depth photo_depth.png
"""

import argparse
import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dmmscene import coremath as C
from dmmscene import exporter as E
from dmmscene import midi as MIDI
from dmmscene import scenebuild as S

W, H = C.W, C.H  # 544 x 306, the size of every 3DMM backdrop


# ------------------------------------------------------------------------------------------------ camera maths
def camera_from_up(ux, uy, uz, pos):
    """4x3 BRender camera matrix (rows right, up, back, position) from the world 'up' direction written in camera
    coordinates (u).  Heading is free: the camera looks towards -Z, like the stock cameras.  Level camera: u = (0,1,0)."""
    uz = max(-0.999, min(0.999, uz))
    w = math.sqrt(max(1 - uz * uz, 1e-12))
    e1 = np.array([1.0, 0.0, 0.0])
    e2 = np.array([0.0, w, -uz])
    back = np.array([0.0, uz, w])
    phi = math.atan2(ux, uy)
    r0 = math.cos(phi) * e1 + math.sin(phi) * e2
    r1 = -math.sin(phi) * e1 + math.cos(phi) * e2
    M = np.zeros((4, 3))
    M[0], M[1], M[2], M[3] = r0, r1, back, pos
    return M


def camera_from_pitch(pitch_deg, roll_deg, height):
    """pitch: degrees looking down; roll: degrees clockwise; height above the floor (y = 0)."""
    p, r = math.radians(pitch_deg), math.radians(roll_deg)
    return camera_from_up(
        math.cos(p) * math.sin(r),
        math.cos(p) * math.cos(r),
        math.sin(p),
        (0.0, height, 0.0),
    )


def ground_depth(M, fovy):
    """view-axis distance of the floor (y = 0) for every pixel; inf where the ray never reaches it"""
    return C.plane_depth(M, fovy, 0.0)


def ground_points(M, fovy, z=None, n=6):
    """a few floor positions that are actually visible in the picture (not hidden behind a wall), for the actor
    start marks, the first being near the middle"""
    zg = ground_depth(M, fovy)
    dirs = C.pixel_dirs(M, fovy)
    o = np.asarray(M)[3]
    vis = np.isfinite(zg)
    if z is not None:
        with np.errstate(invalid="ignore"):
            vis &= np.isfinite(z) & (np.abs(z - zg) <= 0.02 * zg)
    if not vis.any():
        vis = np.isfinite(zg)
    if not vis.any():
        return [(0.0, 0.0, 0.0)]
    jj, ii = np.nonzero(vis)
    pts = []
    for fx_, fy_ in [
        (0.5, 0.78),
        (0.3, 0.88),
        (0.7, 0.88),
        (0.5, 0.96),
        (0.2, 0.7),
        (0.8, 0.7),
    ][:n]:
        k = int(np.argmin(((ii - fx_ * W) / W) ** 2 + ((jj - fy_ * H) / H) ** 2))
        p = o + dirs[jj[k], ii[k]] * zg[jj[k], ii[k]]
        q = (float(p[0]), 0.0, float(p[2]))
        if all(abs(q[0] - a) + abs(q[2] - b) > 1.5 for a, _, b in pts):
            pts.append(q)
    return pts


# ------------------------------------------------------------------------------------------------ inputs
def exif_fov_vertical(img):
    """vertical FOV (degrees) of the whole picture from EXIF 35mm-equivalent focal length, or None"""
    try:
        ex = img.getexif()
        sub = ex.get_ifd(0x8769) if hasattr(ex, "get_ifd") else {}
        f35 = sub.get(0xA405) or ex.get(0xA405)
        if f35 and float(f35) > 0:
            w, h = img.size
            diag = 43.2666
            tan_d = (diag / 2) / float(f35)
            return math.degrees(2 * math.atan(tan_d * h / math.hypot(w, h)))
    except Exception:
        pass
    return None


def crop_box(w, h, anchor_y=0.5, anchor_x=0.5):
    """largest 16:9 window of a (width) x (height) picture -> (x0, y0, x1, y1)"""
    tw = min(w, int(round(h * W / H)))
    th = min(h, int(round(w * H / W)))
    if tw * H != th * W:  # keep the exact ratio
        th = int(round(tw * H / W))
    x0 = int(round((w - tw) * anchor_x))
    y0 = int(round((h - th) * anchor_y))
    return x0, y0, x0 + tw, y0 + th


def colormap_to_scalar(rgb, name):
    """a depth picture painted with a colour map (turbo, jet, ...) -> 0..1 values (1 = the map's last colour)"""
    from dmmscene.colormaps import LUTS

    lut = np.asarray(LUTS[name], np.float32)
    # interpolate the table to 256 steps
    t64 = np.linspace(0, 1, len(lut))
    t = np.linspace(0, 1, 256)
    lut = np.stack([np.interp(t, t64, lut[:, k]) for k in range(3)], 1)
    px = rgb.reshape(-1, 3).astype(np.float32)
    out = np.empty(len(px), np.float32)
    for i in range(0, len(px), 20000):
        d = ((px[i : i + 20000, None, :] - lut[None]) ** 2).sum(-1)
        out[i : i + 20000] = d.argmin(1) / 255.0
    return out.reshape(rgb.shape[:2])


def load_depth_array(path, scale=1.0, colormap=None, log=print):
    """float32 array from PNG/TIFF/.npy/.exr.  Integer images are returned as raw values * scale.
    colormap: the picture is a coloured visualisation (turbo, jet, ...): converted back to 0..1 (loses the real units)."""
    low = path.lower()
    if low.endswith(".npy"):
        a = np.load(path).astype(np.float32)
    elif low.endswith(".exr"):
        a = None
        try:  # 1) the OpenEXR package (pip install OpenEXR)
            import OpenEXR

            ch = OpenEXR.File(path).channels()
            for k in ("Y", "Z", "depth", "R"):
                if k in ch:
                    a = np.asarray(ch[k].pixels, np.float32)
                    break
            if a is None:
                a = np.asarray(next(iter(ch.values())).pixels, np.float32)
        except ImportError:
            pass
        if a is None:  # 2) OpenCV, if its build can read EXR
            os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
            try:
                import cv2

                a = cv2.imread(path, cv2.IMREAD_UNCHANGED)
            except ImportError:
                pass
            if a is not None:
                a = a.astype(np.float32)
        if a is None:
            raise SystemExit(
                "could not read %s: pip install OpenEXR (or opencv-python), or save the depth as .npy"
                % path
            )
    else:
        im = Image.open(path)
        if im.mode in ("RGB", "RGBA", "P"):
            rgb = np.asarray(im.convert("RGB"))
            sat = float(np.abs(rgb[..., 0].astype(int) - rgb[..., 2]).mean())
            if colormap:
                return colormap_to_scalar(rgb, colormap) * scale
            if sat > 8:
                raise SystemExit(
                    "%s is a coloured picture, not raw depth. If it is a colour-mapped visualisation, add "
                    "--colormap turbo (or jet/viridis/...); better, use the raw depth the model saves (.npy/.exr/16-bit PNG)"
                    % path
                )
            a = rgb[..., 0].astype(np.float32)
        else:
            a = np.asarray(im).astype(np.float32)
    if a.ndim == 3:
        a = a[..., 0]
    return a * scale


def read_fov_json(path):
    """MoGe fov.json: {"fov_x": deg, "fov_y": deg} -> (fov_y or None, fov_x or None)"""
    import json

    j = json.load(open(path))
    return j.get("fov_y"), j.get("fov_x")


FILTERS = {
    "nearest": Image.NEAREST,
    "bilinear": Image.BILINEAR,
    "bicubic": Image.BICUBIC,
    "lanczos": Image.LANCZOS,
    "box": Image.BOX,
}
DEPTH_FILTERS = ("nearest", "bilinear", "bicubic")


def plan_layout(w, h, fit, anchor_x=0.5, anchor_y=0.5):
    """How a (width) x (height) picture lands on the 544 x 306 canvas.
    Returns (src_box, dst_box, scale): src_box = part of the picture used (float px), dst_box = where it goes on the
    canvas (int px), scale = canvas pixels per picture pixel (vertical), used for the field of view.
      crop    : fill the canvas, cut off what sticks out (keeps the aspect ratio)
      stretch : squeeze/stretch to the canvas (ignores the aspect ratio)
      pad     : whole picture, aspect ratio kept, empty bars where it does not fill the canvas
      native  : 1 picture pixel = 1 canvas pixel; too big -> cropped, too small -> bars (no resampling at all)"""
    if fit == "stretch":
        return (0, 0, w, h), (0, 0, W, H), H / h
    if fit == "crop":
        s = max(W / w, H / h)
    elif fit == "pad":
        s = min(W / w, H / h)
    elif fit == "native":
        s = 1.0
    else:
        raise SystemExit("unknown --fit " + fit)
    vw, vh = w * s, h * s  # size of the whole picture on the canvas
    ox = (W - vw) * anchor_x
    oy = (H - vh) * anchor_y  # where its top-left corner is (may be negative)
    dx0, dy0 = max(0.0, ox), max(0.0, oy)
    dx1, dy1 = min(float(W), ox + vw), min(float(H), oy + vh)
    src = ((dx0 - ox) / s, (dy0 - oy) / s, (dx1 - ox) / s, (dy1 - oy) / s)
    dst = (int(round(dx0)), int(round(dy0)), int(round(dx1)), int(round(dy1)))
    return src, dst, s


def parse_color(txt):
    t = txt.strip().lstrip("#")
    if "," in t:
        v = [int(x) for x in t.split(",")]
    else:
        v = [int(t[i : i + 2], 16) for i in (0, 2, 4)]
    return tuple(max(0, min(255, c)) for c in v)


def place_image(img, src, dst, filt, color):
    """picture -> canvas (H, W, 3) uint8 and a mask of the pixels that really come from the picture"""
    canvas = Image.new("RGB", (W, H), color)
    dw, dh = dst[2] - dst[0], dst[3] - dst[1]
    if dw > 0 and dh > 0:
        canvas.paste(img.resize((dw, dh), filt, box=src), (dst[0], dst[1]))
    mask = np.zeros((H, W), bool)
    mask[dst[1] : dst[3], dst[0] : dst[2]] = True
    return np.asarray(canvas), mask


def place_depth(a, frac_src, dst, filt):
    """depth array -> canvas (H, W) float32; the same part of the scene as place_image (frac_src = src box / picture size).
    Bars are filled with the median depth (they are cut out later)."""
    ah, aw = a.shape
    box = (frac_src[0] * aw, frac_src[1] * ah, frac_src[2] * aw, frac_src[3] * ah)
    dw, dh = dst[2] - dst[0], dst[3] - dst[1]
    out = np.full((H, W), float(np.median(a)), np.float32)
    if dw > 0 and dh > 0:
        im = Image.fromarray(np.ascontiguousarray(a, np.float32), mode="F").resize(
            (dw, dh), FILTERS[filt], box=box
        )
        out[dst[1] : dst[3], dst[0] : dst[2]] = np.asarray(im, np.float32)
    return out


def window_resize(a, box_frac, w=W, h=H):
    """crop a float array to a fractional window and bilinear-resize to (width) x (height)"""
    ah, aw = a.shape
    x0, y0, x1, y1 = box_frac
    sub = a[
        int(round(y0 * ah)) : max(int(round(y1 * ah)), int(round(y0 * ah)) + 1),
        int(round(x0 * aw)) : max(int(round(x1 * aw)), int(round(x0 * aw)) + 1),
    ]
    im = Image.fromarray(sub.astype(np.float32), mode="F").resize(
        (w, h), Image.BILINEAR
    )
    return np.asarray(im, np.float32)


def box_blur(a, r):
    if r <= 0:
        return a
    k = 2 * r + 1
    p = np.pad(a, r, mode="edge")
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


# ------------------------------------------------------------------------------------------------ depth calibration
def robust_affine(x, y, iters=8):
    """y ~ a*x + b with Huber-style reweighting; returns (a, b)"""
    wgt = np.ones_like(x)
    a, b = 1.0, 0.0
    for _ in range(iters):
        A = np.stack([x * wgt, wgt], 1)
        sol = np.linalg.lstsq(A, y * wgt, rcond=None)[0]
        a, b = sol
        res = np.abs(a * x + b - y)
        s = max(1.4826 * np.median(res), 1e-9)
        wgt = 1.0 / np.maximum(1.0, res / (1.5 * s))
    return float(a), float(b)


def calibrate_depth(
    d,
    kind,
    M,
    fovy,
    ground_rows=0.3,
    depth_scale=1.0,
    near=None,
    far=None,
    log=print,
    valid=None,
):
    """d: depth map (H, W) in the picture's own units -> distance along the view axis in 3DMM units (inf = nothing).

    kind = 'metric'    d is a distance (times --depth-scale gives 3DMM units); the floor only refines one scale factor
           'disparity' bright = near, any affine range (Depth Anything / MiDaS output): 1/z = a*d + b
           'depth'     bright = far, any affine range: z = a*d + b
    For the two relative kinds the unknown range is fitted so that the lower part of the picture lands on the floor
    plane implied by the camera (or set with --near/--far)."""
    zg = ground_depth(M, fovy)
    rows = np.zeros((H, W), bool)
    rows[int(H * (1 - ground_rows)) :, :] = True
    sel = rows & np.isfinite(zg) & np.isfinite(d)
    if valid is not None:
        sel &= valid
    if kind == "metric":
        z = d  # already in 3DMM units (scaled when loaded)
        if sel.sum() > 200:
            s = float(np.median(zg[sel] / np.maximum(z[sel], 1e-6)))
            log("metric depth vs floor: scale factor %.3f (1.0 = they agree)" % s)
            if far is None and near is None:
                z = z * s if 0.2 < s < 5 else z
        return np.where(z > 0, z, np.inf)
    dn = d.astype(np.float64)
    lo, hi = (
        (np.percentile(dn[valid], 1), np.percentile(dn[valid], 99))
        if valid is not None
        else (np.percentile(dn, 1), np.percentile(dn, 99))
    )
    dn = np.clip((dn - lo) / max(hi - lo, 1e-9), 0, 1)
    if near is not None and far is not None:  # explicit range
        if kind == "disparity":
            inv = 1 / far + dn * (1 / near - 1 / far)
            return 1.0 / np.maximum(inv, 1e-9)
        return near + dn * (far - near)
    if sel.sum() < 200:
        raise SystemExit(
            "not enough floor in the picture to calibrate the depth: pass --near and --far, or lower --horizon"
        )
    if kind == "disparity":
        a, b = robust_affine(dn[sel], 1.0 / zg[sel])
        inv = a * dn + b
        log("disparity -> depth: 1/z = %.4f * d + %.4f  (floor fit)" % (a, b))
        z = np.where(inv > 1e-6, 1.0 / np.maximum(inv, 1e-6), np.inf)
    else:
        a, b = robust_affine(dn[sel], zg[sel])
        log("depth -> depth: z = %.3f * d + %.3f  (floor fit)" % (a, b))
        z = a * dn + b
        z = np.where(z > 0, z, np.inf)
    return z


def snap_to_floor(z, M, fovy, tol=0.04):
    """clean the floor: pixels whose depth is within tol of the floor plane get exactly the plane's depth"""
    zg = ground_depth(M, fovy)
    with np.errstate(invalid="ignore"):
        near_floor = np.isfinite(zg) & np.isfinite(z) & (np.abs(z - zg) <= tol * zg)
    out = np.where(near_floor, zg, z)
    return out, float(near_floor.mean())


def fit_floor_plane(
    z,
    fovy,
    rows_from=0.4,
    iters=1500,
    thresh=0.025,
    max_roll=10.0,
    roll_fixed=None,
    lowest=True,
    seed=1,
    log=print,
):
    """RANSAC the floor out of a metric depth map: returns (pitch_deg, roll_deg, height) or None.
    Points are in camera space (x right, y up, z towards the viewer = -depth).
      * inlier distance is a fraction (thresh) of the point's distance: metric depth is noisier far away, and near
        points count more in the final least-squares fit
      * the floor must face up and its roll is limited (|roll| <= max_roll, or within 2 deg of roll_fixed): a podium side,
        a wall or a tilted lawn can't win as 'the floor' and tilt the picture
      * parallel horizontal planes (floor, podium top, bench seat) are told apart by height: with lowest=True, the lowest
        one with a good share of the points is the floor (a floor under a podium is still the floor)"""
    rng = np.random.default_rng(seed)
    t = math.tan(math.radians(fovy) / 2)
    xs = (2 * (np.arange(W) + 0.5) / W - 1) * t * (W / H)
    ys = (1 - 2 * (np.arange(H) + 0.5) / H) * t
    X, Y = np.meshgrid(xs, ys)
    j0 = int(H * rows_from)
    d = z[j0:, :]
    ok = np.isfinite(d) & (d > 0)
    P = np.stack([X[j0:, :] * d, Y[j0:, :] * d, -d], -1)[ok]
    if len(P) < 500:
        return None
    if len(P) > 20000:
        P = P[rng.choice(len(P), 20000, replace=False)]
    dist = np.maximum(-P[:, 2], 1e-3)
    tol = thresh * dist
    r0, rr = (0.0, max_roll) if roll_fixed is None else (roll_fixed, 2.0)

    def roll_of(n):
        return math.degrees(math.atan2(n[0], n[1]))

    def ransac(mask, n_ref=None, ang=None):
        idx = np.nonzero(mask)[0]
        if len(idx) < 60:
            return None
        Q = P[idx]
        tq = tol[idx]
        best, best_n = None, 0
        for _ in range(iters):
            a, b, c = Q[rng.choice(len(Q), 3, replace=False)]
            n = np.cross(b - a, c - a)
            nn = np.linalg.norm(n)
            if nn < 1e-9:
                continue
            n /= nn
            if n[1] < 0:
                n = -n
            if n[1] < 0.7 or abs(roll_of(n) - r0) > rr:
                continue
            if (
                n_ref is not None
                and math.degrees(math.acos(min(1.0, float(n @ n_ref)))) > ang
            ):
                continue
            cnt = int((np.abs((Q - a) @ n) < tq).sum())
            if cnt > best_n:
                best, best_n = (n, float(n @ a)), cnt
        if best is None:
            return None
        return best, best_n

    def refine(n, off):
        """least squares on the inliers, near points weighted more; the tolerance tightens in the last rounds"""
        for k, f in enumerate((1.0, 0.8, 0.6, 0.5, 0.5)):
            sel = np.abs((P @ n) - off) < tol * f
            if sel.sum() < 30:
                break
            Q = P[sel]
            w = 1.0 / dist[sel] ** 2
            c = (Q * w[:, None]).sum(0) / w.sum()
            n = np.linalg.svd((Q - c) * w[:, None], full_matrices=False)[2][2]
            if n[1] < 0:
                n = -n
            off = float(n @ c)
        sel = np.abs((P @ n) - off) < tol
        return n, off, sel

    free = np.ones(len(P), bool)
    found = []  # (n, off, inlier mask)
    for rounds in range(4):
        r = ransac(free, found[0][0] if found else None, 2.0)
        if r is None:
            break
        (n, off), cnt = r
        if cnt < (0.04 if not found else 0.02) * len(P):
            break
        n, off, sel = refine(n, off)
        found.append((n, off, sel))
        free &= ~sel
        if not lowest:
            break
    if not found:
        return None
    n, off, sel = found[0]
    if lowest:
        top = int(sel.sum())
        for n2, off2, sel2 in found[1:]:
            if (
                off2 < off - 0.15 * abs(off) and sel2.sum() >= 0.4 * top
            ):  # clearly lower and big enough
                log(
                    "a lower parallel plane was found (%.2f below the first one): using it as the floor"
                    % (off - off2)
                )
                n, off, sel = n2, off2, sel2
    height = abs(off)
    ux, uy, uz = n
    pitch = math.degrees(math.asin(max(-1, min(1, uz))))
    roll = roll_of(n)
    log(
        "floor plane: %d inliers of %d -> pitch %.1f deg, roll %.1f deg, camera height %.2f"
        % (int(sel.sum()), len(P), pitch, roll, height)
    )
    return pitch, roll, height


# ------------------------------------------------------------------------------------------------ preview
def preview(
    rgb, z, M, fovy, placements, path, actor_height=10.0, hither=1.0, yon=5000.0
):
    """side-by-side check: picture, depth, and a stand-in actor (a pillar 10 units tall) z-tested against the scene"""
    zz = np.where(np.isfinite(z), z, np.nan)
    zi = np.nan_to_num(zz, nan=np.nanmax(zz) if np.isfinite(zz).any() else 1.0)
    lo, hi = np.percentile(zi, 1), np.percentile(zi, 99)
    g = np.clip(1 - (zi - lo) / max(hi - lo, 1e-9), 0, 1)
    depth_img = (np.stack([g, g * 0.7 + 0.15, 1 - g], -1) * 255).astype(np.uint8)
    out = rgb.copy().astype(np.float32)
    dirs = C.pixel_dirs(M, fovy)
    o = np.asarray(M)[3]
    zfwd = -np.asarray(M)[2]
    for k, (px, _, pz) in enumerate(placements[:3]):
        for yy in np.linspace(0, actor_height, 60):
            for ang in np.linspace(0, 2 * math.pi, 16, endpoint=False):
                p = np.array([px + 0.9 * math.cos(ang), yy, pz + 0.9 * math.sin(ang)])
                v = p - o
                depth = float(v @ zfwd)
                if depth <= hither:
                    continue
                cx = v @ np.asarray(M)[0] / depth
                cy = v @ np.asarray(M)[1] / depth
                t = math.tan(math.radians(fovy) / 2)
                sx = (cx / (t * W / H) + 1) / 2 * W
                sy = (1 - cy / t) / 2 * H
                i, j = int(sx), int(sy)
                if 0 <= i < W and 0 <= j < H and depth < z[j, i]:
                    out[max(j - 1, 0) : j + 2, max(i - 1, 0) : i + 2] = (
                        255,
                        40 + 60 * k,
                        40,
                    )
    row = np.concatenate([rgb, depth_img, np.clip(out, 0, 255).astype(np.uint8)], 1)
    Image.fromarray(row).save(path)


# ------------------------------------------------------------------------------------------------ main
def find_bkgds(hint):
    cands = [hint] if hint else []
    home = os.path.expanduser("~")
    for d in [
        ".",  # common paths
        home + "/3dmmex/Microsoft Kids/3D Movie Maker",
        home + "/.local/share/3dmmex/Microsoft Kids/3D Movie Maker",
        "/usr/share/3dmmex/Microsoft Kids/3D Movie Maker",
        "/opt/3dmmex/Microsoft Kids/3D Movie Maker",
        home + "/Microsoft Kids/3D Movie Maker",
        "C:/Program Files (x86)/Microsoft Kids/3D Movie Maker",
        "C:/Program Files/Microsoft Kids/3D Movie Maker",
    ]:
        cands += [
            os.path.join(d, "bkgds.3cn"),
            os.path.join(d, "content-files", "bkgds.3cn"),
            os.path.join(d, "Content", "bkgds.3cn"),
        ]
    for c in cands:
        if c and os.path.isfile(c):
            return c
        if c and os.path.isdir(c):
            for nm in ("bkgds.3cn", "BKGDS.3CN"):
                if os.path.isfile(os.path.join(c, nm)):
                    return os.path.join(c, nm)
    return None


def estimate_depth(img_path, model, log=print):
    """optional: run a depth model (needs: pip install transformers torch).  Returns a disparity-like array."""
    try:
        from transformers import pipeline
    except ImportError:
        raise SystemExit(
            "--estimate needs `pip install transformers torch`; or make the depth map elsewhere and pass --depth"
        )
    log("loading %s ..." % model)
    pipe = pipeline("depth-estimation", model=model)
    res = pipe(Image.open(img_path).convert("RGB"))
    return np.asarray(res["predicted_depth"].squeeze().cpu().numpy(), np.float32)


def resolve_music(spec, bkgds_path, log=print):
    """--music value -> (music, midi) for Settings.  '' = the default song (Imaginopolis), 'none' = silence,
    a path to a .mid file = embedded in the scene, anything else = name of a stock song (snds.3cn next to bkgds.3cn,
    plus the official expansions' sound files if they are installed)."""
    m = (spec or "").strip()
    if not m:
        return S.MUSIC_IMAGINOPOLIS, None
    if m.lower() in ("none", "off", "no", "silence"):
        return None, None
    if m.lower().endswith((".mid", ".midi")) or os.path.isfile(m):
        try:
            mids = MIDI.midi_to_mids(open(m, "rb").read())
        except Exception as ex:
            raise SystemExit("can't use the MIDI file %s: %s" % (m, ex))
        log("music: embedding %s" % m)
        return None, (mids, os.path.splitext(os.path.basename(m))[0])
    snds = os.path.join(os.path.dirname(bkgds_path), "snds.3cn")
    if not os.path.isfile(snds):
        snds = S._find_ci(os.path.dirname(bkgds_path), "snds.3cn") or snds
    if not os.path.isfile(snds):
        raise SystemExit(
            "snds.3cn not found next to %s, so stock song names cannot be looked up (give a .mid file instead)"
            % bkgds_path
        )
    songs = S.stock_songs(snds)
    hit = {k.lower(): v for k, v in songs.items()}.get(m.lower())
    if hit is None:
        close = [k for k in sorted(songs, key=str.lower) if m.lower() in k.lower()]
        raise SystemExit(
            "unknown stock song '%s'.%s\n(--list-songs shows all %d)"
            % (
                m,
                " Did you mean: " + ", ".join(close[:8]) + "?" if close else "",
                len(songs),
            )
        )
    log("music: stock song %s" % m)
    return (hit[0], "MSND", hit[1]), None


def run(a, log=print):
    img = Image.open(a.image)
    try:
        from PIL import ImageOps

        img = ImageOps.exif_transpose(img)
    except Exception:
        pass
    img = img.convert("RGB")
    w, h = img.size
    # --- field of view of the original picture (vertical)
    if a.fov_json:
        fy, fx = read_fov_json(a.fov_json)
        a.fov, a.hfov = (fy, None) if fy else (None, fx)
        log(
            "field of view from %s: %s deg"
            % (a.fov_json, "%.1f vertical" % fy if fy else "%.1f horizontal" % fx)
        )
    if a.fov:
        fovy0 = a.fov
    elif a.hfov:
        fovy0 = math.degrees(2 * math.atan(math.tan(math.radians(a.hfov) / 2) * h / w))
    else:
        fovy0 = exif_fov_vertical(Image.open(a.image)) or 45.0
        log(
            "field of view: %.1f deg vertical (%s)"
            % (
                fovy0,
                "EXIF"
                if exif_fov_vertical(Image.open(a.image))
                else "default, use --fov",
            )
        )
    # --- where the picture goes on the 544x306 canvas
    src, dst, sc = plan_layout(w, h, a.fit, a.crop_x, a.crop_y)
    fovy = math.degrees(2 * math.atan(math.tan(math.radians(fovy0) / 2) * (H / sc) / h))
    rgb, valid = place_image(img, src, dst, FILTERS[a.filter], parse_color(a.pad_color))
    frac = (src[0] / w, src[1] / h, src[2] / w, src[3] / h)
    log(
        "picture %dx%d, fit=%s: uses %.0fx%.0f px of it at x%.2f -> canvas, scene field of view %.1f deg%s"
        % (
            w,
            h,
            a.fit,
            src[2] - src[0],
            src[3] - src[1],
            sc,
            fovy,
            "" if valid.all() else " (empty bars: %.0f%%)" % (100 * (1 - valid.mean())),
        )
    )

    # --- depth
    if a.depth:
        d = load_depth_array(
            a.depth, a.depth_scale if a.depth_kind == "metric" else 1.0, a.colormap, log
        )
    elif a.estimate:
        d = estimate_depth(a.image, a.estimate, log)
    else:
        raise SystemExit("need a depth map: --depth FILE (or --estimate MODEL)")
    # pixels without depth (sky / masked-out areas show up as inf, NaN or <= 0 in metric maps): they become "nothing"
    # (infinitely far) and are kept out of every fit; the numbers underneath are filled so resampling stays clean
    bad = ~np.isfinite(d)
    if a.depth_kind != "disparity":
        bad |= d <= 0
    if bad.any():
        good = d[~bad]
        fill = (
            (good.min() if a.depth_kind == "disparity" else good.max())
            if good.size
            else 0.0
        )
        d = np.where(bad, fill, d).astype(np.float32)
        nodata = place_depth(bad.astype(np.float32), frac, dst, "nearest") > 0.5
        valid = valid & ~nodata
        log(
            "depth map: %.0f%% of it has no depth (sky/masked): treated as infinitely far"
            % (100 * bad.mean())
        )
    d = place_depth(d, frac, dst, a.depth_filter)
    if a.invert:
        d = d.max() + d.min() - d
    d = box_blur(d, a.blur)

    # --- camera
    pitch, roll, height = (a.pitch or 0.0), (a.roll or 0.0), a.cam_height
    if a.horizon is not None:
        row = (
            dst[1] - src[1] * sc + a.horizon * h * sc
        ) / H  # canvas row (0..1) of the horizon; may be off-canvas
        pitch = math.degrees(
            math.atan((0.5 - row) * 2 * math.tan(math.radians(fovy) / 2))
        )
        log(
            "horizon at %.0f%% of the picture height -> camera tilted %.1f deg down"
            % (a.horizon * 100, pitch)
        )
    if a.auto_camera:
        if a.depth_kind != "metric":
            raise SystemExit(
                "--auto-camera needs a metric depth map (--depth-kind metric)"
            )
        fit = fit_floor_plane(
            np.where((d > 0) & valid, d, np.inf),
            fovy,
            max_roll=a.max_roll,
            roll_fixed=a.roll,
            lowest=not a.floor_largest,
            log=log,
        )
        if fit is None:
            log(
                "WARNING: could not find a floor plane in the depth map (is the floor visible in the lower part of the "
                "picture?). Using --horizon/--pitch/--roll/--cam-height instead; set them by hand."
            )
        else:
            f_pitch, f_roll, h_est = fit
            pitch = a.pitch if a.pitch is not None else f_pitch
            roll = a.roll if a.roll is not None else f_roll
            if a.cam_height is None:
                height = h_est
            if abs(f_roll) > 4 and a.roll is None:
                log(
                    "note: the fitted roll is %.1f deg; if the picture is not that crooked, pass --roll 0 (or your value)"
                    % f_roll
                )
    if height is None:
        height = 6.0
    M = camera_from_pitch(pitch, roll, height)

    # --- depth -> distance in 3DMM units
    z = calibrate_depth(
        d,
        a.depth_kind,
        M,
        fovy,
        a.ground_rows,
        a.depth_scale,
        a.near,
        a.far,
        log,
        valid,
    )
    if a.snap_floor > 0:
        z, frac_floor = snap_to_floor(z, M, fovy, a.snap_floor)
        log(
            "floor cleaned: %.0f%% of the picture is now exactly on the floor plane"
            % (frac_floor * 100)
        )
    z = np.where(z > a.far_clip, np.inf, z)
    z = np.where(valid, z, np.inf)  # empty bars (--fit pad/native) are nothing
    z = np.where(np.isfinite(z), np.maximum(z, a.hither * 1.05), z)
    pl = ground_points(M, fovy, z)
    shot = E.Shot(M=M, fov_deg=fovy, rgb=rgb, depth=z, placements=pl)
    bk = find_bkgds(a.bkgds)
    if not bk:
        raise SystemExit(
            "bkgds.3cn (from your 3D Movie Maker content folder) not found: pass --bkgds PATH"
        )
    name = a.name or ("ZZ-" + os.path.splitext(os.path.basename(a.image))[0])[:40]
    st = E.Settings(
        name=name,
        bkgds_path=bk,
        hither=a.hither,
        yon=a.yon,
        dither=a.dither,
        author=a.author,
        addon_name=a.name or name,
    )
    st.music, st.midi = resolve_music("none" if a.no_music else a.music, bk, log)
    path = E.export_vxp([shot], st, a.output, log=log)
    if a.preview:
        pp = os.path.splitext(path)[0] + "_preview.png"
        preview(rgb, z, M, fovy, pl, pp, hither=a.hither, yon=a.yon)
        log("preview: " + pp)
    return path, dict(M=M, fovy=fovy, z=z, rgb=rgb, placements=pl)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", nargs="?")
    ap.add_argument("--depth", help="depth map: PNG (8/16 bit), TIFF or .npy")
    ap.add_argument(
        "--depth-kind",
        choices=["disparity", "depth", "metric"],
        default="metric",
        help="disparity: bright = near (Depth Anything, MiDaS: default); depth: bright = far; metric: real distances",
    )
    ap.add_argument(
        "--colormap",
        choices=["turbo", "jet", "viridis", "magma", "inferno", "plasma", "spectral"],
        help="the depth picture is a colour-mapped visualisation (turbo, jet...): read the colours back as values (relative only)",
    )
    ap.add_argument(
        "--invert",
        action="store_true",
        help="flip the depth map (if near/far came out reversed)",
    )
    ap.add_argument(
        "--depth-scale",
        type=float,
        default=1.0,
        help="metric: depth values * this = 3DMM units (a stock actor is about 12 units tall: ~7 units per metre)",
    )
    ap.add_argument(
        "--estimate",
        metavar="MODEL",
        help="make the depth map with a Hugging Face model, e.g. depth-anything/Depth-Anything-V2-Small-hf",
    )
    ap.add_argument("-o", "--output", default=".")
    ap.add_argument("--name")
    ap.add_argument("--author", default="")
    ap.add_argument("--bkgds", help="bkgds.3cn of your 3D Movie Maker (shared palette)")
    g = ap.add_argument_group("camera")
    g.add_argument(
        "--fov",
        type=float,
        help="vertical field of view of the picture (degrees); default: EXIF, else 45",
    )
    g.add_argument("--fov-json", help="MoGe's fov.json (fov_x / fov_y in degrees)")
    g.add_argument("--hfov", type=float, help="horizontal field of view instead")
    g.add_argument(
        "--horizon",
        type=float,
        help="where the horizon is in the whole picture, 0 = top .. 1 = bottom (0.5 = camera level)",
    )
    g.add_argument(
        "--pitch",
        type=float,
        default=None,
        help="degrees looking down (alternative to --horizon)",
    )
    g.add_argument(
        "--roll",
        type=float,
        default=None,
        help="degrees clockwise; with --auto-camera this fixes the roll instead of fitting it",
    )
    g.add_argument(
        "--max-roll",
        type=float,
        default=10.0,
        help="--auto-camera: the floor can be tilted at most this much (default 10)",
    )
    g.add_argument(
        "--floor-largest",
        action="store_true",
        help="--auto-camera: take the biggest flat surface as the floor (default: the lowest big one, so a podium doesn't count)",
    )
    g.add_argument(
        "--cam-height",
        type=float,
        help="camera height above the floor, 3DMM units (default 6)",
    )
    g.add_argument(
        "--auto-camera",
        action="store_true",
        help="metric depth: find tilt, roll and height from the floor",
    )
    g = ap.add_argument_group("picture size (the scene is always 544x306)")
    g.add_argument(
        "--fit",
        choices=["crop", "stretch", "pad", "native"],
        default="crop",
        help="crop: fill the frame, cut what sticks out (keeps aspect; default). stretch: squeeze to 16:9. "
        "pad: whole picture, aspect kept, empty bars. native: 1 pixel = 1 pixel, no resampling "
        "(bigger pictures are cropped, smaller get bars)",
    )
    g.add_argument(
        "--filter",
        choices=list(FILTERS),
        default="lanczos",
        help="resampling of the picture (nearest = none, hard pixels)",
    )
    g.add_argument(
        "--depth-filter",
        choices=list(DEPTH_FILTERS),
        default="bilinear",
        help="resampling of the depth map",
    )
    g.add_argument(
        "--pad-color",
        default="000000",
        help="colour of the bars, hex rrggbb or r,g,b (bars are empty space: actors are never hidden by them)",
    )
    g.add_argument(
        "--crop-x",
        type=float,
        default=0.5,
        help="which part is kept / where it sits: 0 = left .. 1 = right",
    )
    g.add_argument("--crop-y", type=float, default=0.5, help="0 = top .. 1 = bottom")
    g = ap.add_argument_group("depth")
    g.add_argument(
        "--ground-rows",
        type=float,
        default=0.3,
        help="the bottom part of the picture that is floor (calibration)",
    )
    g.add_argument("--near", type=float)
    g.add_argument(
        "--far", type=float, help="set the depth range yourself (3DMM units)"
    )
    g.add_argument("--blur", type=int, default=1, help="smooth the depth map (pixels)")
    g.add_argument(
        "--snap-floor",
        type=float,
        default=0.04,
        help="pixels this close (fraction) to the floor plane are put exactly on it; 0 = off",
    )
    g.add_argument(
        "--far-clip",
        type=float,
        default=400.0,
        help="farther than this = sky/nothing (never in front of an actor)",
    )
    g.add_argument("--hither", type=float, default=1.0)
    g.add_argument("--yon", type=float, default=5000.0)
    ap.add_argument(
        "--dither",
        action="store_true",
        help="dither the picture to the 3DMM palette (default: nearest colour posterizing)",
    )
    ap.add_argument("--no-music", action="store_true", help="same as --music none")
    ap.add_argument(
        "--music",
        default="",
        help="scene music: name of a 3dmm song (as in the game's list; case insensitive), a path to a .mid file (embedded in the scene), or 'none'. Default: Imaginopolis theme",
    )
    ap.add_argument(
        "--list-songs",
        action="store_true",
        help="print the stock songs found in snds.3cn and exit",
    )
    ap.add_argument(
        "--preview",
        action="store_true",
        help="also write a picture / depth / stand-in actor check image",
    )
    a = ap.parse_args(argv)
    if a.list_songs:
        bk = find_bkgds(a.bkgds)
        if not bk:
            raise SystemExit("bkgds.3cn not found: pass --bkgds PATH")
        snds = S._find_ci(os.path.dirname(bk), "snds.3cn") or os.path.join(
            os.path.dirname(bk), "snds.3cn"
        )
        songs = S.stock_songs(snds)
        print("\n".join(sorted(songs, key=str.lower)))
        print("(%d songs)" % len(songs))
        return
    if not a.image:
        ap.error("the following arguments are required: image")
    path, _ = run(a)
    print("wrote", path)


if __name__ == "__main__":
    main()
