#pragma once

// The one header users include. Everything a game or tool needs lives in the
// veach:: namespace; raw Flecs is reachable as veach::ecs:: for advanced cases.
// Never write `flecs::` outside engine/ (CI enforces this for apps/).

#include <veach/core/assert.hpp>
#include <veach/core/error.hpp>
#include <veach/core/ids.hpp>
#include <veach/core/jobs.hpp>
#include <veach/core/log.hpp>
#include <veach/core/math.hpp>
#include <veach/core/profile.hpp>
#include <veach/core/rng.hpp>
#include <veach/core/suggest.hpp>
#include <veach/core/time.hpp>

#include <veach/ecs/command_buffer.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/flecs.hpp>
#include <veach/ecs/json.hpp>
#include <veach/ecs/module.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/reflection.hpp>
#include <veach/ecs/services.hpp>

#include <veach/platform/input.hpp>
#include <veach/platform/window.hpp>

#include <veach/asset/asset_server.hpp>
#include <veach/asset/handle.hpp>
#include <veach/asset/loader.hpp>

#include <veach/script/script_asset.hpp>
#include <veach/script/script_runtime.hpp>

#include <veach/render/mesh_store.hpp>
#include <veach/render/renderer.hpp>

#include <veach/runtime/config.hpp>
#include <veach/runtime/engine.hpp>
