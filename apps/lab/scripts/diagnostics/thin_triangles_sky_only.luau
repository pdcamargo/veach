--!strict
-- VCH-110 diagnostic: scripts/thin_triangles_realtime.luau with the camera looking straight up, so the scene is
-- imported, cooked and uploaded but no triangle reaches a pixel. Hangs on DX12/WARP too: the import, cook or upload;
-- passes: rasterizing or shading the triangles.
-- Run: veach-lab --rhi dx12 --headless --frames 3 --script scripts/diagnostics/thin_triangles_sky_only.luau
local veach = require("@veach")
local c = veach.components

veach.spawn("thin triangles"):set(c.SceneInstance, { path = "assets/thin_triangles/thin_triangles.gltf" })

local eye = vector.create(0, 1, 2.6)
local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(eye),
	rotation = veach.quat.look_rotation(vector.create(0, 1, -0.01)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(45), near_plane = 0.1, active = true, ev100 = 12 })
camera:set(c.Sky, { mode = "constant", color = vector.create(600, 0, 600) })

local sun = veach.spawn("sun")
sun:set(c.Transform, {
	translation = veach.position(0, 0, 0),
	rotation = veach.quat.look_rotation(vector.create(0.3, -0.6, 0.75)),
	scale = vector.one,
})
sun:set(c.DirectionalLight, { color = vector.create(1, 0.97, 0.92), illuminance = 40000 })

veach.system("DiagnosticSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
