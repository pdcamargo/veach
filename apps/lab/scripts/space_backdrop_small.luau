--!strict
-- The space backdrop's golden (backdrop.AC7, docs/specs/space-backdrop/spec.md): deep space as space_backdrop.luau
-- shows it, small (480 x 270) so the image is quick to compare. Stars, the band, its bulge and dust, and nebulae.
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/space_backdrop_small.luau --screenshot out.png
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

veach.system("BackdropSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 480, height = 270, mode = w.mode, vsync = w.vsync })
end)
