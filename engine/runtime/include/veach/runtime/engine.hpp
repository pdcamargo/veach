#pragma once

// veach::Engine owns the process: config, the init order, the main loop and
// shutdown (ADR 0018, docs/specs/engine-lifecycle/spec.md). User code never
// creates a World, never calls progress() and never creates threads.
//
//   int main(int argc, char** argv) {
//       veach::Engine engine({.name = "my-game"}, argc, argv);
//       engine.import<MyGameModule>();
//       return engine.run();
//   }
//
// Init order:    core -> jobs -> platform -> ecs -> assets -> script -> rhi -> render -> modules
// Shutdown:      jobs drained, then the exact reverse, each step undone by RAII.

#include <veach/asset/asset_server.hpp>
#include <veach/core/error.hpp>
#include <veach/core/time.hpp>
#include <veach/ecs/module.hpp>
#include <veach/ecs/phases.hpp>
#include <veach/ecs/services.hpp>
#include <veach/runtime/config.hpp>
#include <veach/runtime/replay.hpp>

#include <veach/rhi/types.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veach {

class AssetServer;
class JobSystem;
class Platform;
class ScriptRuntime;
namespace rhi {
class Device;
}
namespace render {
class Renderer;
}

class Engine {
public:
    /// Parses argv (flags + project file). Errors are reported by run().
    Engine(AppInfo app, int argc, const char* const* argv);
    /// For tests and tools: an already-resolved config.
    explicit Engine(EngineConfig config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    /// Queues a module. Modules are imported, in call order, during the
    /// 'modules' init step, after every engine subsystem exists.
    template <Module T>
    Engine& import() {
        modules_.push_back([](World& world) { world.import<T>(); });
        return *this;
    }

    /// init() + start() + frames until quit or --frames + shutdown().
    /// Returns the process exit code: 0 if nothing was logged at error level.
    int run();

    // Finer-grained control, for tests and tools. run() is the normal entry point.
    [[nodiscard]] Result<> init();
    /// Loads the startup script and runs the Startup phase.
    [[nodiscard]] Result<> start();
    /// Runs one frame. Returns false once the app should stop.
    bool frame();
    /// While recording: an MCP world edit (set_component, spawn_entity, eval) to repeat before the next frame on
    /// replay (docs/specs/record-replay/spec.md). Does nothing otherwise.
    void record_edit(std::string tool, std::string arguments);
    /// Runs `count` frames (stops early if the app quits). Returns false once the app should stop.
    bool step(std::uint64_t count);
    /// Runs only PreExtract, Extract and Render: draws the current state without advancing simulation.
    void render_frame();
    /// Hot reload (docs/specs/hot-reload/spec.md): reloads `path`, or every asset whose file changed, between
    /// frames. Windowed runs call it themselves every 250 ms when the project's `hot_reload` is on; headless
    /// runs only through this call (MCP `reload`), so they stay deterministic.
    std::vector<ReloadResult> hot_reload(std::optional<std::string_view> path = std::nullopt);
    void shutdown();

    [[nodiscard]] World& world();
    [[nodiscard]] Services& services() { return services_; }
    [[nodiscard]] const EngineConfig& config() const { return config_; }
    [[nodiscard]] std::uint64_t frame_count() const { return frame_; }
    [[nodiscard]] std::uint64_t error_count() const { return errors_.load(); }

private:
    struct Step {
        std::string name;
        std::function<void()> teardown;
    };

    Result<> run_step(std::string name, const std::function<Result<std::string>()>& init,
                      std::function<void()> teardown);
    void run_phase(PhaseId phase, double delta);

    EngineConfig config_;
    std::optional<Error> config_error_;
    std::vector<std::function<void(World&)>> modules_;

    Services services_;
    std::unique_ptr<JobSystem> jobs_;  ///< Before everything that may run jobs, so it is destroyed after them
    std::unique_ptr<Platform> platform_;
    std::unique_ptr<World> world_;
    PhaseRunner phases_;
    std::unique_ptr<AssetServer> assets_;
    std::unique_ptr<ScriptRuntime> scripts_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<render::Renderer> renderer_;

    std::vector<Step> steps_;  ///< Completed init steps; unwound in reverse by shutdown().
    bool started_ = false;
    FixedTimestep fixed_;
    std::uint64_t frame_ = 0;
    std::uint64_t fixed_steps_ = 0;
    double last_frame_time_ = 0.0;
    double last_reload_poll_ = 0.0;
    std::atomic<std::uint64_t> errors_{0};  ///< GPU errors are logged from Metal's completion thread
    rhi::SurfaceId surface_;
    std::uint32_t error_sink_ = 0;
    std::optional<ReplayRecorder> recorder_;  ///< --record (docs/specs/record-replay/spec.md)
    std::vector<ReplayEdit> pending_edits_;   ///< MCP edits since the last recorded frame
    std::optional<ReplayLog> replay_;         ///< --replay
};

}  // namespace veach
