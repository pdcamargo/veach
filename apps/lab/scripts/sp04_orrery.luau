--!strict
-- Spike SP-04 (VCH-080), THROWAWAY: the orrery from Luau (docs/research/spikes/sp-04-orrery-clock.md).
-- The Sol preset paused at J2000.0 (tick 378683424000; Sol's universe epoch is 1800-01-01 TDB) and an observer in
-- Paris; on frame 2 it logs every body's heliocentric distance and the sun as seen from Paris.
--   veach-lab --headless --frames 3 --script scripts/sp04_orrery.luau
-- Over MCP the same entities are live: set_component "orrery" UniverseClock { tick = ..., rate = 0 }, step_frames 1,
-- then query_entities CelestialBody or get_component "paris" SunObserver.
local veach = require("@veach")
local c = veach.components

local au = 149597870700
local j2000 = 378683424000

local orrery = veach.spawn("orrery")
orrery:set(c.Orrery, { system = "sol", scale = 1 })
orrery:set(c.UniverseClock, { tick = j2000, rate = 0 })
veach.spawn("paris"):set(c.SunObserver, { body = "earth", latitude = 48.8566, longitude = 2.3522 })

veach.system("Sp04Bodies", "PostSimulation", veach.query(c.CelestialBody), function(_e, b)
	if veach.time.frame() ~= 2 then
		return
	end
	local x: number, y: number, z: number, d: number = b.x, b.y, b.z, b.distance_to_parent
	local r = math.sqrt(x * x + y * y + z * z)
	veach.log.info(
		string.format(
			"%s (around %s) at tick %d: %.9f AU from the sun, %.6g km from its parent",
			b.name,
			b.parent,
			b.tick,
			r / au,
			d / 1000
		)
	)
end)

veach.system("Sp04Paris", "PostSimulation", veach.query(c.SunObserver), function(_e, o)
	if veach.time.frame() ~= 2 then
		return
	end
	veach.log.info(
		string.format(
			"sun from %s at %.4f, %.4f: elevation %.3f deg, azimuth %.3f deg, local solar time %.3f h",
			o.body,
			o.latitude,
			o.longitude,
			o.elevation,
			o.azimuth,
			o.local_solar_time
		)
	)
end)
