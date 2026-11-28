--!strict
-- The first slice at s = 0.1 (docs/specs/first-slice/spec.md). The scale is the only difference between this script
-- and its siblings (slice_s0.01.luau, slice.luau at the default 0.1, slice_s1.luau): the same code realizes the same
-- seed at another size (ADR 0049). The universe's settings are genesis: set once, before the first step.
local veach = require("@veach")
local c = veach.components

veach.spawn("universe"):set(c.Universe, {
	seed = "42",
	system = "S:1000400100008000",
	scale = 0.1,
	start_tick = 0,
})
