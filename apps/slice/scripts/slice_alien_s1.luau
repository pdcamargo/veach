--!strict
-- The first slice at s = 1 with an alien-looking home (docs/specs/terrain-materials/spec.md): the same seed and
-- flight as slice_s1.luau, with the creator asking for the opt-in procedural albedo. The ground keeps the scans'
-- detail (normal, roughness, AO, height) and takes its colors from the planet's palette (its star, its rock).
local veach = require("@veach")
local c = veach.components

veach.spawn("universe"):set(c.Universe, {
	seed = "42",
	system = "S:1000400100008000",
	scale = 1,
	start_tick = 0,
	surface_colors = "palette",
})
