--!strict
-- The lab's dev fly camera, run after any scene's startup script ("dev_scripts" in veach.project.json).
-- Keyboard: W/A/S/D to move, Q/E down and up, the arrow keys to turn and look, shift to go faster.
-- Mouse: hold the right button to look around (the cursor is captured while it is held).
-- Gamepad: left stick to move, right stick to turn and look, shoulders down and up, right trigger to go faster.
-- It flies whichever camera is active, and writes only while there is input, so goldens, benchmarks and replays
-- keep the scene's own view. Actions and analog axes are bound in veach.project.json; this script never sees a
-- key, a button or a stick.
local veach = require("@veach")
local input = veach.input
local quat = veach.quat
local c = veach.components

local SPEED = 2 -- meters per second; sprint or a full trigger goes 4x
local TURN_SPEED = 1.5 -- radians per second, for the arrow keys and the right stick

type Cam = typeof(c.Camera.defaults)
type Pose = typeof(c.Transform.defaults)
type Win = typeof(c.Window.defaults)

-- Mouse look captures the cursor (relative mouse mode) while the right button is held.
veach.system("DevFlyCameraMouse", "Input", veach.query(c.Window), function(e: veach.Entity, _w: Win)
	if input.pressed("mouse_look") then
		e:set(c.RelativeMouse, { enabled = true })
	elseif input.released("mouse_look") then
		e:remove(c.RelativeMouse)
	end
end)

veach.system("DevFlyCamera", "Simulation", veach.query(c.Camera, c.Transform), function(e: veach.Entity, cam: Cam, t: Pose)
	if not cam.active then
		return
	end
	-- Rates in -1..1 (keys or the right stick), and the mouse's motion since the last step, already in radians.
	local turn = input.axis("turn")
	local look = input.axis("look")
	local mouse_yaw, mouse_pitch = 0, 0
	if input.down("mouse_look") then
		mouse_yaw, mouse_pitch = input.axis("look_x"), input.axis("look_y")
	end
	-- In camera space: +X right, +Y up, -Z forward.
	local move = vector.create(input.axis("move_x"), input.axis("move_down", "move_up"), -input.axis("move_z"))
	if turn == 0 and look == 0 and mouse_yaw == 0 and mouse_pitch == 0 and move == vector.zero then
		return
	end
	-- Yaw and pitch from where the camera looks now, as quat.from_euler builds them (a starting roll is dropped).
	local dt = veach.time.delta()
	local forward = quat.rotate(t.rotation, vector.create(0, 0, -1))
	local yaw = math.atan2(-forward.x, -forward.z) - turn * TURN_SPEED * dt - mouse_yaw
	local pitch = math.asin(math.clamp(forward.y, -1, 1)) + look * TURN_SPEED * dt + mouse_pitch
	local rotation = quat.from_euler(yaw, math.clamp(pitch, -1.5, 1.5))
	local boost = math.max(if input.down("sprint") then 1 else 0, input.axis("boost"))
	local speed = SPEED * (1 + 3 * boost)
	local translation = t.translation + quat.rotate(rotation, move) * (speed * dt)
	e:set(c.Transform, { translation = translation, rotation = rotation, scale = t.scale })
end)
