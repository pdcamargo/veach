--!strict
-- First light, ray traced (compute shader + ray queries against a TLAS).
-- Golden image: tests/golden/triangle_raytraced.png
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/triangle_raytraced.luau --screenshot out.png
local veach = require("@veach")

veach.spawn("triangle"):set(veach.components.TriangleDemo, { mode = "raytraced" })
