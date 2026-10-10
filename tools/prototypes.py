"""
prototypes.py  --  detailed building prototypes for the Eschaton habitat.

Builds every building type from the generator's catalogue at four levels of detail plus a
separately loadable interior, and writes them to prototypes.bin / prototypes.json. Nothing
else in the pipeline changes: the generator still places (type, position, rotation), the
chunk files still carry instance tables, and this script only changes what those instances
resolve to.

    python3 prototypes.py --out ../world/ [--bake-tile] [--report]

export_chunks.py runs this itself after every export, so the two can't drift apart.

Runs in plain CPython (numpy only). No Blender.

Design
------
One BuildingPlan per type is computed first - wall segments, window bay centres, floor
heights, door positions, courtyard colonnade - and all four LOD bands plus the interior are
emitted from that same plan. Authoring the bands independently is what makes windows and
floors drift between levels and pop on transition; here they cannot, by construction.

The 5 x 5 m bay is the unit of everything: the geometry, the facade texture tile, and the
LOD2 parallax shader all come from the same numbers, so a parallax window at 600 m sits
exactly where the real opening was at 300 m.

Parts
-----
    EXTERIOR  outer walls, openings, reveals, courtyard colonnade, roof, passage tunnel
    GLAZING   a thin pane in every window, carrying the interior-mapping shader
    DOOR_GLAZING  the same in the street doors, apart so the engine can drop it wherever
              the real interior is drawn and the player may walk in (not the carriage
              passage, which is an open tunnel)
    INTERIOR  enclosed rooms only: inner wall faces, floor slabs. Loaded near the player.
    COLLISION what the player collides with (prototypes_collision.bin): the outer walls as
              solid slabs open at the ground-floor doors, the carriage passage, the colonnade
              pillars and the roof. Windows stay solid.

The courtyard colonnade is EXTERIOR, not INTERIOR: a courtyard is an outdoor space, visible
from the passage and from above, so its pillars must be present whenever the building is.

LOD bands
---------
    0   0-120 m     real openings, 8-segment arches, deep reveals, sills, cornice
    1   120-400 m   real openings, 3-segment arches, flat reveals
    2   400-1200 m  no openings; parallax facade material reconstructs them
    3   1.2-4 km    no openings; flat baked facade

At LOD2/3 each wall is three full-height quads: the run of window bays carries metric UVs,
and the two corner piers carry a constant UV of (0, 0), which the facade material reads as
plain masonry. Every wall's UVs are phased so its bay centres sit at u = BAY/2 (mod BAY);
that is what lets frac(u / BAY) land the reconstructed windows on the real openings.
    4+              the backdrop shell takes over

Distances live in prototypes.json and can be retuned without rebuilding anything.
"""

import argparse
import json
import math
import os
import struct
import sys
import zlib

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paris_city as pc

pc.bpy = None

VERSION = 2

# --------------------------------------------------------------------------- #
# Facade parameters. These are the contract between geometry, texture and shader.
# --------------------------------------------------------------------------- #
BAY = 5.0                   # window pitch, horizontal and vertical
STORY = 5.0                 # floor to floor
SLAB = 0.5                  # structural slab, leaving 4.5 m clear per storey
WALL_T = 0.12               # wall thickness. The brief says ~10 cm; 0.20-0.25 is kinder to
                            # depth precision and shadow bias at kilometre draw distances.
WINDOW_W = 1.0
WINDOW_H = 2.0              # overall, including the arched head
WINDOW_SILL = 1.0           # above each floor level
ARCH_R = WINDOW_W / 2.0     # semicircular head
MIN_PIER = 0.6              # narrowest masonry allowed at a corner or beside a door
DOOR_W = 1.6
DOOR_H = 3.0
DOOR_ARCH_R = 0.8
PASSAGE_W = pc.PASSAGE_WIDTH        # carriage arch into the courtyard (courtyard types)
PASSAGE_H = pc.PASSAGE_HEIGHT
PILLAR = 0.6                # square section of the courtyard colonnade
PILLAR_PITCH = 5.0
CORNICE = 0.35              # LOD0 only: projection at the roofline
REVEAL_LOD0 = 0.20
GLASS_INSET = 0.08          # how far the pane sits behind the outer face

PART_EXTERIOR, PART_GLAZING, PART_INTERIOR, PART_COLLISION, PART_DOOR_GLAZING = 0, 1, 2, 3, 4
PART_NAMES = {PART_EXTERIOR: "exterior", PART_GLAZING: "glazing", PART_INTERIOR: "interior",
              PART_COLLISION: "collision", PART_DOOR_GLAZING: "door_glazing"}
PART_FILES = {PART_EXTERIOR: "shell", PART_GLAZING: "shell", PART_INTERIOR: "interior",
              PART_COLLISION: "collision", PART_DOOR_GLAZING: "shell"}

LOD_BANDS = [
    dict(lod=0, max_distance=120.0, arch_segments=8, reveal=REVEAL_LOD0,
         openings=True, sills=True, cornice=True, colonnade=True),
    dict(lod=1, max_distance=400.0, arch_segments=3, reveal=0.06,
         openings=True, sills=False, cornice=False, colonnade=True),
    dict(lod=2, max_distance=1200.0, arch_segments=0, reveal=0.0,
         openings=False, sills=False, cornice=False, colonnade=False),
    dict(lod=3, max_distance=4000.0, arch_segments=0, reveal=0.0,
         openings=False, sills=False, cornice=False, colonnade=False),
]
INTERIOR_MAX_DISTANCE = 150.0


# --------------------------------------------------------------------------- #
# Mesh accumulation
# --------------------------------------------------------------------------- #
class Mesh:
    """Positions, normals, tangents and metric UVs. Flat shaded: every face owns its
    vertices, which is what this masonry wants and what keeps the arch creases hard."""

    __slots__ = ("v", "f")

    def __init__(self):
        self.v = []          # (pos, nrm, tan, uv)
        self.f = []

    def poly(self, pts, nrm, tan, uvs):
        base = len(self.v)
        for p, uv in zip(pts, uvs):
            self.v.append((p, nrm, tan, uv))
        for k in range(1, len(pts) - 1):
            self.f.append((base, base + k, base + k + 1))

    @property
    def tris(self):
        return len(self.f)

    def aabb(self):
        if not self.v:
            return (0.0, 0.0, 0.0, 0.0, 0.0, 0.0)
        xs = [p[0][0] for p in self.v]
        ys = [p[0][1] for p in self.v]
        zs = [p[0][2] for p in self.v]
        return (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))


def cross3(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm3(a):
    l = math.sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
    return (a[0] / l, a[1] / l, a[2] / l) if l > 1e-12 else (0.0, 0.0, 1.0)


# --------------------------------------------------------------------------- #
# Wall frame: a plane in (u, v) with an outward normal
# --------------------------------------------------------------------------- #
class Frame:
    """Maps wall-local (u along the wall, v up, w outward) to building-local XYZ.
    UVs are metric and identical to (u, v), so frac(u/BAY), frac(v/BAY) addresses the bay."""

    def __init__(self, origin, direction, outward, u_offset=0.0):
        self.o = origin
        self.d = norm3((direction[0], direction[1], 0.0))
        self.n = norm3((outward[0], outward[1], 0.0))
        self.u_offset = u_offset

    def xyz(self, u, v, w=0.0):
        return (self.o[0] + self.d[0] * u + self.n[0] * w,
                self.o[1] + self.d[1] * u + self.n[1] * w,
                v)

    def uv(self, u, v):
        return (u + self.u_offset, v)

    def flipped(self, w):
        """The same plane seen from the other side, offset inward by w."""
        o = (self.o[0] + self.n[0] * w + self.d[0] * 0.0,
             self.o[1] + self.n[1] * w + self.d[1] * 0.0, 0.0)
        return Frame(o, self.d, (-self.n[0], -self.n[1]), self.u_offset)

    def quad(self, mesh, u0, v0, u1, v1, w=0.0, flip=False):
        pts = [self.xyz(u0, v0, w), self.xyz(u1, v0, w),
               self.xyz(u1, v1, w), self.xyz(u0, v1, w)]
        uvs = [self.uv(u0, v0), self.uv(u1, v0), self.uv(u1, v1), self.uv(u0, v1)]
        n = self.n
        if flip:
            pts = pts[::-1]
            uvs = uvs[::-1]
            n = (-n[0], -n[1], 0.0)
        mesh.poly(pts, n, self.d, uvs)

    def tri(self, mesh, a, b, c, w=0.0, flip=False):
        pts = [self.xyz(*a, w), self.xyz(*b, w), self.xyz(*c, w)]
        uvs = [self.uv(*a), self.uv(*b), self.uv(*c)]
        n = self.n
        if flip:
            pts = pts[::-1]
            uvs = uvs[::-1]
            n = (-n[0], -n[1], 0.0)
        mesh.poly(pts, n, self.d, uvs)


def arch_outline(uc, v_spring, half_w, radius, segments):
    """Opening outline in wall space, counter-clockwise seen from outside, starting at the
    bottom left. Rectangular jambs up to the springline, then a semicircular head."""
    pts = [(uc - half_w, 0.0), (uc + half_w, 0.0), (uc + half_w, v_spring)]
    if segments > 0:
        for k in range(1, segments):
            a = math.pi * k / segments
            pts.append((uc + radius * math.cos(a), v_spring + radius * math.sin(a)))
    pts.append((uc - half_w, v_spring + (radius if segments == 0 else 0.0)))
    return pts


# --------------------------------------------------------------------------- #
# The plan
# --------------------------------------------------------------------------- #
class BuildingPlan:
    """Everything the four LOD bands and the interior agree on."""

    def __init__(self, type_index, btype):
        self.index = type_index
        self.name = btype.name
        self.height = float(btype.H)
        self.courtyard = bool(btype.courtyard)
        # footprint in prototype local space: origin at the middle of the street edge
        self.footprint = [(x - btype.F / 2.0, y) for x, y in btype.local]
        self.inner = None
        if self.courtyard:
            inner = pc.inset_quad(self.footprint, pc.COURTYARD_WALL)
            if inner and pc.poly_area(inner) > 25.0 and \
                    min(math.dist(inner[k], inner[(k + 1) % 4]) for k in range(4)) >= 5.0:
                self.inner = inner
            else:
                self.courtyard = False

        self.floors = [f * STORY for f in range(int(round(self.height / STORY)))]
        self.walls = self._walls(self.footprint, outward=True)
        self.bays = [self._fit_bays(w, street=(i == 0)) for i, w in enumerate(self.walls)]
        # Phase each wall's UVs so its bay centres fall at BAY/2 (mod BAY); bays on a wall
        # are exactly BAY apart, so one offset per wall lines up all of them.
        for wall, bays in zip(self.walls, self.bays):
            wall["frame"].u_offset = (BAY / 2.0 - bays[0]) % BAY if bays else 0.0
        self.doors = self._doors()
        self.pillars = self._pillars()

    # -- geometry of the shell ------------------------------------------------
    @staticmethod
    def _walls(poly, outward=True):
        """CCW polygon: interior on the left, so the outward normal is the right normal."""
        out = []
        n = len(poly)
        u_run = 0.0
        for i in range(n):
            a, b = poly[i], poly[(i + 1) % n]
            d = (b[0] - a[0], b[1] - a[1])
            L = math.hypot(*d)
            nrm = (d[1] / L, -d[0] / L) if outward else (-d[1] / L, d[0] / L)
            out.append(dict(index=i, a=a, b=b, length=L,
                            frame=Frame((a[0], a[1], 0.0), d, nrm, u_offset=u_run)))
            u_run += L
        return out

    def _fit_bays(self, wall, street):
        """Window centres along a wall. The street wall is phased half a bay so the door
        lands in a pier rather than forcing a deletion, which would read as a gap."""
        L = wall["length"]
        mid = L / 2.0
        edge = MIN_PIER + WINDOW_W / 2.0
        centres, k = [], 0
        while True:
            if street:
                offs = [mid + (k + 0.5) * BAY, mid - (k + 0.5) * BAY]
            else:
                offs = [mid + k * BAY] if k == 0 else [mid + k * BAY, mid - k * BAY]
            placed = False
            for u in offs:
                if edge <= u <= L - edge:
                    centres.append(u)
                    placed = True
            if not placed and k > 0:
                break
            if k > 64:
                break
            k += 1
        return sorted(centres)

    def _doors(self):
        """One opening in the middle of the street-facing wall. Courtyard types get the
        carriage arch there instead, with pedestrian doors in the flanking bays."""
        wall = self.walls[0]
        mid = wall["length"] / 2.0
        if self.courtyard:
            doors = [dict(u=mid, width=PASSAGE_W, height=PASSAGE_H, radius=PASSAGE_W / 2.0,
                          kind="carriage")]
            flank = [u for u in self.bays[0] if abs(u - mid) < BAY * 1.01]
            for u in flank[:2]:
                doors.append(dict(u=u, width=DOOR_W, height=DOOR_H, radius=DOOR_ARCH_R,
                                  kind="door"))
            return doors
        if wall["length"] < DOOR_W + 2 * MIN_PIER:
            return []
        return [dict(u=mid, width=DOOR_W, height=DOOR_H, radius=DOOR_ARCH_R, kind="door")]

    def _pillars(self):
        """Courtyard colonnade: the inner ring is open, pillars at every floor."""
        if not self.courtyard:
            return []
        out = []
        for wall in self._walls(self.inner, outward=False):
            L = wall["length"]
            n = max(2, int(round(L / PILLAR_PITCH)))
            for k in range(n + 1):
                u = L * k / n
                p = (wall["a"][0] + (wall["b"][0] - wall["a"][0]) * (u / L),
                     wall["a"][1] + (wall["b"][1] - wall["a"][1]) * (u / L))
                out.append(p)
        # drop duplicates at the ring corners
        uniq = []
        for p in out:
            if not any(math.dist(p, q) < 0.4 for q in uniq):
                uniq.append(p)
        return uniq

    def door_at(self, wall_index, u, tol=0.3):
        if wall_index != 0:
            return None
        for d in self.doors:
            if abs(d["u"] - u) < tol:
                return d
        return None

    def openings_blocked(self, wall_index, u):
        """True if a ground-floor window position is consumed by a door."""
        for d in self.doors if wall_index == 0 else []:
            if abs(d["u"] - u) < (d["width"] + WINDOW_W) / 2.0 + 0.2:
                return True
        return False

    def as_dict(self):
        return dict(
            type=self.index, name=self.name, height=self.height, courtyard=self.courtyard,
            footprint=[[round(x, 3), round(y, 3)] for x, y in self.footprint],
            floors=[round(z, 3) for z in self.floors],
            storey_height=STORY, slab=SLAB, wall_thickness=WALL_T,
            walls=[dict(index=w["index"], length=round(w["length"], 3),
                        a=[round(w["a"][0], 3), round(w["a"][1], 3)],
                        b=[round(w["b"][0], 3), round(w["b"][1], 3)],
                        bays=[round(u, 3) for u in self.bays[i]])
                   for i, w in enumerate(self.walls)],
            doors=[dict(wall=0, u=round(d["u"], 3), width=d["width"],
                        height=d["height"], kind=d["kind"]) for d in self.doors],
            pillars=[[round(p[0], 3), round(p[1], 3)] for p in self.pillars],
        )


# --------------------------------------------------------------------------- #
# Emission
# --------------------------------------------------------------------------- #
def _panel_bounds(ops, L):
    """Split a wall into one panel per opening, cut at the midpoints between them."""
    out = []
    for i, o in enumerate(ops):
        left = 0.0 if i == 0 else (ops[i - 1]["u"] + o["u"]) / 2.0
        right = L if i == len(ops) - 1 else (o["u"] + ops[i + 1]["u"]) / 2.0
        left = max(left, o["u"] - BAY / 2.0)
        right = min(right, o["u"] + BAY / 2.0)
        out.append((max(0.0, left), min(L, right)))
    return out


def _panel_with_opening(mesh, frame, u0, u1, v_base, v_top, op, segments):
    """Solid wall inside the rectangle, minus one arched opening. op is None for a blank
    panel. Emitted as: below, above, left, right, and the two spandrels beside the arch."""
    if op is None:
        frame.quad(mesh, u0, v_base, u1, v_top)
        return []
    uc = op["u"]
    hw = op["width"] / 2.0
    sill = v_base + op["sill"]
    rad = op["radius"]
    spring = sill + op["height"] - rad
    head = sill + op["height"]
    if sill > v_base + 1e-4:
        frame.quad(mesh, u0, v_base, u1, sill)
    if v_top > head + 1e-4:
        frame.quad(mesh, u0, head, u1, v_top)
    frame.quad(mesh, u0, sill, uc - hw, head)
    frame.quad(mesh, uc + hw, sill, u1, head)
    outline = arch_outline(uc, spring - sill, hw, rad, segments)
    outline = [(u, sill + dv) for u, dv in outline]
    if segments > 0:                       # spandrels between the arch and the head line
        arch = outline[2:-1]
        for a, b in zip(arch, arch[1:]):
            corner = (uc - hw, head) if (a[0] + b[0]) / 2 < uc else (uc + hw, head)
            frame.tri(mesh, a, b, corner)
        frame.tri(mesh, arch[0], (uc + hw, head), (uc + hw, spring))
        frame.tri(mesh, arch[-1], (uc - hw, spring), (uc - hw, head))
    return outline


def _reveal(mesh, frame, outline, depth, flip_inner=False):
    """The returns around an opening, from the outer face inwards."""
    n = len(outline)
    for k in range(n):
        a = outline[k]
        b = outline[(k + 1) % n]
        p0 = frame.xyz(a[0], a[1], 0.0)
        p1 = frame.xyz(b[0], b[1], 0.0)
        p2 = frame.xyz(b[0], b[1], -depth)
        p3 = frame.xyz(a[0], a[1], -depth)
        nrm = norm3(cross3((p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]),
                           (p3[0] - p0[0], p3[1] - p0[1], p3[2] - p0[2])))
        mesh.poly([p0, p1, p2, p3], nrm, frame.d,
                  [frame.uv(*a), frame.uv(*b), frame.uv(*b), frame.uv(*a)])


def _glass(mesh, frame, outline, inset):
    pts = [frame.xyz(u, v, -inset) for u, v in outline]
    uvs = [frame.uv(u, v) for u, v in outline]
    mesh.poly(pts, frame.n, frame.d, uvs)


def _ring(mesh, outer, inner, z, up=True):
    """Flat ring between two 4-gons (slab or roof around a courtyard)."""
    for k in range(len(outer)):
        a, b = outer[k], outer[(k + 1) % len(outer)]
        c, d = inner[k], inner[(k + 1) % len(inner)]
        pts = [(a[0], a[1], z), (b[0], b[1], z), (d[0], d[1], z), (c[0], c[1], z)]
        if not up:
            pts = pts[::-1]
        mesh.poly(pts, (0.0, 0.0, 1.0 if up else -1.0), (1.0, 0.0, 0.0),
                  [(p[0], p[1]) for p in pts])


def _plate(mesh, poly, z, up=True):
    pts = [(x, y, z) for x, y in poly]
    if not up:
        pts = pts[::-1]
    mesh.poly(pts, (0.0, 0.0, 1.0 if up else -1.0), (1.0, 0.0, 0.0),
              [(p[0], p[1]) for p in pts])


def _box(mesh, cx, cy, z0, z1, half, axis_u=(1.0, 0.0)):
    ax = norm3((axis_u[0], axis_u[1], 0.0))
    ay = (-ax[1], ax[0], 0.0)
    corners = [(cx + (ax[0] * sx + ay[0] * sy) * half,
                cy + (ax[1] * sx + ay[1] * sy) * half)
               for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    for k in range(4):
        a, b = corners[k], corners[(k + 1) % 4]
        d = norm3((b[0] - a[0], b[1] - a[1], 0.0))
        nrm = (d[1], -d[0], 0.0)
        mesh.poly([(a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1), (a[0], a[1], z1)],
                  nrm, d, [(0, z0), (2 * half, z0), (2 * half, z1), (0, z1)])
    _plate(mesh, corners, z1, up=True)


def emit_exterior(plan, band):
    """Outer shell for one LOD band. Returns (exterior mesh, glazing mesh)."""
    shell, glass, door_glass = Mesh(), Mesh(), Mesh()
    segs = band["arch_segments"]
    for wi, wall in enumerate(plan.walls):
        frame = wall["frame"]
        L = wall["length"]
        for fi, z in enumerate(plan.floors):
            v0, v1 = z, z + STORY
            if not band["openings"]:
                continue
            ops = []
            for u in plan.bays[wi]:
                if fi == 0 and plan.openings_blocked(wi, u):
                    continue
                ops.append(dict(u=u, width=WINDOW_W, height=WINDOW_H, sill=WINDOW_SILL,
                                radius=ARCH_R))
            if fi == 0 and wi == 0:
                for d in plan.doors:
                    ops.append(dict(u=d["u"], width=d["width"], height=d["height"],
                                    sill=0.0, radius=d["radius"], kind=d["kind"]))
            ops.sort(key=lambda o: o["u"])
            # panel boundaries are the midpoints between neighbouring openings, so a door
            # sitting closer than one bay to a window still gets a panel of its own
            bounds = _panel_bounds(ops, L)
            prev = 0.0
            for oi, o in enumerate(ops):
                left, right = bounds[oi]
                if left > prev + 1e-4:
                    frame.quad(shell, prev, v0, left, v1)
                outline = _panel_with_opening(shell, frame, left, right, v0, v1, o, segs)
                if band["reveal"] > 0.0:
                    _reveal(shell, frame, outline, band["reveal"])
                # Doors get their own part: the player walks through them, so the engine
                # drops the pane wherever it draws the real interior behind. The carriage
                # passage is an open tunnel and gets none.
                kind = o.get("kind")
                if kind is None:
                    _glass(glass, frame, outline, GLASS_INSET)
                elif kind == "door":
                    _glass(door_glass, frame, outline, GLASS_INSET)
                prev = right
            if prev < L - 1e-4:
                frame.quad(shell, prev, v0, L, v1)
        if not band["openings"]:
            _flat_wall(shell, frame, L, plan.height, plan.bays[wi])
        if band["cornice"]:
            frame.quad(shell, 0.0, plan.height - CORNICE, L, plan.height, w=CORNICE * 0.5)

    if plan.courtyard:
        _ring(shell, plan.footprint, plan.inner, plan.height, up=True)
        if band["colonnade"]:
            _colonnade(shell, plan, band)
            _passage(shell, plan)
        else:
            # too far for pillars to read: close the ring with plain faces
            for wall in BuildingPlan._walls(plan.inner, outward=False):
                wall["frame"].quad(shell, 0.0, 0.0, wall["length"], plan.height)
    else:
        _plate(shell, plan.footprint, plan.height, up=True)
    return shell, glass, door_glass


def _flat_wall(shell, frame, L, height, bays):
    """A wall without openings for the facade material to fill in. Only the run of bays
    gets real UVs; the corner piers get a constant (0, 0), which is masonry in the tile, so
    the material can't put a window where the geometry has a pier."""
    if not bays:
        _blank_quad(shell, frame, 0.0, L, height)
        return
    u0 = max(0.0, bays[0] - BAY / 2.0)
    u1 = min(L, bays[-1] + BAY / 2.0)
    if u0 > 1e-4:
        _blank_quad(shell, frame, 0.0, u0, height)
    frame.quad(shell, u0, 0.0, u1, height)
    if u1 < L - 1e-4:
        _blank_quad(shell, frame, u1, L, height)


def _blank_quad(shell, frame, u0, u1, height):
    pts = [frame.xyz(u0, 0.0), frame.xyz(u1, 0.0), frame.xyz(u1, height), frame.xyz(u0, height)]
    shell.poly(pts, frame.n, frame.d, [(0.0, 0.0)] * 4)


def emit_collision(plan):
    """What the player collides with: the shell as solid slabs, open wherever the
    building is open at ground level, so the player can walk in through a door, through
    the carriage passage, and between the courtyard pillars into the arcade. Windows
    stay solid; they are glazed. Openings are cut as rectangles of the door's full
    height, which only gives up the arch's corner spandrels."""
    mesh = Mesh()
    for wi, wall in enumerate(plan.walls):
        frame, L = wall["frame"], wall["length"]
        gaps = sorted((d["u"] - d["width"] / 2.0, d["u"] + d["width"] / 2.0, d["height"])
                      for d in (plan.doors if wi == 0 else []))
        u = 0.0
        for g0, g1, gh in gaps:
            _slab(mesh, frame, u, g0, 0.0, plan.height)      # pier up to the roof
            _slab(mesh, frame, g0, g1, gh, plan.height)      # lintel and wall above
            u = g1
        _slab(mesh, frame, u, L, 0.0, plan.height)

    if plan.courtyard:
        _ring(mesh, plan.footprint, plan.inner, plan.height, up=True)
        for p in plan.pillars:
            _box(mesh, p[0], p[1], 0.0, plan.height, PILLAR / 2.0)
        # Carriage passage: a tunnel through the street wing, walled off from the
        # ground-floor rooms either side and roofed at the passage height.
        frame = plan.walls[0]["frame"]
        mid = plan.walls[0]["length"] / 2.0
        depth = pc.COURTYARD_WALL
        for side in (-1.0, 1.0):
            # A frame running into the block from the gap's edge, facing the tunnel, so
            # the slab extrudes away from the tunnel into the room beside it.
            e = frame.xyz(mid + side * PASSAGE_W / 2.0, 0.0)
            side_frame = Frame((e[0], e[1], 0.0), (-frame.n[0], -frame.n[1]),
                               (-side * frame.d[0], -side * frame.d[1]))
            _slab(mesh, side_frame, 0.0, depth, 0.0, PASSAGE_H)
        _slab_flat(mesh, frame, mid - PASSAGE_W / 2.0, mid + PASSAGE_W / 2.0,
                   -WALL_T, -depth, PASSAGE_H)
    else:
        _plate(mesh, plan.footprint, plan.height, up=True)
    return mesh


def _slab(mesh, frame, u0, u1, v0, v1, thickness=WALL_T, w_offset=0.0):
    """A closed box on a wall: u0..u1 along it, v0..v1 up, from w_offset back by thickness."""
    if u1 - u0 < 1e-4 or v1 - v0 < 1e-4:
        return
    wo, wi = w_offset, w_offset - thickness
    frame.quad(mesh, u0, v0, u1, v1, w=wo)                    # outer face
    frame.quad(mesh, u0, v0, u1, v1, w=wi, flip=True)         # inner face
    for u, sgn in ((u0, -1.0), (u1, 1.0)):                    # ends
        p = [frame.xyz(u, v0, wo), frame.xyz(u, v0, wi), frame.xyz(u, v1, wi), frame.xyz(u, v1, wo)]
        if sgn < 0:
            p = p[::-1]
        mesh.poly(p, (frame.d[0] * sgn, frame.d[1] * sgn, 0.0), frame.n, [(0.0, 0.0)] * 4)
    for v, up in ((v1, True), (v0, False)):                   # top and bottom
        p = [frame.xyz(u0, v, wo), frame.xyz(u1, v, wo), frame.xyz(u1, v, wi), frame.xyz(u0, v, wi)]
        if not up:
            p = p[::-1]
        mesh.poly(p, (0.0, 0.0, 1.0 if up else -1.0), frame.d, [(0.0, 0.0)] * 4)


def _slab_flat(mesh, frame, u0, u1, w0, w1, z):
    """A horizontal plate under a wall frame: u0..u1 along it, w0..w1 out from it, at z,
    both faces (the passage ceiling seen from below, the room floor from above)."""
    p = [frame.xyz(u0, z, w0), frame.xyz(u1, z, w0), frame.xyz(u1, z, w1), frame.xyz(u0, z, w1)]
    mesh.poly(p, (0.0, 0.0, 1.0), frame.d, [(0.0, 0.0)] * 4)
    mesh.poly(p[::-1], (0.0, 0.0, -1.0), frame.d, [(0.0, 0.0)] * 4)


def _colonnade(shell, plan, band):
    """Pillars and a spandrel beam at every floor, in place of a courtyard wall."""
    for p in plan.pillars:
        for z in plan.floors:
            _box(shell, p[0], p[1], z, z + STORY - SLAB, PILLAR / 2.0)
    for wall in BuildingPlan._walls(plan.inner, outward=False):
        frame = wall["frame"]
        for z in plan.floors:
            frame.quad(shell, 0.0, z + STORY - SLAB, wall["length"], z + STORY)


def _passage(shell, plan):
    """Carriage arch tunnel through the street wing into the courtyard."""
    wall = plan.walls[0]
    frame = wall["frame"]
    mid = wall["length"] / 2.0
    depth = pc.COURTYARD_WALL
    for side in (-1.0, 1.0):
        u = mid + side * PASSAGE_W / 2.0
        a = frame.xyz(u, 0.0, 0.0)
        b = frame.xyz(u, 0.0, -depth)
        nrm = (frame.d[0] * -side, frame.d[1] * -side, 0.0)
        shell.poly([(a[0], a[1], 0.0), (b[0], b[1], 0.0),
                    (b[0], b[1], PASSAGE_H), (a[0], a[1], PASSAGE_H)],
                   nrm, (0.0, 0.0, 1.0), [(0, 0), (depth, 0), (depth, PASSAGE_H),
                                          (0, PASSAGE_H)])
    c0 = frame.xyz(mid - PASSAGE_W / 2.0, 0.0, 0.0)
    c1 = frame.xyz(mid + PASSAGE_W / 2.0, 0.0, 0.0)
    d0 = frame.xyz(mid - PASSAGE_W / 2.0, 0.0, -depth)
    d1 = frame.xyz(mid + PASSAGE_W / 2.0, 0.0, -depth)
    shell.poly([(c0[0], c0[1], PASSAGE_H), (d0[0], d0[1], PASSAGE_H),
                (d1[0], d1[1], PASSAGE_H), (c1[0], c1[1], PASSAGE_H)],
               (0.0, 0.0, -1.0), (1.0, 0.0, 0.0),
               [(0, 0), (depth, 0), (depth, PASSAGE_W), (0, PASSAGE_W)])


def emit_interior(plan, band):
    """Enclosed rooms only: inner wall faces and floor slabs. Never the colonnade."""
    mesh = Mesh()
    segs = band["arch_segments"]
    for wi, wall in enumerate(plan.walls):
        inner = wall["frame"].flipped(-WALL_T)
        L = wall["length"]
        for fi, z in enumerate(plan.floors):
            ops = []
            for u in plan.bays[wi]:
                if fi == 0 and plan.openings_blocked(wi, u):
                    continue
                ops.append(dict(u=L - u, width=WINDOW_W, height=WINDOW_H,
                                sill=WINDOW_SILL, radius=ARCH_R))
            if fi == 0 and wi == 0:
                for d in plan.doors:
                    ops.append(dict(u=L - d["u"], width=d["width"], height=d["height"],
                                    sill=0.0, radius=d["radius"]))
            ops.sort(key=lambda o: o["u"])
            bounds = _panel_bounds(ops, L)
            prev = 0.0
            for oi, o in enumerate(ops):
                left, right = bounds[oi]
                if left > prev + 1e-4:
                    inner.quad(mesh, prev, z, left, z + STORY)
                _panel_with_opening(mesh, inner, left, right, z, z + STORY, o, segs)
                prev = right
            if prev < L - 1e-4:
                inner.quad(mesh, prev, z, L, z + STORY)

    slab_outer = pc.inset_quad(plan.footprint, WALL_T) or plan.footprint
    for z in plan.floors:
        top, bottom = z + STORY, z + STORY - SLAB
        # The top storey's slab is the roof: its upper face would be coplanar with the
        # exterior roof and z-fight, so only its ceiling is emitted.
        roof = top >= plan.height - 1e-3
        if plan.courtyard:
            _ring(mesh, slab_outer, plan.inner, bottom, up=False)
            if not roof:
                _ring(mesh, slab_outer, plan.inner, top, up=True)
        else:
            _plate(mesh, slab_outer, bottom, up=False)
            if not roof:
                _plate(mesh, slab_outer, top, up=True)
    return mesh


# --------------------------------------------------------------------------- #
# Facade tile: the same numbers the geometry used, for LOD2's parallax material
# --------------------------------------------------------------------------- #
def facade_tile_spec():
    return dict(
        size=BAY, window=dict(width=WINDOW_W, height=WINDOW_H, sill=WINDOW_SILL,
                              arch_radius=ARCH_R),
        recess=REVEAL_LOD0, brick=dict(length=0.21, height=0.065, mortar=0.012),
        note="u and v are metres; frac(u/size), frac(v/size) addresses the bay",
    )


def _png(path, rgba):
    h, w, _ = rgba.shape
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">2I5B", w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def bake_facade_tile(path, px=512):
    """One 5 x 5 m bay: RGB is sintered-regolith masonry, A is the parallax depth mask
    (0 = wall face, 1 = fully recessed opening). Swap for authored art later; the geometry
    reads the same numbers, so the window lands in the same place either way."""
    u = (np.arange(px) + 0.5) / px * BAY
    v = (np.arange(px) + 0.5) / px * BAY
    U, V = np.meshgrid(u, v, indexing="xy")
    rng = np.random.default_rng(7)

    course = np.floor(V / 0.077)
    offset = (course % 2) * 0.105
    bu = np.mod(U + offset, 0.21)
    bv = np.mod(V, 0.077)
    mortar = (bu < 0.012) | (bv < 0.012)
    base = 0.62 + rng.normal(0, 0.035, (px, px)) + 0.02 * np.sin(course * 1.7)
    base = np.where(mortar, base * 0.86, base)
    base = np.clip(base, 0.0, 1.0)

    uc, sill = BAY / 2.0, WINDOW_SILL
    spring = sill + WINDOW_H - ARCH_R
    in_rect = (np.abs(U - uc) < WINDOW_W / 2) & (V > sill) & (V < spring)
    in_arch = (np.hypot(U - uc, V - spring) < ARCH_R) & (V >= spring)
    hole = in_rect | in_arch

    rgba = np.zeros((px, px, 4), dtype=np.uint8)
    rgb = np.where(hole, 0.10, base)
    for c in range(3):
        tint = (1.0, 0.995, 0.985)[c]
        rgba[..., c] = np.clip(rgb * tint * 255, 0, 255).astype(np.uint8)
    rgba[..., 3] = (hole * 255).astype(np.uint8)
    _png(path, rgba[::-1])
    return dict(file=os.path.basename(path), pixels=px, metres=BAY)


# --------------------------------------------------------------------------- #
# Validation: these four checks catch nearly every popping or clipping bug
# --------------------------------------------------------------------------- #
def validate(plan, meshes):
    problems = []
    foot = [b for (lod, part, m) in meshes if part == PART_EXTERIOR
            for b in [m.aabb()]]
    for a, b in zip(foot, foot[1:]):
        for k in range(6):
            if abs(a[k] - b[k]) > 0.5:        # cornice and reveals are allowed to differ
                problems.append(f"{plan.name}: LOD shells disagree on bounds ({k})")
                break
    ext = [m for (lod, part, m) in meshes if lod == 0 and part == PART_EXTERIOR]
    inte = [m for (lod, part, m) in meshes if part == PART_INTERIOR]
    if ext and inte:
        e, i = ext[0].aabb(), inte[0].aabb()
        if not (e[0] - 0.01 <= i[0] and e[1] - 0.01 <= i[1]
                and i[3] <= e[3] + 0.01 and i[4] <= e[4] + 0.01):
            problems.append(f"{plan.name}: interior escapes the exterior shell")
    for wi, wall in enumerate(plan.walls):
        L = wall["length"]
        for u in plan.bays[wi]:
            if u - WINDOW_W / 2 < MIN_PIER - 1e-6 or u + WINDOW_W / 2 > L - MIN_PIER + 1e-6:
                problems.append(f"{plan.name}: wall {wi} bay at {u:.2f} breaks the corner pier")
    for d in plan.doors:
        for u in plan.bays[0]:
            if abs(u - d["u"]) < (d["width"] + WINDOW_W) / 2 + MIN_PIER - 1e-6 \
                    and not plan.openings_blocked(0, u):
                problems.append(f"{plan.name}: window at {u:.2f} collides with a door")
    return problems


# --------------------------------------------------------------------------- #
# Writing
# --------------------------------------------------------------------------- #
def pack_mesh(mesh):
    vtx = bytearray()
    for pos, nrm, tan, uv in mesh.v:
        vtx += struct.pack("<12f", pos[0], pos[1], pos[2], nrm[0], nrm[1], nrm[2],
                           tan[0], tan[1], tan[2], 1.0, uv[0], uv[1])
    idx = bytearray()
    for tri in mesh.f:
        idx += struct.pack("<3I", *tri)
    return bytes(vtx), bytes(idx), len(mesh.v), len(mesh.f) * 3


def build_all(out_dir, bake=False, report=False):
    os.makedirs(out_dir, exist_ok=True)
    entries, blobs, total_tris = [], [], 0
    all_problems = []
    plans = []

    for ti, btype in enumerate(pc.BUILDING_TYPES):
        plan = BuildingPlan(ti, btype)
        plans.append(plan)
        meshes = []
        for band in LOD_BANDS:
            shell, glass, door_glass = emit_exterior(plan, band)
            meshes.append((band["lod"], PART_EXTERIOR, shell))
            if band["openings"]:
                meshes.append((band["lod"], PART_DOOR_GLAZING, door_glass))
            if band["openings"]:
                meshes.append((band["lod"], PART_GLAZING, glass))
        meshes.append((0, PART_INTERIOR, emit_interior(plan, LOD_BANDS[0])))
        all_problems += validate(plan, meshes)
        meshes.append((0, PART_COLLISION, emit_collision(plan)))

        for lod, part, mesh in meshes:
            if not mesh.f:
                continue
            vtx, idx, nv, ni = pack_mesh(mesh)
            entries.append(dict(type=ti, lod=lod, part=part,
                                file=PART_FILES[part],
                                vertices=nv, indices=ni,
                                aabb=[round(c, 3) for c in mesh.aabb()],
                                triangles=mesh.tris))
            blobs.append((ti, lod, part, nv, ni, vtx, idx))
            if part != PART_COLLISION:
                total_tris += mesh.tris
        if report:
            per = {}
            for lod, part, mesh in meshes:
                if part == PART_COLLISION:
                    continue
                per.setdefault(lod, 0)
                per[lod] += mesh.tris
            print(f"  {plan.name:<16} {int(plan.height):>3} m  "
                  f"{len(plan.floors)} floors  "
                  f"bays {sum(len(b) for b in plan.bays):>3}  "
                  f"tris {per}")

    shells = [b for b in blobs if PART_FILES[b[2]] == "shell"]
    interiors = [b for b in blobs if b[2] == PART_INTERIOR]
    collision = [b for b in blobs if b[2] == PART_COLLISION]

    def write_blob(name, items):
        p = os.path.join(out_dir, name)
        with open(p, "wb") as f:
            f.write(b"PROT" + struct.pack("<2I", VERSION, len(items)))
            for ti, lod, part, nv, ni, vtx, idx in items:
                f.write(struct.pack("<3H2I", ti, lod, part, nv, ni))
                f.write(vtx)
                f.write(idx)
        return p

    path = write_blob("prototypes.bin", shells)
    ipath = write_blob("prototypes_interior.bin", interiors)
    write_blob("prototypes_collision.bin", collision)

    index = dict(
        version=VERSION,
        vertex_stride=48,
        vertex_layout=["position:3f", "normal:3f", "tangent:4f", "texcoord0:2f"],
        parts={str(k): v for k, v in PART_NAMES.items()},
        lod_bands=[dict(lod=b["lod"], max_distance=b["max_distance"],
                        openings=b["openings"]) for b in LOD_BANDS],
        interior_max_distance=INTERIOR_MAX_DISTANCE,
        facade_tile=facade_tile_spec(),
        files=dict(shell="prototypes.bin", interior="prototypes_interior.bin",
                   collision="prototypes_collision.bin"),
        meshes=entries,
        plans=[p.as_dict() for p in plans],
    )
    if bake:
        index["facade_tile"]["baked"] = bake_facade_tile(
            os.path.join(out_dir, "facade_tile.png"))
    json.dump(index, open(os.path.join(out_dir, "prototypes.json"), "w"), indent=1)

    print(f"  {len(pc.BUILDING_TYPES)} types, {len(blobs)} meshes, {total_tris} triangles")
    print(f"  prototypes.bin {os.path.getsize(path) / 1e6:.1f} MB  "
          f"prototypes_interior.bin {os.path.getsize(ipath) / 1e6:.1f} MB")
    if all_problems:
        print("  VALIDATION:")
        for p in sorted(set(all_problems)):
            print("   -", p)
    else:
        print("  validation clean")
    return index


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="world")
    ap.add_argument("--bake-tile", action="store_true")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    build_all(a.out, bake=a.bake_tile, report=a.report)
