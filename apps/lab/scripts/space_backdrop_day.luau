--!strict
-- The space backdrop under a midday sky (backdrop.AC9, docs/specs/space-backdrop/spec.md): the physical sky at noon,
-- looking away from the sun 25 degrees up, at EV100 15, with the backdrop on at its default creator settings. A lit
-- sky hides the boost: the image is the physical backdrop's, starless (tests/mcp/test_mcp_lab.py backdrop_sky
-- compares the two, and at midnight).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/space_backdrop_day.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(0, 1.7, 0),
	rotation = veach.quat.look_rotation(vector.create(0, math.tan(math.rad(25)), -1)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(60), near_plane = 0.1, active = true, ev100 = 15 })
camera:set(c.Sky, { time_of_day = 12, latitude = 45, day_of_year = 172 })
camera:set(c.SpaceBackdrop, { seed = 7, visible = true })

veach.system("BackdropDaySize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 480, height = 270, mode = w.mode, vsync = w.vsync })
end)
