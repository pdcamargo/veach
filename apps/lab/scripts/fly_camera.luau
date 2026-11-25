--!strict
-- A fly camera, written as a script (docs/specs/input/spec.md): W/A/S/D to move, Q/E down and up, the arrow
-- keys to turn and look, shift to go faster. The actions are bound in veach.project.json; this script never
-- sees a key.
-- Run: veach-lab --window --rhi metal --script scripts/fly_camera.luau
local veach = require("@veach")
local input = veach.input
local quat = veach.quat
local Transform = veach.components.Transform

local FlyCamera = veach.component("FlyCamera", {
	speed = veach.f32(2), -- meters per second
	turn_speed = veach.f32(1.5), -- radians per second
	yaw = veach.f32(0),
	pitch = veach.f32(0),
})

veach.spawn("triangle"):set(veach.components.TriangleDemo, { mode = "raster" })

local camera = veach.spawn("camera")
camera:set(Transform, { translation = vector.create(0, 0, 2.5), rotation = quat.identity(), scale = vector.one })
camera:set(veach.components.Camera, { fov_y = math.rad(60), near_plane = 0.1, active = true })
camera:set(FlyCamera, { speed = 2, turn_speed = 1.5, yaw = 0, pitch = 0 })

-- Two-component queries need the row types spelled out for the type checker.
type Fly = typeof(FlyCamera.defaults)
type Pose = typeof(Transform.defaults)

veach.system("FlyCamera", "Simulation", veach.query(FlyCamera, Transform), function(e: veach.Entity, fly: Fly, t: Pose)
	local dt = veach.time.delta()
	local yaw = fly.yaw + input.axis("turn_right", "turn_left") * fly.turn_speed * dt
	local pitch = math.clamp(fly.pitch + input.axis("look_down", "look_up") * fly.turn_speed * dt, -1.5, 1.5)
	local rotation = quat.from_euler(yaw, pitch)
	-- In camera space: +X right, +Y up, -Z forward.
	local move = vector.create(
		input.axis("move_left", "move_right"),
		input.axis("move_down", "move_up"),
		-input.axis("move_back", "move_forward")
	)
	local speed = fly.speed * (if input.down("sprint") then 4 else 1)
	local translation = t.translation + quat.rotate(rotation, move) * (speed * dt)
	e:set(Transform, { translation = translation, rotation = rotation, scale = t.scale })
	e:set(FlyCamera, { speed = fly.speed, turn_speed = fly.turn_speed, yaw = yaw, pitch = pitch })
end)
