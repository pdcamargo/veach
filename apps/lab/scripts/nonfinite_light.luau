--!strict
-- A deliberately broken light: its intensity is NaN, so every sample it lights is NaN. The path tracer must leave
-- them out and fail the run with render.nonfinite_samples (tests/CMakeLists.txt, "lab: NaN samples are errors").
local veach = require("@veach")
local c = veach.components

veach.spawn("cornell box"):set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, { translation = vector.create(0, 1, 3.9), rotation = veach.quat.identity(), scale = vector.one })
camera:set(c.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(c.PathTracer, { samples_per_frame = 4, max_bounces = 2 })
camera:set(c.Sky, { mode = "none" })

local nan = 0 / 0
local broken = veach.spawn("broken light")
broken:set(c.Transform, { translation = vector.create(0, 1.5, 0), rotation = veach.quat.identity(), scale = vector.one })
broken:set(c.PointLight, { color = vector.one, intensity = nan })

veach.system("NonfiniteLightSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 64, height = 64, mode = w.mode, vsync = w.vsync })
end)
