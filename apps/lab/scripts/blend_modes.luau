--!strict
-- The RHI's blend modes side by side: opaque, alpha, premultiplied, additive. Golden: tests/golden/blend_modes.png
-- Run: veach-lab --rhi metal --headless --frames 3 --script scripts/blend_modes.luau --screenshot out.png
local veach = require("@veach")

veach.spawn("blend modes"):set(veach.components.BlendDemo, { opacity = 0.5 })
