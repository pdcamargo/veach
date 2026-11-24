--!strict
-- Hello world: one script component, one system, one entity.
-- Run: veach-lab --script scripts/hello.luau --frames 3 --headless
local veach = require("@veach")

local Greeting = veach.component("Greeting", { text = "" })

veach.system("Greet", "Simulation", veach.query(Greeting), function(_entity, greeting)
	veach.log.info(`{greeting.text} — frame {veach.time.frame()}`)
end)

local greeter = veach.spawn("greeter")
greeter:set(Greeting, { text = "Hello from Luau" })
