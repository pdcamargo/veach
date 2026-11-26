--!strict
-- The lab's dev GI switcher, run after any scene's startup script ("dev_scripts" in veach.project.json): number keys
-- switch how the active camera is lit, to compare Candela with the path tracer on the same view.
--   1 GI off (the ambient stopgap, the default)    4 path traced, all light transport (8 bounces)
--   2 Candela                                      5 path traced, Candela's reference (direct + diffuse GI, mode 4)
--   3 Candela's cache alone (a diagnostic)         6 Candela's indirect light alone (linear, before the denoiser)
-- The window title names the mode. Nothing is written until a key is pressed, so goldens, benchmarks and replays
-- keep the scene's own settings. The actions are bound in veach.project.json.
local veach = require("@veach")
local input = veach.input
local c = veach.components

local MODES = {
	{ action = "gi_off", title = "GI off (ambient stopgap)" },
	{ action = "gi_candela", title = "Candela" },
	{ action = "gi_cache", title = "Candela: cache alone" },
	{ action = "gi_path_traced", title = "Path traced: full GI" },
	{ action = "gi_reference", title = "Path traced: Candela's reference (mode 4)" },
	{ action = "gi_indirect", title = "Candela: indirect light alone" },
}

local chosen: number? = nil

type Cam = typeof(c.Camera.defaults)
type Win = typeof(c.Window.defaults)

veach.system("DevGiModes", "Input", veach.query(c.Camera), function(e: veach.Entity, cam: Cam)
	if not cam.active then
		return
	end
	local pick: number? = nil
	for i, mode in MODES do
		if input.pressed(mode.action) then
			pick = i
		end
	end
	if pick == nil then
		return
	end
	chosen = pick
	if pick == 4 then
		e:set(c.PathTracer, { mode = 0, max_bounces = 8 })
	elseif pick == 5 then
		e:set(c.PathTracer, { mode = 4, max_bounces = 8 })
	else
		e:remove(c.PathTracer)
		if pick == 1 then
			e:set(c.RealtimeLighting, { gi = "off" })
		else
			e:set(c.RealtimeLighting, { gi = "candela", view = if pick == 6 then "indirect" else "final" })
			e:set(c.Candela, { gather = if pick == 3 then "surface_cache" else "restir" })
		end
	end
end)

veach.system("DevGiModesTitle", "Input", veach.query(c.Window), function(e: veach.Entity, w: Win)
	if chosen == nil then
		return
	end
	local title = "Veach Lab: " .. MODES[chosen].title
	if w.title ~= title then
		e:set(c.Window, { title = title, width = w.width, height = w.height, mode = w.mode, vsync = w.vsync })
	end
end)
