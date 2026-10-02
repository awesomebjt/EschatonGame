"""
export_chunks.py  --  write the generated world as engine-ready binaries.

No Blender required: this imports paris_city directly and runs the generator in plain
CPython (needs only numpy).

    python3 export_chunks.py --region 20080 13500 25100 18500 --out world/
    python3 export_chunks.py --all --tile 5020 4000 --out world/ [--jobs N]

--all runs tiles in parallel worker processes (default: half the logical CPUs, at low
priority, one BLAS thread each). The whole-map
network (river, hubs, boulevards) is computed once in the parent and shared, and only
the parent writes manifest.json and prototypes.bin, after every tile has finished.

What it writes
--------------
world/manifest.json         map constants, material palette, building catalogue, chunk list
world/prototypes.bin        the 20 building types, LOD0 and LOD1, in local space
world/chunk_<i>_<j>.bin     one file per chunk: terrain mesh + building/monument instances

Everything is little-endian. Chunk geometry is stored relative to the chunk centre, so a
chunk's world transform is just a translation - which is what you want when you rebase the
origin around the player.

chunk_<i>_<j>.bin
    magic      char[4]  "CHNK"
    version    u32      1
    i, j       i32      chunk indices (i wraps modulo the column count)
    origin     f32[2]   chunk centre in map coordinates
    counts     u32[4]   vertices, indices, building instances, monument instances
    vertices   { f32 pos[3]; f32 nrm[3]; u8 rgba[4] }   28 bytes each
    indices    u32
    buildings  { u16 type; u16 lod_hint; f32 x, y, z; f32 rot }   20 bytes each
    monuments  { u16 size; u16 pad;      f32 x, y, z }            16 bytes each

prototypes.bin
    magic      char[4]  "PROT"
    version    u32      1
    count      u32      number of (type, lod) meshes = 2 * len(BUILDING_TYPES)
    then per mesh: u16 type, u16 lod, u32 vtx, u32 idx, vertices, indices (same layouts)

Building instances are placed with: translate(x, y, z) * rotateZ(rot). No scaling - every
type has fixed dimensions, which is why the catalogue instances cleanly.
"""

import argparse
import contextlib
import io
import json
import math
import multiprocessing as mp
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paris_city as pc

pc.bpy = None                      # make sure the generator stays on the pure-Python path

VERSION = 1
PALETTE = {
    pc.MAT_GROUND: pc.COL_GROUND,
    pc.MAT_PARK: pc.COL_PARK,
    pc.MAT_ROAD: pc.COL_ROAD,
    pc.MAT_PLATFORM: pc.COL_PLATFORM,
    pc.MAT_WATER: pc.COL_WATER,
    pc.MAT_GOLD: pc.COL_GOLD,
    pc.MAT_MONUMENT: pc.COL_MONUMENT,
    pc.MAT_BUILDING: pc.COL_BUILDING,
}


# --------------------------------------------------------------------------- #
def _rgba(mat):
    c = PALETTE.get(mat, (1.0, 0.0, 1.0))
    return bytes(int(max(0.0, min(1.0, v)) * 255 + 0.5) for v in c) + b"\xff"


def _face_normal(verts, face):
    nx = ny = nz = 0.0                       # Newell: works for ngons and non-planar quads
    n = len(face)
    for k in range(n):
        ax, ay, az = verts[face[k]]
        bx, by, bz = verts[face[(k + 1) % n]]
        nx += (ay - by) * (az + bz)
        ny += (az - bz) * (ax + bx)
        nz += (ax - bx) * (ay + by)
    l = math.sqrt(nx * nx + ny * ny + nz * nz)
    return (0.0, 0.0, 1.0) if l < 1e-12 else (nx / l, ny / l, nz / l)


def triangulate(acc, origin=(0.0, 0.0), default_mat=pc.MAT_BUILDING):
    """MeshAcc -> flat-shaded vertex/index buffers. Vertices are duplicated per face so
    each triangle keeps a hard normal, which is what this blockout geometry wants."""
    ox, oy = origin
    vtx, idx = bytearray(), bytearray()
    count = 0
    mats = acc.mats if acc.mats else [default_mat] * len(acc.faces)
    for face, mat in zip(acc.faces, mats):
        nrm = _face_normal(acc.verts, face)
        colour = _rgba(mat)
        base = count
        for vi in face:
            x, y, z = acc.verts[vi]
            vtx += struct.pack("<6f", x - ox, y - oy, z, *nrm) + colour
            count += 1
        for k in range(1, len(face) - 1):    # fan: every face here is convex
            idx += struct.pack("<3I", base, base + k, base + k + 1)
    return bytes(vtx), bytes(idx), count, len(idx) // 4


# --------------------------------------------------------------------------- #
def write_prototypes(path):
    """Each building type once, in local space: origin at the middle of the street-facing
    edge, +X along the street, +Y away from it, +Z up."""
    meshes = []
    for ti, T in enumerate(pc.BUILDING_TYPES):
        poly = [(x - T.F / 2.0, y) for x, y in T.local]
        for lod in (0, 1):
            acc = pc.MeshAcc()
            if lod == 0 and T.courtyard:
                pc.add_courtyard_building(acc, poly, T.H, pc.MAT_BUILDING)
            else:
                pc.add_prism(acc, poly, T.H, pc.MAT_BUILDING)
            meshes.append((ti, lod) + triangulate(acc))
    with open(path, "wb") as f:
        f.write(b"PROT" + struct.pack("<2I", VERSION, len(meshes)))
        for ti, lod, vtx, idx, nv, ni in meshes:
            f.write(struct.pack("<2H2I", ti, lod, nv, ni))
            f.write(vtx)
            f.write(idx)
    return len(meshes)


def instance_of(T, poly):
    """Recover (type, x, y, rot) from a placed footprint: the generator lays every building
    out as origin + t*x + n*y, so the front edge gives the rotation directly."""
    ax, ay = poly[0]
    bx, by = poly[1]
    rot = math.atan2(by - ay, bx - ax)
    return (ax + bx) / 2.0, (ay + by) / 2.0, rot


def export_region(region, out_dir, network=None):
    """Write one region's chunk files and return their manifest entries. Touches no
    shared file, so regions can be exported concurrently."""
    r = pc.generate(region=region, network=network)
    reg = r["region"]
    os.makedirs(out_dir, exist_ok=True)
    terrain, _, _, monuments, _ = pc.build_chunk_meshes(r, include_buildings=False)

    by_chunk = {k: ([], []) for k in terrain}

    def chunk_of(x, y):
        return int(math.floor(x / pc.CHUNK_X)), int(math.floor(y / pc.CHUNK_Y))

    types = {id(T): i for i, T in enumerate(pc.BUILDING_TYPES)}
    for T, poly in r["buildings"]:
        cx = sum(p[0] for p in poly) / 4
        cy = sum(p[1] for p in poly) / 4
        k = chunk_of(cx, cy)
        if k in by_chunk:
            by_chunk[k][0].append((types[id(T)],) + instance_of(T, poly))
    for p, size in monuments:
        k = chunk_of(*p)
        if k in by_chunk:
            by_chunk[k][1].append((size, p[0], p[1]))

    chunk_list, total = [], 0
    for (i, j), acc in sorted(terrain.items()):
        centre = ((i + 0.5) * pc.CHUNK_X, (j + 0.5) * pc.CHUNK_Y)
        vtx, idx, nv, ni = triangulate(acc, centre, pc.MAT_GROUND)
        binst, minst = by_chunk[(i, j)]
        path = os.path.join(out_dir, f"chunk_{i:03d}_{j:03d}.bin")
        with open(path, "wb") as f:
            f.write(b"CHNK" + struct.pack("<I2i2f4I", VERSION, i, j, *centre,
                                          nv, ni, len(binst), len(minst)))
            f.write(vtx)
            f.write(idx)
            for ti, x, y, rot in binst:
                f.write(struct.pack("<2H4f", ti, 0, x - centre[0], y - centre[1], 0.0, rot))
            for size, x, y in minst:
                f.write(struct.pack("<2H3f", size, 0, x - centre[0], y - centre[1], 0.0))
        total += os.path.getsize(path)
        chunk_list.append(dict(i=i, j=j, file=os.path.basename(path),
                               centre=[centre[0], centre[1]],
                               vertices=nv, triangles=ni // 3,
                               buildings=len(binst), monuments=len(minst)))

    print(f"  {len(chunk_list)} chunks, {total / 1e6:.1f} MB, "
          f"{sum(c['buildings'] for c in chunk_list)} building instances")
    return chunk_list


def write_manifest(out_dir, chunk_list):
    manifest_path = os.path.join(out_dir, "manifest.json")
    manifest = {}
    if os.path.exists(manifest_path):
        manifest = json.load(open(manifest_path))
    manifest.update(dict(
        version=VERSION,
        map=dict(width=pc.MAP_W, height=pc.MAP_H, wrap_x=True,
                 chunk_x=pc.CHUNK_X, chunk_y=pc.CHUNK_Y,
                 columns=int(round(pc.MAP_W / pc.CHUNK_X)),
                 rows=int(round(pc.MAP_H / pc.CHUNK_Y)),
                 green_border=pc.GREEN_BORDER,
                 cylinder_radius=pc.MAP_W / (2 * math.pi)),
        materials={str(m): list(c) for m, c in PALETTE.items()},
        building_types=[dict(name=T.name, front=T.F, back=T.B, depth=T.D, height=T.H,
                             courtyard=T.courtyard,
                             footprint=[[x - T.F / 2.0, y] for x, y in T.local])
                        for T in pc.BUILDING_TYPES],
        courtyard=dict(wall=pc.COURTYARD_WALL, passage_w=pc.PASSAGE_WIDTH,
                       passage_h=pc.PASSAGE_HEIGHT, threshold=pc.COURTYARD_THRESHOLD),
        monuments=dict(platform_height=pc.PLATFORM_HEIGHT,
                       S=dict(radius=pc.MONUMENT_S[0], height=pc.MONUMENT_S[1]),
                       M=dict(radius=pc.MONUMENT_M[0], height=pc.MONUMENT_M[1]),
                       L=dict(radius=pc.MONUMENT_L[0], drum_height=pc.MONUMENT_L[1],
                              dome_radius=pc.MONUMENT_L[0])),
        roundabouts=[dict(name=n, road_radius=rr, platform_radius=pr, share=sh)
                     for n, rr, pr, sh in pc.ROUNDABOUT_TYPES],
        vertex_stride=28,
    ))
    # Freshly exported chunks replace their old entries (counts change when the
    # generator does); chunks outside this export keep theirs. Sorted, because tiles
    # finish in any order and the manifest should diff cleanly between runs.
    chunks = {(c["i"], c["j"]): c for c in manifest.get("chunks", [])}
    chunks.update({(c["i"], c["j"]): c for c in chunk_list})
    manifest["chunks"] = [chunks[k] for k in sorted(chunks)]
    # Write-then-rename so an interrupted export never leaves a truncated manifest.
    tmp = manifest_path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(manifest, f, indent=1)
    os.replace(tmp, manifest_path)


def export_and_index(region, out_dir, network=None):
    chunk_list = export_region(region, out_dir, network)
    n = write_prototypes(os.path.join(out_dir, "prototypes.bin"))
    print(f"  prototypes.bin: {n} meshes")
    write_manifest(out_dir, chunk_list)


# Worker state: the shared network arrives once per process through the pool
# initializer instead of being pickled with every task.
_worker_network = None


def _init_worker(network):
    global _worker_network
    _worker_network = network
    # Stay out of the way of the desktop while a long export runs.
    with contextlib.suppress(OSError):
        os.nice(10)


def _export_tile(job):
    """Pool task. The generator narrates every pass; keep that out of the shared
    terminal and hand it back only if the tile fails."""
    key, region, out_dir = job
    log = io.StringIO()
    t0 = time.time()
    try:
        with contextlib.redirect_stdout(log):
            chunk_list = export_region(region, out_dir, _worker_network)
    except Exception as e:
        return key, None, time.time() - t0, f"{log.getvalue()}{type(e).__name__}: {e}"
    return key, chunk_list, time.time() - t0, None


def export_all(tile, out_dir, jobs):
    tw, th = tile
    cols, rows = int(round(pc.MAP_W / tw)), int(round(pc.MAP_H / th))
    os.makedirs(out_dir, exist_ok=True)
    t0 = time.time()
    network = pc.generate_network()
    print(f"network ready in {time.time() - t0:.1f} s; "
          f"{rows * cols} tiles on {jobs} processes")

    tasks = [((rr, cc), (cc * tw, rr * th, (cc + 1) * tw, (rr + 1) * th), out_dir)
             for rr in range(rows) for cc in range(cols)]
    chunk_list, failed = [], []
    # Spawned workers inherit this before importing numpy; without it every worker
    # starts its own BLAS pool sized to the whole machine and oversubscribes it.
    for var in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ.setdefault(var, "1")
    with mp.get_context("spawn").Pool(jobs, initializer=_init_worker,
                                      initargs=(network,)) as pool:
        for done, (key, chunks, secs, err) in enumerate(
                pool.imap_unordered(_export_tile, tasks), 1):
            if err is not None:
                failed.append(key)
                print(f"[{done}/{len(tasks)}] tile {key[0]},{key[1]} FAILED after {secs:.0f} s\n"
                      f"{err}", file=sys.stderr)
                continue
            chunk_list += chunks
            print(f"[{done}/{len(tasks)}] tile {key[0]},{key[1]}: {len(chunks)} chunks, "
                  f"{sum(c['buildings'] for c in chunks)} buildings, {secs:.0f} s")

    if failed:
        # Chunk files from the good tiles are on disk, but the manifest is left alone
        # so it never claims a partial world is complete.
        print(f"{len(failed)} tile(s) failed: {sorted(failed)}; manifest not written",
              file=sys.stderr)
        return False
    n = write_prototypes(os.path.join(out_dir, "prototypes.bin"))
    write_manifest(out_dir, chunk_list)
    print(f"done: {len(chunk_list)} chunks, prototypes.bin {n} meshes, "
          f"{time.time() - t0:.0f} s total")
    return True


# --------------------------------------------------------------------------- #
if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--region", nargs=4, type=float)
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--tile", nargs=2, type=float, default=[5020.0, 4000.0])
    ap.add_argument("--out", default="world")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="worker processes for --all (default: half the logical CPUs)")
    a = ap.parse_args()
    if a.all:
        sys.exit(0 if export_all(a.tile, a.out, max(1, a.jobs)) else 1)
    else:
        export_and_index(tuple(a.region) if a.region else "river", a.out)
