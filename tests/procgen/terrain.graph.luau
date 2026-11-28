--!strict
-- The terrain graph of terrain.field.json, built in Luau (procgen.AC10). Run by Sandbox::build_graph, which passes
-- the builder library as `procgen`; see docs/guides/procgen.md.

type Ref = { ref: string }
type Graph = {
	input: (self: Graph, name: string, kind: string) -> Ref,
	node: (self: Graph, id: string, node_type: string, args: { [string]: any }) -> Ref,
	output: (self: Graph, name: string, value: Ref) -> (),
}
type Procgen = {
	graph: (id: string) -> Graph,
	planet_m: (x: number) -> { planet_m: number },
	m: (x: number) -> { m: number },
}

return function(procgen: Procgen): Graph
	local g = procgen.graph("sp06:terrain")
	local p = g:input("p", "p_planet")
	local continents = g:node("continents", "veach:fbm@1", {
		p = p,
		wavelength = procgen.planet_m(3000000),
		octaves = 6,
		lacunarity = 2.0,
		gain = 0.5,
	})
	local mountains = g:node("mountains", "veach:fbm@1", {
		p = p,
		wavelength = procgen.planet_m(600000),
		octaves = 5,
		lacunarity = 2.0,
		gain = 0.5,
	})
	local ridges = g:node("ridges", "veach:ridge@1", { x = mountains, offset = 1.0 })
	local peaks = g:node("peaks", "veach:remap@1", { x = ridges, from = { 0.0, 1.0 }, to = { 0.35, 1.0 } })
	local land = g:node("land", "veach:remap@1", { x = continents, from = { -1.0, 1.0 }, to = { 0.0, 0.6 } })
	local mask_raw = g:node("mask_raw", "veach:remap@1", { x = continents, from = { 0.05, 0.3 }, to = { 0.0, 1.0 } })
	local mask = g:node("mask", "veach:clamp@1", { x = mask_raw, min = 0.0, max = 1.0 })
	g:output("height", g:node("height", "veach:blend@1", { a = land, b = peaks, t = mask }))
	return g
end
