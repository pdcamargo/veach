--!strict
-- The Cornell box and its camera 1 AU from the origin, in the root world (docs/specs/frames/spec.md, frames.AC8).
-- Positions are doubles and extraction is camera-relative, so the image matches tests/golden/cornell_box_realtime.png
-- (cornell_box_realtime.luau's view); float32 world positions turned this black (SP-03).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/frames_1au.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

-- Away from the origin along a direction with all three axes large, so every coordinate is a large double.
local far = veach.position(0.6 * 1.495978707e11, 0.48 * 1.495978707e11, 0.64 * 1.495978707e11)

local box = veach.spawn("cornell box")
box:set(c.Transform, { translation = far, rotation = veach.quat.identity(), scale = vector.one })
box:set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = far + vector.create(0, 1, 3.9),
	rotation = veach.quat.identity(),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "none" })

veach.system("FramesSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
