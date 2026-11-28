--!strict
-- Spike SP-04 (VCH-080), THROWAWAY: the physical Sky lit by the sun from the system (docs/research/spikes/sp-04-orrery-clock.md).
-- The lab module spawns an Earth-like orrery and a SunObserver at 60°N from the environment (apps/lab/src/sp04_spike.hpp):
--   VEACH_SP04_SCALE=1 VEACH_SP04_EVENT=june VEACH_SP04_TIME=noon \
--     veach-lab --rhi metal --headless --frames 3 --script scripts/sp04_sky.luau --screenshot out.png
-- Today's Sky only takes Earth astronomy (time_of_day, latitude, day_of_year, north), so this maps the observer's
-- (east, north, up) sun direction onto it: at latitude 0 on an equinox the sun moves in the east-west plane, the
-- hour angle sets its elevation and `north` turns it to the right azimuth. ADR 0048 replaces this with a direction.
local veach = require("@veach")
local c = veach.components

local camera = veach.spawn("camera")
local sky = veach.spawn("sky")
-- The renderer draws nothing without meshes: the helmet (CC0) stands 2 m away, toward the sun, lit by it.
local helmet = veach.spawn("helmet")
helmet:set(c.SceneInstance, { path = "assets/flight_helmet/FlightHelmet.gltf" })

veach.system("Sp04Sky", "PostSimulation", veach.query(c.SunObserver), function(_e, o)
	local east: number, north: number, up: number, elevation: number = o.east, o.north, o.up, o.elevation
	-- World axes: +X east, +Y up, -Z north. The sun's horizontal direction:
	local h = math.sqrt(east * east + north * north)
	local hx, hz = east / h, -north / h
	-- Sky at latitude 0, declination 0: east = -sin(H), up = cos(H), along -east_axis = (-cos n, 0, sin n).
	local hour_angle = math.acos(math.clamp(up, -1, 1))
	sky:set(c.Sky, {
		time_of_day = 12 + hour_angle * 12 / math.pi,
		latitude = 0,
		day_of_year = 81.25, -- the Sky's own equinox (declination 0)
		north = math.atan2(hz, -hx),
	})
	helmet:set(c.Transform, {
		translation = veach.position(2 * hx, 0, 2 * hz),
		rotation = veach.quat.look_rotation(vector.create(hx, 0, hz)), -- its face (+Z) toward the camera
		scale = vector.one,
	})
	-- Look toward the sun's azimuth, 20 degrees up.
	camera:set(c.Transform, {
		translation = veach.position(0, 0.3, 0),
		rotation = veach.quat.look_rotation(vector.create(hx, math.tan(math.rad(20)), hz)),
		scale = vector.one,
	})
	camera:set(c.Camera, {
		fov_y = math.rad(90),
		near_plane = 0.1,
		active = true,
		ev100 = if elevation > 10 then 14 else 11,
	})
end)

veach.system("Sp04Size", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 640, height = 360, mode = w.mode, vsync = w.vsync })
end)
