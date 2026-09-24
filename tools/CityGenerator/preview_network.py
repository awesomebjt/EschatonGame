"""
preview_network.py  --  whole-map overview, and a batch builder for the real thing.

Two entry points:

  preview_network()   Builds the entire 25.1 x 32 km map as flat ribbons: river, islands,
                      boulevards, roundabouts, green borders. No superblocks, no minor
                      streets, no buildings, no parks. Takes seconds, and is the fastest way
                      to see all five river laps and the whole boulevard network at once.

                          blender --background --python preview_network.py -- --preview \
                              --out //preview.blend

  build_map()         Generates the map one region tile at a time, saving each to its own
                      .blend. This is the practical way to build everything: a single
                      REGION = None run has to hold ~375k buildings and a 128-megapixel park
                      raster in memory at once.

                          blender --background --python preview_network.py -- --build \
                              --tile 5020 4000 --out //tiles
"""

import math
import os
import sys

import numpy as np
import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paris_city as pc


# --------------------------------------------------------------------------- #
def preview_network(seed=pc.SEED, cull_bridges=True):
    import random
    river = pc.River(np.random.default_rng(seed))
    hubs, edges, _, sizes, dropped = pc.finalize_network(
        pc.generate_hubs(random.Random(seed), river), river)
    smlh = {pc.ROUNDABOUT_TYPES[t][0]: sizes.count(t) for t in (0, 1, 2)}
    print(f"  roundabouts {smlh}, {dropped} river-grazing boulevards dropped")
    print(f"  {len(hubs)} hubs, {len(edges)} boulevards, "
          f"{len(river.x) * pc.RIVER_STEP / 1000:.0f} km of river")

    old = bpy.data.collections.get("CityPreview")
    if old:
        for o in list(old.objects):
            bpy.data.objects.remove(o, do_unlink=True)
        bpy.data.collections.remove(old)
    coll = bpy.data.collections.new("CityPreview")
    bpy.context.scene.collection.children.link(coll)

    mats = [pc.get_material("City_Ground", pc.COL_GROUND),
            pc.get_material("City_Park", pc.COL_PARK),
            pc.get_material("City_Road", pc.COL_ROAD),
            pc.get_material("City_Platform", pc.COL_PLATFORM),
            pc.get_material("City_Water", pc.COL_WATER)]
    G, P, R, PL, WA = 0, 1, 2, 3, 4

    acc = pc.MeshAcc()
    acc.quad_xy([(0, 0), (pc.MAP_W, 0), (pc.MAP_W, pc.MAP_H), (0, pc.MAP_H)], pc.Z_GROUND, G)
    for y0, y1 in ((0.0, pc.GREEN_BORDER), (pc.MAP_H - pc.GREEN_BORDER, pc.MAP_H)):
        acc.quad_xy([(0, y0), (pc.MAP_W, y0), (pc.MAP_W, y1), (0, y1)], pc.Z_PARK, P)

    wx = river.x % pc.MAP_W                       # water + islands, split at the seam
    for i in range(len(wx) - 1):
        if abs(wx[i + 1] - wx[i]) > pc.MAP_W / 2:
            continue
        sh = wx[i] - river.x[i]
        for ha, hb, z, mat in ((river.w[i] / 2, river.w[i + 1] / 2, pc.Z_WATER, WA),
                               (river.ih[i], river.ih[i + 1], pc.Z_ISLAND, P)):
            if ha < 0.5 or hb < 0.5:
                continue
            l0, r0 = river.bank_points(i, ha)
            l1, r1 = river.bank_points(i + 1, hb)
            quad = [(l0[0] + sh, l0[1]), (l1[0] + sh, l1[1]),
                    (r1[0] + sh, r1[1]), (r0[0] + sh, r0[1])]
            if pc.poly_area(quad) < 0:
                quad.reverse()
            acc.quad_xy(quad, z, mat)

    ylo, yhi = pc.GREEN_BORDER, pc.MAP_H - pc.GREEN_BORDER
    for e in edges:                               # boulevards, both seam copies
        a, b = pc.edge_segment(hubs, e)
        for sh in (-pc.MAP_W, 0.0, pc.MAP_W):
            aa, bb = (a[0] + sh, a[1]), (b[0] + sh, b[1])
            if max(aa[0], bb[0]) < 0 or min(aa[0], bb[0]) > pc.MAP_W:
                continue
            c = pc.clip_segment(aa, bb, 0.0, ylo, pc.MAP_W, yhi)
            if c and math.dist(*c) > 1.0:
                acc.quad_xy(pc.rect_poly(c[0], c[1], pc.MAJOR_WIDTH / 2), pc.Z_MAJOR, R)
    for i, h in enumerate(hubs):
        _, road_r, plat_r, _ = pc.ROUNDABOUT_TYPES[sizes[i]]
        acc.quad_xy(pc.circle_poly(h, road_r, 16), pc.Z_ROUNDABOUT, R)
        acc.quad_xy(pc.circle_poly(h, plat_r, 12), pc.Z_ROUNDABOUT + 0.05, PL)

    pc.make_object("MapPreview", acc, mats, coll)
    print(f"  preview mesh: {len(acc.faces)} faces")


# --------------------------------------------------------------------------- #
def _trim_to_tile(x0, y0, x1, y1):
    """Delete chunk objects outside the tile (used when tiles are generated with a pad)."""
    i0, i1 = int(math.floor(x0 / pc.CHUNK_X)), int(math.ceil(x1 / pc.CHUNK_X))
    j0, j1 = int(math.floor(y0 / pc.CHUNK_Y)), int(math.ceil(y1 / pc.CHUNK_Y))
    for o in list(bpy.data.objects):
        if not o.name.startswith("Chunk_"):
            continue
        parts = o.name.split("_")
        try:
            i, j = int(parts[1]), int(parts[2])
        except (IndexError, ValueError):
            continue
        if not (i0 <= i < i1 and j0 <= j < j1):
            bpy.data.objects.remove(o, do_unlink=True)


def build_map(tile_w=5020.0, tile_h=4000.0, out_dir="//tiles", pad=0.0, skip_existing=True):
    """Generate the whole map tile by tile, one .blend per tile.

    pad > 0 generates each tile with that much extra context and then trims back to the
    tile. ~1500 m makes the tile borders agree exactly with their neighbours, at roughly
    3x the cost; pad = 0 leaves a handful of overlapping buildings within ~100 m of a
    border (about 5 pairs per 1000 buildings in the seam band)."""
    out_dir = bpy.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    cols = int(round(pc.MAP_W / tile_w))
    rows = int(round(pc.MAP_H / tile_h))
    assert abs(cols * tile_w - pc.MAP_W) < 1e-6 and abs(rows * tile_h - pc.MAP_H) < 1e-6, \
        "tile size must divide 25100 and 32000 exactly"
    print(f"  {rows} x {cols} = {rows * cols} tiles of {tile_w:.0f} x {tile_h:.0f} m")
    for r in range(rows):
        for c in range(cols):
            path = os.path.join(out_dir, f"tile_{r:02d}_{c:02d}.blend")
            if skip_existing and os.path.exists(path):
                print(f"[tile {r},{c}] exists, skipping")
                continue
            x0, y0 = c * tile_w, r * tile_h
            print(f"[tile {r},{c}] {x0:.0f},{y0:.0f}")
            pc.generate(region=(x0 - pad, max(0.0, y0 - pad),
                                x0 + tile_w + pad, min(pc.MAP_H, y0 + tile_h + pad)))
            if pad:
                _trim_to_tile(x0, y0, x0 + tile_w, y0 + tile_h)
            bpy.ops.wm.save_as_mainfile(filepath=path)
    print(f"  done -> {out_dir}")


# --------------------------------------------------------------------------- #
if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out = argv[argv.index("--out") + 1] if "--out" in argv else None
    if "--build" in argv:
        tw, th = 5020.0, 4000.0
        if "--tile" in argv:
            i = argv.index("--tile")
            tw, th = float(argv[i + 1]), float(argv[i + 2])
        pad = float(argv[argv.index("--pad") + 1]) if "--pad" in argv else 0.0
        build_map(tw, th, out or "//tiles", pad=pad)
    else:
        preview_network()
        if out:
            bpy.ops.wm.save_as_mainfile(filepath=bpy.path.abspath(out))
