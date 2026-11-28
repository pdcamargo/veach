--!strict
-- The Cornell box through the real-time renderer (docs/specs/realtime-renderer/spec.md, rt.AC3). The ceiling panel and
-- the glow material emit but do not light anything in real time (ADR 0040); the room is lit by its one point light,
-- unshadowed. Golden image: tests/golden/cornell_box_realtime.png (256x256).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/cornell_box_realtime.luau --screenshot out.png
local veach = require("@veach")

veach.spawn("cornell box"):set(veach.components.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(veach.components.Transform, {
	translation = veach.position(0, 1, 3.9),
	rotation = veach.quat.identity(),
	scale = vector.one,
})
camera:set(veach.components.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
-- A closed room lit by its own lights: no sky.
camera:set(veach.components.Sky, { mode = "none" })

-- Square, small: the golden runs on WARP too.
veach.system("CornellBoxSize", "Startup", veach.query(veach.components.Window), function(e, w)
	e:set(veach.components.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
