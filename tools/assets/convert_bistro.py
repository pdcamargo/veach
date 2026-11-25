"""Convert Amazon Lumberyard Bistro (BistroExterior.fbx, ORCA v5.2) to glTF 2.0 (.gltf + .bin + PNG/JPEG).

Used once to make apps/lab/assets/bistro (see its README). Run (Blender 5.2, headless):
  blender --background --factory-startup --python convert_bistro.py -- \
      <BistroExterior.fbx> <work_dir> <out.gltf>

What it does, and why:
- Imports the FBX with Blender's default unit handling. The FBX root carries the unit scale and the
  Y-up rotation; the exporter turns that back into a +Y-up glTF whose world extent is in meters.
- Bistro ships BC-compressed DDS (and a few TGA). Veach reads only PNG/JPEG, so every texture is
  decoded by Blender and written to <work_dir>/textures first, then the exporter copies those bytes:
    BaseColor without real alpha -> JPEG (q95)      BaseColor with alpha -> PNG RGBA
    Normal (DirectX, BC5)        -> PNG RGB, green flipped to glTF/OpenGL convention
    Specular (R=AO, G=rough, B=metal, per README) -> PNG RGB, used as metallicRoughness as is
    Emissive                     -> JPEG (q95)
- Rebuilds each material as a plain Principled BSDF, since the FBX importer wires the Specular map to
  "Specular IOR Level" and base color alpha to Alpha (HASHED) on every material:
    Base Color <- BaseColor.rgb; Alpha <- Round(BaseColor.a) only where the texture has real alpha
    (exports as alphaMode MASK, alphaCutoff 0.5, double-sided); Roughness <- Specular.G;
    Metallic <- Specular.B; Normal <- Normal map; Emission Color <- Emissive, strength 1.
- Drops the camera and its animation; keeps the analytic light (sun) as KHR_lights_punctual.
"""

import os
import sys
import time

import bpy
import numpy as np
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:]
FBX, WORK, OUT = (os.path.abspath(a) for a in args[:3])
TEX_WORK = os.path.join(WORK, "textures")
os.makedirs(TEX_WORK, exist_ok=True)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
JPEG_QUALITY = 95
ALPHA_PIXELS = 0.001  # a BaseColor counts as alpha-tested if >= 0.1% of its texels are below 0.5

t_start = time.time()
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=FBX)
print(f"CONVERT import done in {time.time() - t_start:.1f}s")


def role_of(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    for role in ("BaseColor", "Normal", "Specular", "Emissive"):
        if stem.endswith("_" + role) or stem.endswith(role):
            return role
    return None


def read_pixels(img):
    a = np.empty(img.size[0] * img.size[1] * 4, dtype=np.float32)
    img.pixels.foreach_get(a)
    return a.reshape(-1, 4)


def write_image(name, pixels, w, h, path, fmt, alpha, colorspace):
    """Write pixels through a fresh byte image (no view transform), then load the file back."""
    out = bpy.data.images.new(name + "__tmp", w, h, alpha=alpha)
    out.colorspace_settings.name = colorspace
    out.pixels.foreach_set(pixels.ravel())
    out.filepath_raw = path
    out.file_format = fmt
    if fmt == "JPEG":
        out.save(filepath=path, quality=JPEG_QUALITY)
    else:
        out.save(filepath=path)
    bpy.data.images.remove(out)
    img = bpy.data.images.load(path, check_existing=True)
    img.colorspace_settings.name = colorspace
    return img


converted = {}  # source path -> (image, has_alpha)
alpha_report = []


def convert(src_img):
    src = os.path.normpath(bpy.path.abspath(src_img.filepath))
    if src in converted:
        return converted[src]
    role = role_of(src)
    stem = os.path.splitext(os.path.basename(src))[0]
    w, h = src_img.size
    if w == 0:
        print(f"CONVERT WARNING: could not decode {src}")
        converted[src] = (None, False)
        return converted[src]
    p = read_pixels(src_img)
    has_alpha = False
    if role == "BaseColor":
        below = float((p[:, 3] < 0.5).mean())
        has_alpha = below >= ALPHA_PIXELS
        if has_alpha:
            alpha_report.append((stem, w, h, below))
            img = write_image(stem, p, w, h, os.path.join(TEX_WORK, stem + ".png"), "PNG", True, "sRGB")
        else:
            p[:, 3] = 1.0
            img = write_image(stem, p, w, h, os.path.join(TEX_WORK, stem + ".jpg"), "JPEG", False, "sRGB")
    elif role == "Normal":
        p[:, 1] = 1.0 - p[:, 1]  # DirectX (+Y down) -> OpenGL/glTF (+Y up)
        p[:, 3] = 1.0
        img = write_image(stem, p, w, h, os.path.join(TEX_WORK, stem + ".png"), "PNG", False, "Non-Color")
    elif role == "Specular":
        p[:, 3] = 1.0
        img = write_image(stem, p, w, h, os.path.join(TEX_WORK, stem + ".png"), "PNG", False, "Non-Color")
    else:  # Emissive or unknown colour data
        p[:, 3] = 1.0
        img = write_image(stem, p, w, h, os.path.join(TEX_WORK, stem + ".jpg"), "JPEG", False, "sRGB")
    converted[src] = (img, has_alpha)
    return converted[src]


t_tex = time.time()
mat_report = []
for mat in bpy.data.materials:
    nt = mat.node_tree
    by_role = {}
    for node in nt.nodes:
        if node.type == "TEX_IMAGE" and node.image:
            role = role_of(node.image.filepath)
            if role and role not in by_role:
                by_role[role] = node.image
    old = nt.nodes.get("Principled BSDF")
    base_factor = tuple(old.inputs["Base Color"].default_value) if old else (0.8, 0.8, 0.8, 1.0)
    emit_strength = old.inputs["Emission Strength"].default_value if old else 1.0
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    bsdf.inputs["Roughness"].default_value = 0.5
    bsdf.inputs["Metallic"].default_value = 0.0
    masked = False

    def tex(img):
        n = nt.nodes.new("ShaderNodeTexImage")
        n.image = img
        return n

    if "BaseColor" in by_role:
        img, has_alpha = convert(by_role["BaseColor"])
        if img:
            t = tex(img)
            nt.links.new(t.outputs["Color"], bsdf.inputs["Base Color"])
            if has_alpha:
                rnd = nt.nodes.new("ShaderNodeMath")
                rnd.operation = "ROUND"
                nt.links.new(t.outputs["Alpha"], rnd.inputs[0])
                nt.links.new(rnd.outputs["Value"], bsdf.inputs["Alpha"])
                masked = True
    else:
        bsdf.inputs["Base Color"].default_value = base_factor
    if "Specular" in by_role:
        img, _ = convert(by_role["Specular"])
        if img:
            t = tex(img)
            sep = nt.nodes.new("ShaderNodeSeparateColor")
            nt.links.new(t.outputs["Color"], sep.inputs["Color"])
            nt.links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
            nt.links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
            bsdf.inputs["Roughness"].default_value = 1.0
            bsdf.inputs["Metallic"].default_value = 1.0
    if "Normal" in by_role:
        img, _ = convert(by_role["Normal"])
        if img:
            t = tex(img)
            nm = nt.nodes.new("ShaderNodeNormalMap")
            nt.links.new(t.outputs["Color"], nm.inputs["Color"])
            nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    if "Emissive" in by_role:
        img, _ = convert(by_role["Emissive"])
        if img:
            t = tex(img)
            nt.links.new(t.outputs["Color"], bsdf.inputs["Emission Color"])
            bsdf.inputs["Emission Strength"].default_value = emit_strength
    # glTF doubleSided = not use_backface_culling. Only alpha-tested cards (foliage, letters...) need both sides.
    mat.use_backface_culling = not masked
    mat_report.append((mat.name, sorted(by_role), masked))
print(f"CONVERT textures done in {time.time() - t_tex:.1f}s: {len(converted)} source images")
for stem, w, h, below in alpha_report:
    print(f"CONVERT alpha-tested BaseColor {stem} {w}x{h} texels<0.5: {below:.1%}")
for name, roles, masked in mat_report:
    if masked or "BaseColor" not in roles:
        print(f"CONVERT material {name} roles={roles} masked={masked}")

# Camera (and its animated path) are not imported by Veach; drop them.
for obj in [o for o in bpy.data.objects if o.type == "CAMERA"]:
    bpy.data.objects.remove(obj, do_unlink=True)

# Size sanity check in Blender space (Z up, meters).
lo, hi = Vector((1e30,) * 3), Vector((-1e30,) * 3)
for o in bpy.context.scene.objects:
    if o.type == "MESH":
        for c in o.bound_box:
            v = o.matrix_world @ Vector(c)
            lo, hi = Vector(map(min, lo, v)), Vector(map(max, hi, v))
print(f"CONVERT scene extent (m, Blender Z-up): {tuple(round(x, 2) for x in hi - lo)}")
samples = 0
for o in bpy.context.scene.objects:
    if o.type == "MESH" and any(k in o.name for k in ("Chair", "Vespa", "Door", "Lamp")) and samples < 8:
        mlo = Vector(map(min, *[o.matrix_world @ Vector(c) for c in o.bound_box]))
        mhi = Vector(map(max, *[o.matrix_world @ Vector(c) for c in o.bound_box]))
        print(f"CONVERT size check {o.name}: {tuple(round(x, 2) for x in mhi - mlo)} m")
        samples += 1

t_exp = time.time()
bpy.ops.export_scene.gltf(
    filepath=OUT,
    export_format="GLTF_SEPARATE",
    export_texture_dir="textures",
    export_image_format="AUTO",
    export_keep_originals=False,
    export_yup=True,
    export_apply=False,
    export_cameras=False,
    export_lights=True,
    export_animations=False,
    export_extras=False,
    export_tangents=False,
    export_materials="EXPORT",
)
print(f"CONVERT export done in {time.time() - t_exp:.1f}s, total {time.time() - t_start:.1f}s")
