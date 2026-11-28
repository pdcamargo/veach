#pragma once

// Render graph v0 (ADR 0029). Passes declare what they read and write; the
// graph derives the execution order, culls passes whose results nobody uses,
// emits the barriers, and declares residency. Nothing else issues barriers.
//
//   RenderGraph graph(device);
//   auto color = graph.import(scene_color, "scene_color", ResourceState::undefined);
//   graph.add_graphics_pass("triangle",
//       [&](PassBuilder& b) { b.color_target(color, rhi::LoadOp::clear); b.read(vertices); },
//       [&](PassContext& ctx) { ctx.cmd.set_pipeline(pipeline); ctx.cmd.draw(3); });
//   graph.compile();   // order, culling, barriers
//   graph.execute(cmd);

#include <veach/core/error.hpp>
#include <veach/rhi/rhi.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace veach::render {

/// A resource inside one graph (index into the graph's resource table).
struct GraphResource {
    std::uint32_t id = UINT32_MAX;
    [[nodiscard]] constexpr bool is_valid() const { return id != UINT32_MAX; }
};

enum class PassKind : std::uint8_t {
    graphics,  ///< draws inside a render pass (targets declared with color_target and depth_target)
    compute,   ///< dispatches inside a compute pass
    transfer,  ///< copies and acceleration-structure builds/refits
};

class PassBuilder {
public:
    void read(GraphResource resource, rhi::ResourceState state = rhi::ResourceState::shader_read);
    void write(GraphResource resource, rhi::ResourceState state = rhi::ResourceState::shader_write);
    /// Graphics passes only. `slice` picks the mip and layer drawn into (an array layer, a cube face, a 3D slice);
    /// the graph's barriers cover the whole texture.
    void color_target(GraphResource resource, rhi::LoadOp load = rhi::LoadOp::load,
                      std::array<float, 4> clear_color = {0, 0, 0, 1}, rhi::TargetSlice slice = {});
    /// An integer color target (rhi::is_integer: rg32_uint, r32_uint), cleared to `clear_value` exactly.
    void color_target_uint(GraphResource resource, rhi::LoadOp load = rhi::LoadOp::load,
                           std::array<std::uint32_t, 4> clear_value = {}, rhi::TargetSlice slice = {});
    /// The depth attachment (one per pass), written in depth_write. Reverse-Z: the far plane clears to 0. A shadow
    /// cascade draws into one layer of a d2_array with `slice`.
    void depth_target(GraphResource resource, rhi::LoadOp load = rhi::LoadOp::load, float clear_depth = 0.0f,
                      rhi::TargetSlice slice = {});
    /// Spike SP-16 (VCH-092), Metal only: at the end of the pass, the multisampled color or depth target
    /// `multisampled` (declared first) is resolved into the single-sampled `target`, which the pass writes.
    void resolve(GraphResource multisampled, GraphResource target);
    /// Keep this pass even if nothing reads what it writes (e.g. debug output).
    void side_effect();

private:
    friend class RenderGraph;
    explicit PassBuilder(class RenderGraph& graph, std::uint32_t pass) : graph_(graph), pass_(pass) {}
    class RenderGraph& graph_;
    std::uint32_t pass_;
};

class PassContext {
public:
    rhi::CommandList& cmd;
    rhi::Device& device;

    [[nodiscard]] rhi::TextureHandle texture(GraphResource resource) const;
    [[nodiscard]] rhi::BufferHandle buffer(GraphResource resource) const;
    [[nodiscard]] rhi::AccelHandle accel(GraphResource resource) const;
    /// The mips a resource made by RenderGraph::mips() covers (the whole texture for any other).
    [[nodiscard]] rhi::TextureRange range(GraphResource resource) const;

private:
    friend class RenderGraph;
    PassContext(rhi::CommandList& c, rhi::Device& d, const class RenderGraph& g) : cmd(c), device(d), graph_(g) {}
    const class RenderGraph& graph_;
};

/// Keeps transient textures alive across frames, reused by description.
class TransientPool {
public:
    explicit TransientPool(rhi::Device& device) : device_(device) {}
    ~TransientPool();
    TransientPool(const TransientPool&) = delete;
    TransientPool& operator=(const TransientPool&) = delete;
    TransientPool(TransientPool&&) = delete;
    TransientPool& operator=(TransientPool&&) = delete;

    [[nodiscard]] Result<rhi::TextureHandle> acquire(const rhi::TextureDesc& desc);
    void release_all();  ///< end of frame: everything becomes reusable
    /// State a pooled texture was left in by the last frame that used it (synchronization across frames).
    [[nodiscard]] rhi::ResourceState state(rhi::TextureHandle texture) const;
    void set_state(rhi::TextureHandle texture, rhi::ResourceState state);

private:
    struct Entry {
        rhi::TextureDesc desc;
        rhi::TextureHandle texture;
        bool in_use = false;
        rhi::ResourceState state = rhi::ResourceState::undefined;
    };
    rhi::Device& device_;
    std::vector<Entry> entries_;
};

class RenderGraph {
public:
    using Setup = std::function<void(PassBuilder&)>;
    using Execute = std::function<void(PassContext&)>;

    explicit RenderGraph(rhi::Device& device, TransientPool* pool = nullptr) : device_(device), pool_(pool) {}

    /// Brings an existing resource into the graph. `current` is the state it is in
    /// now; if `final_state` is set the graph transitions it there at the end.
    /// Imported resources are outputs: passes writing them are never culled.
    GraphResource import(rhi::TextureHandle texture, std::string name, rhi::ResourceState current,
                         std::optional<rhi::ResourceState> final_state = std::nullopt);
    GraphResource import(rhi::BufferHandle buffer, std::string name, rhi::ResourceState current,
                         std::optional<rhi::ResourceState> final_state = std::nullopt);
    GraphResource import(rhi::AccelHandle accel, std::string name, rhi::ResourceState current,
                         std::optional<rhi::ResourceState> final_state = std::nullopt);
    /// A texture that lives only within this frame (needs a TransientPool).
    GraphResource create_texture(const rhi::TextureDesc& desc);
    /// The description of a texture in the graph (imported or transient), or null for buffers and invalid ids.
    [[nodiscard]] const rhi::TextureDesc* texture_desc(GraphResource resource) const;
    /// Mips [first_mip, first_mip + mip_count) of `texture` (imported or transient) as a resource of their own: the
    /// graph keeps a state per mip, so one pass can read a mip while it writes the next (a HiZ pyramid, a mip chain),
    /// and orders, culls and barriers by the mips each pass touches. A pass may not touch a mip twice, through the
    /// texture or any of its views. Shaders reach the mips through Device::descriptor(texture, view,
    /// PassContext::range(view)). At the end of the graph a texture whose mips were left in different states moves
    /// to its final state, or to mip 0's.
    GraphResource mips(GraphResource texture, std::uint32_t first_mip, std::uint32_t mip_count = 1);

    void add_graphics_pass(std::string name, const Setup& setup, Execute execute);
    void add_compute_pass(std::string name, const Setup& setup, Execute execute);
    void add_transfer_pass(std::string name, const Setup& setup, Execute execute);

    /// Orders and culls passes, allocates transients, computes barriers.
    [[nodiscard]] Result<> compile();
    void execute(rhi::CommandList& cmd);

    /// The compiled plan as text: order, culled passes, barriers per pass.
    [[nodiscard]] std::string describe() const;
    /// State of a resource after execute() (to carry imported resources' state into the next frame).
    [[nodiscard]] rhi::ResourceState final_state(GraphResource resource) const;
    [[nodiscard]] std::vector<std::string> execution_order() const;

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Resource {
        std::string name;
        rhi::ResourceRef ref;
        rhi::ResourceState initial = rhi::ResourceState::undefined;
        std::optional<rhi::ResourceState> final_state;
        rhi::ResourceState end_state = rhi::ResourceState::undefined;
        bool imported = false;
        std::optional<rhi::TextureDesc> transient;
        std::uint32_t root = UINT32_MAX;  ///< the texture a mips() view belongs to; UINT32_MAX for a whole resource
        std::uint32_t first_mip = 0;
        std::uint32_t mip_count = 0;  ///< 0: every mip
    };
    struct Access {
        GraphResource resource;
        rhi::ResourceState state;
    };
    struct Pass {
        std::string name;
        PassKind kind;
        std::vector<Access> accesses;
        std::array<rhi::ColorAttachment, 4> colors{};
        std::array<GraphResource, 4> color_resources{};
        std::array<GraphResource, 4> resolve_resources{};
        std::uint32_t color_count = 0;
        rhi::DepthAttachment depth{};
        GraphResource depth_resource{};
        GraphResource depth_resolve_resource{};
        bool side_effect = false;
        Execute execute;
        std::vector<rhi::Barrier> barriers;  ///< computed by compile()
    };

    void add_pass(std::string name, PassKind kind, const Setup& setup, Execute execute);
    [[nodiscard]] std::uint32_t root_of(std::uint32_t resource) const;
    [[nodiscard]] std::uint32_t mip_levels(std::uint32_t root) const;
    void add_color_target(std::uint32_t pass, GraphResource resource, rhi::ColorAttachment attachment);
    std::unexpected<Error> invalid(const Pass& pass, std::string message) const;

    rhi::Device& device_;
    TransientPool* pool_;
    std::vector<Resource> resources_;
    std::vector<Pass> passes_;
    std::vector<std::uint32_t> order_;
    std::vector<std::uint32_t> culled_;
    std::vector<rhi::Barrier> final_barriers_;
    std::vector<std::string> errors_;  ///< from builders, reported by compile()
    bool compiled_ = false;
};

}  // namespace veach::render
