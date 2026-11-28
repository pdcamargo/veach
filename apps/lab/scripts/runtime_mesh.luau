--!strict
-- A mesh made at run time, in photo mode (docs/specs/runtime-meshes/spec.md, rtmesh.AC4): the scene of
-- scripts/runtime_mesh_realtime.luau path traced, with the patch's BLAS built from the MeshStore's data. The relief
-- rises on frame 2, which restarts the accumulation with a new BLAS: frames 2 to 4 give 96 samples per pixel.
-- Golden image: tests/golden/runtime_mesh.png (256x256).
-- Run: veach-lab --rhi metal --headless --frames 4 --script scripts/runtime_mesh.luau --screenshot out.png
local veach = require("@veach")
local c = veach.components

local patch = veach.spawn("patch")
patch:set(c.Transform, { translation = vector.zero, rotation = veach.quat.identity(), scale = vector.one })
patch:set(c.RuntimeMeshDemo, { size = 8, cells = 64, height = 0, rock_above = 0.15 })

veach.system("RaiseRelief", "Simulation", veach.query(c.RuntimeMeshDemo), function(e, demo)
	if veach.time.frame() == 2 then
		e:set(c.RuntimeMeshDemo, { size = demo.size, cells = demo.cells, height = 0.6, rock_above = demo.rock_above })
	end
end)

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = vector.create(0, 3, 7.5),
	rotation = veach.quat.look_rotation(vector.create(0, -2.7, -7.5)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(50), near_plane = 0.1, active = true, ev100 = 14 })
camera:set(c.PhotoMode, { samples_per_frame = 32, max_bounces = 4 })
veach.spawn("sky"):set(c.Sky, { time_of_day = 18, latitude = 45, day_of_year = 172 })

veach.system("RuntimeMeshSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 256, height = 256, mode = w.mode, vsync = w.vsync })
end)
