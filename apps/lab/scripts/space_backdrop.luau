--!strict
-- Deep space: the space backdrop alone, no sky and no planet (docs/specs/space-backdrop/spec.md). The backdrop's
-- galactic center is toward -Z with the north galactic pole +Y; the camera looks along the band toward the center,
-- turned a little so the band crosses the frame. The camera's exposure is fixed at EV100 -4 (deep space's).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/space_backdrop.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(0, 0, 0),
	rotation = veach.quat.mul(
		veach.quat.from_euler(math.rad(20), math.rad(8)),
		veach.quat.from_euler(0, 0, math.rad(-25))
	),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(60), near_plane = 0.1, active = true, ev100 = -4 })
camera:set(c.Sky, { mode = "none" })
camera:set(c.SpaceBackdrop, { seed = 7, visible = true, exposure_compensation = 4, exposure_follow = 0.1 })

-- 1440p, the size the backdrop's GPU budget is measured at (0.3 ms).
veach.system("BackdropSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 2560, height = 1440, mode = w.mode, vsync = w.vsync })
end)
