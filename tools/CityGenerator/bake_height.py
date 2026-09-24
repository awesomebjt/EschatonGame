"""
bake_height.py  --  companion height map for the cylinder backdrop, registered pixel-for-pixel
with the colour plate from render_map.py.

Two ways to get the same map:

  1. Z pass (bake_height_zpass): renders the scene's depth through the same orthographic
     camera. Because the camera is orthographic, depth is a linear distance along the view
     axis, so height = camera_z - depth with no perspective correction anywhere.

  2. Analytic (bake_height_geometry): rasterises the generator's own building footprints,
     platforms and bridge decks. Same result, perfectly hard edges, no renderer involved.
     Use this one for POM - anti-aliased depth ramps at roof edges are what make parallax
     shaders look melted.

Both write a single-channel 16-bit PNG where pixel = height / MAX_HEIGHT, linear, with the
Raw view transform so no sRGB curve is baked in. Import it as non-colour data.

    blender city.blend --background --python bake_height.py -- \
        --frame 13052 13500 18574 18500 --ppm 0.32 --out //height.png [--zpass]
"""

import math
import os
import sys

import numpy as np
import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) if "__file__" in dir() else ".")
import paris_city as pc
import render_map as rm

MAX_HEIGHT = 112.0         # ceiling of the 0..1 range: large dome monuments reach ~106 m
CAM_Z = 6000.0


# --------------------------------------------------------------------------- #
def _save_gray(arr01, path):
    """arr01: float array, rows bottom-to-top, values 0..1 -> 16-bit grayscale PNG."""
    h, w = arr01.shape
    img = bpy.data.images.new("HeightBake", width=w, height=h, float_buffer=True,
                              is_data=True)
    rgba = np.empty((h, w, 4), dtype=np.float32)
    rgba[..., 0] = rgba[..., 1] = rgba[..., 2] = arr01
    rgba[..., 3] = 1.0
    img.pixels.foreach_set(rgba.ravel())
    sc = bpy.context.scene
    st = sc.render.image_settings
    old = (st.file_format, st.color_mode, st.color_depth,
           sc.view_settings.view_transform)
    st.file_format, st.color_mode, st.color_depth = "PNG", "BW", "16"
    sc.view_settings.view_transform = "Raw"
    img.save_render(bpy.path.abspath(path), scene=sc)
    (st.file_format, st.color_mode, st.color_depth,
     sc.view_settings.view_transform) = old
    bpy.data.images.remove(img)
    print(f"  wrote {w} x {h} height map -> {path}  (1.0 = {MAX_HEIGHT:.0f} m)")


# --------------------------------------------------------------------------- #
def bake_height_zpass(x0, y0, x1, y1, ppm, out="//height.png", max_h=MAX_HEIGHT):
    """Depth pass through the same ortho camera, remapped to 0..1 over 0..max_h metres."""
    sc = bpy.context.scene
    for o in list(bpy.data.objects):
        if o.type == "CAMERA":
            bpy.data.objects.remove(o, do_unlink=True)
    cam, res_x, res_y = rm.setup_camera(x0, y0, x1, y1, ppm, engine="WORKBENCH")
    cam.location.z = CAM_Z

    sc.render.engine = "CYCLES"           # Workbench does not write a usable Z pass
    sc.cycles.samples = 1
    sc.cycles.use_denoising = False
    sc.render.filter_size = 0.01          # no AA: hard roof edges, no depth ramps
    bpy.context.view_layer.use_pass_z = True

    sc.use_nodes = True
    tree = sc.node_tree
    tree.nodes.clear()
    rl = tree.nodes.new("CompositorNodeRLayers")
    mr = tree.nodes.new("CompositorNodeMapRange")
    mr.inputs["From Min"].default_value = CAM_Z              # depth of ground (z = 0)
    mr.inputs["From Max"].default_value = CAM_Z - max_h      # depth of the ceiling height
    mr.inputs["To Min"].default_value = 0.0
    mr.inputs["To Max"].default_value = 1.0
    mr.use_clamp = True
    comp = tree.nodes.new("CompositorNodeComposite")
    tree.links.new(rl.outputs["Depth"], mr.inputs["Value"])
    tree.links.new(mr.outputs[0], comp.inputs["Image"])

    st = sc.render.image_settings
    st.file_format, st.color_mode, st.color_depth = "PNG", "BW", "16"
    sc.view_settings.view_transform = "Raw"
    sc.render.filepath = bpy.path.abspath(out)
    bpy.ops.render.render(write_still=True)
    print(f"  wrote Z-pass height map -> {out}  (1.0 = {max_h:.0f} m)")


# --------------------------------------------------------------------------- #
def _raster_height(grid, poly, h, cx, cy, ox, oy, mode="max"):
    """Blend a convex polygon's height into the grid (rows are +Y). mode: max | set."""
    H, W = grid.shape
    xs = [p[0] - ox for p in poly]
    ys = [p[1] - oy for p in poly]
    i0, i1 = max(0, int(min(xs) // cx)), min(W - 1, int(max(xs) // cx))
    j0, j1 = max(0, int(min(ys) // cy)), min(H - 1, int(max(ys) // cy))
    if i0 > i1 or j0 > j1:
        return
    X, Y = np.meshgrid((np.arange(i0, i1 + 1) + 0.5) * cx,
                       (np.arange(j0, j1 + 1) + 0.5) * cy, indexing="xy")
    inside = np.ones_like(X, dtype=bool)
    n = len(poly)
    for k in range(n):
        ax, ay = xs[k], ys[k]
        bx, by = xs[(k + 1) % n], ys[(k + 1) % n]
        inside &= (bx - ax) * (Y - ay) - (by - ay) * (X - ax) >= 0
    view = grid[j0:j1 + 1, i0:i1 + 1]
    if mode == "set":
        view[inside] = h
    else:
        np.maximum(view, np.where(inside, h, 0.0), out=view)


def bake_height_geometry(x0, y0, x1, y1, ppm, out="//height.png", max_h=MAX_HEIGHT,
                         pad=502.0, result=None):
    """Rasterise heights straight from the generator. Sharp edges, no renderer."""
    if result is None:
        result = pc.generate(region=(x0 - pad, max(0.0, y0 - pad),
                                     x1 + pad, min(pc.MAP_H, y1 + pad)))
    # the rounded resolution is the authority, exactly as in render_map.setup_camera,
    # so the height map and the colour plate land on the same pixel grid
    w = int(round((x1 - x0) * ppm / 4)) * 4
    h = int(round((y1 - y0) * ppm / 4)) * 4
    cx, cy = (x1 - x0) / w, (y1 - y0) / h
    grid = np.zeros((h, w), dtype=np.float32)

    for T, poly in result["buildings"]:
        _raster_height(grid, poly, T.H, cx, cy, x0, y0)
    for T, poly in result["buildings"]:          # courtyards are holes, not solid roof
        if not T.courtyard:
            continue
        inner = pc.inset_quad(poly, pc.COURTYARD_WALL)
        if inner and pc.poly_area(inner) > 25.0:
            _raster_height(grid, inner, 0.0, cx, cy, x0, y0, mode="set")
    for i, hub in enumerate(result["hubs"]):
        size = result["sizes"][i]
        plat_r = pc.ROUNDABOUT_TYPES[size][2]
        for sh in (-pc.MAP_W, 0.0, pc.MAP_W):
            p = (hub[0] + sh, hub[1])
            if not (x0 - 200 < p[0] < x1 + 200 and y0 - 200 < p[1] < y1 + 200):
                continue
            z = pc.PLATFORM_HEIGHT
            _raster_height(grid, pc.circle_poly(p, plat_r, 32), z, cx, cy, x0, y0)
            if size == 0:
                r, hh = pc.MONUMENT_S
                _raster_height(grid, pc.circle_poly(p, r, 12), z + hh, cx, cy, x0, y0)
            elif size == 1:
                r, hh = pc.MONUMENT_M
                _raster_height(grid, pc.circle_poly(p, r, 16), z + hh, cx, cy, x0, y0)
            else:
                r, hh = pc.MONUMENT_L
                _raster_height(grid, pc.circle_poly(p, r, 48), z + hh, cx, cy, x0, y0)
                for k in range(1, 9):                       # hemispherical cap
                    phi = (math.pi / 2) * k / 8
                    _raster_height(grid, pc.circle_poly(p, r * math.cos(phi), 48),
                                   z + hh + r * math.sin(phi), cx, cy, x0, y0)
    river = result["river"]
    for a, b, _ in result["majors"]:                       # bridge decks
        L = math.dist(a, b)
        if L < 1e-6:
            continue
        t = pc.unit(pc.sub(b, a))
        steps = max(1, int(L / 20.0))
        for k in range(steps):
            pa = pc.add(a, pc.mul(t, L * k / steps))
            pb = pc.add(a, pc.mul(t, L * (k + 1) / steps))
            z = max(river.bridge_z(pa), river.bridge_z(pb))
            if z > 0.05:
                _raster_height(grid, pc.rect_poly(pa, pb, pc.MAJOR_WIDTH / 2),
                               z, cx, cy, x0, y0)
    _save_gray(np.clip(grid / max_h, 0.0, 1.0), out)
    return grid


# --------------------------------------------------------------------------- #
if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    i = argv.index("--frame")
    fx0, fy0, fx1, fy1 = (float(v) for v in argv[i + 1:i + 5])
    ppm = float(argv[argv.index("--ppm") + 1]) if "--ppm" in argv else 0.32
    out = argv[argv.index("--out") + 1] if "--out" in argv else "//height.png"
    if "--zpass" in argv:
        bake_height_zpass(fx0, fy0, fx1, fy1, ppm, out)
    else:
        bake_height_geometry(fx0, fy0, fx1, fy1, ppm, out)
