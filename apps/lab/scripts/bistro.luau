--!strict
-- Amazon Lumberyard Bistro, exterior (CC BY 4.0, apps/lab/assets/bistro): the terrace from the street at eye level, lit by the
-- scene's own sun (102,450 lux). 2560x1440, the real-time renderer's benchmark size
-- (docs/specs/realtime-renderer-v0/spec.md).
-- Run: veach-lab --rhi metal --window --script scripts/bistro.luau
local veach = require("@veach")
local c = veach.components

veach.spawn("bistro"):set(c.SceneInstance, { path = "assets/bistro/BistroExterior.gltf" })

local camera = veach.spawn("camera")
camera:set(c.Transform, {
	translation = vector.create(-3, 2.5, -8),
	rotation = veach.quat.look_rotation(vector.create(-10, -0.8, -21)),
	scale = vector.one,
})
camera:set(c.Camera, { fov_y = math.rad(55), near_plane = 0.05, active = true, ev100 = 14 })

veach.system("BistroSize", "Startup", veach.query(c.Window), function(e, w)
	e:set(c.Window, { title = w.title, width = 2560, height = 1440, mode = w.mode, vsync = w.vsync })
end)
