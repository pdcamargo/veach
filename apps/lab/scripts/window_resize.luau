--!strict
-- Windowed smoke test (dx12-backend spec, dx12.AC8): the raster triangle in a window that is resized on
-- frame 10, so the swapchain is recreated mid-run.
-- Run: veach-lab --window --rhi dx12 --frames 30 --script scripts/window_resize.luau
local veach = require("@veach")
local Window = veach.components.Window

veach.spawn("triangle"):set(veach.components.TriangleDemo, { mode = "raster" })

veach.system("ResizeOnce", "Input", veach.query(Window), function(e, w)
	if veach.time.frame() == 10 then
		e:set(Window, { title = w.title, width = 960, height = 540, mode = w.mode, vsync = w.vsync })
		veach.log.info(`resizing the window to 960x540`)
	end
end)
