--!strict
-- The first-light triangle through a perspective camera (docs/specs/render-views/spec.md, views.AC3).
-- Golden image: tests/golden/triangle_camera.png, shared by the raster and ray-traced runs.
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/triangle_camera_raytraced.luau --screenshot out.png
local veach = require("@veach")

veach.spawn("triangle"):set(veach.components.TriangleDemo, { mode = "raytraced" })

-- 1.5 m right, 0.05 m down and 2.6 m back, turned 30° left (about +Y) to face the triangle at the origin.
local camera = veach.spawn("camera")
camera:set(veach.components.Transform, {
	translation = vector.create(1.5, -0.05, 2.6),
	rotation = { x = 0, y = 0.258819, z = 0, w = 0.965926 },
	scale = vector.create(1, 1, 1),
})
camera:set(veach.components.Camera, { fov_y = math.rad(50), near_plane = 0.1, active = true })
