#!/usr/bin/env python3
"""
tools/generate_tilemap.py — Eschaton cylinder biome map generator.

Implements the five-pass procedural pipeline from CLAUDE.md:

  Pass 1 — Water:     Poisson disk lake seeds → noise-modulated blob growth → A* rivers
  Pass 2 — Green:     Primary forest reservation + waterfront BFS ring expansion
  Pass 3 — Fill:      Remaining cells split into urban/industrial via value noise
  Pass 4 — Cleanup:   Industrial-waterfront removal, majority-vote boundary smoothing
  Pass 5 — (Tile ID assignment is omitted here — this script outputs the biome PNG only)

Output PNG layout:
  X-axis → Z  (cylinder axis, 0 – 32 km)
  Y-axis → θ  (circumference, 0 – 25 km)
  Each cell is --cell-px pixels square (default 20 px → 125 m × 123 m per cell)

Colours:
  Blue       = water       Green      = park
  Light gray = urban       Dark gray  = industrial

Requires: Pillow  (pip install Pillow)

Usage examples:
  python tools/generate_tilemap.py
  python tools/generate_tilemap.py --seed 12345 --meander 0.9 --output test.png
  python tools/generate_tilemap.py --grid-z 64 --grid-theta 51 --cell-px 8
"""

from __future__ import annotations

import argparse
from collections import deque
import heapq
import math
import random
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required.  Install it with:  pip install Pillow")


# ---------------------------------------------------------------------------
# Biome IDs and display colours
# ---------------------------------------------------------------------------

UNASSIGNED  = 0
WATER       = 1
PARK        = 2
URBAN       = 3
INDUSTRIAL  = 4

COLORS: dict[int, tuple[int, int, int]] = {
    UNASSIGNED:  ( 50,  50,  50),
    WATER:       ( 40, 110, 220),
    PARK:        ( 55, 165,  55),
    URBAN:       (195, 195, 195),
    INDUSTRIAL:  ( 90,  90,  90),
}

BIOME_NAMES: dict[int, str] = {
    UNASSIGNED: "Unassigned",
    WATER:      "Water",
    PARK:       "Park",
    URBAN:      "Urban",
    INDUSTRIAL: "Industrial",
}

DIRS4 = ((-1, 0), (1, 0), (0, -1), (0, 1))
DIRS8 = [(-1,-1),(-1, 0),(-1, 1),
         ( 0,-1),         ( 0, 1),
         ( 1,-1),( 1, 0),( 1, 1)]


def _torus_dt(t1: float, t2: float, grid_t: int) -> float:
    """Shortest angular distance in theta, wrapping at grid_t."""
    dt = abs(t1 - t2)
    return min(dt, grid_t - dt)


# ---------------------------------------------------------------------------
# Value noise  (no external deps — smooth bilinear noise on a random lattice)
# ---------------------------------------------------------------------------

def _smoothstep(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


def make_value_noise(
    rng: random.Random,
    rows: int,
    cols: int,
    wavelength: float,
) -> list[list[float]]:
    """
    2D smooth value noise over a (rows × cols) grid.
    wavelength controls feature scale in cells.  Returns values in [0, 1].
    Independent noise fields are produced on each call because the shared RNG
    advances its state, giving each pass its own distinct spatial pattern.

    The noise is always seamlessly periodic in the column (theta) direction.
    This is achieved by choosing lt = round(cols / wavelength) lattice columns
    so that the period divides cols exactly — the effective per-column
    wavelength becomes cols/lt rather than the requested wavelength.  At the
    seam (c == cols wraps to c == 0) the interpolation is continuous because
    the right-hand lattice column for ic == lt-1 is lattice column 0.
    """
    lz = max(2, math.ceil(rows / wavelength) + 2)  # Z is not periodic
    lt = max(2, round(cols / wavelength))            # theta IS periodic
    eff_wl_t = cols / lt                             # exact tiling wavelength

    lattice = [[rng.random() for _ in range(lt)] for _ in range(lz)]

    out = [[0.0] * cols for _ in range(rows)]
    for r in range(rows):
        fz = r / wavelength
        iz = int(fz)
        tz = _smoothstep(fz - iz)
        for c in range(cols):
            fc     = c / eff_wl_t
            ic_raw = int(fc)
            ic     = ic_raw % lt          # should already be < lt; % is safety
            tc     = _smoothstep(fc - ic_raw)
            v00 = lattice[ iz      % lz][ ic          ]
            v10 = lattice[(iz + 1) % lz][ ic          ]
            v01 = lattice[ iz      % lz][(ic + 1) % lt]
            v11 = lattice[(iz + 1) % lz][(ic + 1) % lt]
            out[r][c] = (v00 * (1 - tz) * (1 - tc)
                       + v10 *      tz  * (1 - tc)
                       + v01 * (1 - tz) *      tc
                       + v11 *      tz  *      tc)
    return out


# ---------------------------------------------------------------------------
# Pass 1: Water
# ---------------------------------------------------------------------------

def _poisson_disk_seeds(
    rng: random.Random,
    grid_z: int, grid_t: int,
    min_sep: float,
    n_min: int, n_max: int,
    end_sea_depth: int = 0,
) -> list[tuple[int, int]]:
    """
    Poisson disk sampling for lake seed positions.
    Guarantees seeds are at least min_sep cells apart and spread across the grid.
    Seeds are confined to z in [end_sea_depth, grid_z - end_sea_depth) so they
    never land inside the pre-filled end-sea zones.
    Returns a list of (z, t) integer positions.
    """
    target = rng.randint(n_min, n_max)
    cell   = min_sep / math.sqrt(2)
    bg_h   = math.ceil(grid_z / cell)
    bg_w   = math.ceil(grid_t / cell)
    bg: list[list[int]] = [[-1] * bg_w for _ in range(bg_h)]
    pts: list[tuple[float, float]] = []
    active: list[int] = []

    # Usable Z range, excluding the solid-water end-sea zones.
    z_lo = float(end_sea_depth)
    z_hi = float(grid_z - end_sea_depth)

    # bg_cell wraps theta so the background grid is also toroidal.
    def bg_cell(z: float, t: float) -> tuple[int, int]:
        return int(z / cell), int((t % grid_t) / cell)

    def valid(z: float, t: float) -> bool:
        # Z must lie within the non-sea zone; theta wraps.
        if not (z_lo <= z < z_hi):
            return False
        t = t % grid_t
        gy, gx = bg_cell(z, t)
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                ny, nx = gy + dy, (gx + dx) % bg_w   # wrap theta bg column
                if 0 <= ny < bg_h:
                    si = bg[ny][nx]
                    if si >= 0:
                        sz, st = pts[si]
                        dist = math.hypot(z - sz, _torus_dt(t, st, grid_t))
                        if dist < min_sep:
                            return False
        return True

    # Keep seeds away from the sea zones and away from the Z edges.
    margin_z = min_sep * 0.5
    z0 = rng.uniform(z_lo + margin_z, z_hi - margin_z)
    t0 = rng.uniform(0, grid_t)
    pts.append((z0, t0))
    active.append(0)
    gy, gx = bg_cell(z0, t0)
    bg[gy][gx] = 0

    while active and len(pts) < target:
        ai       = rng.randrange(len(active))
        si       = active[ai]
        sz, st   = pts[si]
        placed   = False
        for _ in range(30):          # 30 attempts per active point
            angle = rng.uniform(0.0, 2 * math.pi)
            dist  = rng.uniform(min_sep, 2 * min_sep)
            nz = sz + dist * math.cos(angle)
            nt = (st + dist * math.sin(angle)) % grid_t   # wrap theta
            if valid(nz, nt):
                new_i = len(pts)
                pts.append((nz, nt))
                active.append(new_i)
                gy, gx = bg_cell(nz, nt)
                bg[gy][gx] = new_i
                placed = True
                break
        if not placed:
            active.pop(ai)

    return [(int(z), int(t % grid_t)) for z, t in pts]


def _grow_lake(
    grid: list[list[int]],
    noise: list[list[float]],
    rng: random.Random,
    sz: int, st: int,
    grid_z: int, grid_t: int,
    budget: int,
) -> int:
    """
    BFS flood-fill lake from (sz, st).
    Noise in [0,1] modulates per-cell acceptance probability, producing
    irregular organic shorelines instead of uniform blobs.
    Returns the number of WATER cells created.
    """
    if grid[sz][st] != UNASSIGNED:
        return 0
    grid[sz][st] = WATER
    frontier = [(sz, st)]
    added = 1

    while frontier and added < budget:
        # Random-index pop keeps the frontier unordered → organic shape
        idx = rng.randrange(len(frontier))
        frontier[idx], frontier[-1] = frontier[-1], frontier[idx]
        z, t = frontier.pop()

        for dz, dt in DIRS4:
            nz = z + dz
            nt = (t + dt) % grid_t   # theta wraps; z is bounded
            if 0 <= nz < grid_z and grid[nz][nt] == UNASSIGNED:
                # Noise in [0,1] biases growth toward certain directions,
                # producing irregular organic shorelines.  The 0.60 floor
                # prevents frontier exhaustion in low-noise plateaus: at
                # minimum probability 0.60, the chance that all 4 neighbours
                # of a newly-added cell are simultaneously rejected is only
                # 0.40^4 = 2.6%, so lakes reliably reach their target size.
                prob = 0.60 + 0.40 * noise[nz][nt]
                if rng.random() < prob:
                    grid[nz][nt] = WATER
                    frontier.append((nz, nt))
                    added += 1
                    if added >= budget:
                        break

    return added


def _astar_river(
    noise: list[list[float]],
    start: tuple[int, int],
    end:   tuple[int, int],
    grid_z: int, grid_t: int,
    meander: float,
) -> list[tuple[int, int]] | None:
    """
    A* river from start to end.
    Edge cost = 1 + noise * meander_weight so higher meander → more winding.
    The heuristic adds a small θ-distance penalty to prefer Z-axis river flow
    over purely circumferential paths, which would be unnatural given Coriolis.
    Returns the path as (z, t) positions, or None if unreachable.
    """
    ez, et       = end
    noise_weight = meander * 5.0     # noise contribution: 0–5 extra cost

    def h(z: int, t: int) -> float:
        # Use toroidal theta distance so the heuristic is admissible when a
        # river can take the short way around the cylinder seam.
        # Penalise circumferential offset slightly to favour axial flow.
        dt = _torus_dt(t, et, grid_t)
        return math.hypot(z - ez, dt) + 0.15 * dt

    g_score: dict[tuple[int, int], float] = {start: 0.0}
    came_from: dict[tuple[int, int], tuple[int, int]] = {}
    heap: list[tuple[float, float, int, int]] = [(h(*start), 0.0, start[0], start[1])]

    while heap:
        _, g, z, t = heapq.heappop(heap)
        if (z, t) == end:
            path: list[tuple[int, int]] = []
            cur = end
            while cur in came_from:
                path.append(cur)
                cur = came_from[cur]
            path.append(start)
            return path[::-1]

        if g > g_score.get((z, t), math.inf):
            continue   # stale heap entry

        for dz, dt in DIRS4:
            nz = z + dz
            nt = (t + dt) % grid_t   # theta wraps
            if 0 <= nz < grid_z:
                ng = g + 1.0 + noise[nz][nt] * noise_weight
                if ng < g_score.get((nz, nt), math.inf):
                    g_score[(nz, nt)] = ng
                    came_from[(nz, nt)] = (z, t)
                    heapq.heappush(heap, (ng + h(nz, nt), ng, nz, nt))

    return None


def pass1_water(
    grid: list[list[int]],
    rng: random.Random,
    grid_z: int, grid_t: int,
    target_ratio: float,
    lake_min: int, lake_max: int,
    lake_min_sep: float,
    meander: float,
    end_sea_depth: int = 10,
) -> None:
    """
    Full Pass 1.
    Fills both Z-axis end caps with solid water (end_sea_depth rows each),
    representing open seas at the cylinder end walls.  The remaining water
    budget is then spent on interior lakes (Poisson disk + blob growth) and
    rivers (A*) connecting nearby lake pairs.  Lake seeds are confined to the
    non-sea zone so they never overlap with the pre-filled seas.
    """
    water_budget = int(grid_z * grid_t * target_ratio)

    # ------------------------------------------------------------------
    # End-cap seas: solid water at both Z boundaries.
    # These run the full circumference so no land ever butts up against
    # the cylinder end walls.  They are placed unconditionally before any
    # lake or river logic runs, and their cell count is deducted from the
    # remaining lake budget so the overall water ratio is still honoured.
    # ------------------------------------------------------------------
    for z in range(end_sea_depth):
        for t in range(grid_t):
            grid[z][t] = WATER
    for z in range(grid_z - end_sea_depth, grid_z):
        for t in range(grid_t):
            grid[z][t] = WATER

    sea_cells    = 2 * end_sea_depth * grid_t
    water_budget = max(0, water_budget - sea_cells)

    # Two independent noise fields: one for shoreline texture, one for rivers
    shore_noise = make_value_noise(rng, grid_z, grid_t, wavelength=12.0)
    river_noise = make_value_noise(rng, grid_z, grid_t, wavelength=22.0)

    seeds = _poisson_disk_seeds(rng, grid_z, grid_t, lake_min_sep, lake_min, lake_max,
                                end_sea_depth=end_sea_depth)
    n = len(seeds)

    # Assign varied size budgets proportional to the water target so the lakes
    # collectively can reach the goal.  A few large lakes carry most of the
    # water area; several medium lakes add variety; small ponds fill gaps.
    # Multipliers (large ≈ 2×, medium ≈ 1×, small ≈ 0.3×) sum to roughly
    # n * 1.0 on average, so the total budget ≈ water_budget.
    base = water_budget / max(n, 1)
    budgets: list[int] = []
    for i in range(n):
        frac = i / max(n - 1, 1)
        if frac < 0.25:
            budgets.append(int(base * rng.uniform(1.6, 2.8)))   # large
        elif frac < 0.65:
            budgets.append(int(base * rng.uniform(0.7, 1.4)))   # medium
        else:
            budgets.append(int(base * rng.uniform(0.15, 0.45))) # small
    rng.shuffle(budgets)

    water_cells    = 0
    lake_centers: list[tuple[int, int]] = []

    for (sz, st), bgt in zip(seeds, budgets):
        if water_cells >= water_budget:
            break
        added = _grow_lake(
            grid, shore_noise, rng, sz, st,
            grid_z, grid_t, min(bgt, water_budget - water_cells),
        )
        water_cells += added
        if added > 0:
            lake_centers.append((sz, st))

    if len(lake_centers) < 2:
        return

    # Sort all lake pairs by toroidal distance; connect close pairs with rivers.
    pairs = sorted(
        (math.hypot(lake_centers[i][0] - lake_centers[j][0],
                    _torus_dt(lake_centers[i][1], lake_centers[j][1], grid_t)), i, j)
        for i in range(len(lake_centers))
        for j in range(i + 1, len(lake_centers))
    )
    max_dist   = max(grid_z, grid_t) * 0.55   # skip very distant lake pairs
    connected: set[int] = set()

    for dist, i, j in pairs:
        if dist > max_dist:
            continue
        if i in connected and j in connected:
            continue
        if water_cells >= water_budget:
            break
        path = _astar_river(river_noise, lake_centers[i], lake_centers[j],
                             grid_z, grid_t, meander)
        if path:
            for z, t in path:
                if grid[z][t] == UNASSIGNED:
                    grid[z][t] = WATER
                    water_cells += 1
            connected.add(i)
            connected.add(j)


# ---------------------------------------------------------------------------
# Pass 2: Urban / industrial prelabeling
# ---------------------------------------------------------------------------

def pass2_prelabel(
    grid: list[list[int]],
    rng: random.Random,
    grid_z: int, grid_t: int,
    noise_wavelength: float,
) -> None:
    """
    Pass 2 (moved before green space so park placement is biome-aware).
    All remaining UNASSIGNED cells become URBAN or INDUSTRIAL based on a
    low-frequency noise field.  Median threshold → ~50/50 split.
    """
    noise = make_value_noise(rng, grid_z, grid_t, wavelength=noise_wavelength)

    vals = sorted(
        noise[z][t]
        for z in range(grid_z) for t in range(grid_t)
        if grid[z][t] == UNASSIGNED
    )
    if not vals:
        return
    threshold = vals[len(vals) // 2]

    for z in range(grid_z):
        for t in range(grid_t):
            if grid[z][t] == UNASSIGNED:
                grid[z][t] = URBAN if noise[z][t] < threshold else INDUSTRIAL


# ---------------------------------------------------------------------------
# Pass 3: Biome-aware park placement
# ---------------------------------------------------------------------------

def _grow_park(
    grid: list[list[int]],
    noise: list[list[float]],
    rng: random.Random,
    sz: int, st: int,
    grid_z: int, grid_t: int,
    budget: int,
    biome: int,
) -> int:
    """
    BFS flood-fill park from (sz, st), only expanding into cells of `biome`.
    Noise modulates acceptance (0.60–1.00 floor) for organic edges.
    Returns number of cells converted to PARK.
    """
    if grid[sz][st] != biome:
        return 0
    grid[sz][st] = PARK
    frontier = [(sz, st)]
    added = 1

    while frontier and added < budget:
        idx = rng.randrange(len(frontier))
        frontier[idx], frontier[-1] = frontier[-1], frontier[idx]
        z, t = frontier.pop()
        for dz, dt in DIRS4:
            nz = z + dz
            nt = (t + dt) % grid_t
            if 0 <= nz < grid_z and grid[nz][nt] == biome:
                prob = 0.60 + 0.40 * noise[nz][nt]
                if rng.random() < prob:
                    grid[nz][nt] = PARK
                    frontier.append((nz, nt))
                    added += 1
                    if added >= budget:
                        break

    return added


def _sample_park_size(
    rng: random.Random,
    min_s: int,
    max_s: int,
    alpha: float,
) -> int:
    """
    Sample a park cluster size from a discrete power-law distribution.
    P(s) ∝ s^(-alpha).  Higher alpha → stronger bias toward small parks.
    """
    sizes   = list(range(min_s, max_s + 1))
    weights = [s ** (-alpha) for s in sizes]
    return rng.choices(sizes, weights=weights)[0]


def _bfs_park_distances(
    grid: list[list[int]],
    grid_z: int, grid_t: int,
) -> list[list[float]]:
    """
    Multi-source BFS from every PARK cell.
    Returns a distance grid; unreachable cells hold math.inf.
    """
    dist: list[list[float]] = [[math.inf] * grid_t for _ in range(grid_z)]
    q: deque[tuple[int, int]] = deque()
    for z in range(grid_z):
        for t in range(grid_t):
            if grid[z][t] == PARK:
                dist[z][t] = 0.0
                q.append((z, t))
    while q:
        z, t = q.popleft()
        d = dist[z][t]
        for dz, dt in DIRS4:
            nz = z + dz
            nt = (t + dt) % grid_t
            if 0 <= nz < grid_z and dist[nz][nt] == math.inf:
                dist[nz][nt] = d + 1.0
                q.append((nz, nt))
    return dist


def _ensure_urban_coverage(
    grid: list[list[int]],
    grid_z: int, grid_t: int,
    coverage_radius: int,
) -> int:
    """
    Guarantee every URBAN cell is within coverage_radius steps of a PARK.
    Repeatedly places a single park cell at the most isolated uncovered
    urban cell, then propagates updated distances, until satisfied.
    Returns total gap-filling cells added.
    """
    dist = _bfs_park_distances(grid, grid_z, grid_t)
    added_total = 0

    while True:
        worst_d, worst_z, worst_t = coverage_radius, -1, -1
        for z in range(grid_z):
            for t in range(grid_t):
                if grid[z][t] == URBAN and dist[z][t] > worst_d:
                    worst_d  = dist[z][t]
                    worst_z, worst_t = z, t
        if worst_z == -1:
            break  # every urban cell is within range

        grid[worst_z][worst_t] = PARK
        added_total += 1

        # Propagate updated distances outward from the new park cell.
        # Stop at coverage_radius — no need to compute beyond that.
        dist[worst_z][worst_t] = 0.0
        q: deque[tuple[int, int]] = deque([(worst_z, worst_t)])
        while q:
            z, t = q.popleft()
            d = dist[z][t]
            if d >= coverage_radius:
                continue
            for dz, dt in DIRS4:
                nz = z + dz
                nt = (t + dt) % grid_t
                if 0 <= nz < grid_z and dist[nz][nt] > d + 1.0:
                    dist[nz][nt] = d + 1.0
                    q.append((nz, nt))

    return added_total


def pass3_green(
    grid: list[list[int]],
    rng: random.Random,
    grid_z: int, grid_t: int,
    urban_park_ratio: float,
    urban_wf_rings: int,
    industrial_wf_rings: int,
    waterfront_prob: float,
    park_decay: float,
    coverage_radius: int,
) -> None:
    """
    Pass 3: biome-aware park placement.  Requires URBAN/INDUSTRIAL already set.

    Step A — Waterfront buffer
        BFS ring expansion from water edges into adjacent land cells.
        Industrial zones: industrial_wf_rings rings at boosted probability
        → thick green belt hugging the water.
        Urban zones: urban_wf_rings rings at standard probability
        → thin green edge.

    Step B — Urban scatter (power-law sizes, α=1.7, range 2–30)
        Seeds drawn uniformly from remaining URBAN cells; most parks are
        pocket-sized (2–8 cells) with occasional medium parks.  Even
        spatial distribution emerges from uniform random seeding rather
        than clustering.

    Step C — Industrial scatter (half density, α=1.3, range 8–60)
        Larger, sparser parks in industrial zones.  Target = urban_park_ratio/2
        of original industrial cell count; the thick waterfront buffer
        already contributes substantial coverage.

    Step D — Urban coverage guarantee
        Multi-source BFS from all parks.  Any URBAN cell farther than
        coverage_radius steps from a park gets a gap-filling cell placed
        at the worst offender, repeated until the constraint holds.
    """
    park_noise = make_value_noise(rng, grid_z, grid_t, wavelength=8.0)

    # Snapshot original biome counts before any parks are placed.
    n_urban      = sum(grid[z][t] == URBAN      for z in range(grid_z) for t in range(grid_t))
    n_industrial = sum(grid[z][t] == INDUSTRIAL for z in range(grid_z) for t in range(grid_t))

    # ------------------------------------------------------------------
    # Step A: Waterfront buffer
    # ------------------------------------------------------------------
    current_ring: list[tuple[int, int]] = [
        (z, t)
        for z in range(grid_z) for t in range(grid_t)
        if grid[z][t] in (URBAN, INDUSTRIAL)
        and any(0 <= z + dz < grid_z
                and grid[z + dz][(t + dt) % grid_t] == WATER
                for dz, dt in DIRS4)
    ]
    rng.shuffle(current_ring)
    prob = waterfront_prob

    for ring_i in range(max(urban_wf_rings, industrial_wf_rings)):
        next_ring: set[tuple[int, int]] = set()
        for z, t in current_ring:
            biome = grid[z][t]
            if biome not in (URBAN, INDUSTRIAL):
                continue
            max_r = industrial_wf_rings if biome == INDUSTRIAL else urban_wf_rings
            if ring_i >= max_r:
                continue
            # Industrial gets a 25 % probability boost for the denser buffer.
            p = min(1.0, prob * 1.25) if biome == INDUSTRIAL else prob
            if rng.random() < p:
                grid[z][t] = PARK
                for dz, dt in DIRS4:
                    nz = z + dz
                    nt = (t + dt) % grid_t
                    if 0 <= nz < grid_z and grid[nz][nt] in (URBAN, INDUSTRIAL):
                        next_ring.add((nz, nt))
        current_ring = list(next_ring)
        rng.shuffle(current_ring)
        prob *= (1.0 - park_decay)

    # ------------------------------------------------------------------
    # Step B: Urban scatter — small, power-law distributed
    # ------------------------------------------------------------------
    urban_target  = int(n_urban * urban_park_ratio)
    urban_placed  = n_urban - sum(
        grid[z][t] == URBAN for z in range(grid_z) for t in range(grid_t)
    )  # cells already turned to PARK by the waterfront step

    urban_seeds = [(z, t) for z in range(grid_z) for t in range(grid_t)
                   if grid[z][t] == URBAN]
    rng.shuffle(urban_seeds)

    for sz, st in urban_seeds:
        if urban_placed >= urban_target:
            break
        if grid[sz][st] != URBAN:
            continue
        size  = _sample_park_size(rng, min_s=2, max_s=30, alpha=1.7)
        added = _grow_park(grid, park_noise, rng, sz, st, grid_z, grid_t, size, URBAN)
        urban_placed += added

    # ------------------------------------------------------------------
    # Step C: Industrial scatter — half density, larger clusters
    # ------------------------------------------------------------------
    industrial_target = int(n_industrial * urban_park_ratio / 2)
    industrial_placed = n_industrial - sum(
        grid[z][t] == INDUSTRIAL for z in range(grid_z) for t in range(grid_t)
    )

    industrial_seeds = [(z, t) for z in range(grid_z) for t in range(grid_t)
                        if grid[z][t] == INDUSTRIAL]
    rng.shuffle(industrial_seeds)

    for sz, st in industrial_seeds:
        if industrial_placed >= industrial_target:
            break
        if grid[sz][st] != INDUSTRIAL:
            continue
        size  = _sample_park_size(rng, min_s=8, max_s=60, alpha=1.3)
        added = _grow_park(grid, park_noise, rng, sz, st, grid_z, grid_t, size, INDUSTRIAL)
        industrial_placed += added

    # ------------------------------------------------------------------
    # Step D: Urban coverage guarantee
    # ------------------------------------------------------------------
    gap_cells = _ensure_urban_coverage(grid, grid_z, grid_t, coverage_radius)
    if gap_cells:
        print(f"  Coverage: placed {gap_cells} gap-filling park cells")


# ---------------------------------------------------------------------------
# Pass 4: Adjacency cleanup
# ---------------------------------------------------------------------------

def _remove_small_islands(
    grid: list[list[int]],
    grid_z: int, grid_t: int,
    min_island_size: int,
) -> int:
    """
    Find every connected component of non-water cells that is entirely
    enclosed by water (an "island"), and convert it to water if its cell
    count is below min_island_size.

    Connectivity is 4-directional.  Theta wraps; Z is bounded.
    A component is considered an island when every valid 4-neighbour of
    every cell in the component is either water or another cell in the
    same component — i.e. there is no land path out of the region.

    Returns the number of cells converted to water.
    """
    visited = [[False] * grid_t for _ in range(grid_z)]
    converted = 0

    for sz in range(grid_z):
        for st in range(grid_t):
            if grid[sz][st] == WATER or visited[sz][st]:
                continue

            # BFS — collect the full connected non-water component.
            component: list[tuple[int, int]] = []
            q: deque[tuple[int, int]] = deque([(sz, st)])
            visited[sz][st] = True
            while q:
                z, t = q.popleft()
                component.append((z, t))
                for dz, dt in DIRS4:
                    nz = z + dz
                    nt = (t + dt) % grid_t
                    if 0 <= nz < grid_z and not visited[nz][nt] and grid[nz][nt] != WATER:
                        visited[nz][nt] = True
                        q.append((nz, nt))

            if len(component) >= min_island_size:
                continue   # large enough — keep it regardless

            # Check whether the component is truly an island: every valid
            # neighbour of every member cell must be water or in-component.
            cell_set = set(component)
            is_island = all(
                grid[z + dz][(t + dt) % grid_t] == WATER
                or (z + dz, (t + dt) % grid_t) in cell_set
                for z, t in component
                for dz, dt in DIRS4
                if 0 <= z + dz < grid_z
            )

            if is_island:
                for z, t in component:
                    grid[z][t] = WATER
                converted += len(component)

    return converted

def pass4_cleanup(grid: list[list[int]], grid_z: int, grid_t: int) -> None:
    """
    Pass 4.
    - Industrial cells on the waterfront are converted to urban (design rule:
      no heavy industry directly on the water).
    - Two rounds of 8-neighbour majority-vote smoothing eliminate jagged
      biome boundaries.  Water cells are never reassigned.
    - Any non-water island (connected land region fully enclosed by water)
      with fewer than 10 cells is converted to water.
    """
    # Industrial-waterfront removal
    for z in range(grid_z):
        for t in range(grid_t):
            if grid[z][t] == INDUSTRIAL:
                for dz, dt in DIRS4:
                    nz = z + dz
                    nt = (t + dt) % grid_t
                    if 0 <= nz < grid_z and grid[nz][nt] == WATER:
                        grid[z][t] = URBAN
                        break

    # Majority-vote boundary smoothing (2 passes).
    # Only URBAN and INDUSTRIAL cells participate — water and intentionally-
    # placed parks are never reassigned, so small power-law park clusters
    # survive even when fully surrounded by one biome.
    for _ in range(2):
        changes: list[tuple[int, int, int]] = []
        for z in range(grid_z):
            for t in range(grid_t):
                if grid[z][t] not in (URBAN, INDUSTRIAL):
                    continue  # never reassign water, park, or unassigned
                counts: dict[int, int] = {}
                for dz, dt in DIRS8:
                    nz = z + dz
                    nt = (t + dt) % grid_t
                    if 0 <= nz < grid_z:   # theta already valid via %
                        b = grid[nz][nt]
                        counts[b] = counts.get(b, 0) + 1
                if not counts:
                    continue
                dominant = max(counts, key=lambda k: counts[k])
                # Only smooth urban↔industrial transitions; never flip to water
                # or park via majority vote (those biomes are set deliberately).
                if (dominant in (URBAN, INDUSTRIAL)
                        and counts[dominant] >= 6
                        and dominant != grid[z][t]):
                    changes.append((z, t, dominant))
        for z, t, biome in changes:
            grid[z][t] = biome

    # Remove land islands smaller than 10 cells.
    _remove_small_islands(grid, grid_z, grid_t, min_island_size=10)


# ---------------------------------------------------------------------------
# Image rendering
# ---------------------------------------------------------------------------

def render_png(
    grid: list[list[int]],
    grid_z: int, grid_t: int,
    cell_px: int,
) -> Image.Image:
    """
    Render the biome grid as a PNG.
    Builds a 1-pixel-per-cell image then scales up with NEAREST-neighbour
    resampling for crisp, grid-aligned cell boundaries.

    Image axes:
      X (width)  → Z-axis (cylinder length)
      Y (height) → θ-axis (circumference)
    """
    small = Image.new("RGB", (grid_z, grid_t))
    px    = small.load()
    for z in range(grid_z):
        for t in range(grid_t):
            px[z, t] = COLORS[grid[z][t]]
    if cell_px == 1:
        return small
    return small.resize((grid_z * cell_px, grid_t * cell_px), Image.NEAREST)


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def print_stats(grid: list[list[int]], grid_z: int, grid_t: int) -> None:
    total  = grid_z * grid_t
    counts = {b: 0 for b in BIOME_NAMES}
    for z in range(grid_z):
        for t in range(grid_t):
            counts[grid[z][t]] += 1
    print("\nFinal biome coverage:")
    for b in (WATER, PARK, URBAN, INDUSTRIAL, UNASSIGNED):
        n = counts[b]
        if n:
            bar = "█" * int(40 * n / total)
            print(f"  {BIOME_NAMES[b]:12s}  {n:7,} cells  {100*n/total:5.1f}%  {bar}")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="generate_tilemap",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    gen = p.add_argument_group("generator")
    gen.add_argument("--seed",       type=int,   default=847291,
                     help="RNG seed (same seed + args → identical output)")
    gen.add_argument("--grid-z",     type=int,   default=256,
                     help="Grid cells along the Z-axis (cylinder length) [default: 256]")
    gen.add_argument("--grid-theta", type=int,   default=204,
                     help="Grid cells around the circumference (θ-axis) [default: 204]")

    w = p.add_argument_group("water  (Pass 1)")
    w.add_argument("--water-ratio",    type=float, default=0.30,
                   help="Target water coverage fraction [default: 0.30]")
    w.add_argument("--end-sea-depth",  type=int,   default=10,
                   help="Rows of solid water at each cylinder end cap (the 'seas') [default: 10]")
    w.add_argument("--lake-count-min", type=int,   default=8,
                   help="Minimum lake seed count [default: 8]")
    w.add_argument("--lake-count-max", type=int,   default=12,
                   help="Maximum lake seed count [default: 12]")
    w.add_argument("--lake-min-sep",   type=float, default=40.0,
                   help="Minimum cell distance between lake seeds [default: 40]")
    w.add_argument("--meander",        type=float, default=0.6,
                   help="River meander factor: 0=straight, 1=very winding [default: 0.6]")

    u = p.add_argument_group("urban/industrial  (Pass 2)")
    u.add_argument("--noise-wavelength", type=float, default=70.0,
                   help="Value noise wavelength (cells) for urban/industrial split [default: 70]")

    g = p.add_argument_group("green  (Pass 3)")
    g.add_argument("--green-ratio",          type=float, default=0.20,
                   help="Target park fraction of urban cells; industrial gets half [default: 0.20]")
    g.add_argument("--urban-wf-rings",       type=int,   default=2,
                   help="Waterfront buffer rings in urban zones (thin edge) [default: 2]")
    g.add_argument("--industrial-wf-rings",  type=int,   default=4,
                   help="Waterfront buffer rings in industrial zones (thick belt) [default: 4]")
    g.add_argument("--waterfront-prob",      type=float, default=0.80,
                   help="Park acceptance probability at the first waterfront ring [default: 0.80]")
    g.add_argument("--park-decay",           type=float, default=0.50,
                   help="Probability decay per ring away from water [default: 0.50]")
    g.add_argument("--coverage-radius",      type=int,   default=16,
                   help="Max cells from any urban cell to nearest park (~2 km) [default: 16]")

    o = p.add_argument_group("output")
    o.add_argument("--cell-px", type=int, default=20,
                   help="Pixel edge length of each grid cell [default: 20]")
    o.add_argument("--output",  type=str, default="cylinder_biome.png",
                   help="Output PNG file path [default: cylinder_biome.png]")

    return p


def main() -> None:
    args = build_parser().parse_args()
    rng  = random.Random(args.seed)

    total = args.grid_z * args.grid_theta
    img_w = args.grid_z    * args.cell_px
    img_h = args.grid_theta * args.cell_px

    print(f"Seed   : {args.seed}")
    print(f"Grid   : {args.grid_z}z × {args.grid_theta}θ  ({total:,} cells)")
    print(f"Image  : {img_w:,} × {img_h:,} px  →  {args.output}")

    grid: list[list[int]] = [[UNASSIGNED] * args.grid_theta for _ in range(args.grid_z)]

    print("\nPass 1 — water placement…")
    pass1_water(
        grid, rng, args.grid_z, args.grid_theta,
        args.water_ratio,
        args.lake_count_min, args.lake_count_max,
        args.lake_min_sep, args.meander,
        args.end_sea_depth,
    )

    print("Pass 2 — urban / industrial prelabeling…")
    pass2_prelabel(
        grid, rng, args.grid_z, args.grid_theta,
        args.noise_wavelength,
    )

    print("Pass 3 — park placement…")
    pass3_green(
        grid, rng, args.grid_z, args.grid_theta,
        args.green_ratio,
        args.urban_wf_rings, args.industrial_wf_rings,
        args.waterfront_prob, args.park_decay,
        args.coverage_radius,
    )

    print("Pass 4 — adjacency cleanup…")
    pass4_cleanup(grid, args.grid_z, args.grid_theta)

    print_stats(grid, args.grid_z, args.grid_theta)

    print(f"\nRendering PNG…")
    img = render_png(grid, args.grid_z, args.grid_theta, args.cell_px)
    img.save(args.output)
    print(f"Saved  : {args.output}")


if __name__ == "__main__":
    main()
