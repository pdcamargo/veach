--!strict
-- The sky's ambient light comes from the air around each surface, not around the camera (docs/specs/ambient-light/spec.md,
-- ambient.AC2): the camera stands where the sun is 5 degrees up (the equator, the equinox, 17:40); two flat patches lie
-- 1,500 km away on the curve of the Sky's 6,360 km planet, one toward the sun (18 degrees up there) and one away from
-- it (8 degrees down: night). Only the sky lights them (its sun is no light here) and there is no haze, so the patch
-- away from the sun is dark: before ADR 0095 both got the camera's sky.
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/ambient_night_side.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local sky = { mode = "physical", drives_sun = false, time_of_day = 17.67, latitude = 0, day_of_year = 80,
	aerial_perspective = false }
veach.spawn("sky"):set(c.Sky, sky)

-- The sun's azimuth at 17:40 on the equinox at the equator: due west (-X with the Sky's default north, -Z).
local distance = 1.5e6
local drop = distance * distance / (2 * 6.36e6)
for _, side in { { name = "day", x = -distance }, { name = "night", x = distance } } do
	local patch = veach.spawn(side.name)
	patch:set(c.Transform, {
		translation = veach.position(side.x, -drop, 0),
		rotation = veach.quat.identity(),
		scale = vector.create(10000, 1, 10000),
	})
	patch:set(c.RuntimeMeshDemo, { size = 8, cells = 16, height = 0, rock_above = 1 })
end

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = veach.position(0, 2000, 0),
	rotation = veach.quat.look_rotation(vector.create(1, -0.12, 0)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(10), near_plane = 1, active = true, ev100 = 12 })

veach.system("NightSideSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 960, height = 540, mode = w.mode, vsync = w.vsync })
end)
