--!strict
-- Temporal AA across a render-origin jump (docs/specs/temporal-aa/spec.md, taa.AC4): the Cornell box at x = 95.625 m
-- and a camera strafing past it at 5 cm a frame, from x = 95.425 m. At frame 12 the camera crosses x = 96 m, where the
-- render origin (the camera's position on a 64 m grid, ADR 0085) jumps from 64 m to 128 m, so every render-space
-- position moves by 64 m. taa_origin_still.luau is the same scene 32 m to the left, where the origin stays at 64 m: the
-- two images at frame 12, the first after the jump, must agree (by FLIP), so the history survived the jump. Had it been
-- lost, frame 12 would be one jittered frame alone.
-- Run: veach-lab --rhi metal --headless --frames 12 --script scripts/taa_origin_jump.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local x0 = 95.625  -- the box; taa_origin_still.luau: 63.625

local box = veach.spawn("cornell box")
box:set(c.Transform, { translation = veach.position(x0, 0, 0), rotation = veach.quat.identity(), scale = vector.one })
box:set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(c.Sky, { mode = "none" })
local function place(frame: number)
	camera:set(c.Transform, {
		translation = veach.position(x0 - 0.2 + 0.05 * frame, 1, 3.9),
		rotation = veach.quat.identity(),
		scale = vector.one,
	})
end
place(0)
veach.system("TaaOriginStrafe", "Simulation", veach.query(c.Camera), function(e, _)
	place(veach.time.frame())
end)

veach.system("TaaOriginSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
