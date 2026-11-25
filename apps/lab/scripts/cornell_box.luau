--!strict
-- The Cornell box through the reference path tracer (docs/specs/first-real-scene/spec.md, scene.AC5).
-- Golden image: tests/golden/cornell_box.png (256x256, 64 samples per pixel: 4 frames x 16).
-- Run: veach-lab --rhi metal --headless --frames 4 --script scripts/cornell_box.luau --screenshot out.png
-- Or windowed, to watch it converge: veach-lab --rhi metal --script scripts/cornell_box.luau
local veach = require("@veach")

veach.spawn("cornell box"):set(veach.components.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(veach.components.Transform, {
	translation = vector.create(0, 1, 3.9),
	rotation = veach.quat.identity(),
	scale = vector.one,
})
camera:set(veach.components.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
camera:set(veach.components.PathTracer, { samples_per_frame = 16, max_bounces = 8 })
-- A closed room lit by its own lights: no sky.
camera:set(veach.components.Sky, { mode = "none" })

-- Square, small: the golden runs on WARP too.
veach.system("CornellBoxSize", "Startup", veach.query(veach.components.Window), function(e, w)
	e:set(veach.components.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
