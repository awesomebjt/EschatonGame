"""
render_map.py  --  orthographic "photo" of the flat city for use as a cylinder backdrop.

Renders an exact-scale plan view of a rectangle of the map. The frame is metric: the
image's left and right edges land exactly on x0 and x1, so if you frame the full
0..MAP_W width the texture wraps around a cylinder with no seam.

Two ways to use it:

  A. One region already built in the scene:
         blender city.blend --background --python render_map.py -- --frame 13052 13500 18574 18500
  B. Whole map, tile by tile (generates each tile itself, so the full map is never in
     memory at once):
         blender --background --python render_map.py -- --atlas --ppm 0.32 --tile 5020 4000

Tiles are written as tile_<row>_<col>.png and stitched with stitch_tiles() (needs Pillow),
or with ImageMagick:  magick montage tile_*.png -tile 5x8 -geometry +0+0 map.png
"""

import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paris_city as pc


# --------------------------------------------------------------------------- #
def setup_camera(x0, y0, x1, y1, ppm, engine="WORKBENCH", sun_angle=50.0):
    """Ortho camera framing exactly (x0, y0)-(x1, y1), at ppm pixels per metre."""
    w_m, h_m = x1 - x0, y1 - y0
    res_x = int(round(w_m * ppm / 4)) * 4          # multiples of 4 keep BC compression happy
    res_y = int(round(h_m * ppm / 4)) * 4
    sc = bpy.context.scene

    cam_data = bpy.data.cameras.new("MapCam")
    cam_data.type = "ORTHO"
    cam_data.sensor_fit = "HORIZONTAL"             # ortho_scale then always means width
    cam_data.ortho_scale = w_m
    cam_data.clip_start = 1.0
    cam_data.clip_end = 20000.0
    cam = bpy.data.objects.new("MapCam", cam_data)
    sc.collection.objects.link(cam)
    cam.location = ((x0 + x1) / 2, (y0 + y1) / 2, 6000.0)
    cam.rotation_euler = (0.0, 0.0, 0.0)           # straight down, +Y up in the image
    sc.camera = cam

    sc.render.resolution_x = res_x
    sc.render.resolution_y = res_y
    sc.render.resolution_percentage = 100
    sc.render.pixel_aspect_x = sc.render.pixel_aspect_y = 1.0
    sc.render.film_transparent = False
    sc.view_settings.view_transform = "Standard"   # AgX would wash the flat colours out
    sc.view_settings.look = "None"

    if engine == "WORKBENCH":
        sc.render.engine = "BLENDER_WORKBENCH"
        sh = sc.display.shading
        sh.light = "STUDIO"
        sh.color_type = "MATERIAL"
        sh.show_shadows = True
        sh.show_cavity = True
        sh.shadow_intensity = 0.4
    else:
        sc.render.engine = "BLENDER_EEVEE_NEXT"
        if not any(o.type == "LIGHT" for o in sc.objects):
            ld = bpy.data.lights.new("MapSun", type="SUN")
            ld.energy = 4.0
            ld.angle = math.radians(2.0)
            sun = bpy.data.objects.new("MapSun", ld)
            sc.collection.objects.link(sun)
            sun.rotation_euler = (math.radians(90 - sun_angle), 0.0, math.radians(35.0))
    print(f"  frame {w_m:.0f} x {h_m:.0f} m -> {res_x} x {res_y} px "
          f"({w_m / max(1, res_x):.2f} m/px)")
    return cam, res_x, res_y


def render_to(path):
    bpy.context.scene.render.image_settings.file_format = "PNG"
    bpy.context.scene.render.image_settings.color_mode = "RGB"
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)


def render_atlas(tile_w=5020.0, tile_h=4000.0, ppm=0.32, out_dir="//map_tiles",
                 engine="WORKBENCH", pad=502.0):
    """Generate and render the whole map one tile at a time.

    Each tile is generated with `pad` metres of extra context so buildings that overhang
    the tile edge (and across the x seam) are present, then framed to the exact tile."""
    out_dir = bpy.path.abspath(out_dir)
    os.makedirs(out_dir, exist_ok=True)
    cols = int(round(pc.MAP_W / tile_w))
    rows = int(round(pc.MAP_H / tile_h))
    assert abs(cols * tile_w - pc.MAP_W) < 1e-6 and abs(rows * tile_h - pc.MAP_H) < 1e-6, \
        "tile size must divide MAP_W and MAP_H exactly"
    for r in range(rows):
        for c in range(cols):
            x0, y0 = c * tile_w, r * tile_h
            x1, y1 = x0 + tile_w, y0 + tile_h
            print(f"[tile {r},{c}] generating")
            pc.generate(region=(x0 - pad, max(0.0, y0 - pad),
                                x1 + pad, min(pc.MAP_H, y1 + pad)))
            for o in list(bpy.data.objects):
                if o.type == "CAMERA":
                    bpy.data.objects.remove(o, do_unlink=True)
            setup_camera(x0, y0, x1, y1, ppm, engine)
            render_to(os.path.join(out_dir, f"tile_{rows - 1 - r:02d}_{c:02d}.png"))
    print(f"done: {rows} x {cols} tiles in {out_dir}")


def stitch_tiles(out_dir="//map_tiles", out_file="//city_map.png"):
    """Paste tile_<row>_<col>.png into one image (row 0 = top). Needs Pillow."""
    from PIL import Image
    out_dir = bpy.path.abspath(out_dir)
    names = sorted(n for n in os.listdir(out_dir) if n.startswith("tile_"))
    rows = max(int(n.split("_")[1]) for n in names) + 1
    cols = max(int(n.split("_")[2].split(".")[0]) for n in names) + 1
    first = Image.open(os.path.join(out_dir, names[0]))
    tw, th = first.size
    sheet = Image.new("RGB", (tw * cols, th * rows))
    for n in names:
        r, c = int(n.split("_")[1]), int(n.split("_")[2].split(".")[0])
        sheet.paste(Image.open(os.path.join(out_dir, n)), (c * tw, r * th))
    sheet.save(bpy.path.abspath(out_file))
    print(f"wrote {sheet.size[0]} x {sheet.size[1]} to {out_file}")


# --------------------------------------------------------------------------- #
if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    ppm = float(argv[argv.index("--ppm") + 1]) if "--ppm" in argv else 0.32
    engine = "EEVEE" if "--eevee" in argv else "WORKBENCH"
    if "--atlas" in argv:
        tw, th = (5020.0, 4000.0)
        if "--tile" in argv:
            i = argv.index("--tile")
            tw, th = float(argv[i + 1]), float(argv[i + 2])
        render_atlas(tw, th, ppm, engine=engine)
    else:
        i = argv.index("--frame")
        x0, y0, x1, y1 = (float(v) for v in argv[i + 1:i + 5])
        setup_camera(x0, y0, x1, y1, ppm, engine)
        render_to(argv[argv.index("--out") + 1] if "--out" in argv else "//map.png")
