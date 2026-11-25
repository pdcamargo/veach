--!strict
-- The lab's dev fly camera, run after any scene's startup script ("dev_scripts" in veach.project.json): W/A/S/D
-- to move, Q/E down and up, the arrow keys to turn and look, shift to go faster. It flies whichever camera is
-- active, and writes its Transform only while a key is held, so goldens, benchmarks and replays keep the
-- scene's own view. The actions are bound in veach.project.json; this script never sees a key.
local veach = require("@veach")
local input = veach.input
local quat = veach.quat
local c = veach.components

local SPEED = 2 -- meters per second; shift goes 4x
local TURN_SPEED = 1.5 -- radians per second

type Cam = typeof(c.Camera.defaults)
type Pose = typeof(c.Transform.defaults)

veach.system("DevFlyCamera", "Simulation", veach.query(c.Camera, c.Transform), function(e: veach.Entity, cam: Cam, t: Pose)
	if not cam.active then
		return
	end
	local turn = input.axis("turn_left", "turn_right")
	local look = input.axis("look_down", "look_up")
	-- In camera space: +X right, +Y up, -Z forward.
	local move = vector.create(
		input.axis("move_left", "move_right"),
		input.axis("move_down", "move_up"),
		-input.axis("move_back", "move_forward")
	)
	if turn == 0 and look == 0 and move == vector.zero then
		return
	end
	-- Yaw and pitch from where the camera looks now, as quat.from_euler builds them (a starting roll is dropped).
	local dt = veach.time.delta()
	local forward = quat.rotate(t.rotation, vector.create(0, 0, -1))
	local yaw = math.atan2(-forward.x, -forward.z) - turn * TURN_SPEED * dt
	local pitch = math.clamp(math.asin(math.clamp(forward.y, -1, 1)) + look * TURN_SPEED * dt, -1.5, 1.5)
	local rotation = quat.from_euler(yaw, pitch)
	local speed = SPEED * (if input.down("sprint") then 4 else 1)
	local translation = t.translation + quat.rotate(rotation, move) * (speed * dt)
	e:set(c.Transform, { translation = translation, rotation = rotation, scale = t.scale })
end)
