--!strict
-- taa_origin_jump.luau 32 m to the left (docs/specs/temporal-aa/spec.md, taa.AC4): the camera strafes from
-- x = 63.425 m and stays within 32 m of 64 m, so the render origin never moves. The jump's image is compared with this.
-- Run: veach-lab --rhi metal --headless --frames 12 --script scripts/taa_origin_still.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local x0 = 63.625  -- the box; taa_origin_jump.luau: 95.625

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
