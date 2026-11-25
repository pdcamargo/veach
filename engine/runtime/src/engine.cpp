#include <veach/runtime/engine.hpp>
#include <veach/runtime/mcp_server.hpp>

#include <veach/asset/asset_server.hpp>
#include <veach/core/log.hpp>
#include <veach/core/profile.hpp>
#include <veach/ecs/components.hpp>
#include <veach/ecs/reflection.hpp>
#include <veach/platform/clock.hpp>
#include <veach/platform/fs.hpp>
#include <veach/platform/platform.hpp>
#include <veach/platform/platform_module.hpp>
#include <veach/render/renderer.hpp>
#include <veach/rhi/rhi.hpp>
#include <veach/script/luau_types.hpp>
#include <veach/script/script_asset.hpp>
#include <veach/script/script_runtime.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <iostream>
#include <thread>

namespace veach {

namespace {

std::atomic<bool> interrupt_requested{false};

extern "C" void on_interrupt(int /*signal*/) {
    interrupt_requested.store(true);
}

constexpr const char* engine_version = "0.1.0";

/// Lets agents capture complete traces of short headless runs: start the app
/// with --wait-for-profiler, then start tracy-capture (docs/guides/profiling.md).
Result<> wait_for_profiler() {
#if VEACH_PROFILER_ENABLED
    log::info("engine", "waiting for a Tracy capture to connect on localhost (up to 30 s)");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!TracyIsConnected) {
        if (std::chrono::steady_clock::now() > deadline) {
            return make_error(ErrorCode::engine_init_failed, "no Tracy capture connected within 30 s",
                              "Start tracy-capture after the app, e.g. tracy-capture -o trace.tracy -f");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    log::info("engine", "Tracy connected");
    return {};
#else
    log::warn("engine", "--wait-for-profiler ignored: this build has no Tracy (use the 'profile' preset)");
    return {};
#endif
}

/// The Tracy server fetches zone names lazily; give it a moment before the
/// process exits so the capture is complete (TRACY_NO_EXIT is unreliable here).
void linger_for_profiler() {
#if VEACH_PROFILER_ENABLED
    if (TracyIsConnected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
#endif
}

}  // namespace

Engine::Engine(AppInfo app, int argc, const char* const* argv) {
    std::vector<std::string_view> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    auto resolved = resolve_config(app, args);
    if (resolved) {
        config_ = std::move(*resolved);
    } else {
        config_.app = std::move(app);
        config_error_ = std::move(resolved.error());
    }
}

Engine::Engine(EngineConfig config) : config_(std::move(config)) {}

Engine::~Engine() {
    shutdown();
}

World& Engine::world() {
    VEACH_ASSERT(world_ != nullptr, "the world exists only between init() and shutdown()");
    return *world_;
}

Result<> Engine::run_step(std::string name, const std::function<Result<std::string>()>& init,
                          std::function<void()> teardown) {
    VEACH_ZONE_TEXT("init step", name.data(), name.size());
    auto detail = init();
    if (!detail) {
        // Keep the cause's code in the message: it is the actionable one (and what tools match on).
        return make_error(ErrorCode::engine_init_failed,
                          std::format("init step '{}' failed: {} (cause: {})", name, detail.error().message,
                                      slug(detail.error().code)),
                          detail.error().hint);
    }
    log::info("engine", "init {}{}", name, detail->empty() ? "" : " (" + *detail + ")");
    steps_.push_back({std::move(name), std::move(teardown)});
    return {};
}

Result<> Engine::init() {
    VEACH_ZONE("Engine::init");
    if (config_error_) {
        return std::unexpected(*config_error_);
    }
    const ProjectConfig& project = config_.project;
    const auto started = std::chrono::steady_clock::now();

    auto step = run_step(
        "core",
        [&]() -> Result<std::string> {
            log::configure({.format = project.log_format,
                            .min_level = project.log_level,
                            .console = config_.log_to_console,
                            .console_stderr = config_.mcp});
            error_sink_ = log::add_sink([this](const log::Record& r) {
                if (r.level == log::Level::error) {
                    ++errors_;
                }
            });
            if (config_.wait_for_profiler) {
                if (auto connected = wait_for_profiler(); !connected) {
                    return std::unexpected(connected.error());
                }
            }
            return std::format("veach {}, {} logs, project '{}'", engine_version,
                               project.log_format == log::Format::json ? "json" : "human",
                               config_.project_dir.generic_string());
        },
        [this] { log::remove_sink(error_sink_); });
    if (!step) {
        return step;
    }

    step = run_step(
        "platform",
        [&]() -> Result<std::string> {
            auto platform = create_platform(project.headless ? PlatformBackend::headless : PlatformBackend::sdl);
            if (!platform) {
                return std::unexpected(platform.error());
            }
            platform_ = std::move(*platform);
            services_.provide<Platform>(*platform_);
            return std::string(platform_->name());
        },
        [this] {
            services_.revoke<Platform>();
            platform_.reset();
        });
    if (!step) {
        return step;
    }

    step = run_step(
        "ecs",
        [&]() -> Result<std::string> {
            world_ = std::make_unique<World>();
            attach_services(*world_, &services_);
            world_->import<CoreModule>();
            phases_.register_phases(*world_);
            world_->import<PlatformModule>();
            world_->set<Time>({});
            world_->set<Random>({.seed = project.seed, .rng = Rng(project.seed)});
            if (project.rest) {
                world_->import<flecs::stats>();
                world_->set<flecs::Rest>({});
                log::info("engine",
                          "Flecs REST API on http://localhost:27750 (explorer: https://www.flecs.dev/explorer)");
            }
            world_->entity("primary_window").set<Window>(project.window).add<PrimaryWindow>();
            for (const ActionBinding& binding : project.input) {
                bind_action(*world_, binding.action, binding.primary, binding.secondary);
            }
            sync_windows(*world_);
            return std::format("flecs {}.{}.{}, seed {}", FLECS_VERSION_MAJOR, FLECS_VERSION_MINOR, FLECS_VERSION_PATCH,
                               project.seed);
        },
        [this] { world_.reset(); });
    if (!step) {
        return step;
    }

    step = run_step(
        "assets",
        [&]() -> Result<std::string> {
            world_->import<AssetModule>();
            assets_ = std::make_unique<AssetServer>(*world_, config_.project_dir);
            services_.provide<AssetServer>(*assets_);
            return std::format("root '{}'", config_.project_dir.generic_string());
        },
        [this] {
            services_.revoke<AssetServer>();
            assets_.reset();
        });
    if (!step) {
        return step;
    }

    step = run_step(
        "script",
        [&]() -> Result<std::string> {
            scripts_ = std::make_unique<ScriptRuntime>(*world_);
            assets_->add_loader(std::make_unique<ScriptLoader>());
            services_.provide<ScriptRuntime>(*scripts_);
            // A reloaded script that already ran runs again as if fresh (docs/specs/hot-reload/spec.md).
            assets_->on_reloaded([this](AssetId, AssetTypeKey type, const void* data) -> Result<> {
                if (type != asset_type_key<ScriptAsset>() || !scripts_) {
                    return {};
                }
                auto reloaded = scripts_->reload(*static_cast<const ScriptAsset*>(data));
                if (!reloaded) {
                    log::error("script", reloaded.error());
                }
                return reloaded;
            });
            return std::string("luau");
        },
        [this] {
            services_.revoke<ScriptRuntime>();
            scripts_->shutdown();
            scripts_.reset();
        });
    if (!step) {
        return step;
    }

    step = run_step(
        "rhi",
        [&]() -> Result<std::string> {
#if defined(NDEBUG)
            constexpr bool validation = false;
#else
            constexpr bool validation = true;  // API validation errors become error logs (exit code 1)
#endif
            auto device = rhi::create_device(
                {.backend = project.rhi, .validation = validation, .frames_in_flight = project.frames_in_flight});
            if (!device) {
                return std::unexpected(device.error());
            }
            device_ = std::move(*device);
            services_.provide<rhi::Device>(*device_);
            const Entity window = world_->lookup("primary_window");
            const NativeWindowHandle handle = native_window_handle(*world_, window);
            if (handle.kind != NativeWindowHandle::Kind::none) {
                auto surface = device_->create_surface(handle, {handle.width, handle.height, project.window.vsync});
                if (!surface) {
                    return std::unexpected(surface.error());
                }
                surface_ = *surface;
                log::debug("engine", "platform produced a valid native window handle; RHI surface {} created",
                           surface->value);
            }
            const auto& caps = device_->capabilities();
            return std::format("{}, device '{}', hardware RT: {}", rhi::to_string(device_->backend()), caps.device_name,
                               caps.hardware_ray_tracing ? "yes" : "no");
        },
        [this] {
            services_.revoke<rhi::Device>();
            if (surface_.is_valid()) {
                device_->destroy_surface(surface_);
                surface_ = {};
            }
            device_.reset();
        });
    if (!step) {
        return step;
    }

    step = run_step(
        "render",
        [&]() -> Result<std::string> {
            renderer_ = std::make_unique<render::Renderer>(*device_);
            if (surface_.is_valid()) {
                const NativeWindowHandle handle = native_window_handle(*world_, world_->lookup("primary_window"));
                renderer_->set_surface(surface_, handle.width, handle.height);
            }
            services_.provide<render::Renderer>(*renderer_);
            world_->import<render::RenderModule>();
            return std::string("render graph; extraction is the only ECS crossing");
        },
        [this] {
            services_.revoke<render::Renderer>();
            renderer_.reset();
        });
    if (!step) {
        return step;
    }

    step = run_step(
        "modules",
        [&]() -> Result<std::string> {
            for (const auto& import_module : modules_) {
                import_module(*world_);
            }
            if (auto reflected = check_reflection(*world_); !reflected) {
                return std::unexpected(reflected.error());
            }
            return std::format("{} user module{}", modules_.size(), modules_.size() == 1 ? "" : "s");
        },
        [] {});
    if (!step) {
        return step;
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started);
    log::info("engine", "ready in {:.1f} ms", elapsed.count());
    return {};
}

Result<> Engine::start() {
    VEACH_ZONE("Engine::start");
    if (!config_.project.script.empty()) {
        const auto handle = assets_->load<ScriptAsset>(config_.project.script);
        auto script = assets_->wait(handle);
        if (!script) {
            return std::unexpected(script.error());
        }
        if (auto executed = scripts_->execute(**script); !executed) {
            return executed;
        }
        log::info("engine", "startup script '{}' loaded", config_.project.script);
    }
    run_phase(PhaseId::startup, 0.0);
    started_ = true;
    last_frame_time_ = clock::now_seconds();
    std::signal(SIGINT, on_interrupt);
    return {};
}

void Engine::run_phase(PhaseId phase, double delta) {
    world_->get_mut<Time>().delta = delta;
    phases_.run(*world_, phase, static_cast<float>(delta));
}

bool Engine::frame() {
    VEACH_ZONE("frame");
    const ProjectConfig& project = config_.project;
    const bool unpaced = project.headless && (project.frames > 0 || config_.mcp);

    double frame_delta = fixed_delta_seconds;
    std::uint32_t steps = 1;
    if (!project.headless) {
        const double now = clock::now_seconds();
        frame_delta = std::min(now - last_frame_time_, 0.25);
        last_frame_time_ = now;
        steps = fixed_.advance(frame_delta);
    }

    if (!project.headless && project.hot_reload) {
        const double now = clock::now_seconds();
        if (now - last_reload_poll_ >= 0.25) {
            last_reload_poll_ = now;
            static_cast<void>(hot_reload());
        }
    }

    ++frame_;
    world_->get_mut<Time>().frame = frame_;
    if (!config_.screenshot.empty() && frame_ == project.frames) {
        renderer_->request_screenshot(config_.screenshot);
    }
    world_->frame_begin(static_cast<float>(frame_delta));

    run_phase(PhaseId::input, frame_delta);
    for (std::uint32_t i = 0; i < steps; ++i) {
        VEACH_ZONE("fixed step");
        run_phase(PhaseId::simulation, fixed_delta_seconds);
        run_phase(PhaseId::post_simulation, fixed_delta_seconds);
        run_phase(PhaseId::transform_propagation, fixed_delta_seconds);
        ++fixed_steps_;
        Time& time = world_->get_mut<Time>();
        time.fixed_step = fixed_steps_;
        time.elapsed = static_cast<double>(fixed_steps_) * fixed_delta_seconds;
    }
    run_phase(PhaseId::pre_extract, frame_delta);
    run_phase(PhaseId::extract, frame_delta);
    run_phase(PhaseId::render, frame_delta);
    if (project.rest) {
        world_->run_pipeline(0, static_cast<float>(frame_delta));  // Flecs builtins: REST, stats
    }

    world_->frame_end();
    VEACH_FRAME_MARK();

    if (!unpaced) {
        // Nothing presents yet, so pace the loop ourselves instead of spinning a core.
        const double next = last_frame_time_ + fixed_delta_seconds;
        std::this_thread::sleep_for(std::chrono::duration<double>(std::max(0.0, next - clock::now_seconds())));
    }

    if (project.frames > 0 && frame_ >= project.frames) {
        return false;
    }
    if (interrupt_requested.load()) {
        log::info("engine", "interrupted (SIGINT)");
        return false;
    }
    return !world_->should_quit();
}

bool Engine::step(std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!frame()) {
            return false;
        }
    }
    return true;
}

std::vector<ReloadResult> Engine::hot_reload(std::optional<std::string_view> path) {
    VEACH_ZONE("hot reload");
    if (path) {
        return {assets_->reload(*path)};
    }
    return assets_->reload_changed();
}

void Engine::render_frame() {
    VEACH_ZONE("render frame");
    world_->frame_begin(static_cast<float>(fixed_delta_seconds));
    run_phase(PhaseId::pre_extract, fixed_delta_seconds);
    run_phase(PhaseId::extract, fixed_delta_seconds);
    run_phase(PhaseId::render, fixed_delta_seconds);
    world_->frame_end();
}

void Engine::shutdown() {
    if (steps_.empty()) {
        return;
    }
    VEACH_ZONE("Engine::shutdown");
    if (started_) {
        run_phase(PhaseId::shutdown, 0.0);
        started_ = false;
        log::info("engine", "stopped after {} frame{} ({} fixed steps)", frame_, frame_ == 1 ? "" : "s", fixed_steps_);
    }
    while (!steps_.empty()) {
        Step step = std::move(steps_.back());
        steps_.pop_back();
        log::info("engine", "shutdown {}", step.name);
        step.teardown();
    }
    std::signal(SIGINT, SIG_DFL);
    if (config_.wait_for_profiler) {
        linger_for_profiler();
    }
}

int Engine::run() {
    if (config_.show_help) {
        std::fputs(usage(config_.app).c_str(), stdout);
        return 0;
    }
    if (auto initialized = init(); !initialized) {
        log::error("engine", initialized.error());
        shutdown();
        return 1;
    }
    if (!config_.write_types.empty()) {
        const std::string types = generate_luau_types(*world_);
        auto written = fs::write_file(config_.write_types, types);
        if (!written) {
            log::error("engine", written.error());
        } else {
            log::info("engine", "wrote Luau types to {}", config_.write_types.generic_string());
        }
        shutdown();
        return written ? 0 : 1;
    }
    if (auto started = start(); !started) {
        log::error("engine", started.error());
        shutdown();
        return 1;
    }
    if (config_.mcp) {
        McpServer server(*this);
        server.run(std::cin, std::cout);
    } else {
        while (frame()) {
        }
    }
    const std::uint64_t errors = errors_.load();
    shutdown();
    if (errors > 0) {
        std::fprintf(stdout, "%s: %llu error%s logged; exit code 1\n", config_.app.name.c_str(),
                     static_cast<unsigned long long>(errors), errors == 1 ? "" : "s");
        return 1;
    }
    return 0;
}

}  // namespace veach
