--!strict
-- Spike SP-09 (VCH-085), THROWAWAY: a governed descent from orbit onto a streamed chunk-octree planet
-- (docs/research/spikes/sp-09-descent.md). The lab module (apps/lab/src/sp09_spike.hpp) flies the camera in double
-- precision, streams and meshes the chunks and draws them relative to the camera; this script only sets the scene.
-- The environment picks the case:
--   VEACH_SP09_SCALE=0.01 VEACH_SP09_OUT=build/sp09 VEACH_SP09_PREFIX=s001 \
--     veach-lab --rhi metal --headless --frames 20000 --script scripts/sp09_descent.luau
-- (VEACH_SP09_BODY=asteroid, VEACH_SP09_RELIEF=0|0.5|1, VEACH_SP09_COLORS=level, VEACH_SP09_BUDGET=N; the run quits
-- by itself after the last skim.)
local veach = require("@veach")
local c = veach.components

local camera = veach.spawn("camera")
camera:set(c.Transform, { translation = veach.position(0, 0, 0), rotation = veach.quat.identity(), scale = vector.one })
camera:set(c.Camera, { fov_y = math.rad(60), near_plane = 0.5, active = true, ev100 = 15 })
camera:set(c.Sp09Descent, {})

-- Space: a dim constant sky, so faces away from the sun are not black. The module spawns the sun.
veach.spawn("sky"):set(c.Sky, { mode = "constant", color = vector.create(1500, 1800, 2400) })

veach.system("Sp09Size", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 960, height = 540, mode = w.mode, vsync = w.vsync })
end)
