--!strict
-- Spike SP-03 (VCH-079), THROWAWAY: the precision ladder (docs/research/spikes/sp-03-precision-frames.md).
-- The Cornell box and a camera 3.9 m from it, both placed `distance` meters from the world origin, either the
-- current way (float32 Transform) or with a double Sp03LargePosition that the lab's spike extractor makes
-- camera-relative. The case comes from the environment (the lab module spawns Sp03Ladder from it):
--   VEACH_SP03_DISTANCE=1.496e11 VEACH_SP03_MODE=double \
--     veach-lab --rhi metal --headless --frames 3 --script scripts/sp03_ladder.luau --screenshot out.png
-- VEACH_SP03_PHOTO=1 renders it in photo mode (--frames 4). Without VEACH_SP03_DISTANCE nothing is spawned.
local veach = require("@veach")
local c = veach.components

-- Away from the origin along a direction with all three axes large, so every coordinate loses precision.
local direction = { x = 0.6, y = 0.48, z = 0.64 }
local eye = { x = 0, y = 1, z = 3.9 } -- the camera, relative to the box (cornell_box_realtime.luau's view)

veach.system("Sp03Place", "Startup", veach.query(c.Sp03Ladder), function(_e, ladder)
	local d: number = ladder.distance
	local px, py, pz = direction.x * d, direction.y * d, direction.z * d
	local box = veach.spawn("cornell box")
	box:set(c.SceneInstance, { path = "assets/cornell_box/cornell_box.gltf" })
	local camera = veach.spawn("camera")
	if ladder.large then
		-- Doubles: the large offsets live in Sp03LargePosition; the float32 Transforms stay at the origin.
		box:set(c.Sp03LargePosition, { x = px, y = py, z = pz })
		camera:set(c.Sp03LargePosition, { x = px + eye.x, y = py + eye.y, z = pz + eye.z })
		camera:set(c.Transform, { translation = vector.zero, rotation = veach.quat.identity(), scale = vector.one })
	else
		-- Today: float32 translations (the nearest float32 to each true position).
		box:set(c.Transform, {
			translation = vector.create(px, py, pz),
			rotation = veach.quat.identity(),
			scale = vector.one,
		})
		camera:set(c.Transform, {
			translation = vector.create(px + eye.x, py + eye.y, pz + eye.z),
			rotation = veach.quat.identity(),
			scale = vector.one,
		})
	end
	camera:set(c.Camera, { fov_y = math.rad(40), near_plane = 0.1, active = true, ev100 = 0 })
	camera:set(c.Sky, { mode = "none" })
	if ladder.photo then
		camera:set(c.PhotoMode, { samples_per_frame = 16, max_bounces = 8 })
	end
end)

veach.system("Sp03Size", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
