--!strict
-- First light, rasterized. Golden image: tests/golden/triangle_raster.png
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/triangle_raster.luau --screenshot out.png
local veach = require("@veach")

veach.spawn("triangle"):set(veach.components.TriangleDemo, { mode = "raster" })
