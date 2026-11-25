--!strict
-- Explicit field types (scripting spec, script.AC9). Type-checked by luau-analyze
-- in CTest and executed by tests/script_tests.cpp.
local veach = require("@veach")

local Health = veach.component("Health", {
	hp = veach.f32(100),
	level = veach.i32(1),
	armor = veach.u32(),
	precise = veach.f64(0.5),
	offset = veach.vec3(vector.create(0, 1, 0)),
	target = veach.entity(),
	nickname = veach.optional(""),
	shield = veach.optional(veach.f32(10)),
	alive = true,
})

local a = veach.spawn("a")
local b = veach.spawn("b")
a:set(Health, {
	hp = 75.5,
	level = 3,
	armor = 7,
	precise = 0.25,
	offset = vector.create(1, 2, 3),
	target = b,
	nickname = "ace",
	shield = nil,
	alive = true,
})
b:set(Health, Health.defaults)

veach.system("Report", "Simulation", veach.query(Health), function(_e, h)
	local target = if h.target then "target" else "none"
	veach.log.info(
		`frame {veach.time.frame()} hp={h.hp} level={h.level} armor={h.armor} precise={h.precise} y={h.offset.y} {target} nick={h.nickname or "-"} shield={h.shield or -1}`
	)
end)

-- On frame 1, the entity without a nickname despawns itself; a's reference to it then reads as nil.
veach.system("DespawnB", "PostSimulation", veach.query(Health), function(e, h)
	if veach.time.frame() == 1 and h.nickname == nil then
		e:despawn()
	end
end)
