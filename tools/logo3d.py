"""Hogs media player logo: a glossy 3D hog head whose snout carries a play button.
Signed-distance-field ray-marcher (numpy), soft shadows, AO, Blinn-Phong + fresnel. 4x supersampled."""
import numpy as np, sys, time
from PIL import Image, ImageFilter

N = int(sys.argv[1]) if len(sys.argv) > 1 else 1024
OUT = sys.argv[2] if len(sys.argv) > 2 else "logo.png"
f32 = np.float32

def norm(v): return v / np.linalg.norm(v)

def rot(yaw, pitch):
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]], f32)
    Rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]], f32)
    return Ry @ Rx

R = rot(np.radians(-16), np.radians(10))   # object turned a little to the right, seen slightly from above

def smin(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0, 1)
    return b * (1 - h) + a * h - k * h * (1 - h)

def ellipsoid(p, r):
    k0 = np.linalg.norm(p / r, axis=-1); k1 = np.linalg.norm(p / (r * r), axis=-1)
    return k0 * (k0 - 1) / np.maximum(k1, 1e-6)

def rotz(p, a):
    c, s = np.cos(a), np.sin(a); q = p.copy()
    q[..., 0] = c * p[..., 0] - s * p[..., 1]; q[..., 1] = s * p[..., 0] + c * p[..., 1]; return q

def tri2d(px, py, r):
    # equilateral triangle pointing +x, circumradius ~r (iq-style), rotated so a vertex points right
    k = np.sqrt(3.0)
    x, y = py, px                       # local 'up' = world +x: the triangle points right
    x = np.abs(x) - r; y = y + r / k
    m = x + k * y > 0
    x2 = np.where(m, (x - k * y) / 2, x); y2 = np.where(m, (-k * x - y) / 2, y)
    x2 = x2 - np.clip(x2, -2 * r, 0)
    return -np.sqrt(x2 * x2 + y2 * y2) * np.sign(y2)

SN_C = np.array([0.0, -0.20, 0.47], f32); SN_A, SN_B, SN_H = 0.43, 0.31, 0.17
def scene(p):
    """returns distance and material id (0 head, 1 snout, 2 play, 3 eye, 4 ear-inner)"""
    head = ellipsoid(p - np.array([0, 0.02, 0], f32), np.array([1.0, 0.88, 0.58], f32))
    ears = []
    for sgn in (-1, 1):
        q = p - np.array([0.56 * sgn, 0.80, 0.02], f32)
        q = rotz(q, -sgn * 0.42)
        # lean the ear forward (toward the viewer) and taper it to a point: a hog ear, not a bear's
        c, s_ = np.cos(0.55), np.sin(0.55); y, z = q[..., 1].copy(), q[..., 2].copy()
        q[..., 1] = c * y - s_ * z; q[..., 2] = s_ * y + c * z
        taper = 1.0 - 0.62 * np.clip((q[..., 1] + 0.05) / 0.50, 0, 1)
        q[..., 0] = q[..., 0] / taper
        ears.append(ellipsoid(q, np.array([0.30, 0.50, 0.075], f32)) * 0.75)
    d_head = smin(smin(head, ears[0], 0.10), ears[1], 0.10)
    # snout: elliptic rounded cylinder along z
    q = p - SN_C
    exy = np.sqrt((q[..., 0] / SN_A) ** 2 + (q[..., 1] / SN_B) ** 2) - 1.0
    dxy = exy * min(SN_A, SN_B)
    dz = np.abs(q[..., 2]) - SN_H
    rr = 0.07
    a = np.maximum(dxy + rr, 0); b = np.maximum(dz + rr, 0)
    snout = np.sqrt(a * a + b * b) + np.minimum(np.maximum(dxy + rr, dz + rr), 0) - rr
    d_hs = smin(d_head, snout, 0.07)
    # play triangle, extruded from the snout's front face
    face_z = SN_C[2] + SN_H
    q2 = p - np.array([0.04, SN_C[1], face_z], f32)
    t2 = tri2d(q2[..., 0], q2[..., 1], 0.16) - 0.025
    tz = np.abs(q2[..., 2]) - 0.055
    a = np.maximum(t2, 0); b = np.maximum(tz, 0)
    play = np.sqrt(a * a + b * b) + np.minimum(np.maximum(t2, tz), 0) - 0.012
    # eyes
    eyes = np.minimum(np.linalg.norm(p - np.array([-0.36, 0.25, 0.43], f32), axis=-1) - 0.125,
                      np.linalg.norm(p - np.array([0.36, 0.25, 0.43], f32), axis=-1) - 0.125)
    d = np.minimum(np.minimum(d_hs, play), eyes)
    mat = np.zeros(d.shape, np.int8)
    mat = np.where(snout < d_head - 0.01, 1, mat)
    mat = np.where(play <= d + 1e-4, 2, mat)
    mat = np.where(eyes <= d + 1e-4, 3, mat)
    return d, mat

def sdf(p): return scene(p)[0]

t0 = time.time()
ys, xs = np.mgrid[0:N, 0:N].astype(f32)
S = 1.32                                           # half-extent of the view in object units
cx = (xs + 0.5) / N * 2 - 1; cy = 1 - (ys + 0.5) / N * 2
ro_cam = np.stack([cx * S, cy * S + 0.03, np.full_like(cx, 4.0)], -1).reshape(-1, 3)
rd_cam = np.array([0, 0, -1], f32)
Rt = R.T
ro = ro_cam @ Rt.T; rd = (rd_cam @ Rt.T).astype(f32)
M = ro.shape[0]
t = np.zeros(M, f32); hit = np.zeros(M, bool); alive = np.ones(M, bool)
for i in range(260):
    idx = np.nonzero(alive)[0]
    if idx.size == 0: break
    p = ro[idx] + t[idx, None] * rd
    d = sdf(p)
    h = d < 2e-4
    hit[idx[h]] = True; alive[idx[h]] = False
    t[idx] += np.where(h, 0, d * 0.7)
    far = t[idx] > 8.0
    alive[idx[far]] = False
print("march", round(time.time() - t0, 1), "s, hits", int(hit.sum()))

hi = np.nonzero(hit)[0]
P = ro[hi] + t[hi, None] * rd
_, MAT = scene(P)
e = 1.2e-3
Nn = np.stack([sdf(P + [e, 0, 0]) - sdf(P - [e, 0, 0]),
               sdf(P + [0, e, 0]) - sdf(P - [0, e, 0]),
               sdf(P + [0, 0, e]) - sdf(P - [0, 0, e])], -1)
Nn /= np.linalg.norm(Nn, axis=-1, keepdims=True) + 1e-9

def softshadow(P, L, k=10.0):
    res = np.ones(P.shape[0], f32); tt = np.full(P.shape[0], 0.02, f32)
    for _ in range(48):
        d = sdf(P + tt[:, None] * L)
        res = np.minimum(res, k * d / tt)
        tt += np.clip(d, 0.01, 0.2)
    return np.clip(res, 0, 1)

def ao(P, Nn):
    occ = np.zeros(P.shape[0], f32); sca = 1.0
    for i in range(5):
        hh = 0.02 + 0.07 * i
        d = sdf(P + Nn * hh)
        occ += (hh - d) * sca; sca *= 0.75
    return np.clip(1 - 2.2 * occ, 0, 1)

V = -rd
L1 = norm(np.array([-0.55, 0.75, 0.70], f32)) @ Rt.T   # key light, upper-left front (camera space -> object)
L2 = norm(np.array([0.70, -0.15, 0.55], f32)) @ Rt.T   # cool fill from the right
L3 = norm(np.array([0.10, 0.60, -0.80], f32)) @ Rt.T   # rim light from behind
sh = softshadow(P, L1); occ = ao(P, Nn)

base = np.array([[1.00, 0.40, 0.66],    # head: hot pink
                 [1.00, 0.72, 0.84],    # snout: light pink
                 [0.98, 0.98, 1.00],    # play: white
                 [0.05, 0.04, 0.07],    # eyes: glossy black
                 [0.90, 0.30, 0.55]], f32)
spec_pow = np.array([48, 40, 160, 220, 48], f32)[MAT]
spec_k = np.array([0.55, 0.45, 0.9, 1.4, 0.5], f32)[MAT]
col = base[MAT]
# subtle vertical gradient on the head for depth (darker toward the chin)
grad = np.clip(0.80 + 0.30 * (P[:, 1] + 0.9) / 1.8, 0.7, 1.1)[:, None]
col = np.where((MAT == 0)[:, None], col * grad, col)

def lam(L): return np.clip(Nn @ L, 0, 1)
def blinn(L, pw):
    H = (L + V); H /= np.linalg.norm(H, axis=-1, keepdims=True)
    return np.clip(np.sum(Nn * H, -1), 0, 1) ** pw

ambient = 0.22 * occ
diff = lam(L1) * sh * 0.95 + lam(L2) * 0.30 + 0.10 * np.clip(Nn[:, 1] * 0.5 + 0.5, 0, 1)
c = col * (ambient + diff)[:, None]
c += (spec_k * blinn(L1, spec_pow) * sh)[:, None] * np.array([1.0, 0.97, 0.95], f32)
c += (0.35 * spec_k * blinn(L2, spec_pow * 0.5))[:, None] * np.array([0.75, 0.85, 1.0], f32)
fres = (1 - np.clip(np.sum(Nn * V, -1), 0, 1)) ** 3
c += (fres * 0.55 * lam(L3) + fres * 0.18)[:, None] * np.array([1.0, 0.80, 0.92], f32)
# play button: faint cyan inner glow so the "media" read pops at small sizes
c = np.where((MAT == 2)[:, None], c * 1.15 + 0.10, c)   # bright white play button
c = c / (1 + 0.25 * c)                        # soft tonemap
c = np.clip(c ** (1 / 1.05), 0, 1)

img = np.zeros((M, 4), f32)
img[hi, :3] = c; img[hi, 3] = 1
img = img.reshape(N, N, 4)
im = Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGBA")
# soft drop shadow beneath for depth
a = np.array(im)[..., 3]
shadow = Image.fromarray(a).filter(ImageFilter.GaussianBlur(N / 40))
shadow = Image.fromarray((np.array(shadow) * 0.45).astype(np.uint8))
canvas = Image.new("RGBA", (N, N), (0, 0, 0, 0))
sh_img = Image.new("RGBA", (N, N), (40, 10, 30, 0)); sh_img.putalpha(shadow)
canvas.alpha_composite(sh_img, (int(N * 0.015), int(N * 0.035)))
canvas.alpha_composite(im)
canvas.save(OUT)
print("done", OUT, round(time.time() - t0, 1), "s")
