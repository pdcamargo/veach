// Seams sweep, throwaway: terrain, digging and walking at the places of a body-local octree where something could
// break (docs/research/spikes/seams-sweep.md). The terrain is a 3D density field (ADR 0053, 0074, 0079), so there are
// no cube faces in it; the special places are the octant planes x = 0, y = 0, z = 0 through the planet's center
// (lattice coordinates change sign), the poles (the body-fixed frame's spin axis: no "north"), the cube-corner
// directions and the chunk corners where 8 chunks meet. SP-12's planet, terrain region, colliders and dig flow are
// reused with a site parameter; SP-13's character controller is reused on SP-12's cooked colliders. No Jolt types here.
#pragma once

#include "sp12/sp12_scenario.hpp"
#include "support/sp08_field.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace veach::seams {

using sp12::Site;
using Vec = std::array<double, 3>;

/// A place to test: a direction from the planet's center (body-fixed: +Y is the spin axis, north; longitude 0 is +X).
struct SiteSpec {
    std::string_view name;
    Vec direction;  ///< need not be unit
    std::string_view what;
};

/// The sweep: both poles, a point on each octant plane with the other two coordinates negative, both cube-corner
/// directions and the equator at longitude 180°.
const std::vector<SiteSpec>& sweep_sites();
/// SP-12's own site along (0.6, 0.8, 0), the control.
SiteSpec control_site();

/// The surface along `spec.direction` (bisection on SP-12's field, then rounded to the millimeter; a coordinate that
/// is exactly 0 in the direction stays exactly 0) and its tangent frame: east = ŷ × up, north = up × east; at the
/// poles, where ŷ × up is 0, east is +X. Canonical: + − × ÷ √ only.
Site make_site(const SiteSpec& spec);

/// The octant planes the site lies on (a coordinate of its center is exactly 0): bit a for axis a.
unsigned octant_planes(const Site& site);

// --- 1. Terrain: a multi-level restricted octree around the site, meshed, welded --------------------------------

struct LodConfig {
    Site site;
    int root_level = 3;         ///< roots of 128 m
    int roots_half = 1;         ///< (2 h + 1)³ roots around the site's root chunk
    double lambda = 2.0;        ///< split distance in child chunk edges (SP-08's selection)
    std::uint32_t workers = 0;  ///< veach::JobSystem threads; 0 = the default
    bool keep_meshes = false;
};

struct LodResult {
    std::size_t leaves = 0, sampled = 0, meshed = 0;
    std::array<std::size_t, 8> meshed_per_level{};
    std::size_t transition_triangles = 0;
    sp08::SeamReport seams;
    std::uint64_t mesh_hash = 0;  ///< every leaf's mesh_hash, folded in key order
    /// Samples of chunks on both sides of an octant plane, at lattice points within one cell of it: every lattice
    /// point seen by more than one chunk (of any level, grid or full-resolution face) must hold the same sample.
    std::size_t shared_points = 0;  ///< lattice points held by two or more chunks
    std::size_t shared_mismatches = 0;
    std::array<std::int64_t, 3> region_lo{}, region_hi{};  ///< LOD0 units
    std::vector<sp08::MeshedChunk> meshes;                 ///< with keep_meshes
};

LodResult run_lod(const LodConfig& config);

/// The density field across an octant plane, along lines through the plane near the site's surface.
struct Continuity {
    std::size_t lines = 0;
    double max_step_across = 0.0;  ///< |d(lattice 0) − d(lattice −1)| over the lines, meters (quantized samples)
    double max_step_within = 0.0;  ///< the same between neighbors 2 to 8 cells from the plane
    double max_jump = 0.0;         ///< |distance(+ε) − distance(−ε)|, ε = 1 µm, unquantized
    double lipschitz_bound = 0.0;  ///< the field's bound on a 0.5 m step
};
Continuity check_continuity(const Site& site, int axis);

// --- 2. Physics: walks across boundaries -------------------------------------------------------------------------

enum class Ring : std::uint8_t { none, duplicate, adjacency };

struct WalkConfig {
    std::string_view name;
    Site site;        ///< region center and frame
    Vec through{};    ///< body-local meters: the walk's midpoint (projected onto the surface)
    Vec direction{};  ///< tangent at `through`, unit
    double half_length = 25.0;
    int legs = 4;  ///< there and back is two legs
    int half = 2;  ///< (2 h + 1)³ LOD0 chunks of terrain around through's chunk
    std::uint32_t workers = 0;
    bool merged = false;  ///< the control: one MeshShape of all chunks, welded, so no collider boundaries
    /// CharacterVirtualSettings::mEnhancedInternalEdgeRemoval: Jolt removes the bumps from edges a body's own mesh
    /// can't tell are internal (a chunk collider's border edges are, against its neighbor's).
    bool edge_removal = false;
    /// Each chunk's collider also holds the neighbor triangles touching its box, so its border edges are interior
    /// to its own MeshShape (candidate fixes for the border-edge bumps, measured against `merged`): `duplicate`
    /// collides with them too; `adjacency` tags them and the character ignores them, so they only inform Jolt's
    /// active edges.
    Ring ring = Ring::none;
    bool trace = false;  ///< fill WalkResult::trace
};

struct WalkResult {
    std::size_t steps = 0;           ///< steps walking straight (the stops and turnarounds excluded)
    std::size_t unsupported = 0;     ///< of those, steps not OnGround
    double max_unsupported_gap = 0;  ///< feet above the ground in those steps, meters (ray cast)
    double min_gap = 0;              ///< lowest feet height above the ground (negative: below it), meters
    double max_gap = 0;
    double max_dv_boundary = 0;  ///< max |Δv| per step within 0.5 m of a LOD0 chunk boundary plane, m/s
    double max_dv_plane = 0;     ///< within 0.5 m of an octant plane
    double max_dv_interior = 0;  ///< elsewhere
    double p99_dv_interior = 0;
    double p99_dv_boundary = 0;
    std::size_t boundary_steps = 0, plane_steps = 0;
    std::size_t chunk_crossings = 0;   ///< LOD0 chunk changes of the feet
    std::size_t octant_crossings = 0;  ///< sign changes of a coordinate of the feet
    double max_up_error = 0;           ///< angle between the character's up and radial up, radians
    double max_heading_step = 0;       ///< largest change of the forward vector in one straight step, radians
    double compass_jump = 0;           ///< largest one-step change of the compass heading (from geographic north)
    double distance = 0;               ///< walked, meters
    double end_error = 0;              ///< after the last leg, distance from the start point, meters
    int first_loss_leg = -1, first_loss_step = -1, first_loss_state = 0;  ///< the first unsupported step
    Vec first_loss_feet{};
    double first_loss_gap = 0;
    double max_from_mid = 0;  ///< farthest the feet got from the walk's midpoint (half_length: on track)
    std::size_t colliders = 0;
    std::size_t collider_triangles = 0;  ///< in the per-chunk colliders (ring triangles included)
    struct Step {
        Vec feet{};
        double dv = 0, dv_up = 0;
        bool boundary = false;
    };
    std::vector<Step> trace;     ///< the straight steps, with WalkConfig::trace
    std::size_t ray_misses = 0;  ///< measuring rays retried 1 mm aside (Jolt's ray test isn't watertight)
    std::uint64_t chain = 0;     ///< every step's character state, chained
};

WalkResult run_walk(const WalkConfig& config);

/// A LOD0 chunk corner (all three coordinates multiples of 16 m) near the surface, within `radius` m of `site`'s
/// center along its tangent plane: the one closest to the surface. Body-local meters.
Vec find_chunk_corner(const Site& site, double radius);

/// The point on the surface radially below or above `p` (bisection on SP-12's field), body-local meters.
Vec surface_point(const Vec& p);

}  // namespace veach::seams

namespace veach::seams {

/// The walks of the sweep: across each octant plane, through a chunk corner where 8 chunks meet, over both poles
/// (diagonally through the 4-chunk line on the spin axis, and along the z = 0 boundary plane) and across longitude
/// 180°, there and back twice.
std::vector<WalkConfig> sweep_walks();

}  // namespace veach::seams
