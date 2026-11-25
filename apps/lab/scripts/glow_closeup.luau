--!strict
-- GlowMaterial up close (rt.AC3 in docs/specs/realtime-renderer-v0/spec.md): one material definition must render
-- the same in the real-time renderer and the path tracer. tools/bench/compare_direct.py renders it both ways.
local veach = require("@veach")
local c = veach.components

veach.spawn("cornell box"):set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = vector.create(0.55, 0.32, 1.05),
	rotation = veach.quat.look_rotation(vector.create(-0.25, -0.02, -1)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(35), near_plane = 0.05, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "none" })

veach.system("GlowCloseupSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 160, height = 160, mode = w.mode, vsync = w.vsync })
end)
