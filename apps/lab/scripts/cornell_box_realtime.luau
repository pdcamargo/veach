--!strict
-- The Cornell box through the real-time renderer (docs/specs/realtime-renderer-v0/spec.md, rt.AC2).
-- Golden image: tests/golden/cornell_box_realtime.png (256x256, 4 emissive light samples per pixel).
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/cornell_box_realtime.luau --screenshot out.png
local veach = require("@veach")

veach.spawn("cornell box"):set(veach.components.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })

local camera = veach.spawn("camera")
camera:set(veach.components.Transform, {
	translation = vector.create(0, 1, 3.9),
	rotation = veach.quat.identity(),
	scale = vector.one,
})
camera:set(veach.components.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })

-- Square, small: the golden runs on WARP too.
veach.system("CornellBoxSize", "Startup", veach.query(veach.components.Window), function(e, w)
	e:set(veach.components.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
