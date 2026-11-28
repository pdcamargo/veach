--!strict
-- Upright thin triangles standing in a smooth ground, through the real-time renderer (VCH-108, rt.AC8): what SP-08's
-- Transvoxel seams showed. Every vertex normal points up, so the fins and slivers must shade like the ground around them.
-- Before the fix the camera-facing ones, which face away from the sun, kept only the sky's ambient light (with a white
-- material under SP-08's magenta sky that looked like pinholes), and the ones seen from behind turned dark.
-- Scene: tools/assets/make_thin_triangles.py. Golden image: tests/golden/thin_triangles_realtime.png (256x256).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/thin_triangles_realtime.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

veach.spawn("thin triangles"):set(c.SceneInstance, { path = "assets/thin_triangles/thin_triangles.gltf" })

local eye = vector.create(0, 1, 2.6)
local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = eye,
	rotation = veach.quat.look_rotation(vector.create(0, 0.05, 0) - eye),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(45), near_plane = 0.1, active = true, ev100 = 12 })
-- SP-08's constant magenta sky: the ambient light is all a surface gets when the sun is cut off.
camera:set(c.Sky, { mode = "constant", color = vector.create(600, 0, 600) })

-- Behind the fins: their camera-facing side faces away from it, their (upward) normals toward it.
local sun = veach.spawn("sun")
sun:set(c.Transform, {
	translation = vector.zero,
	rotation = veach.quat.look_rotation(vector.create(0.3, -0.6, 0.75)),
	scale = vector.one,
})
sun:set(c.DirectionalLight, { color = vector.create(1, 0.97, 0.92), illuminance = 40000 })

-- Square, small: the golden runs on WARP too.
veach.system("ThinTrianglesSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
