--!strict
-- The first-light triangle in front of a camera at the origin's +Z, looking down -Z: the scene input.AC5 and the
-- record/replay tests fly with the lab's dev fly camera (scripts/dev/fly_camera.luau, docs/specs/input/spec.md).
-- Run: veach-lab --window --rhi metal --script scripts/triangle_fly.luau
local veach = require("@veach")

veach.spawn("triangle"):set(veach.components.TriangleDemo, { mode = "raster" })

local camera = veach.spawn("camera")
camera:set(veach.components.Transform, { translation = vector.create(0, 0, 2.5), rotation = veach.quat.identity(), scale = vector.one })
camera:set(veach.components.Camera, { fov_y = math.rad(60), near_plane = 0.1, active = true })
