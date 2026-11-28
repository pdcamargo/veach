--!strict
-- Spike SP-07 (VCH-083), throwaway: a climate and biome generator hook for the Luau procgen sandbox (ADR 0052).
-- Run by tests/procgen_sandbox_tests.cpp and tests/bench/sp07_luau_generator_bench.cpp on a 64 x 64 x 6 cube map;
-- type-checked by luau-analyze in CTest. docs/research/spikes/sp-07-luau-generator.md.
--
-- Per cell: annual-mean temperature (K) from the latitude's insolation under the body's axial tilt, its equilibrium
-- temperature and a lapse rate; precipitation (mm/yr) from circulation bands, Clausius-Clapeyron and relief; and a
-- Whittaker-style biome id. `math` here is veach::dmath, and `^` is not allowed (it is the platform's libm pow).

type Context = {
	params: { [string]: number },
	random: (...number) -> number,
}
type Buffers = { [string]: buffer }

local PI = math.pi
local SEASONS = 8
local LAPSE_K_PER_M = 0.0065
local SEA_ICE_K = 271.35

-- Daily-mean insolation at latitude `lat` and solar declination `dec`, as a fraction of the stellar flux.
local function daily_insolation(lat: number, dec: number): number
	local c = -math.tan(lat) * math.tan(dec)
	local h0: number
	if c >= 1 then
		return 0 -- polar night
	elseif c <= -1 then
		h0 = PI -- polar day
	else
		h0 = math.acos(c)
	end
	return (h0 * math.sin(lat) * math.sin(dec) + math.cos(lat) * math.cos(dec) * math.sin(h0)) / PI
end

-- Annual mean over a circular orbit, sampled at SEASONS points.
local function annual_insolation(lat: number, tilt: number): number
	local sum = 0
	for k = 0, SEASONS - 1 do
		local dec = math.asin(math.sin(tilt) * math.sin(2 * PI * k / SEASONS))
		sum += daily_insolation(lat, dec)
	end
	return sum / SEASONS
end

local function band(x: number, center: number, width: number): number
	local d = (x - center) / width
	return math.exp(-d * d)
end

local function biome(temperature: number, precipitation: number, altitude: number): number
	if altitude < 0 then
		return if temperature <= SEA_ICE_K then 1 else 0 -- sea ice, ocean
	end
	if altitude > 4500 then
		return 12 -- alpine
	end
	if temperature < 263 then
		return 2 -- ice sheet
	elseif temperature < 271 then
		return 3 -- tundra
	elseif temperature < 278 then
		return 4 -- boreal forest
	elseif temperature < 293 then
		if precipitation < 500 then
			return 5 -- temperate grassland
		elseif precipitation < 1500 then
			return 6 -- temperate forest
		end
		return 7 -- temperate rainforest
	end
	if precipitation < 250 then
		return 8 -- desert
	elseif precipitation < 1200 then
		return 9 -- savanna
	elseif precipitation < 2200 then
		return 10 -- tropical seasonal forest
	end
	return 11 -- tropical rainforest
end

return function(ctx: Context, inputs: Buffers, outputs: Buffers)
	local cells = ctx.params.cells
	local t_eq = ctx.params.equilibrium_temperature_k
	local tilt = ctx.params.axial_tilt_rad
	local greenhouse = ctx.params.greenhouse_k
	local latitude = inputs.latitude
	local altitude = inputs.altitude
	local out_t = outputs.temperature
	local out_p = outputs.precipitation
	local out_b = outputs.biome

	for i = 0, cells - 1 do
		local lat = buffer.readf64(latitude, i * 8)
		local alt = buffer.readf64(altitude, i * 8)
		local alat = math.abs(lat)

		-- Radiative balance per latitude, mixed toward the global mean (a stand-in for heat transport).
		local q = 4 * annual_insolation(lat, tilt)
		local t = t_eq * (0.55 * math.pow(q, 0.25) + 0.45) + greenhouse - LAPSE_K_PER_M * math.max(alt, 0)

		-- Hadley, Ferrel and polar cells: wet ITCZ, dry subtropics, wet mid-latitude storm tracks.
		local circulation = 2400 * band(lat, 0, 0.14) + 900 * band(alat, 0.95, 0.2) + 450 - 380 * band(alat, 0.52, 0.12)
		local humidity = math.clamp(math.exp(0.07 * (t - 288)), 0.05, 3)
		local relief = if alt > 0 then 1 + 0.25 * math.min(alt, 3000) / 1000 - 0.3 * math.max(alt - 3000, 0) / 1000 else 1
		local jitter = 0.85 + 0.3 * ctx.random(1, i)
		local p = math.max(circulation * humidity * math.max(relief, 0.1) * jitter, 0)

		buffer.writef64(out_t, i * 8, t)
		buffer.writef64(out_p, i * 8, p)
		buffer.writeu8(out_b, i, biome(t, p, alt))
	end
end
