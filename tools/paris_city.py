"""
paris_city.py  --  Procedural Haussmann-style city generator for Blender (4.x)

Map: 25.1 km (X, tiles seamlessly) x 32 km (Y, open ends with a 1 km green border).
A river helixes from one short edge to the other in 5 laps; boulevards bridge it,
minor streets stop at the quay.

Run in Blender's Text Editor, or:  blender --background --python paris_city.py

The full map is ~800 km^2 (roughly 375k buildings), so REGION controls how much geometry
is actually built. Hubs, boulevards and the river are always computed for the whole map
and are deterministic per SEED, so regions generated separately line up with each other.

    REGION = "river"            5 km window on the river at mid-length (default)
    REGION = (x0, y0, x1, y1)   explicit window in metres
    REGION = None               the entire map (very slow, very heavy)

Measured on the default seed: a 5.5 x 5 km region takes about 80 s and yields ~12.5k
buildings; the whole map is ~160x that area, so expect the better part of an hour and a
scene of roughly ten million triangles. Generating a few regions is usually the better move.

Output, inside a collection named "ParisCity":
    Chunk_XXX_YYY                    Empty per chunk (502 x 500 m)
      Chunk_XXX_YYY_Terrain          ground, river, islands, parks, roads, roundabouts
      Chunk_XXX_YYY_Buildings        Empty (LOD group parent)
        ..._Buildings_LOD0           full detail (courtyards + street passages)
        ..._Buildings_LOD1           solid prisms (hidden in viewport/render)
      Monument_NNN                   Empty on top of each roundabout platform
"""

import math
import random
import time
import zlib

import numpy as np

try:
    import bpy
except ImportError:          # the geometry stages also run in plain CPython for testing
    bpy = None

# --------------------------------------------------------------------------- #
# CONFIG
# --------------------------------------------------------------------------- #
SEED = 7

MAP_W = 25100.0             # X, periodic: x and x + MAP_W are the same place
MAP_H = 32000.0             # Y, open ends
GREEN_BORDER = 1000.0       # green band along both 25.1 km edges (river passes through)
REGION = None            # "river" | None | (x0, y0, x1, y1)
REGION_SIZE = 5000.0        # size of the "river" sample window
WORK_MARGIN = 3000.0        # network context generated around the region

CHUNK_X = 502.0             # 25100 / 502 = 50 chunks, so chunks tile with the map
CHUNK_Y = 500.0             # 32000 / 500 = 64 chunks

# Hubs / boulevards
# Hub spacing is 500 m / 1 km scaled by √2, which halves the roundabout density.
# _lattice_basis's lattice integers are scaled to match.
HUB_MIN_DIST = 707.0
HUB_NEAR_DIST = 1414.0
HUB_LINKS_WANTED = 5
HUB_LINKS_MIN = 3           # soft floor: 3 or 4 links is acceptable
CARDINAL_EXCLUSION = 20.0   # degrees
HUB_SWEEPS = 14             # random-walk sweeps over all hubs (more = less lattice-like)
HUB_JITTER = 78.0

# River
RIVER_LAPS = 5              # complete laps around the cylinder between the two short edges
RIVER_START_X = 4000.0      # where it meets the y = 0 edge
RIVER_END_X = 16000.0       # where it meets the y = MAP_H edge
RIVER_END_TAPER = 3000.0    # meanders fade out over this much arc at each end
RIVER_STEP = 20.0           # centreline sampling
RIVER_W_MIN, RIVER_W_MAX = 50.0, 300.0
ISLAND_WIDTH = 50.0         # middle 50 m where the river is widest
ISLAND_MIN_WIDTH = 0.9 * RIVER_W_MAX   # river width at which an island starts to appear
ISLAND_MIN_LENGTH = 120.0
RIVER_SETBACK = 12.0        # quay: nothing is built within this of the bank
QUAI_ROADS = True           # minor street following each bank (never crosses, only follows)
QUAI_STEP = 40.0
BRIDGE_HEIGHT = 6.0
BRIDGE_RAMP = 80.0
MAX_BRIDGE_SPAN = 900.0     # 3 × the widest river; boulevards that would graze the river for longer are dropped

# Roads
MAJOR_WIDTH = 30.0
MINOR_WIDTH = 8.0
CURB = 3.0
BLOCK_DEPTH = 90.0          # street edge to street edge: 3 + 40 + 4 (alley) + 40 + 3
STREET_PITCH = BLOCK_DEPTH + MINOR_WIDTH
MIN_BEND = 50.0
MAX_BEND_ANGLE = 60.0
STREET_END_MARGIN = 70.0
SUPERBLOCK_SPLIT_AREA = 1.2e6   # m²; larger superblocks are halved by a minor street

# Roundabouts / monuments
#   name, roadway radius, platform radius, share out of ROUNDABOUT_MULTIPLE
ROUNDABOUT_TYPES = [("S", 45.0, 15.0, 31),
                    ("M", 70.0, 40.0, 17),
                    ("L", 100.0, 70.0, 2)]
ROUNDABOUT_MULTIPLE = 50    # hub count is trimmed to a multiple of this so the ratio is exact
ROUNDABOUT_RADIUS = 45.0    # smallest type; used for hub placement clearance
PLATFORM_HEIGHT = 1.5
LARGE_MIN_SEPARATION = 2500.0
MONUMENT_S = (1.5, 5.0)     # gold cylinder: radius, height
MONUMENT_M = (3.0, 10.0)
MONUMENT_L = (65.0, 40.0)   # off-white drum, plus a 65 m hemisphere on top (~105 m total)

# Green zones
GREEN_ZONE_SHARE = 0.075    # fraction of superblocks left entirely as parkland

# Buildings
COURTYARD_THRESHOLD = 60.0
COURTYARD_WALL = 10.0
PASSAGE_WIDTH = 2.0
PASSAGE_HEIGHT = 3.0

# Parks
PARK_CELL = 2.5
PARK_MIN_AREA = 500.0
PARK_OPENING = 5.0

# Z layering
# ground layers are stacked a few cm apart so they never z-fight; swap them for a single
# terrain material with masks once you move to real textures
Z_GROUND, Z_WATER, Z_PARK, Z_ISLAND = 0.0, 0.05, 0.10, 0.15
Z_MINOR, Z_MAJOR, Z_ROUNDABOUT = 0.20, 0.25, 0.30

COL_GROUND = (0.60, 0.52, 0.39)
COL_BUILDING = (0.87, 0.81, 0.68)
COL_ROAD = (0.33, 0.33, 0.34)
COL_PARK = (0.30, 0.52, 0.20)
COL_PLATFORM = (0.78, 0.76, 0.72)
COL_WATER = (0.16, 0.27, 0.34)
MAT_GROUND, MAT_PARK, MAT_ROAD, MAT_PLATFORM, MAT_WATER, MAT_GOLD, MAT_MONUMENT = range(7)
MAT_BUILDING = 7

COL_GOLD = (0.83, 0.66, 0.20)
COL_MONUMENT = (0.93, 0.91, 0.86)


# --------------------------------------------------------------------------- #
# 2D helpers (X is periodic: use wrapdx for any x difference)
# --------------------------------------------------------------------------- #
def sub(a, b): return (a[0] - b[0], a[1] - b[1])
def add(a, b): return (a[0] + b[0], a[1] + b[1])
def mul(a, s): return (a[0] * s, a[1] * s)
def dot(a, b): return a[0] * b[0] + a[1] * b[1]
def cross(a, b): return a[0] * b[1] - a[1] * b[0]
def length(a): return math.hypot(a[0], a[1])
def left(a): return (-a[1], a[0])


def wrapdx(dx):
    return dx - MAP_W * round(dx / MAP_W)


def wrap_near(x, ref):
    """The image of x (which may be many laps out, as river coordinates are) nearest ref."""
    return x - MAP_W * round((x - ref) / MAP_W)


def unit(a):
    l = length(a)
    return (a[0] / l, a[1] / l) if l > 1e-12 else (0.0, 0.0)


def poly_area(p):
    return 0.5 * sum(cross(p[i], p[(i + 1) % len(p)]) for i in range(len(p)))


def point_in_poly(p, poly):
    inside = False
    n = len(poly)
    for k in range(n):
        ax, ay = poly[k]
        bx, by = poly[(k + 1) % n]
        if (ay > p[1]) != (by > p[1]) and p[0] < ax + (p[1] - ay) / (by - ay) * (bx - ax):
            inside = not inside
    return inside


def bearing_ok(dx, dy):
    a = math.degrees(math.atan2(dy, dx)) % 90.0
    return CARDINAL_EXCLUSION < a < 90.0 - CARDINAL_EXCLUSION


def rect_poly(a, b, half):
    n = mul(left(unit(sub(b, a))), half)
    return [sub(a, n), sub(b, n), add(b, n), add(a, n)]


def circle_poly(c, r, n=16):
    return [(c[0] + r * math.cos(2 * math.pi * i / n), c[1] + r * math.sin(2 * math.pi * i / n))
            for i in range(n)]


def seg_intersect(p, p2, q, q2, eps=1e-9):
    r, s = sub(p2, p), sub(q2, q)
    den = cross(r, s)
    if abs(den) < eps:
        return None
    qp = sub(q, p)
    return cross(qp, s) / den, cross(qp, r) / den


def clip_segment(a, b, x0, y0, x1, y1):
    dx, dy = b[0] - a[0], b[1] - a[1]
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, a[0] - x0), (dx, x1 - a[0]), (-dy, a[1] - y0), (dy, y1 - a[1])):
        if abs(p) < 1e-12:
            if q < 0:
                return None
        else:
            t = q / p
            if p < 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
    if t0 > t1 - 1e-9:
        return None
    return ((a[0] + t0 * dx, a[1] + t0 * dy), (a[0] + t1 * dx, a[1] + t1 * dy))


def convex_overlap(A, B, eps=0.05):
    for poly in (A, B):
        n = len(poly)
        for i in range(n):
            x1, y1 = poly[i]
            x2, y2 = poly[(i + 1) % n]
            ax, ay = y1 - y2, x2 - x1
            l = math.hypot(ax, ay)
            if l < 1e-9:
                continue
            ax, ay = ax / l, ay / l
            pa = [ax * x + ay * y for x, y in A]
            pb = [ax * x + ay * y for x, y in B]
            if max(pa) <= min(pb) + eps or max(pb) <= min(pa) + eps:
                return False
    return True


def sub_rng(*key):
    s = "|".join(f"{v:.1f}" if isinstance(v, float) else str(v) for v in key)
    return random.Random(zlib.crc32(s.encode()) ^ (SEED * 2654435761 & 0xFFFFFFFF))


class SpatialHash:
    def __init__(self, cell=100.0):
        self.cell = cell
        self.grid = {}
        self.polys = []
        self.boxes = []

    @staticmethod
    def bbox(poly):
        xs = [p[0] for p in poly]
        ys = [p[1] for p in poly]
        return min(xs), min(ys), max(xs), max(ys)

    def _cells(self, box):
        c = self.cell
        for i in range(int(math.floor(box[0] / c)), int(math.floor(box[2] / c)) + 1):
            for j in range(int(math.floor(box[1] / c)), int(math.floor(box[3] / c)) + 1):
                yield i, j

    def add(self, poly):
        box = self.bbox(poly)
        idx = len(self.polys)
        self.polys.append(poly)
        self.boxes.append(box)
        for k in self._cells(box):
            self.grid.setdefault(k, []).append(idx)

    def hits(self, poly):
        box = self.bbox(poly)
        seen = set()
        for k in self._cells(box):
            for idx in self.grid.get(k, ()):
                if idx in seen:
                    continue
                seen.add(idx)
                b = self.boxes[idx]
                if b[0] >= box[2] or b[2] <= box[0] or b[1] >= box[3] or b[3] <= box[1]:
                    continue
                if convex_overlap(poly, self.polys[idx]):
                    return True
        return False


# --------------------------------------------------------------------------- #
# RIVER
# --------------------------------------------------------------------------- #
class River:
    """Helical centreline: enters at (RIVER_START_X, 0), leaves at (RIVER_END_X, MAP_H),
    and makes exactly RIVER_LAPS crossings of the x seam on the way. Meanders are applied
    perpendicular to that base direction, so they read as meanders whatever the drift angle.
    Width varies 50-300 m; where it is widest a tapered island takes the middle 50 m."""

    def __init__(self, rng):
        drift = RIVER_LAPS * MAP_W + (RIVER_END_X - RIVER_START_X)
        base_len = math.hypot(drift, MAP_H)
        ux, uy = drift / base_len, MAP_H / base_len       # along the helix
        nx, ny = -uy, ux                                  # perpendicular to it
        t = np.arange(0.0, base_len, 10.0)

        env = np.clip(np.minimum(t, base_len - t) / RIVER_END_TAPER, 0.0, 1.0)
        env = env * env * (3 - 2 * env)                   # meanders vanish at both mouths
        meanders = [(amp, lam, rng.uniform(0, 2 * np.pi))
                    for amp, lam in ((620.0, 7300.0), (260.0, 2600.0), (95.0, 880.0), (40.0, 360.0))]

        w = np.zeros_like(t)
        for amp, lam in ((1.0, 5200.0), (0.55, 1700.0), (0.3, 640.0)):
            w = w + amp * np.sin(2 * np.pi * t / lam + rng.uniform(0, 2 * np.pi))
        w = (w - w.min()) / max(1e-6, float(w.max() - w.min()))
        w = RIVER_W_MIN + (RIVER_W_MAX - RIVER_W_MIN) * w

        # A bank offset by more than the bend radius folds over itself (bow-tie water
        # quads, holes in the occupancy raster). Where the river is wide, damp the short
        # meanders so the worst-case curvature, every term peaking at once, stays under
        # 1 / (half-width + setback + margin). Narrow stretches keep the full wiggle.
        def kappa(terms):
            return sum(amp * (2 * np.pi / lam) ** 2 for amp, lam, _ in terms)
        long_m = [m for m in meanders if m[1] >= 2000.0]
        short_m = [m for m in meanders if m[1] < 2000.0]
        k_ok = 1.0 / (w / 2.0 + RIVER_SETBACK + 10.0)
        damp = np.clip((k_ok - kappa(long_m)) / kappa(short_m), 0.0, 1.0)
        off = np.zeros_like(t)
        for amp, lam, ph in meanders:
            a = amp * damp if lam < 2000.0 else amp
            off = off + a * env * np.sin(2 * np.pi * t / lam + ph)
        xs = RIVER_START_X + ux * t + nx * off
        ys = uy * t + ny * off

        seg = np.hypot(np.diff(xs), np.diff(ys))          # resample at uniform arclength
        s = np.concatenate([[0.0], np.cumsum(seg)])
        su = np.append(np.arange(0.0, s[-1], RIVER_STEP), s[-1])   # keep the exact mouth
        self.x = np.interp(su, s, xs)
        self.y = np.clip(np.interp(su, s, ys), 0.0, MAP_H)
        self.w = np.interp(su, s, w)

        f = np.clip((self.w - ISLAND_MIN_WIDTH) / (RIVER_W_MAX - ISLAND_MIN_WIDTH), 0.0, 1.0)
        self.ih = (ISLAND_WIDTH / 2.0) * (f * f * (3 - 2 * f))     # smooth taper
        self._drop_short_islands()

        tx, ty = np.gradient(self.x), np.gradient(self.y)
        tl = np.hypot(tx, ty)
        self.nx, self.ny = -ty / tl, tx / tl

        # nearest() searches one cell around the query, so a cell must cover the widest
        # half-width plus the farthest bank distance anyone asks about (~250 m).
        self.cell = RIVER_W_MAX / 2.0 + 250.0
        self.ncx = int(math.ceil(MAP_W / self.cell))
        self.grid = {}
        for i, (px, py) in enumerate(zip(self.x % MAP_W, self.y)):
            self.grid.setdefault((int(px // self.cell) % self.ncx,
                                  int(py // self.cell)), []).append(i)

    def _drop_short_islands(self):
        on = self.ih > 0.5
        i = 0
        while i < len(on):
            if on[i]:
                j = i
                while j < len(on) and on[j]:
                    j += 1
                if (j - i) * RIVER_STEP < ISLAND_MIN_LENGTH:
                    self.ih[i:j] = 0.0
                i = j
            else:
                i += 1

    def nearest(self, p):
        """(distance to the centreline, sample index), periodic in X."""
        cx = int((p[0] % MAP_W) // self.cell)
        cy = int(p[1] // self.cell)
        best = (1e18, -1)
        for i in range(cx - 1, cx + 2):
            for j in range(cy - 1, cy + 2):
                for k in self.grid.get((i % self.ncx, j), ()):
                    d = math.hypot(wrapdx(p[0] - self.x[k]), p[1] - self.y[k])
                    if d < best[0]:
                        best = (d, k)
        return best

    def signed(self, p):
        """Distance outside the bank; negative means in the water (or on an island)."""
        d, k = self.nearest(p)
        return 1e6 if k < 0 else d - self.w[k] / 2.0

    def blocks(self, p, extra=RIVER_SETBACK):
        return self.signed(p) < extra

    def bank_points(self, i, half):
        c = (float(self.x[i]), float(self.y[i]))
        n = (float(self.nx[i]), float(self.ny[i]))
        return sub(c, mul(n, half)), add(c, mul(n, half))

    def bridge_z(self, p):
        sd = self.signed(p)
        if sd >= BRIDGE_RAMP:
            return 0.0
        return BRIDGE_HEIGHT * min(1.0, max(0.0, 1.0 - max(0.0, sd) / BRIDGE_RAMP))


# --------------------------------------------------------------------------- #
# HUBS  (sheared lattice on the X-cylinder, then a constraint-preserving walk)
# --------------------------------------------------------------------------- #
def _lattice_basis(rng):
    """Bases whose integer span contains (MAP_W, 0), so the layout tiles in X."""
    for _ in range(200):
        nn = rng.choice([13, 14])
        mm = rng.choice([18, 19])
        L = 2 * MAP_W / (nn * (1 + math.sqrt(3)))
        d = math.sqrt(2) * nn * L / (2 * mm)
        e1 = (d / math.sqrt(2), -d / math.sqrt(2))
        e2 = (L * math.cos(math.radians(30)), L * math.sin(math.radians(30)))
        k = rng.uniform(-0.05, 0.05)
        sy = rng.uniform(0.97, 1.03)
        e1 = (e1[0] + k * e1[1] * sy, e1[1] * sy)
        e2 = (e2[0] + k * e2[1] * sy, e2[1] * sy)
        ok, links = True, 0
        for i in range(-3, 4):
            for j in range(-3, 4):
                if i == j == 0:
                    continue
                v = add(mul(e1, i), mul(e2, j))
                l = length(v)
                if l < HUB_MIN_DIST + 3:
                    ok = False
                elif l <= HUB_NEAR_DIST:
                    if bearing_ok(v[0], v[1]):
                        links += 1
                    else:
                        ok = False
        if ok and links >= HUB_LINKS_WANTED:
            return e1, e2
    raise RuntimeError("no valid lattice basis found")


def generate_hubs(rng, river):
    e1, e2 = _lattice_basis(rng)
    det = cross(e1, e2)
    ylo, yhi = GREEN_BORDER + 80.0, MAP_H - GREEN_BORDER - 80.0
    idx = [(cross(c, e2) / det, cross(e1, c) / det)
           for c in ((0, ylo), (MAP_W, ylo), (0, yhi), (MAP_W, yhi))]
    i0 = int(min(a for a, _ in idx)) - 2
    i1 = int(max(a for a, _ in idx)) + 2
    j0 = int(min(b for _, b in idx)) - 2
    j1 = int(max(b for _, b in idx)) + 2
    seen, pts = set(), []
    for i in range(i0, i1 + 1):
        for j in range(j0, j1 + 1):
            p = add(mul(e1, i), mul(e2, j))
            if not (ylo <= p[1] <= yhi):
                continue
            q = (p[0] % MAP_W, p[1])
            key = (round(q[0] / 5), round(q[1] / 5))
            if key in seen:
                continue
            seen.add(key)
            pts.append([q[0], q[1]])

    ncx = int(MAP_W // HUB_NEAR_DIST)    # cells at least one link long
    cw = MAP_W / ncx
    grid = {}

    def cell(p):
        return int(p[0] // cw) % ncx, int(p[1] // cw)

    for i, p in enumerate(pts):
        grid.setdefault(cell(p), []).append(i)

    def neighbours(p, skip=-1):
        ci, cj = cell(p)
        out = []
        for a in range(ci - 1, ci + 2):
            for b in range(cj - 1, cj + 2):
                for k in grid.get((a % ncx, b), ()):
                    if k != skip:
                        out.append(k)
        return out

    def cost(i, pos=None):
        p = pos if pos is not None else pts[i]
        c, e = 0, 0.0
        for j in neighbours(p, i):
            dx, dy = wrapdx(p[0] - pts[j][0]), p[1] - pts[j][1]
            d = math.hypot(dx, dy)
            if d < HUB_MIN_DIST:
                e += (HUB_MIN_DIST - d) ** 2 / 200.0
            if d <= HUB_NEAR_DIST and bearing_ok(dx, dy):
                c += 1
        e += 40.0 * max(0, HUB_LINKS_MIN - c) + 4.0 * max(0, HUB_LINKS_WANTED - c)
        clearance = river.signed(p) - (ROUNDABOUT_RADIUS + 30.0)
        if clearance < 0:
            e += 2.0 * (-clearance)
        return e

    walk = random.Random(SEED * 7919 + 13)
    for _ in range(HUB_SWEEPS):
        order = list(range(len(pts)))
        walk.shuffle(order)
        for i in order:
            old = list(pts[i])
            new = [(old[0] + walk.gauss(0, HUB_JITTER)) % MAP_W,
                   min(yhi, max(ylo, old[1] + walk.gauss(0, HUB_JITTER)))]
            if any(math.hypot(wrapdx(new[0] - pts[j][0]), new[1] - pts[j][1]) < HUB_MIN_DIST
                   for j in neighbours(new, i)):
                continue                      # 500 m separation is a hard rule
            affected = set(neighbours(old, i)) | set(neighbours(new, i)) | {i}
            before = sum(cost(k) for k in affected)
            oc = cell(old)
            grid[oc].remove(i)
            pts[i] = new
            grid.setdefault(cell(new), []).append(i)
            if sum(cost(k) for k in affected) > before:
                grid[cell(new)].remove(i)
                pts[i] = old
                grid.setdefault(oc, []).append(i)

    return [tuple(p) for p in pts if river.signed(p) > ROUNDABOUT_RADIUS + 10.0]


def water_span(river, a, b):
    """Metres of a straight segment that lie in the water."""
    L = math.dist(a, b)
    if L < 1e-6:
        return 0.0
    t = unit(sub(b, a))
    if min(river.signed(a), river.signed(b),
           river.signed(add(a, mul(t, L / 2)))) > 600.0:
        return 0.0
    n = max(2, int(L / 15.0))
    inside = sum(1 for k in range(n + 1)
                 if river.signed(add(a, mul(t, L * k / n))) < 0.0)
    return inside * L / n


def build_edges(hubs, banned=()):
    ncx = int(MAP_W // HUB_NEAR_DIST)    # cells at least one link long
    cw = MAP_W / ncx
    grid = {}
    for i, p in enumerate(hubs):
        grid.setdefault((int(p[0] // cw) % ncx, int(p[1] // cw)), []).append(i)
    edges, counts = set(), []
    for i, p in enumerate(hubs):
        ci, cj = int(p[0] // cw) % ncx, int(p[1] // cw)
        cand = []
        for a in range(ci - 1, ci + 2):
            for b in range(cj - 1, cj + 2):
                for j in grid.get((a % ncx, b), ()):
                    if j == i:
                        continue
                    dx, dy = wrapdx(hubs[j][0] - p[0]), hubs[j][1] - p[1]
                    d = math.hypot(dx, dy)
                    if d <= HUB_NEAR_DIST and bearing_ok(dx, dy):
                        cand.append((d, j))
        cand.sort()
        cand = [(d, j) for d, j in cand if (min(i, j), max(i, j)) not in banned]
        counts.append(min(len(cand), HUB_LINKS_WANTED))
        for _, j in cand[:HUB_LINKS_WANTED]:
            edges.add((min(i, j), max(i, j)))
    return sorted(edges), counts


def finalize_network(hubs, river):
    """Cull unbridgeable boulevards and under-connected hubs, trim the hub count to a
    multiple of ROUNDABOUT_MULTIPLE, then size every roundabout. Shared by the generator
    and the whole-map preview so both see the same network."""
    # Under-connected hubs are culled until none remain, and only then is the count
    # trimmed. Trimming inside the cull loop wastes up to ROUNDABOUT_MULTIPLE - 1 hubs
    # per pass, because each trim can leave a neighbour short and restart the cascade.
    # Trim candidates are hubs whose loss leaves every neighbour at or above the floor,
    # picked one at a time so two picks can't both draw down a shared neighbour.
    while True:
        long_bridge = {e for e in build_edges(hubs)[0]
                       if water_span(river, *edge_segment(hubs, e)) > MAX_BRIDGE_SPAN}
        edges, counts = build_edges(hubs, banned=long_bridge)
        drop = {i for i, c in enumerate(counts) if c < HUB_LINKS_MIN}
        if not drop:
            extra = len(hubs) % ROUNDABOUT_MULTIPLE
            if not extra:
                break
            adj = [[] for _ in hubs]
            for a, b in edges:
                adj[a].append(b)
                adj[b].append(a)
            left = list(counts)
            rank = sorted((c, round(hubs[i][0], 1), round(hubs[i][1], 1), i)
                          for i, c in enumerate(counts))
            drop = set()
            for *_, i in rank:
                if len(drop) == extra:
                    break
                if any(left[j] <= HUB_LINKS_MIN for j in adj[i] if j not in drop):
                    continue
                drop.add(i)
                for j in adj[i]:
                    left[j] -= 1
            for *_, i in rank:              # nothing safe left: fall back to weakest
                if len(drop) == extra:
                    break
                drop.add(i)
        hubs = [h for i, h in enumerate(hubs) if i not in drop]
    return hubs, edges, counts, assign_roundabout_sizes(hubs, river), len(long_bridge)


def assign_roundabout_sizes(hubs, river):
    """Deterministic S/M/L assignment in exactly the configured ratio. Larger types need
    room from the water, and the large ones are spread out rather than clustered."""
    n = len(hubs)
    per = n // ROUNDABOUT_MULTIPLE
    want = [per * share for _, _, _, share in ROUNDABOUT_TYPES]
    want[0] = n - want[1] - want[2]                     # remainder goes to the small type
    rng = sub_rng("roundabout-sizes", n)
    order = list(range(n))
    rng.shuffle(order)
    sizes = [0] * n
    taken = []
    for t in (2, 1):                                    # large first, then medium
        _, road_r, _, _ = ROUNDABOUT_TYPES[t]
        need = want[t]
        sep = LARGE_MIN_SEPARATION if t == 2 else 0.0
        while need > 0:
            for i in order:
                if need == 0:
                    break
                if sizes[i] != 0 or river.signed(hubs[i]) < road_r + 20.0:
                    continue
                if sep > 0 and any(math.hypot(wrapdx(hubs[i][0] - hubs[j][0]),
                                              hubs[i][1] - hubs[j][1]) < sep for j in taken):
                    continue
                sizes[i] = t
                need -= 1
                if t == 2:
                    taken.append(i)
            if sep > 0:
                sep *= 0.6                              # relax until the quota is met
                if sep < 200:
                    sep = 0.0
            elif need > 0:
                break                                   # no room left near the water
    return sizes


def hub_radius(sizes, i):
    return ROUNDABOUT_TYPES[sizes[i]][1]


def edge_segment(hubs, e):
    a, b = hubs[e[0]], hubs[e[1]]
    return a, (a[0] + wrapdx(b[0] - a[0]), b[1])


# --------------------------------------------------------------------------- #
# PLANAR ARRANGEMENT -> SUPERBLOCKS
# --------------------------------------------------------------------------- #
def build_faces(segments):
    """segments: [(a, b, road_id)], road_id < 0 = boundary. Returns [(pts CCW, [rid])]."""
    cell = 1000.0
    buckets = {}
    for i, (a, b, _) in enumerate(segments):
        x0, x1 = sorted((a[0], b[0]))
        y0, y1 = sorted((a[1], b[1]))
        for ci in range(int(x0 // cell), int(x1 // cell) + 1):
            for cj in range(int(y0 // cell), int(y1 // cell) + 1):
                buckets.setdefault((ci, cj), []).append(i)
    pairs = set()
    for ids in buckets.values():
        for a_ in range(len(ids)):
            for b_ in range(a_ + 1, len(ids)):
                pairs.add((ids[a_], ids[b_]))

    params = [[0.0, 1.0] for _ in segments]
    for i, j in pairs:
        a, b, _ = segments[i]
        c, d, _ = segments[j]
        r = seg_intersect(a, b, c, d)
        if r is None:
            continue
        t, u = r
        if -1e-6 <= t <= 1 + 1e-6 and -1e-6 <= u <= 1 + 1e-6:
            params[i].append(min(max(t, 0.0), 1.0))
            params[j].append(min(max(u, 0.0), 1.0))

    nodes, node_of = [], {}

    def node_id(p):
        k = (round(p[0] * 20), round(p[1] * 20))
        if k not in node_of:
            node_of[k] = len(nodes)
            nodes.append(p)
        return node_of[k]

    half = {}
    for (a, b, rid), ts in zip(segments, params):
        ts = sorted(set(round(t, 9) for t in ts))
        ids = [node_id((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))) for t in ts]
        for u, v in zip(ids, ids[1:]):
            if u != v:
                half[(u, v)] = rid
                half[(v, u)] = rid

    out = {}
    for (u, v) in half:
        out.setdefault(u, []).append(v)
    for u in out:
        pu = nodes[u]
        out[u].sort(key=lambda w: math.atan2(nodes[w][1] - pu[1], nodes[w][0] - pu[0]))

    used, faces = set(), []
    for start in half:
        if start in used:
            continue
        loop, e = [], start
        while e not in used:
            used.add(e)
            loop.append(e)
            u, v = e
            nb = out[v]
            e = (v, nb[nb.index(u) - 1])
        pts = [nodes[a] for a, _ in loop]
        if len(pts) >= 3 and poly_area(pts) > 1.0:
            faces.append((pts, [half[x] for x in loop]))
    return faces


# --------------------------------------------------------------------------- #
# MINOR STREETS
# --------------------------------------------------------------------------- #
class GreenZones:
    """Superblocks set aside as parkland: no minor streets, no buildings on their frontage."""

    def __init__(self, faces):
        self.cell = 500.0
        self.grid = {}
        self.polys = []
        for pts in faces:
            idx = len(self.polys)
            self.polys.append(pts)
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            for i in range(int(min(xs) // self.cell), int(max(xs) // self.cell) + 1):
                for j in range(int(min(ys) // self.cell), int(max(ys) // self.cell) + 1):
                    self.grid.setdefault((i, j), []).append(idx)

    def contains(self, p):
        return any(point_in_poly(p, self.polys[idx])
                   for idx in self.grid.get((int(p[0] // self.cell), int(p[1] // self.cell)), ()))


def is_green_face(pts):
    cx = sum(p[0] for p in pts) / len(pts)
    cy = sum(p[1] for p in pts) / len(pts)
    return sub_rng("green-zone", round(cx % MAP_W, 1), round(cy, 1)).random() < GREEN_ZONE_SHARE


def ray_face(p, d, pts):
    best = None
    n = len(pts)
    for i in range(n):
        r = seg_intersect(p, add(p, d), pts[i], pts[(i + 1) % n])
        if r is None:
            continue
        t, u = r
        if t > 1e-3 and -1e-9 <= u <= 1 + 1e-9 and (best is None or t < best[0]):
            best = (t, i)
    return best


def segs_cross(a, b, c, d):
    r = seg_intersect(a, b, c, d)
    return r is not None and 1e-6 < r[0] < 1 - 1e-6 and 1e-6 < r[1] < 1 - 1e-6


def cut_at_obstacles(line, river, discs=()):
    """Minor streets never bridge and never cross a roundabout: truncate them. A street
    cut by the river resumes on the far bank (that piece starts at the quay), so land
    beyond the river still gets streets. Returns the pieces, each a polyline."""
    def in_disc(p):
        return any(math.hypot(wrapdx(p[0] - c[0]), p[1] - c[1]) < r for c, r in discs)

    if river.blocks(line[0]) or in_disc(line[0]):
        return []
    pieces, out = [], [line[0]]     # out is None while crossing the river
    for a, b in zip(line, line[1:]):
        L = math.dist(a, b)
        steps = max(1, int(L / 10.0))
        t = unit(sub(b, a))
        for k in range(1, steps + 1):
            p = add(a, mul(t, L * k / steps))
            wet = river.blocks(p)
            if out is not None and (wet or in_disc(p)):
                hit = add(a, mul(t, max(0.0, L * (k - 1) / steps - 10.0)))
                if math.dist(out[-1], hit) > 30.0:
                    out.append(hit)
                if len(out) > 1:
                    pieces.append(out)
                if not wet:
                    return pieces               # roundabout: the street ends here
                out = None
            elif out is None and not wet:
                if in_disc(p):
                    return pieces
                out = [p]
        if out is not None and math.dist(out[-1], b) > 1e-6:
            out.append(b)
    if out is not None and len(out) > 1:
        pieces.append(out)
    return pieces


def build_quais(river, box):
    """A minor street along each bank, so the riverside gets frontage instead of dead ends."""
    off = RIVER_SETBACK + MINOR_WIDTH / 2 + 1.0
    step = max(1, int(QUAI_STEP / RIVER_STEP))
    ref = (box[0] + box[2]) / 2
    out = []
    for side in (0, 1):
        for sh in (-MAP_W, 0.0, MAP_W):
            run = []
            for i in range(0, len(river.x), step):
                p = river.bank_points(i, river.w[i] / 2 + off)[side]
                p = (wrap_near(p[0], ref) + sh, p[1])
                inside = (box[0] - 250 < p[0] < box[2] + 250 and box[1] - 250 < p[1] < box[3] + 250
                          and GREEN_BORDER < p[1] < MAP_H - GREEN_BORDER)
                if inside:
                    run.append(p)
                elif run:
                    if len(run) > 2:
                        out.append(run)
                    run = []
            if len(run) > 2:
                out.append(run)
    return out


def streets_for_face(face, river, discs=()):
    pts, rids = face
    n = len(pts)
    sides, start = [], 0
    while start < n and rids[start] == rids[start - 1]:
        start += 1
    start %= n
    cur = [start]
    for k in range(1, n):
        i = (start + k) % n
        if rids[i] == rids[cur[-1]]:
            cur.append(i)
        else:
            sides.append(cur)
            cur = [i]
    sides.append(cur)
    cand = [s for s in sides if rids[s[0]] >= 0]
    if not cand:
        return []
    spine = max(cand, key=lambda s: math.dist(pts[s[0]], pts[(s[-1] + 1) % n]))
    P0, P1 = pts[spine[0]], pts[(spine[-1] + 1) % n]
    L = math.dist(P0, P1)
    if L < 2 * STREET_END_MARGIN:
        return []
    t = unit(sub(P1, P0))
    nrm = left(t)
    count = int((L - 2 * STREET_END_MARGIN) // STREET_PITCH) + 1
    s0 = (L - (count - 1) * STREET_PITCH) / 2
    result = []
    for k in range(count):
        p = add(P0, mul(t, s0 + k * STREET_PITCH))
        hit = ray_face(add(p, mul(nrm, 0.5)), nrm, pts)
        if hit is None:
            continue
        thit, ei = hit
        thit += 0.5
        straight = [p, add(p, mul(nrm, thit))]
        line = straight
        if rids[ei] >= 0 and thit > 2 * MIN_BEND:
            e_dir = unit(sub(pts[(ei + 1) % n], pts[ei]))
            d2 = mul(left(e_dir), -1.0)
            ang = math.degrees(math.acos(max(-1, min(1, dot(nrm, d2)))))
            if 3.0 < ang < MAX_BEND_ANGLE:
                b = add(p, mul(nrm, max(MIN_BEND, thit * 0.5)))
                h2 = ray_face(b, d2, pts)
                if h2 is not None and rids[h2[1]] == rids[ei]:
                    line = [p, b, add(b, mul(d2, h2[0]))]
        ok = all(not segs_cross(a1, b1, a2, b2)
                 for a1, b1 in zip(line, line[1:])
                 for o in result for a2, b2 in zip(o, o[1:]))
        if not ok and len(line) == 3:
            line = straight
            ok = all(not segs_cross(line[0], line[1], a2, b2)
                     for o in result for a2, b2 in zip(o, o[1:]))
        if not ok:
            continue
        result.extend(piece for piece in cut_at_obstacles(line, river, discs)
                      if math.dist(piece[0], piece[-1]) > 40.0)
    return result


def split_face(pts, rids, c, a, cut_rid):
    """Clip a superblock to the half-plane dot(p - c, a) >= 0. rids[i] names the edge
    leaving pts[i]; edges along the cut get cut_rid."""
    n = len(pts)
    side = [dot(sub(p, c), a) for p in pts]
    out, out_r = [], []
    for i in range(n):
        j = (i + 1) % n
        S, E = pts[i], pts[j]
        if side[j] >= 0:
            if side[i] < 0:
                out.append(add(S, mul(sub(E, S), side[i] / (side[i] - side[j]))))
                out_r.append(rids[i])
            out.append(E)
            out_r.append(rids[j])
        elif side[i] >= 0:
            out.append(add(S, mul(sub(E, S), side[i] / (side[i] - side[j]))))
            out_r.append(cut_rid)
    return out, out_r


def streets_for_superblock(face, river, discs=(), depth=0):
    """streets_for_face fans streets from one side only, so it leaves most of an
    oversized superblock empty (and the park pass turns that into lawn). The wide river
    makes these common: hubs keep clear of it, so the boulevards around it enclose
    blocks several times the usual size. Halve those along a minor street across
    their long axis until every piece is a normal size."""
    pts, rids = face
    area = abs(poly_area(pts))
    if area <= SUPERBLOCK_SPLIT_AREA or depth >= 4:
        return streets_for_face(face, river, discs)
    n = len(pts)
    c = (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n)
    sxx = sum((p[0] - c[0]) ** 2 for p in pts)
    syy = sum((p[1] - c[1]) ** 2 for p in pts)
    sxy = sum((p[0] - c[0]) * (p[1] - c[1]) for p in pts)
    ang = 0.5 * math.atan2(2 * sxy, sxx - syy)          # principal (long) axis
    a = (math.cos(ang), math.sin(ang))
    cut_rid = 10 ** 6 + depth                           # any id >= 0 that isn't a boulevard
    halves = [split_face(pts, rids, c, a, cut_rid),
              split_face(pts, rids, c, mul(a, -1.0), cut_rid)]
    # The cut street: the parts of the line through c, across the long axis, that lie
    # inside the superblock (more than one if the block is concave).
    d = left(a)
    ts = sorted(r[0] for i in range(n)
                for r in [seg_intersect(c, add(c, d), pts[i], pts[(i + 1) % n])]
                if r is not None and 0.0 <= r[1] < 1.0)
    result = []
    for t0, t1 in zip(ts, ts[1:]):
        m = add(c, mul(d, (t0 + t1) / 2))
        if t1 - t0 > 40.0 and point_in_poly(m, pts):
            result.extend(cut_at_obstacles([add(c, mul(d, t0)), add(c, mul(d, t1))], river, discs))
    for hp, hr in halves:
        if len(hp) >= 3 and abs(poly_area(hp)) > 1e4:
            result.extend(streets_for_superblock((hp, hr), river, discs, depth + 1))
    return result


# --------------------------------------------------------------------------- #
# BUILDINGS
# --------------------------------------------------------------------------- #
class BType:
    def __init__(self, name, front, back, depth, height, align="center", weight=1.0):
        self.name, self.F, self.B, self.D, self.H = name, front, back, depth, height
        self.weight = weight
        bx = {"center": (front - back) / 2, "left": 0.0, "right": front - back}[align]
        self.local = [(0.0, 0.0), (front, 0.0), (bx + back, depth), (bx, depth)]
        sides = [math.dist(self.local[i], self.local[(i + 1) % 4]) for i in range(4)]
        self.courtyard = max(front, back, depth, max(sides)) > COURTYARD_THRESHOLD

    def world(self, origin, t, n):
        return [(origin[0] + t[0] * x + n[0] * y, origin[1] + t[1] * x + n[1] * y)
                for x, y in self.local]


BUILDING_TYPES = [
    BType("Cube",            40, 40, 40, 40, weight=3),
    BType("Townhouse",       20, 20, 40, 25, weight=3),
    BType("Slim",            15, 15, 40, 30, weight=2),
    BType("Sliver",          10, 10, 35, 20, weight=1),
    BType("Wide",            50, 50, 40, 30, weight=2),
    BType("LongBlock",       60, 60, 40, 35, weight=2),
    BType("Court65",         65, 65, 40, 30, weight=1.5),
    BType("Court80",         80, 80, 40, 35, weight=1.5),
    BType("Court100",       100, 100, 40, 25, weight=1),
    BType("Shallow",         30, 30, 30, 20, weight=1.5),
    BType("TaperedCube",     40, 25, 40, 35, weight=1),
    BType("Wedge",           40,  5, 40, 30, weight=0.7),
    BType("SmallWedge",      25,  5, 35, 25, weight=0.7),
    BType("InverseWedge",     5, 30, 40, 20, weight=0.5),
    BType("CornerL",         40, 20, 40, 30, align="left", weight=1),
    BType("CornerR",         40, 20, 40, 30, align="right", weight=1),
    BType("TaperedCourt",    70, 45, 40, 30, weight=1),
    BType("TaperedTall",     55, 35, 40, 40, weight=1),
    BType("NarrowCornerL",   30, 10, 40, 25, align="left", weight=0.7),
    BType("Flared",          30, 45, 40, 35, weight=0.7),
]
assert len(BUILDING_TYPES) == 20


def weighted_order(rng, boost_courtyard=1.0):
    keyed = []
    for T in BUILDING_TYPES:
        w = T.weight * (boost_courtyard if T.courtyard else 1.0)
        keyed.append((rng.random() ** (1.0 / w), T))
    keyed.sort(key=lambda kv: -kv[0])
    return [T for _, T in keyed]


def in_map(poly):
    return all(GREEN_BORDER <= y <= MAP_H - GREEN_BORDER for _, y in poly)


def fill_frontage(a, b, offset, hsh, river, rng, out, boost_courtyard=1.0, green=None):
    t = unit(sub(b, a))
    n = left(t)
    L = math.dist(a, b)
    s = 0.0
    while s < L - 4.99:
        rem = L - s
        origin = add(add(a, mul(t, s)), mul(n, offset))
        placed = False
        for T in weighted_order(rng, boost_courtyard):
            if T.F > rem + 0.01:
                continue
            poly = T.world(origin, t, n)
            if not in_map(poly) or any(river.blocks(p) for p in poly) or hsh.hits(poly):
                continue
            if green is not None and green.contains((sum(q[0] for q in poly) / 4,
                                                     sum(q[1] for q in poly) / 4)):
                continue
            hsh.add(poly)
            out.append((T, poly))
            s += T.F
            placed = True
            break
        if not placed:
            s += 5.0


# --------------------------------------------------------------------------- #
# PARKS
# --------------------------------------------------------------------------- #
def raster_convex(grid, poly, cell, ox, oy):
    H, W = grid.shape
    xs = [p[0] - ox for p in poly]
    ys = [p[1] - oy for p in poly]
    i0, i1 = max(0, int(min(xs) // cell)), min(W - 1, int(max(xs) // cell))
    j0, j1 = max(0, int(min(ys) // cell)), min(H - 1, int(max(ys) // cell))
    if i0 > i1 or j0 > j1:
        return
    cx = (np.arange(i0, i1 + 1) + 0.5) * cell
    cy = (np.arange(j0, j1 + 1) + 0.5) * cell
    X, Y = np.meshgrid(cx, cy, indexing="xy")
    inside = np.ones_like(X, dtype=bool)
    for k in range(len(poly)):
        ax, ay = xs[k], ys[k]
        bx, by = xs[(k + 1) % len(poly)], ys[(k + 1) % len(poly)]
        inside &= (bx - ax) * (Y - ay) - (by - ay) * (X - ax) >= 0
    grid[j0:j1 + 1, i0:i1 + 1] |= inside


def shift(a, dj, di, fill):
    out = np.full_like(a, fill)
    H, W = a.shape
    out[max(0, dj):H + min(0, dj), max(0, di):W + min(0, di)] = \
        a[max(0, -dj):H + min(0, -dj), max(0, -di):W + min(0, -di)]
    return out


def find_parks(occupied):
    empty = ~occupied
    steps = max(1, int(round(PARK_OPENING / PARK_CELL)))
    eroded = empty.copy()
    for _ in range(steps):
        src = eroded.copy()
        for dj in (-1, 0, 1):
            for di in (-1, 0, 1):
                eroded &= shift(src, dj, di, False)
    H, W = eroded.shape
    labels = np.zeros((H, W), dtype=np.int32)
    flat, lab, cur = eroded.ravel(), labels.ravel(), 0
    for start in np.flatnonzero(flat):
        if lab[start]:
            continue
        cur += 1
        stack = [start]
        lab[start] = cur
        while stack:
            k = stack.pop()
            j, i = divmod(k, W)
            for nk, ok in ((k - 1, i > 0), (k + 1, i < W - 1), (k - W, j > 0), (k + W, j < H - 1)):
                if ok and flat[nk] and not lab[nk]:
                    lab[nk] = cur
                    stack.append(nk)
    for _ in range(steps):
        src = labels.copy()
        for dj in (-1, 0, 1):
            for di in (-1, 0, 1):
                s = shift(src, dj, di, 0)
                labels = np.where((labels == 0) & empty & (s > 0), s, labels)
    counts = np.bincount(labels.ravel())
    keep = counts * PARK_CELL * PARK_CELL >= PARK_MIN_AREA
    keep[0] = False
    return keep[labels]


def mask_to_rects(mask, x_off, y_off, cell):
    rects, active = [], {}
    H, W = mask.shape
    for j in range(H + 1):
        runs = set()
        if j < H:
            row = mask[j]
            i = 0
            while i < W:
                if row[i]:
                    i0 = i
                    while i < W and row[i]:
                        i += 1
                    runs.add((i0, i))
                else:
                    i += 1
        new_active = {}
        for rr, j0 in active.items():
            if rr in runs:
                new_active[rr] = j0
            else:
                rects.append((x_off + rr[0] * cell, y_off + j0 * cell,
                              x_off + rr[1] * cell, y_off + j * cell))
        for rr in runs:
            if rr not in new_active:
                new_active[rr] = j
        active = new_active
    return rects


# --------------------------------------------------------------------------- #
# MESHES
# --------------------------------------------------------------------------- #
class MeshAcc:
    def __init__(self):
        self.verts, self.faces, self.mats = [], [], []

    def vert(self, x, y, z):
        self.verts.append((x, y, z))
        return len(self.verts) - 1

    def face(self, idx, mat=0):
        self.faces.append(tuple(idx))
        self.mats.append(mat)

    def quad_xy(self, pts, z, mat):
        self.face([self.vert(x, y, z) for x, y in pts], mat)


def add_prism(acc, poly, h, mat=0, z0=0.0):
    b = [acc.vert(x, y, z0) for x, y in poly]
    t = [acc.vert(x, y, z0 + h) for x, y in poly]
    for i in range(len(poly)):
        j = (i + 1) % len(poly)
        acc.face([b[i], b[j], t[j], t[i]], mat)
    acc.face(t, mat)


def add_dome(acc, c, r, z0, mat=0, segs=24, rings=6):
    """Hemisphere sitting on z0, open at the bottom."""
    prev = [acc.vert(c[0] + r * math.cos(2 * math.pi * k / segs),
                     c[1] + r * math.sin(2 * math.pi * k / segs), z0) for k in range(segs)]
    for ri in range(1, rings + 1):
        phi = (math.pi / 2) * ri / rings
        rr, zz = r * math.cos(phi), z0 + r * math.sin(phi)
        if ri == rings:
            top = acc.vert(c[0], c[1], z0 + r)
            for k in range(segs):
                acc.face([prev[k], prev[(k + 1) % segs], top], mat)
            break
        cur = [acc.vert(c[0] + rr * math.cos(2 * math.pi * k / segs),
                        c[1] + rr * math.sin(2 * math.pi * k / segs), zz) for k in range(segs)]
        for k in range(segs):
            j = (k + 1) % segs
            acc.face([prev[k], prev[j], cur[j], cur[k]], mat)
        prev = cur


def add_monument(acc, centre, size, gold_mat, stone_mat):
    """Whatever stands on the platform of an S / M / L roundabout."""
    z = PLATFORM_HEIGHT
    if size == 0:
        r, h = MONUMENT_S
        add_prism(acc, circle_poly(centre, r, 12), h, gold_mat, z0=z)
    elif size == 1:
        r, h = MONUMENT_M
        add_prism(acc, circle_poly(centre, r, 16), h, gold_mat, z0=z)
    else:
        r, h = MONUMENT_L
        add_prism(acc, circle_poly(centre, r, 48), h, stone_mat, z0=z)
        add_dome(acc, centre, r, z + h, stone_mat, segs=48, rings=8)


def inset_quad(q, w):
    lines = []
    for i in range(4):
        a, b = q[i], q[(i + 1) % 4]
        nn = mul(left(unit(sub(b, a))), w)
        lines.append((add(a, nn), add(b, nn)))
    out = []
    for i in range(4):
        (a1, b1), (a2, b2) = lines[i - 1], lines[i]
        r = seg_intersect(a1, b1, a2, b2)
        if r is None:
            return None
        out.append(add(a1, mul(sub(b1, a1), r[0])))
    return out


def add_courtyard_building(acc, O, h, mat=0):
    I = inset_quad(O, COURTYARD_WALL)
    if I is None or poly_area(I) < 25.0 or \
            min(math.dist(I[k], I[(k + 1) % 4]) for k in range(4)) < 5.0:
        add_prism(acc, O, h, mat)
        return
    t = unit(sub(O[1], O[0]))
    n = left(t)
    mid = mul(add(O[0], O[1]), 0.5)
    Pa, Pb = sub(mid, mul(t, PASSAGE_WIDTH / 2)), add(mid, mul(t, PASSAGE_WIDTH / 2))
    Qa, Qb = add(Pa, mul(n, COURTYARD_WALL)), add(Pb, mul(n, COURTYARD_WALL))
    outer = [O[0], Pa, Pb, O[1], O[2], O[3]]
    inner = [I[0], Qa, Qb, I[1], I[2], I[3]]
    ob = [acc.vert(x, y, 0) for x, y in outer]
    ot = [acc.vert(x, y, h) for x, y in outer]
    ib = [acc.vert(x, y, 0) for x, y in inner]
    it = [acc.vert(x, y, h) for x, y in inner]
    pa_p, pb_p = acc.vert(*Pa, PASSAGE_HEIGHT), acc.vert(*Pb, PASSAGE_HEIGHT)
    qa_p, qb_p = acc.vert(*Qa, PASSAGE_HEIGHT), acc.vert(*Qb, PASSAGE_HEIGHT)
    acc.face([ob[0], ob[1], pa_p, ot[1], ot[0]], mat)
    acc.face([pa_p, pb_p, ot[2], ot[1]], mat)
    acc.face([ob[2], ob[3], ot[3], ot[2], pb_p], mat)
    for i in (3, 4, 5):
        j = (i + 1) % 6
        acc.face([ob[i], ob[j], ot[j], ot[i]], mat)
    acc.face([ib[0], it[0], it[1], qa_p, ib[1]], mat)
    acc.face([qa_p, it[1], it[2], qb_p], mat)
    acc.face([ib[2], qb_p, it[2], it[3], ib[3]], mat)
    for i in (3, 4, 5):
        j = (i + 1) % 6
        acc.face([ib[i], it[i], it[j], ib[j]], mat)
    for i in range(6):
        j = (i + 1) % 6
        acc.face([ot[i], ot[j], it[j], it[i]], mat)
    acc.face([ob[1], ib[1], qa_p, pa_p], mat)
    acc.face([ob[2], pb_p, qb_p, ib[2]], mat)
    acc.face([pa_p, qa_p, qb_p, pb_p], mat)


def srgb_to_linear(c):
    return tuple(((x + 0.055) / 1.055) ** 2.4 if x > 0.04045 else x / 12.92 for x in c)


def get_material(name, col):
    m = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    m.diffuse_color = (*col, 1.0)
    m.use_nodes = True
    bsdf = m.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = (*srgb_to_linear(col), 1.0)
        bsdf.inputs["Roughness"].default_value = 0.9
    return m


def make_object(name, acc, mats, coll, parent=None, origin=(0.0, 0.0)):
    mesh = bpy.data.meshes.new(name)
    ox, oy = origin
    mesh.from_pydata([(x - ox, y - oy, z) for x, y, z in acc.verts], [], acc.faces)
    for m in mats:
        mesh.materials.append(m)
    mesh.polygons.foreach_set("material_index", acc.mats)
    mesh.update()
    mesh.validate()
    obj = bpy.data.objects.new(name, mesh)
    coll.objects.link(obj)
    obj.parent = parent
    return obj


def make_empty(name, coll, loc=(0, 0, 0), parent=None, size=5.0):
    e = bpy.data.objects.new(name, None)
    e.empty_display_size = size
    e.location = loc
    coll.objects.link(e)
    e.parent = parent
    return e


# --------------------------------------------------------------------------- #
# MAIN
# --------------------------------------------------------------------------- #
def resolve_region(region, river):
    if region is None:
        box = (0.0, 0.0, MAP_W, MAP_H)
    elif region == "river":
        y = MAP_H / 2
        k = int(np.argmin(np.abs(river.y - y)))
        cx = float(river.x[k]) % MAP_W
        box = (cx - REGION_SIZE / 2, y - REGION_SIZE / 2, cx + REGION_SIZE / 2, y + REGION_SIZE / 2)
    else:
        box = tuple(float(v) for v in region)
    if box[2] - box[0] <= MAP_W:            # keep the window inside one tile of the cylinder
        shiftx = min(0.0, MAP_W - box[2]) if box[2] > MAP_W else max(0.0, -box[0])
        box = (box[0] + shiftx, box[1], box[2] + shiftx, box[3])
    x0 = math.floor(box[0] / CHUNK_X) * CHUNK_X
    y0 = math.floor(box[1] / CHUNK_Y) * CHUNK_Y
    x1 = math.ceil(box[2] / CHUNK_X) * CHUNK_X
    y1 = math.ceil(box[3] / CHUNK_Y) * CHUNK_Y
    return (x0, max(0.0, y0), x1, min(MAP_H, y1))


def generate_network(seed=SEED):
    """The whole-map passes: river, hubs and the boulevard graph. They don't depend on
    the region, so a tiled export computes them once and passes the result to every
    tile via generate(network=...). Later passes draw from sub_rng, never from these
    generators, so skipping this step leaves a tile's output unchanged."""
    rng = np.random.default_rng(seed)
    base_rng = random.Random(seed)

    print("[1/8] river")
    river = River(rng)
    print(f"  {len(river.x) * RIVER_STEP / 1000:.1f} km of channel, "
          f"{float((river.ih > 0.5).sum()) * RIVER_STEP / 1000:.1f} km of it with islands")

    print("[2/8] hubs")
    hubs, edges, counts, sizes, dropped_bridges = finalize_network(
        generate_hubs(base_rng, river), river)
    hist = {k: counts.count(k) for k in sorted(set(counts))}
    smlh = {ROUNDABOUT_TYPES[t][0]: sizes.count(t) for t in (0, 1, 2)}
    print(f"  {len(hubs)} hubs, {len(edges)} boulevards, links per hub {hist}")
    print(f"  {dropped_bridges} river-grazing boulevards dropped; roundabouts {smlh}")
    return dict(river=river, hubs=hubs, edges=edges, sizes=sizes)


def generate(seed=SEED, region=REGION, network=None):
    t0 = time.time()
    net = network if network is not None else generate_network(seed)
    river, hubs, edges, sizes = net["river"], net["hubs"], net["edges"], net["sizes"]

    reg = resolve_region(region, river)
    wx0, wy0 = reg[0] - WORK_MARGIN, max(GREEN_BORDER, reg[1] - WORK_MARGIN)
    wx1, wy1 = reg[2] + WORK_MARGIN, min(MAP_H - GREEN_BORDER, reg[3] + WORK_MARGIN)
    print(f"  region {reg[0]:.0f},{reg[1]:.0f} .. {reg[2]:.0f},{reg[3]:.0f}  "
          f"({(reg[2] - reg[0]) / 1000:.1f} x {(reg[3] - reg[1]) / 1000:.1f} km)")

    print("[3/8] boulevards in window")
    ylo, yhi = GREEN_BORDER, MAP_H - GREEN_BORDER
    majors = []
    for ei, e in enumerate(edges):
        a, b = edge_segment(hubs, e)
        for sh in (-MAP_W, 0.0, MAP_W):
            aa, bb = (a[0] + sh, a[1]), (b[0] + sh, b[1])
            if max(aa[0], bb[0]) < wx0 - 1200 or min(aa[0], bb[0]) > wx1 + 1200:
                continue
            if max(aa[1], bb[1]) < wy0 - 1200 or min(aa[1], bb[1]) > wy1 + 1200:
                continue
            c = clip_segment(aa, bb, -1e9, ylo, 1e9, yhi)
            if c and math.dist(*c) > 1.0:
                majors.append((c[0], c[1], ei))
    print(f"  {len(majors)} boulevard segments")

    print("[4/8] superblocks")
    bnd = [((wx0, wy0), (wx1, wy0), -1), ((wx1, wy0), (wx1, wy1), -1),
           ((wx1, wy1), (wx0, wy1), -1), ((wx0, wy1), (wx0, wy0), -1)]
    faces = []
    for pts, rids in build_faces(majors + bnd):
        bx0, bx1 = min(p[0] for p in pts), max(p[0] for p in pts)
        by0, by1 = min(p[1] for p in pts), max(p[1] for p in pts)
        if bx0 <= wx0 + 1 or bx1 >= wx1 - 1:
            continue                                   # truncated by the work window
        if (by0 <= wy0 + 1 and wy0 > ylo + 1) or (by1 >= wy1 - 1 and wy1 < yhi - 1):
            continue
        if bx1 < reg[0] - 200 or bx0 > reg[2] + 200 or by1 < reg[1] - 200 or by0 > reg[3] + 200:
            continue
        faces.append((pts, rids))
    print(f"  {len(faces)} superblocks near the region")

    print("[5/8] minor streets")
    green = GreenZones([pts for pts, _ in faces if is_green_face(pts)])
    print(f"  {len(green.polys)} of {len(faces)} superblocks set aside as parkland")
    discs = []
    for i, h in enumerate(hubs):
        for sh in (-MAP_W, 0.0, MAP_W):
            p = (h[0] + sh, h[1])
            if reg[0] - 400 < p[0] < reg[2] + 400 and reg[1] - 400 < p[1] < reg[3] + 400:
                discs.append((p, hub_radius(sizes, i) + 15.0))
    streets = []
    for pts, rids in faces:
        if green.contains((sum(p[0] for p in pts) / len(pts),
                           sum(p[1] for p in pts) / len(pts))):
            continue
        streets.extend(streets_for_superblock((pts, rids), river, discs))
    if QUAI_ROADS:
        quais = build_quais(river, reg)
        streets.extend(quais)
        print(f"  {len(streets)} streets ({len(quais)} quai runs)")
    else:
        print(f"  {len(streets)} streets")

    print("[6/8] buildings")
    near = [m for m in majors
            if not (max(m[0][0], m[1][0]) < reg[0] - 300 or min(m[0][0], m[1][0]) > reg[2] + 300
                    or max(m[0][1], m[1][1]) < reg[1] - 300 or min(m[0][1], m[1][1]) > reg[3] + 300)]
    hsh = SpatialHash(100.0)
    for a, b, _ in near:
        hsh.add(rect_poly(a, b, MAJOR_WIDTH / 2 + CURB - 0.1))
    for i, h in enumerate(hubs):
        for sh in (-MAP_W, 0.0, MAP_W):
            p = (h[0] + sh, h[1])
            if reg[0] - 400 < p[0] < reg[2] + 400 and reg[1] - 400 < p[1] < reg[3] + 400:
                hsh.add(circle_poly(p, hub_radius(sizes, i) + CURB, 20))
    for s in streets:
        for a, b in zip(s, s[1:]):
            hsh.add(rect_poly(a, b, MINOR_WIDTH / 2 + CURB - 0.1))
        for p in s[1:-1]:
            hsh.add(circle_poly(p, MINOR_WIDTH / 2 + CURB, 12))

    buildings = []
    for a, b, ei in sorted(near, key=lambda m: (round(m[0][0] % MAP_W, 1), round(m[0][1], 1))):
        # keys are wrapped so a road's two seam copies generate the identical frontage
        r = sub_rng("major", round(a[0] % MAP_W, 1), round(a[1], 1), round((b[0] - a[0]), 1))
        fill_frontage(a, b, MAJOR_WIDTH / 2 + CURB, hsh, river, r, buildings, 2.5, green)
        fill_frontage(b, a, MAJOR_WIDTH / 2 + CURB, hsh, river, r, buildings, 2.5, green)
    for s in sorted(streets, key=lambda s: (round(s[0][0] % MAP_W, 1), round(s[0][1], 1))):
        r = sub_rng("street", round(s[0][0] % MAP_W, 1), round(s[0][1], 1))
        for a, b in zip(s, s[1:]):
            fill_frontage(a, b, MINOR_WIDTH / 2 + CURB, hsh, river, r, buildings, 1.0, green)
            fill_frontage(b, a, MINOR_WIDTH / 2 + CURB, hsh, river, r, buildings, 1.0, green)
    buildings = [(T, p) for T, p in buildings
                 if reg[0] <= sum(q[0] for q in p) / 4 < reg[2]
                 and reg[1] <= sum(q[1] for q in p) / 4 < reg[3]]
    print(f"  {len(buildings)} buildings, "
          f"{sum(1 for T, _ in buildings if T.courtyard)} with courtyards")

    print("[7/8] parks")
    pad = 200.0
    px0 = math.floor((reg[0] - pad) / PARK_CELL) * PARK_CELL
    py0 = math.floor((reg[1] - pad) / PARK_CELL) * PARK_CELL
    nx = int((reg[2] + pad - px0) / PARK_CELL)
    ny = int((reg[3] + pad - py0) / PARK_CELL)
    occ = np.zeros((ny, nx), dtype=bool)
    for poly in hsh.polys:
        raster_convex(occ, poly, PARK_CELL, px0, py0)
    for i in range(len(river.x) - 1):
        if not (py0 - 300 < river.y[i] < py0 + ny * PARK_CELL + 300):
            continue
        base = wrap_near(river.x[i], px0 + nx * PARK_CELL / 2) - river.x[i]
        for sh in (base - MAP_W, base, base + MAP_W):
            if not (px0 - 300 < river.x[i] + sh < px0 + nx * PARK_CELL + 300):
                continue
            l0, r0 = river.bank_points(i, river.w[i] / 2 + RIVER_SETBACK)
            l1, r1 = river.bank_points(i + 1, river.w[i + 1] / 2 + RIVER_SETBACK)
            quad = [(l0[0] + sh, l0[1]), (l1[0] + sh, l1[1]),
                    (r1[0] + sh, r1[1]), (r0[0] + sh, r0[1])]
            if poly_area(quad) < 0:
                quad.reverse()
            raster_convex(occ, quad, PARK_CELL, px0, py0)
    ys = py0 + (np.arange(ny) + 0.5) * PARK_CELL
    occ[(ys < GREEN_BORDER) | (ys > MAP_H - GREEN_BORDER), :] = True    # band is green anyway
    parks = find_parks(occ)
    print(f"  park area {parks.sum() * PARK_CELL ** 2 / 1e4:.1f} ha")

    result = dict(river=river, hubs=hubs, sizes=sizes, edges=edges, majors=majors, faces=faces,
                  streets=streets, buildings=buildings, parks=parks,
                  park_origin=(px0, py0), region=reg)
    if bpy is not None:
        print("[8/8] meshes")
        build_blender(result)
    print(f"done in {time.time() - t0:.1f} s")
    return result


def build_chunk_meshes(r, include_buildings=True):
    """Bin everything into per-chunk MeshAccs. Pure Python - no Blender needed, so the
    exporter can call it too. Returns (terrain, lod0, lod1, monuments, bridge_count)."""
    reg = r["region"]
    river = r["river"]
    G, P, R, PL, WA, GOLD, MON = (MAT_GROUND, MAT_PARK, MAT_ROAD, MAT_PLATFORM,
                                  MAT_WATER, MAT_GOLD, MAT_MONUMENT)

    ci0, cj0 = int(reg[0] // CHUNK_X), int(reg[1] // CHUNK_Y)
    ci1, cj1 = int(math.ceil(reg[2] / CHUNK_X)), int(math.ceil(reg[3] / CHUNK_Y))
    terrain, lod0, lod1 = {}, {}, {}
    for i in range(ci0, ci1):
        for j in range(cj0, cj1):
            terrain[(i, j)] = MeshAcc()
            lod0[(i, j)] = MeshAcc()
            lod1[(i, j)] = MeshAcc()

    def chunk_of(x, y):
        return int(math.floor(x / CHUNK_X)), int(math.floor(y / CHUNK_Y))

    def acc_at(store, x, y):
        return store.get(chunk_of(x, y))

    for (i, j), acc in terrain.items():                      # ground + green border band
        x0, y0 = i * CHUNK_X, j * CHUNK_Y
        x1, y1 = x0 + CHUNK_X, min(MAP_H, y0 + CHUNK_Y)
        acc.quad_xy([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], Z_GROUND, G)
        for by0, by1 in ((0.0, GREEN_BORDER), (MAP_H - GREEN_BORDER, MAP_H)):
            gy0, gy1 = max(y0, by0), min(y1, by1)
            if gy1 > gy0:
                acc.quad_xy([(x0, gy0), (x1, gy0), (x1, gy1), (x0, gy1)], Z_PARK, P)

    px0, py0 = r["park_origin"]                              # parks
    pmask = r["parks"]
    for (i, j), acc in terrain.items():
        # CHUNK_X is not a whole number of cells (502 / 2.5 = 200.8), so chunk edges
        # fall mid-cell. Take every cell the chunk overlaps, keep the rects at their
        # true raster position, and clip them to the chunk: neighbours then meet
        # exactly at the shared edge instead of leaving a gap or drifting by a
        # fraction of a cell.
        x0, y0 = i * CHUNK_X, j * CHUNK_Y
        x1, y1 = x0 + CHUNK_X, min(MAP_H, y0 + CHUNK_Y)
        ix0 = max(0, int(math.floor((x0 - px0) / PARK_CELL)))
        jy0 = max(0, int(math.floor((y0 - py0) / PARK_CELL)))
        ix1 = min(pmask.shape[1], int(math.ceil((x1 - px0) / PARK_CELL)))
        jy1 = min(pmask.shape[0], int(math.ceil((y1 - py0) / PARK_CELL)))
        if ix1 <= ix0 or jy1 <= jy0:
            continue
        sub_mask = pmask[jy0:jy1, ix0:ix1]
        for rx0, ry0, rx1, ry1 in mask_to_rects(sub_mask, px0 + ix0 * PARK_CELL,
                                                py0 + jy0 * PARK_CELL, PARK_CELL):
            rx0, ry0 = max(rx0, x0), max(ry0, y0)
            rx1, ry1 = min(rx1, x1), min(ry1, y1)
            if rx1 - rx0 > 1e-6 and ry1 - ry0 > 1e-6:
                acc.quad_xy([(rx0, ry0), (rx1, ry0), (rx1, ry1), (rx0, ry1)], Z_PARK, P)

    for i in range(len(river.x) - 1):                        # water + islands
        if not (reg[1] - 200 < river.y[i] < reg[3] + 200):
            continue
        base = wrap_near(river.x[i], (reg[0] + reg[2]) / 2) - river.x[i]
        for sh in (base - MAP_W, base, base + MAP_W):
            xi = river.x[i] + sh
            if not (reg[0] - 200 < xi < reg[2] + 200):
                continue
            acc = acc_at(terrain, min(max(xi, reg[0]), reg[2] - 1.0), river.y[i])
            if acc is None:
                continue
            for half_a, half_b, z, mat in (
                    (river.w[i] / 2, river.w[i + 1] / 2, Z_WATER, WA),
                    (river.ih[i], river.ih[i + 1], Z_ISLAND, P)):
                if half_a < 0.5 or half_b < 0.5:
                    continue
                l0, r0 = river.bank_points(i, half_a)
                l1, r1 = river.bank_points(i + 1, half_b)
                quad = [(l0[0] + sh, l0[1]), (l1[0] + sh, l1[1]),
                        (r1[0] + sh, r1[1]), (r0[0] + sh, r0[1])]
                if poly_area(quad) < 0:
                    quad.reverse()
                acc.quad_xy(quad, z, mat)

    def emit_road(a, b, width, z_base, bridging):
        pieces = [(a, b)]
        Ltot = math.dist(a, b)
        if bridging and Ltot > 1e-6:
            n = max(1, int(Ltot / 25.0))
            t = unit(sub(b, a))
            pieces = [(add(a, mul(t, Ltot * k / n)), add(a, mul(t, Ltot * (k + 1) / n)))
                      for k in range(n)]
        for pa, pb in pieces:
            za = z_base + (river.bridge_z(pa) if bridging else 0.0)
            zb = z_base + (river.bridge_z(pb) if bridging else 0.0)
            seg_len = max(1e-6, math.dist(pa, pb))
            for (i, j), acc in terrain.items():
                c = clip_segment(pa, pb, i * CHUNK_X, j * CHUNK_Y,
                                 (i + 1) * CHUNK_X, (j + 1) * CHUNK_Y)
                if not c or math.dist(*c) < 0.01:
                    continue
                q = rect_poly(c[0], c[1], width / 2)
                z0 = za + (zb - za) * math.dist(pa, c[0]) / seg_len
                z1 = za + (zb - za) * math.dist(pa, c[1]) / seg_len
                acc.face([acc.vert(q[0][0], q[0][1], z0), acc.vert(q[1][0], q[1][1], z1),
                          acc.vert(q[2][0], q[2][1], z1), acc.vert(q[3][0], q[3][1], z0)], R)

    bridges = 0
    for a, b, _ in r["majors"]:
        if max(a[0], b[0]) < reg[0] - 200 or min(a[0], b[0]) > reg[2] + 200:
            continue
        if max(a[1], b[1]) < reg[1] - 200 or min(a[1], b[1]) > reg[3] + 200:
            continue
        t = unit(sub(b, a))
        bridging = any(river.signed(add(a, mul(t, float(d)))) < BRIDGE_RAMP
                       for d in np.linspace(0, math.dist(a, b), 30))
        bridges += bool(bridging)
        emit_road(a, b, MAJOR_WIDTH, Z_MAJOR, bridging)
    for s in r["streets"]:
        for a, b in zip(s, s[1:]):
            emit_road(a, b, MINOR_WIDTH, Z_MINOR, False)
        for p in s[1:-1]:
            acc = acc_at(terrain, *p)
            if acc:
                acc.quad_xy(circle_poly(p, MINOR_WIDTH / 2, 12), Z_MINOR, R)

    monuments = []
    for i, h in enumerate(r["hubs"]):
        size = r["sizes"][i]
        _, road_r, plat_r, _ = ROUNDABOUT_TYPES[size]
        for sh in (-MAP_W, 0.0, MAP_W):
            p = (h[0] + sh, h[1])
            acc = acc_at(terrain, *p)
            if acc is None:
                continue
            acc.quad_xy(circle_poly(p, road_r, 32), Z_ROUNDABOUT, R)
            add_prism(acc, circle_poly(p, plat_r, 32), PLATFORM_HEIGHT, PL)
            add_monument(acc, p, size, GOLD, MON)
            monuments.append((p, size))

    if include_buildings:
        for T, poly in r["buildings"]:
            k = chunk_of(sum(p[0] for p in poly) / 4, sum(p[1] for p in poly) / 4)
            if k not in lod0:
                continue
            if T.courtyard:
                add_courtyard_building(lod0[k], poly, T.H)
            else:
                add_prism(lod0[k], poly, T.H)
            add_prism(lod1[k], poly, T.H)
    return terrain, lod0, lod1, monuments, bridges


def build_blender(r):
    old = bpy.data.collections.get("ParisCity")
    if old:
        for o in list(old.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        bpy.data.collections.remove(old)
    for m in list(bpy.data.meshes):
        if m.users == 0:
            bpy.data.meshes.remove(m)
    coll = bpy.data.collections.new("ParisCity")
    bpy.context.scene.collection.children.link(coll)
    mats = [get_material("City_Ground", COL_GROUND), get_material("City_Park", COL_PARK),
            get_material("City_Road", COL_ROAD), get_material("City_Platform", COL_PLATFORM),
            get_material("City_Water", COL_WATER), get_material("City_Gold", COL_GOLD),
            get_material("City_Monument", COL_MONUMENT)]
    m_bld = get_material("City_Building", COL_BUILDING)
    terrain, lod0, lod1, monuments, bridges = build_chunk_meshes(r)

    def chunk_of(x, y):
        return int(math.floor(x / CHUNK_X)), int(math.floor(y / CHUNK_Y))

    roots = {}
    for (i, j) in terrain:
        base = f"Chunk_{i:03d}_{j:03d}"
        centre = ((i + 0.5) * CHUNK_X, (j + 0.5) * CHUNK_Y)
        root = make_empty(base, coll, (*centre, 0), size=25)
        roots[(i, j)] = (root, centre)
        make_object(base + "_Terrain", terrain[(i, j)], mats, coll, parent=root, origin=centre)
        if lod0[(i, j)].faces:
            grp = make_empty(base + "_Buildings", coll, parent=root, size=10)
            for lod, acc in (("LOD0", lod0[(i, j)]), ("LOD1", lod1[(i, j)])):
                o = make_object(f"{base}_Buildings_{lod}", acc, [m_bld], coll,
                                parent=grp, origin=centre)
                if lod == "LOD1":
                    o.hide_set(True)
                    o.hide_render = True

    for n_, (p, size) in enumerate(monuments):
        root, centre = roots[chunk_of(*p)]
        make_empty(f"Monument_{ROUNDABOUT_TYPES[size][0]}_{n_:03d}", coll,
                   (p[0] - centre[0], p[1] - centre[1], PLATFORM_HEIGHT), parent=root, size=8)
    smlh = {ROUNDABOUT_TYPES[t][0]: sum(1 for _, s_ in monuments if s_ == t) for t in (0, 1, 2)}
    print(f"  {len(terrain)} chunks, {len(monuments)} roundabouts {smlh}, "
          f"{bridges} bridged boulevards")


if __name__ == "__main__":
    generate()
