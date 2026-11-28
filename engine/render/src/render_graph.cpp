#include <veach/render/render_graph.hpp>

#include <veach/core/assert.hpp>
#include <veach/core/profile.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <utility>
#include <vector>

namespace veach::render {

// ---- PassBuilder / PassContext ----------------------------------------------------

void PassBuilder::read(GraphResource resource, rhi::ResourceState state) {
    graph_.passes_.at(pass_).accesses.push_back({resource, state});
}

void PassBuilder::write(GraphResource resource, rhi::ResourceState state) {
    graph_.passes_.at(pass_).accesses.push_back({resource, state});
}

void PassBuilder::color_target(GraphResource resource, rhi::LoadOp load, std::array<float, 4> clear_color,
                               rhi::TargetSlice slice) {
    graph_.add_color_target(pass_, resource, {{}, load, clear_color, {}, slice, {}});
}

void PassBuilder::color_target_uint(GraphResource resource, rhi::LoadOp load, std::array<std::uint32_t, 4> clear_value,
                                    rhi::TargetSlice slice) {
    graph_.add_color_target(pass_, resource, {{}, load, {}, clear_value, slice, {}});
}

void PassBuilder::depth_target(GraphResource resource, rhi::LoadOp load, float clear_depth, rhi::TargetSlice slice) {
    auto& pass = graph_.passes_.at(pass_);
    if (pass.kind != PassKind::graphics || pass.depth_resource.is_valid()) {
        graph_.errors_.push_back(
            std::format("pass '{}': depth_target() needs a graphics pass with no other depth target", pass.name));
        return;
    }
    pass.depth = {{}, load, clear_depth, slice, {}};
    pass.depth_resource = resource;
    pass.accesses.push_back({resource, rhi::ResourceState::depth_write});
}

void PassBuilder::resolve(GraphResource multisampled, GraphResource target) {
    auto& pass = graph_.passes_.at(pass_);
    if (pass.depth_resource.id == multisampled.id && multisampled.is_valid()) {
        pass.depth_resolve_resource = target;
        pass.accesses.push_back({target, rhi::ResourceState::depth_write});
        return;
    }
    for (std::uint32_t c = 0; c < pass.color_count; ++c) {
        if (pass.color_resources.at(c).id == multisampled.id) {
            pass.resolve_resources.at(c) = target;
            pass.accesses.push_back({target, rhi::ResourceState::color_target});
            return;
        }
    }
    graph_.errors_.push_back(
        std::format("pass '{}': resolve() names a resource that is not one of its targets", pass.name));
}

void PassBuilder::side_effect() {
    graph_.passes_.at(pass_).side_effect = true;
}

rhi::TextureHandle PassContext::texture(GraphResource resource) const {
    const auto& r = graph_.resources_.at(graph_.root_of(resource.id)).ref;
    VEACH_ASSERT(r.kind == rhi::ResourceKind::texture, "graph resource {} is not a texture", resource.id);
    return {r.index, r.generation};
}

rhi::BufferHandle PassContext::buffer(GraphResource resource) const {
    const auto& r = graph_.resources_.at(resource.id).ref;
    VEACH_ASSERT(r.kind == rhi::ResourceKind::buffer, "graph resource {} is not a buffer", resource.id);
    return {r.index, r.generation};
}

rhi::TextureRange PassContext::range(GraphResource resource) const {
    const auto& r = graph_.resources_.at(resource.id);
    if (r.root == UINT32_MAX) {
        return {};
    }
    return {.base_mip = r.first_mip, .mip_count = r.mip_count};
}

rhi::AccelHandle PassContext::accel(GraphResource resource) const {
    const auto& r = graph_.resources_.at(resource.id).ref;
    VEACH_ASSERT(r.kind == rhi::ResourceKind::acceleration_structure, "graph resource {} is not an AS", resource.id);
    return {r.index, r.generation};
}

// ---- TransientPool ----------------------------------------------------------------------

TransientPool::~TransientPool() {
    for (const Entry& e : entries_) {
        device_.destroy(e.texture);
    }
}

Result<rhi::TextureHandle> TransientPool::acquire(const rhi::TextureDesc& desc) {
    for (Entry& e : entries_) {
        if (!e.in_use && e.desc.width == desc.width && e.desc.height == desc.height && e.desc.format == desc.format &&
            e.desc.usage == desc.usage && e.desc.type == desc.type && e.desc.depth == desc.depth &&
            e.desc.layers == desc.layers && std::max(1u, e.desc.mip_levels) == std::max(1u, desc.mip_levels) &&
            e.desc.sample_count == desc.sample_count) {
            e.in_use = true;
            return e.texture;
        }
    }
    auto texture = device_.create_texture(desc);
    if (!texture) {
        return texture;
    }
    entries_.push_back({desc, *texture, true});
    return *texture;
}

rhi::ResourceState TransientPool::state(rhi::TextureHandle texture) const {
    for (const Entry& e : entries_) {
        if (e.texture == texture) {
            return e.state;
        }
    }
    return rhi::ResourceState::undefined;
}

void TransientPool::set_state(rhi::TextureHandle texture, rhi::ResourceState state) {
    for (Entry& e : entries_) {
        if (e.texture == texture) {
            e.state = state;
        }
    }
}

void TransientPool::release_all() {
    for (Entry& e : entries_) {
        e.in_use = false;
    }
}

// ---- RenderGraph -------------------------------------------------------------------------

GraphResource RenderGraph::import(rhi::TextureHandle texture, std::string name, rhi::ResourceState current,
                                  std::optional<rhi::ResourceState> final_state) {
    resources_.push_back({std::move(name), texture, current, final_state, current, true, std::nullopt});
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

GraphResource RenderGraph::import(rhi::BufferHandle buffer, std::string name, rhi::ResourceState current,
                                  std::optional<rhi::ResourceState> final_state) {
    resources_.push_back({std::move(name), buffer, current, final_state, current, true, std::nullopt});
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

GraphResource RenderGraph::import(rhi::AccelHandle accel, std::string name, rhi::ResourceState current,
                                  std::optional<rhi::ResourceState> final_state) {
    resources_.push_back({std::move(name), accel, current, final_state, current, true, std::nullopt});
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

GraphResource RenderGraph::create_texture(const rhi::TextureDesc& desc) {
    resources_.push_back(
        {desc.debug_name, {}, rhi::ResourceState::undefined, std::nullopt, rhi::ResourceState::undefined, false, desc});
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

GraphResource RenderGraph::mips(GraphResource texture, std::uint32_t first_mip, std::uint32_t mip_count) {
    if (!texture.is_valid() || texture.id >= resources_.size()) {
        errors_.push_back("mips() of an invalid resource");
        return {};
    }
    const std::uint32_t root = root_of(texture.id);
    const Resource& base = resources_[root];
    if (!base.transient && base.ref.kind != rhi::ResourceKind::texture) {
        errors_.push_back(std::format("mips() of '{}', which is not a texture", base.name));
        return {};
    }
    const std::uint32_t levels = mip_levels(root);
    if (mip_count == 0 || first_mip + mip_count > levels) {
        errors_.push_back(
            std::format("mips {}..{} of '{}', which has {}", first_mip, first_mip + mip_count, base.name, levels));
        return {};
    }
    Resource view = base;
    view.name = std::format("{} mip {}{}", base.name, first_mip,
                            mip_count == 1 ? std::string() : std::format("+{}", mip_count));
    view.transient.reset();
    view.root = root;
    view.first_mip = first_mip;
    view.mip_count = mip_count;
    resources_.push_back(std::move(view));
    return {static_cast<std::uint32_t>(resources_.size() - 1)};
}

std::uint32_t RenderGraph::root_of(std::uint32_t resource) const {
    const std::uint32_t root = resources_.at(resource).root;
    return root == UINT32_MAX ? resource : root;
}

std::uint32_t RenderGraph::mip_levels(std::uint32_t root) const {
    const Resource& r = resources_.at(root);
    if (r.transient) {
        return std::max(1u, r.transient->mip_levels);
    }
    if (r.ref.kind != rhi::ResourceKind::texture) {
        return 1;
    }
    const rhi::TextureDesc* desc = device_.texture_desc({r.ref.index, r.ref.generation});
    return desc != nullptr ? std::max(1u, desc->mip_levels) : 1;
}

const rhi::TextureDesc* RenderGraph::texture_desc(GraphResource resource) const {
    if (!resource.is_valid() || resource.id >= resources_.size()) {
        return nullptr;
    }
    const Resource& r = resources_[root_of(resource.id)];
    if (r.transient) {
        return &*r.transient;
    }
    if (r.ref.kind != rhi::ResourceKind::texture) {
        return nullptr;
    }
    return device_.texture_desc({r.ref.index, r.ref.generation});
}

void RenderGraph::add_pass(std::string name, PassKind kind, const Setup& setup, Execute execute) {
    passes_.push_back({std::move(name), kind, {}, {}, {}, {}, 0, {}, {}, {}, false, std::move(execute), {}});
    PassBuilder builder(*this, static_cast<std::uint32_t>(passes_.size() - 1));
    setup(builder);
    compiled_ = false;
}

void RenderGraph::add_color_target(std::uint32_t pass_index, GraphResource resource, rhi::ColorAttachment attachment) {
    auto& pass = passes_.at(pass_index);
    if (pass.kind != PassKind::graphics || pass.color_count == pass.colors.size()) {
        errors_.push_back(std::format("pass '{}': color_target() needs a graphics pass with < 4 targets", pass.name));
        return;
    }
    pass.colors.at(pass.color_count) = attachment;
    pass.color_resources.at(pass.color_count) = resource;
    ++pass.color_count;
    pass.accesses.push_back({resource, rhi::ResourceState::color_target});
}

void RenderGraph::add_graphics_pass(std::string name, const Setup& setup, Execute execute) {
    add_pass(std::move(name), PassKind::graphics, setup, std::move(execute));
}

void RenderGraph::add_compute_pass(std::string name, const Setup& setup, Execute execute) {
    add_pass(std::move(name), PassKind::compute, setup, std::move(execute));
}

void RenderGraph::add_transfer_pass(std::string name, const Setup& setup, Execute execute) {
    add_pass(std::move(name), PassKind::transfer, setup, std::move(execute));
}

std::unexpected<Error> RenderGraph::invalid(const Pass& pass, std::string message) const {
    return make_error(ErrorCode::render_graph_invalid, std::format("render graph pass '{}': {}", pass.name, message),
                      "Declare every access in the pass setup; see RenderGraph::describe() at --log-level debug.");
}

Result<> RenderGraph::compile() {
    VEACH_ZONE("RenderGraph::compile");
    if (!errors_.empty()) {
        return make_error(ErrorCode::render_graph_invalid, errors_.front());
    }
    const auto pass_count = static_cast<std::uint32_t>(passes_.size());

    // Subresources: one slot per mip of every whole resource (buffers and
    // acceleration structures have one); an access covers the slots of the mips
    // it names. `base[r]` is resource r's first slot.
    std::vector<std::uint32_t> base(resources_.size(), 0);
    std::vector<std::uint32_t> levels(resources_.size(), 1);
    std::uint32_t slot_count = 0;
    for (std::uint32_t r = 0; r < resources_.size(); ++r) {
        if (resources_[r].root == UINT32_MAX) {
            levels[r] = mip_levels(r);
            base[r] = slot_count;
            slot_count += levels[r];
        }
    }
    struct Span {
        std::uint32_t root;
        std::uint32_t first;  ///< mip
        std::uint32_t count;
    };
    const auto span_of = [&](GraphResource g) {
        const Resource& r = resources_[g.id];
        const std::uint32_t root = root_of(g.id);
        return r.root == UINT32_MAX ? Span{root, 0, levels[root]} : Span{root, r.first_mip, r.mip_count};
    };

    // Validate accesses. `seen` stamps each slot with the pass (+ 1) that last touched it: a scene pass declares
    // thousands of mesh buffers, so this stays linear.
    std::vector<std::uint32_t> seen(slot_count, 0);
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        const Pass& pass = passes_[p];
        for (const Access& a : pass.accesses) {
            if (!a.resource.is_valid() || a.resource.id >= resources_.size()) {
                return invalid(pass, "uses an invalid resource");
            }
            const Span span = span_of(a.resource);
            for (std::uint32_t m = span.first; m < span.first + span.count; ++m) {
                if (std::exchange(seen[base[span.root] + m], p + 1) == p + 1) {
                    return invalid(pass, std::format("accesses '{}' twice; declare one "
                                                     "state per resource (or mip) "
                                                     "per pass",
                                                     resources_[a.resource.id].name));
                }
            }
            if ((a.state == rhi::ResourceState::color_target || a.state == rhi::ResourceState::depth_write) &&
                pass.kind != PassKind::graphics) {
                return invalid(pass, std::format("{} access outside a graphics pass", rhi::to_string(a.state)));
            }
            // VCH-071: indirect arguments feed a draw or a dispatch, indices a draw;
            // both are buffers.
            if (a.state == rhi::ResourceState::indirect_argument || a.state == rhi::ResourceState::index_read) {
                const Resource& r = resources_[a.resource.id];
                if (r.transient || r.ref.kind != rhi::ResourceKind::buffer) {
                    return invalid(
                        pass, std::format("'{}' is read in {} but is not a buffer", r.name, rhi::to_string(a.state)));
                }
                if (pass.kind == PassKind::transfer ||
                    (a.state == rhi::ResourceState::index_read && pass.kind != PassKind::graphics)) {
                    return invalid(pass,
                                   std::format("{} access in a pass that cannot draw{}", rhi::to_string(a.state),
                                               a.state == rhi::ResourceState::indirect_argument ? " or dispatch" : ""));
                }
            }
        }
    }

    // Dependencies. Declaration order defines data flow ("version" of each
    // subresource); edges cover read-after-write, write-after-write and
    // write-after-read.
    std::vector<std::set<std::uint32_t>> successors(pass_count);
    std::vector<std::uint32_t> in_degree(pass_count, 0);
    std::vector<std::optional<std::uint32_t>> last_writer(slot_count);
    std::vector<std::vector<std::uint32_t>> readers(slot_count);
    const auto edge = [&](std::uint32_t from, std::uint32_t to) {
        if (from != to && successors[from].insert(to).second) {
            ++in_degree[to];
        }
    };
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        for (const Access& a : passes_[p].accesses) {
            const Span span = span_of(a.resource);
            for (std::uint32_t m = span.first; m < span.first + span.count; ++m) {
                const std::uint32_t slot = base[span.root] + m;
                if (rhi::is_write(a.state)) {
                    if (last_writer[slot]) {
                        edge(*last_writer[slot], p);
                    }
                    for (const std::uint32_t reader : readers[slot]) {
                        edge(reader, p);
                    }
                    last_writer[slot] = p;
                    readers[slot].clear();
                } else {
                    if (last_writer[slot]) {
                        edge(*last_writer[slot], p);
                    } else if (!resources_[span.root].imported) {
                        return invalid(passes_[p], std::format("reads transient '{}' before any pass writes it",
                                                               resources_[a.resource.id].name));
                    }
                    readers[slot].push_back(p);
                }
            }
        }
    }

    // Culling: keep passes with side effects or writes to imported resources, and
    // everything they depend on.
    std::vector<bool> needed(pass_count, false);
    std::vector<std::vector<std::uint32_t>> predecessors(pass_count);
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        for (const std::uint32_t s : successors[p]) {
            predecessors[s].push_back(p);
        }
    }
    std::vector<std::uint32_t> stack;
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        bool output = passes_[p].side_effect;
        for (const Access& a : passes_[p].accesses) {
            output = output || (rhi::is_write(a.state) && resources_[root_of(a.resource.id)].imported);
        }
        if (output) {
            needed[p] = true;
            stack.push_back(p);
        }
    }
    while (!stack.empty()) {
        const std::uint32_t p = stack.back();
        stack.pop_back();
        for (const std::uint32_t q : predecessors[p]) {
            if (!needed[q]) {
                needed[q] = true;
                stack.push_back(q);
            }
        }
    }

    // Order: topological (Kahn). Among ready passes prefer the kind of the pass
    // just emitted (fewer encoder switches), then declaration order.
    order_.clear();
    culled_.clear();
    std::vector<std::uint32_t> ready;
    for (std::uint32_t p = 0; p < pass_count; ++p) {
        if (in_degree[p] == 0) {
            ready.push_back(p);
        }
    }
    std::optional<PassKind> last_kind;
    while (!ready.empty()) {
        auto pick = std::ranges::min_element(ready, [&](std::uint32_t a, std::uint32_t b) {
            const bool a_same = last_kind && passes_[a].kind == *last_kind;
            const bool b_same = last_kind && passes_[b].kind == *last_kind;
            return a_same != b_same ? a_same : a < b;
        });
        const std::uint32_t p = *pick;
        ready.erase(pick);
        if (needed[p]) {
            order_.push_back(p);
            last_kind = passes_[p].kind;
        } else {
            culled_.push_back(p);
        }
        for (const std::uint32_t s : successors[p]) {
            if (--in_degree[s] == 0) {
                ready.push_back(s);
            }
        }
    }
    VEACH_ASSERT(order_.size() + culled_.size() == pass_count, "render graph has a cycle");

    // Transients (views take their texture's).
    for (Resource& r : resources_) {
        if (r.transient) {
            if (!pool_) {
                return make_error(ErrorCode::render_graph_invalid,
                                  std::format("transient '{}' needs a TransientPool", r.name));
            }
            auto texture = pool_->acquire(*r.transient);
            if (!texture) {
                return std::unexpected(texture.error());
            }
            r.ref = *texture;
            // A pooled texture may have been used by an earlier frame still in
            // flight: its first barrier this frame starts from that real state
            // (synchronization) but discards the contents.
            r.initial = pool_->state(*texture);
        }
    }
    for (Resource& r : resources_) {
        if (r.root != UINT32_MAX) {
            r.ref = resources_[r.root].ref;
        }
    }

    // Barriers, one per run of mips in the same state.
    std::vector<rhi::ResourceState> state(slot_count);
    std::vector<bool> touched(slot_count, false);
    for (std::uint32_t r = 0; r < resources_.size(); ++r) {
        if (resources_[r].root == UINT32_MAX) {
            std::fill_n(state.begin() + base[r], levels[r], resources_[r].initial);
        }
    }
    const auto barrier = [&](std::uint32_t root, std::uint32_t first, std::uint32_t count, rhi::ResourceState before,
                             rhi::ResourceState after, bool discard) {
        rhi::Barrier b{resources_[root].ref, before, after, discard};
        if (count != levels[root]) {
            b.first_mip = first;
            b.mip_count = count;
        }
        return b;
    };
    for (const std::uint32_t p : order_) {
        Pass& pass = passes_[p];
        pass.barriers.clear();
        for (const Access& a : pass.accesses) {
            const Span span = span_of(a.resource);
            const std::uint32_t first_slot = base[span.root];
            const bool transient = resources_[span.root].transient.has_value();
            std::uint32_t m = span.first;
            while (m < span.first + span.count) {
                const rhi::ResourceState current = state[first_slot + m];
                const bool first_use = transient && !touched[first_slot + m];
                std::uint32_t end = m + 1;
                while (end < span.first + span.count && state[first_slot + end] == current &&
                       (transient && !touched[first_slot + end]) == first_use) {
                    ++end;
                }
                const bool uav =
                    a.state == rhi::ResourceState::shader_write && current == rhi::ResourceState::shader_write;
                if (current != a.state || uav || first_use) {
                    pass.barriers.push_back(barrier(span.root, m, end - m, current, a.state, first_use));
                }
                for (std::uint32_t k = m; k < end; ++k) {
                    state[first_slot + k] = a.state;
                    touched[first_slot + k] = true;
                }
                m = end;
            }
        }
        for (std::uint32_t c = 0; c < pass.color_count; ++c) {
            pass.colors.at(c).texture = {resources_[pass.color_resources.at(c).id].ref.index,
                                         resources_[pass.color_resources.at(c).id].ref.generation};
            if (const GraphResource r = pass.resolve_resources.at(c); r.is_valid()) {
                pass.colors.at(c).resolve = {resources_[r.id].ref.index, resources_[r.id].ref.generation};
            }
        }
        if (pass.depth_resource.is_valid()) {
            const rhi::ResourceRef& ref = resources_[pass.depth_resource.id].ref;
            pass.depth.texture = {ref.index, ref.generation};
            if (pass.depth_resolve_resource.is_valid()) {
                const rhi::ResourceRef& r = resources_[pass.depth_resolve_resource.id].ref;
                pass.depth.resolve = {r.index, r.generation};
            }
        }
    }
    // The end: every mip of a texture in its final state, or in mip 0's when they
    // differ and none is given.
    final_barriers_.clear();
    for (std::uint32_t r = 0; r < resources_.size(); ++r) {
        if (resources_[r].root != UINT32_MAX) {
            continue;
        }
        const rhi::ResourceState target = resources_[r].final_state.value_or(state[base[r]]);
        std::uint32_t m = 0;
        while (m < levels[r]) {
            const rhi::ResourceState current = state[base[r] + m];
            std::uint32_t end = m + 1;
            while (end < levels[r] && state[base[r] + end] == current) {
                ++end;
            }
            if (current != target) {
                final_barriers_.push_back(barrier(r, m, end - m, current, target, false));
            }
            m = end;
        }
        resources_[r].end_state = target;
    }
    for (Resource& r : resources_) {
        if (r.root != UINT32_MAX) {
            r.end_state = resources_[r.root].end_state;
        }
    }
    compiled_ = true;
    return {};
}
void RenderGraph::execute(rhi::CommandList& cmd) {
    VEACH_ZONE("RenderGraph::execute");
    VEACH_ASSERT(compiled_, "execute() before a successful compile()");
    for (const std::uint32_t p : order_) {
        Pass& pass = passes_[p];
        VEACH_ZONE_TEXT("pass", pass.name.data(), pass.name.size());
        cmd.push_debug_group(pass.name);
        if (!pass.barriers.empty()) {
            cmd.barrier(pass.barriers);
        }
        cmd.begin_timing(pass.name);  // GPU time per pass (VCH-049): Device::last_frame_timings()
        switch (pass.kind) {
            case PassKind::graphics:
                cmd.begin_render_pass({pass.colors, pass.color_count, pass.depth, pass.name});
                break;
            case PassKind::compute: cmd.begin_compute_pass(pass.name); break;
            case PassKind::transfer: break;
        }
        for (const Access& a : pass.accesses) {
            cmd.use(resources_[a.resource.id].ref, a.state);
        }
        PassContext ctx(cmd, device_, *this);
        if (pass.execute) {
            pass.execute(ctx);
        }
        switch (pass.kind) {
            case PassKind::graphics: cmd.end_render_pass(); break;
            case PassKind::compute: cmd.end_compute_pass(); break;
            case PassKind::transfer: break;
        }
        cmd.end_timing();
        cmd.pop_debug_group();
    }
    if (!final_barriers_.empty()) {
        cmd.barrier(final_barriers_);
    }
    if (pool_) {
        for (const Resource& r : resources_) {
            if (r.transient) {
                pool_->set_state({r.ref.index, r.ref.generation}, r.end_state);
            }
        }
        pool_->release_all();
    }
}

std::string RenderGraph::describe() const {
    const auto name_of = [&](const rhi::ResourceRef& ref) {
        for (const Resource& r : resources_) {
            if (r.ref == ref) {
                return r.name;
            }
        }
        return std::string("?");
    };
    std::string out = std::format("render graph: {} passes, {} culled\n", order_.size(), culled_.size());
    int n = 1;
    for (const std::uint32_t p : order_) {
        const Pass& pass = passes_[p];
        constexpr std::array<const char*, 3> kinds{"graphics", "compute", "transfer"};
        out += std::format("  {}. {} [{}]\n", n++, pass.name, kinds.at(static_cast<std::size_t>(pass.kind)));
        for (const rhi::Barrier& b : pass.barriers) {
            out += std::format("       barrier {}{}: {} -> {}{}\n", name_of(b.resource),
                               b.mip_count == 0 ? std::string() : std::format(" mips {}+{}", b.first_mip, b.mip_count),
                               rhi::to_string(b.before), rhi::to_string(b.after), b.discard ? " (discard)" : "");
        }
        // Runs of accesses to resources of one name in one state collapse into one line (a pass reading every mesh).
        for (std::size_t i = 0; i < pass.accesses.size();) {
            const Access& a = pass.accesses[i];
            const std::string& name = resources_[a.resource.id].name;
            std::size_t run = 1;
            while (i + run < pass.accesses.size() && pass.accesses[i + run].state == a.state &&
                   resources_[pass.accesses[i + run].resource.id].name == name) {
                ++run;
            }
            out += std::format("       {} {}{} ({})\n", rhi::is_write(a.state) ? "writes" : "reads", name,
                               run > 1 ? std::format(" x{}", run) : std::string(), rhi::to_string(a.state));
            i += run;
        }
    }
    for (const std::uint32_t p : culled_) {
        out += std::format("  culled: {}\n", passes_[p].name);
    }
    for (const rhi::Barrier& b : final_barriers_) {
        out += std::format("  final barrier {}{}: {} -> {}\n", name_of(b.resource),
                           b.mip_count == 0 ? std::string() : std::format(" mips {}+{}", b.first_mip, b.mip_count),
                           rhi::to_string(b.before), rhi::to_string(b.after));
    }
    return out;
}

rhi::ResourceState RenderGraph::final_state(GraphResource resource) const {
    return resources_.at(resource.id).end_state;
}

std::vector<std::string> RenderGraph::execution_order() const {
    std::vector<std::string> names;
    for (const std::uint32_t p : order_) {
        names.push_back(passes_[p].name);
    }
    return names;
}

}  // namespace veach::render
