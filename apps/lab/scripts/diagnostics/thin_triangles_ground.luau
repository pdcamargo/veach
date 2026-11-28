--!strict
-- VCH-110 diagnostic: scripts/thin_triangles_realtime.luau with the ground's primitive alone (no thin triangles, one
-- primitive), same camera, sun and sky. Hangs on DX12/WARP too: the light, sky or cook; passes: the thin triangles or
-- the second primitive. Run: veach-lab --rhi dx12 --headless --frames 3 --script scripts/diagnostics/thin_triangles_ground.luau
local veach = require("@veach")
local c = veach.components

veach.spawn("ground"):set(c.SceneInstance, { path = "assets/thin_triangles/thin_triangles_ground.gltf" })

local eye = vector.create(0, 1, 2.6)
local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = eye,
	rotation = veach.quat.look_rotation(vector.create(0, 0.05, 0) - eye),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(45), near_plane = 0.1, active = true, ev100 = 12 })
camera:set(c.Sky, { mode = "constant", color = vector.create(600, 0, 600) })

local sun = veach.spawn("sun")
sun:set(c.Transform, {
	translation = vector.zero,
	rotation = veach.quat.look_rotation(vector.create(0.3, -0.6, 0.75)),
	scale = vector.one,
})
sun:set(c.DirectionalLight, { color = vector.create(1, 0.97, 0.92), illuminance = 40000 })

veach.system("DiagnosticSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
