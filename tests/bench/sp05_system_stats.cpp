// Spike SP-05 (VCH-081), throwaway: generates systems through S1 and B1 headless, writes histograms, counts physical
// violations, measures systems/s on 1 and N threads, and checks that stage hashes don't depend on thread count or
// scale. docs/research/spikes/sp-05-system-statistics.md.
//
//   veach-bench-sp05-systems [--systems N] [--seed S] [--threads N] [--bias B] [--out DIR]
//
// Exit code 1 if any violation is found or a hash differs across thread counts or scales.

#include <veach/core/hash.hpp>
#include <veach/space/gen/address.hpp>
#include <veach/space/gen/realize.hpp>
#include <veach/space/gen/system.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace sg = space::gen;
namespace si = space::gen::si;
using Clock = std::chrono::steady_clock;

/// A deterministic spread of distinct addresses over all 8 layers: 64 systems per cell, cells in a 16 × 16 block near
/// the galaxy's center at each layer. Every address must encode and decode back.
std::optional<sg::SystemAddress> sample_address(std::uint64_t i) {
    sg::SystemCoords c;
    c.layer = static_cast<std::uint32_t>(i % 8u);
    const std::uint64_t j = i / 8u;
    c.system = j % 64u;
    const std::uint64_t cell = j / 64u;
    const std::uint32_t center_xz = 1u << (sg::cell_bits_xz(c.layer) - 1u);
    const std::uint32_t center_y = 1u << (sg::cell_bits_y(c.layer) - 1u);
    c.x = center_xz + static_cast<std::uint32_t>(cell % 16u);
    c.z = center_xz + static_cast<std::uint32_t>((cell / 16u) % 16u);
    c.y = center_y + static_cast<std::uint32_t>((cell / 256u) % 8u);
    const auto a = sg::SystemAddress::encode(c);
    if (!a || a->decode() != c) {
        return std::nullopt;
    }
    return a;
}

struct Hashes {
    std::uint64_t s1 = 0, b1 = 0;
};

Hashes hashes_of(const sg::UniverseRecord& u, sg::SystemAddress a) {
    const sg::StageContext ctx{&u, a};
    const sg::SystemRecord r = sg::generate_system(ctx);
    return {sg::stage_hash(r), sg::stage_hash(sg::derive_all_body_physics(ctx, r))};
}

std::uint64_t combine(const std::vector<Hashes>& h) {  // in index order, whatever order they were computed in
    std::uint64_t out = veach::hash_id("sp05");
    for (const Hashes& x : h) {
        out = veach::hash(out, x.s1, x.b1);
    }
    return out;
}

std::vector<Hashes> run_threads(const sg::UniverseRecord& u, const std::vector<sg::SystemAddress>& addrs,
                                unsigned threads) {
    std::vector<Hashes> out(addrs.size());
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            // Interleaved, and each thread walks backward: the order differs from the single-threaded run.
            for (std::size_t i = addrs.size(); i-- > 0;) {
                if (i % threads == t) {
                    out[i] = hashes_of(u, addrs[i]);
                }
            }
        });
    }
    for (std::thread& th : pool) {
        th.join();
    }
    return out;
}

struct Hist {
    std::string name, unit;
    std::vector<double> edges;
    std::vector<std::uint64_t> counts;  // [< e0], [e0, e1), ..., [>= e_last]
    bool integer = false;               // counts: label bins by value ("0", "3", "15-19", "30+")
    Hist(std::string n, std::string un, std::vector<double> e, bool is_integer = false)
        : name(std::move(n)),
          unit(std::move(un)),
          edges(std::move(e)),
          counts(edges.size() + 1, 0),
          integer(is_integer) {}
    void add(double x) {
        const auto it = std::upper_bound(edges.begin(), edges.end(), x);
        ++counts[static_cast<std::size_t>(it - edges.begin())];
    }
    std::string label(std::size_t i) const {
        if (integer) {
            const double lo = i == 0 ? 0.0 : edges[i - 1];
            if (i == edges.size()) {
                return std::format("{:g}+", lo);
            }
            return edges[i] - lo == 1.0 ? std::format("{:g}", lo) : std::format("{:g}-{:g}", lo, edges[i] - 1.0);
        }
        if (i == 0) {
            return std::format("< {:g}", edges[0]);
        }
        if (i == edges.size()) {
            return std::format(">= {:g}", edges.back());
        }
        return std::format("{:g} - {:g}", edges[i - 1], edges[i]);
    }
};

struct Cat {
    std::string name;
    std::vector<std::string> labels;
    std::vector<std::uint64_t> counts;
    Cat(std::string n, std::vector<std::string> l) : name(std::move(n)), labels(std::move(l)), counts(labels.size()) {}
    void add(std::string_view x) {
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (labels[i] == x) {
                ++counts[i];
                return;
            }
        }
        labels.emplace_back(x);
        counts.push_back(1);
    }
};

std::uint64_t total(const std::vector<std::uint64_t>& c) {
    std::uint64_t t = 0;
    for (const std::uint64_t x : c) {
        t += x;
    }
    return t;
}

void print_hist(const Hist& h, const std::filesystem::path& out) {
    const double n = static_cast<double>(std::max<std::uint64_t>(total(h.counts), 1));
    std::printf("\n### %s (%s), n = %llu\n\n| bin | count | %% |\n|---|---:|---:|\n", h.name.c_str(), h.unit.c_str(),
                static_cast<unsigned long long>(total(h.counts)));
    std::ofstream csv;
    if (!out.empty()) {
        csv.open(out / (h.name + ".csv"));
        csv << "bin,count,fraction\n";
    }
    for (std::size_t i = 0; i < h.counts.size(); ++i) {
        const double f = static_cast<double>(h.counts[i]) / n;
        std::printf("| %s | %llu | %.2f |\n", h.label(i).c_str(), static_cast<unsigned long long>(h.counts[i]),
                    100.0 * f);
        if (csv) {
            csv << '"' << h.label(i) << "\"," << h.counts[i] << ',' << f << '\n';
        }
    }
}

void print_cat(const Cat& c, const std::filesystem::path& out) {
    const double n = static_cast<double>(std::max<std::uint64_t>(total(c.counts), 1));
    std::printf("\n### %s, n = %llu\n\n| value | count | %% |\n|---|---:|---:|\n", c.name.c_str(),
                static_cast<unsigned long long>(total(c.counts)));
    std::ofstream csv;
    if (!out.empty()) {
        csv.open(out / (c.name + ".csv"));
        csv << "bin,count,fraction\n";
    }
    for (std::size_t i = 0; i < c.counts.size(); ++i) {
        const double f = static_cast<double>(c.counts[i]) / n;
        std::printf("| %s | %llu | %.2f |\n", c.labels[i].c_str(), static_cast<unsigned long long>(c.counts[i]),
                    100.0 * f);
        if (csv) {
            csv << '"' << c.labels[i] << "\"," << c.counts[i] << ',' << f << '\n';
        }
    }
}

template <typename T>
bool parse(std::string_view s, T& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}

/// Neighboring keys must not correlate (ADR 0044's reopen condition): Pearson r of unit_double over (i, i + 1), and
/// avalanche of the index word.
void hash_quality(std::uint64_t seed) {
    constexpr std::uint64_t n = 1'000'000;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    double prev = veach::unit_double(veach::hash(seed, veach::hash_id("orbits"), 7u, 0u));
    for (std::uint64_t i = 1; i <= n; ++i) {
        const double x = veach::unit_double(veach::hash(seed, veach::hash_id("orbits"), 7u, i));
        sx += prev, sy += x, sxx += prev * prev, syy += x * x, sxy += prev * x;
        prev = x;
    }
    const double dn = static_cast<double>(n);
    const double r = (sxy - sx * sy / dn) / std::sqrt((sxx - sx * sx / dn) * (syy - sy * sy / dn));
    // Avalanche: flip each bit of the index word, count output bits that change; ideal mean 32, per-bit 0.5.
    constexpr std::uint64_t keys = 20'000;
    std::vector<std::uint64_t> bit_flips(64, 0);
    std::uint64_t flips = 0;
    for (std::uint64_t k = 0; k < keys; ++k) {
        const std::uint64_t index = veach::hash(seed, k);
        const std::uint64_t h0 = veach::hash(seed, 1u, 2u, index);
        for (unsigned b = 0; b < 64; ++b) {
            const std::uint64_t d = h0 ^ veach::hash(seed, 1u, 2u, index ^ (std::uint64_t{1} << b));
            flips += static_cast<std::uint64_t>(std::popcount(d));
            for (unsigned o = 0; o < 64; ++o) {
                bit_flips[o] += (d >> o) & 1u;
            }
        }
    }
    double worst = 0;
    for (const std::uint64_t c : bit_flips) {
        worst = std::max(worst, std::abs(static_cast<double>(c) / static_cast<double>(keys * 64) - 0.5));
    }
    std::printf("\n## Hash quality\n\n| test | result | ideal |\n|---|---:|---:|\n");
    std::printf(
        "| Pearson r, unit_double(hash(.., i)) vs (.., i+1), 10^6 pairs | %+.5f | 0 (abs(r) < %.4f at 3 sigma) |\n", r,
        3.0 / std::sqrt(dn));
    std::printf("| avalanche: mean output bits flipped per input bit, index word | %.3f | 32 |\n",
                static_cast<double>(flips) / static_cast<double>(keys * 64));
    std::printf("| avalanche: worst output-bit flip probability bias | %.4f | 0 (%.4f at 3 sigma) |\n", worst,
                3.0 * 0.5 / std::sqrt(static_cast<double>(keys * 64)));
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t systems = 100'000, seed = 1;
    unsigned threads = std::max(2u, std::thread::hardware_concurrency());
    double bias = 0.5;
    std::filesystem::path out;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const std::string_view v = i + 1 < argc ? argv[i + 1] : "";
        bool ok = true;
        if (a == "--systems") {
            ok = parse(v, systems), ++i;
        } else if (a == "--seed") {
            ok = parse(v, seed), ++i;
        } else if (a == "--threads") {
            ok = parse(v, threads), ++i;
        } else if (a == "--bias") {
            ok = parse(v, bias), ++i;
        } else if (a == "--out") {
            out = v, ++i;
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(
                stderr,
                "usage: veach-bench-sp05-systems [--systems N] [--seed S] [--threads N] [--bias B] [--out DIR]\n");
            return 2;
        }
    }
    if (!out.empty()) {
        std::filesystem::create_directories(out);
    }
    const sg::UniverseRecord universe{seed, bias};
    int failures = 0;

    std::vector<sg::SystemAddress> addrs;
    std::uint64_t address_failures = 0;
    for (std::uint64_t i = 0; i < systems; ++i) {
        if (const auto a = sample_address(i)) {
            addrs.push_back(*a);
            if (sg::parse_system_address(sg::to_string(*a)) != a) {
                ++address_failures;
            }
        } else {
            ++address_failures;
        }
    }

    Cat star_class("star_class", {"O", "B", "A", "F", "G", "K", "M", "D", "NS", "BH"});
    Cat star_kind("star_kind", {});
    Cat composition("planet_composition", {});
    Cat temp_class("planet_temperature_class", {"frigid", "cold", "cool", "temperate", "warm", "hot", "torrid"});
    Hist planets_per_system("planets_per_system", "count",
                            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 25, 30}, true);
    Hist mass("planet_mass", "Earth masses", {0.01, 0.03, 0.1, 0.3, 1, 3, 10, 30, 100, 300, 1000, 3000});
    Hist radius("planet_radius", "Earth radii", {0.25, 0.35, 0.5, 0.71, 1, 1.41, 2, 2.83, 4, 5.66, 8, 11.3, 16});
    Hist sma("planet_semi_major_axis", "AU", {0.01, 0.03, 0.1, 0.3, 1, 3, 10, 30, 100});
    Hist period("planet_period", "days", {1, 3, 10, 30, 100, 300, 1000, 3000, 10000, 30000, 100000});
    Hist ratio("adjacent_period_ratio", "P_out / P_in", {1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.4, 2.6, 2.8, 3.0});
    Hist spacing("adjacent_mutual_hill_spacing", "mutual Hill radii, periapsis to apoapsis",
                 {10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60});
    Hist teq("planet_equilibrium_temperature", "K", {50, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550, 600});
    Hist hz("hz_planets_per_system", "planets in the habitable zone", {1, 2, 3, 4}, true);
    Hist ph("potentially_habitable_per_system", "rocky 0.1-5 Earth masses in the HZ keeping N2", {1, 2, 3}, true);
    Hist moons_per_planet("moons_per_planet", "count", {1, 2, 3, 4, 5, 6}, true);
    std::vector<double> planets_by_class(10, 0), systems_by_class(10, 0), giant_by_class(10, 0), hz_by_class(10, 0),
        ph_by_class(10, 0);
    std::uint64_t planets_total = 0, moons_total = 0, max_bodies = 0, max_planets_seen = 0, capped = 0;
    std::uint64_t hz_planets = 0, hz_locked = 0, rocky = 0, rocky_atm = 0, anchors = 0, systems_with_ph = 0;
    sg::Violations violations;

    // ---- Pass 1: single thread, timed per stage, with statistics ----
    std::vector<Hashes> single(addrs.size());
    double s1_s = 0, b1_s = 0;
    for (std::size_t i = 0; i < addrs.size(); ++i) {
        const sg::StageContext ctx{&universe, addrs[i]};
        const auto t0 = Clock::now();
        const sg::SystemRecord r = sg::generate_system(ctx);
        const auto t1 = Clock::now();
        const std::vector<sg::BodyPhysics> phys = sg::derive_all_body_physics(ctx, r);
        const auto t2 = Clock::now();
        s1_s += std::chrono::duration<double>(t1 - t0).count();
        b1_s += std::chrono::duration<double>(t2 - t1).count();
        single[i] = {sg::stage_hash(r), sg::stage_hash(phys)};

        violations += sg::validate(r, phys);
        star_class.add(r.star.spectral_class);
        star_kind.add(r.star.kind);
        std::size_t ci = 0;
        while (ci < 10 && star_class.labels[ci] != r.star.spectral_class) {
            ++ci;
        }
        planets_by_class[ci] += r.planet_count;
        systems_by_class[ci] += 1;
        planets_per_system.add(r.planet_count);
        planets_total += r.planet_count;
        moons_total += r.bodies.size() - r.planet_count;
        max_bodies = std::max<std::uint64_t>(max_bodies, r.bodies.size());
        max_planets_seen = std::max<std::uint64_t>(max_planets_seen, r.planet_count);
        capped += r.planet_count >= 30 ? 1u : 0u;
        anchors += r.habitability_anchor ? 1u : 0u;
        std::uint64_t in_hz = 0, habitable = 0;
        std::vector<std::uint64_t> moon_count(r.planet_count, 0);
        for (const sg::BodyRecord& b : r.bodies) {
            if (b.parent != 0) {
                ++moon_count[b.parent - 1];
            }
        }
        for (std::uint32_t k = 0; k < r.planet_count; ++k) {
            const sg::BodyRecord& b = r.bodies[k];
            const sg::BodyPhysics& p = phys[k];
            composition.add(b.composition);
            temp_class.add(p.temperature_class);
            mass.add(b.mass_kg / si::earth_mass);
            radius.add(b.radius_m / si::earth_radius);
            sma.add(b.orbit.semi_major_axis_m / si::au);
            const double pd = 2.0 * si::pi *
                              std::sqrt(b.orbit.semi_major_axis_m * b.orbit.semi_major_axis_m *
                                        b.orbit.semi_major_axis_m / (si::G * (r.star.mass_kg + b.mass_kg)));
            period.add(pd / 86400.0);
            teq.add(p.equilibrium_temperature_k);
            moons_per_planet.add(static_cast<double>(moon_count[k]));
            if (k > 0) {
                const sg::BodyRecord& a = r.bodies[k - 1];
                const double ra = b.orbit.semi_major_axis_m / a.orbit.semi_major_axis_m;
                ratio.add(ra * std::sqrt(ra));
                const double gap = b.orbit.semi_major_axis_m * (1 - b.orbit.eccentricity) -
                                   a.orbit.semi_major_axis_m * (1 + a.orbit.eccentricity);
                spacing.add(gap / sg::mutual_hill_radius_m(a.mass_kg, b.mass_kg, r.star.mass_kg,
                                                           a.orbit.semi_major_axis_m, b.orbit.semi_major_axis_m));
            }
            if (p.in_habitable_zone) {
                ++in_hz;
                ++hz_planets;
                hz_locked += b.tidally_locked ? 1u : 0u;
            }
            if (b.composition == "space:composition.rocky") {
                ++rocky;
                rocky_atm += p.retains_atmosphere ? 1u : 0u;
            }
        }
        for (const sg::BodyPhysics& p : phys) {
            habitable += p.potentially_habitable ? 1u : 0u;  // planets and moons
        }
        bool has_giant = false;
        for (std::uint32_t k = 0; k < r.planet_count; ++k) {
            has_giant = has_giant || r.bodies[k].composition == "space:composition.gas_giant";
        }
        giant_by_class[ci] += has_giant ? 1.0 : 0.0;
        hz_by_class[ci] += in_hz > 0 ? 1.0 : 0.0;
        ph_by_class[ci] += habitable > 0 ? 1.0 : 0.0;
        hz.add(static_cast<double>(in_hz));
        ph.add(static_cast<double>(habitable));
        systems_with_ph += habitable > 0 ? 1u : 0u;
    }
    const std::uint64_t single_hash = combine(single);
    const double n = static_cast<double>(addrs.size());

    // ---- Pass 2: N threads, a different order ----
    const auto t0 = Clock::now();
    const std::vector<Hashes> multi = run_threads(universe, addrs, threads);
    const double multi_s = std::chrono::duration<double>(Clock::now() - t0).count();
    const std::uint64_t multi_hash = combine(multi);

    // ---- Pass 3: the pipeline at three scales. The stage API has no scale input; s goes to realize() only. ----
    const double scales[] = {1.0, 0.1, 0.01};
    std::uint64_t record_hash_at[3] = {}, realized_hash_at[3] = {};
    std::uint64_t realized_viol[3] = {};
    for (int si_ = 0; si_ < 3; ++si_) {
        std::uint64_t rh = veach::hash_id("records"), zh = veach::hash_id("realized");
        const sg::ScaleFactors s{scales[si_], scales[si_]};
        for (const sg::SystemAddress a : addrs) {
            const sg::StageContext ctx{&universe, a};
            const sg::SystemRecord r = sg::generate_system(ctx);
            const std::vector<sg::BodyPhysics> phys = sg::derive_all_body_physics(ctx, r);
            const sg::RealizedSystem real = sg::realize(r, phys, s);
            const sg::RealizedViolations rv = sg::validate_realized(r, real);
            realized_viol[si_] += rv.moon_hill + rv.moon_roche;
            rh = veach::hash(rh, sg::stage_hash(r), sg::stage_hash(phys));  // hashed after realization ran
            zh = veach::hash(zh, sg::realized_hash(real));
        }
        record_hash_at[si_] = rh;
        realized_hash_at[si_] = zh;
    }
    // Separate planet and orbit factors (ADR 0049's advanced setting): moon/Hill ratio moves by (s_p/s_o)^(1/3).
    const sg::ScaleFactors split[] = {{0.1, 0.01}, {0.01, 0.1}};
    std::uint64_t split_viol[2] = {};
    for (int k = 0; k < 2; ++k) {
        for (const sg::SystemAddress a : addrs) {
            const sg::StageContext ctx{&universe, a};
            const sg::SystemRecord r = sg::generate_system(ctx);
            const sg::RealizedViolations rv =
                sg::validate_realized(r, sg::realize(r, sg::derive_all_body_physics(ctx, r), split[k]));
            split_viol[k] += rv.moon_hill + rv.moon_roche;
        }
    }

    // ---- Report ----
    std::printf("# SP-05 system statistics: %llu systems, universe seed %llu, habitability bias %.2f\n",
                static_cast<unsigned long long>(addrs.size()), static_cast<unsigned long long>(seed), bias);
    std::printf("\n## Throughput\n\n| run | time (s) | systems/s | us/system |\n|---|---:|---:|---:|\n");
    std::printf("| S1 only, 1 thread | %.3f | %.0f | %.2f |\n", s1_s, n / s1_s, 1e6 * s1_s / n);
    std::printf("| B1 only, 1 thread | %.3f | %.0f | %.2f |\n", b1_s, n / b1_s, 1e6 * b1_s / n);
    std::printf("| S1 + B1, 1 thread | %.3f | %.0f | %.2f |\n", s1_s + b1_s, n / (s1_s + b1_s),
                1e6 * (s1_s + b1_s) / n);
    std::printf("| S1 + B1 + hashes, %u threads | %.3f | %.0f | %.2f |\n", threads, multi_s, n / multi_s,
                1e6 * multi_s / n);

    std::printf("\n## Hashes\n\n| check | value |\n|---|---|\n");
    std::printf("| S1+B1 over all systems, 1 thread, forward | 0x%016llx |\n",
                static_cast<unsigned long long>(single_hash));
    std::printf("| S1+B1 over all systems, %u threads, interleaved backward | 0x%016llx |\n", threads,
                static_cast<unsigned long long>(multi_hash));
    for (int k = 0; k < 3; ++k) {
        std::printf("| records after realizing at s = %g | 0x%016llx |\n", scales[k],
                    static_cast<unsigned long long>(record_hash_at[k]));
    }
    for (int k = 0; k < 3; ++k) {
        std::printf("| realized geometry at s = %g | 0x%016llx |\n", scales[k],
                    static_cast<unsigned long long>(realized_hash_at[k]));
    }
    const bool threads_equal = single_hash == multi_hash;
    const bool scales_equal = record_hash_at[0] == record_hash_at[1] && record_hash_at[1] == record_hash_at[2];
    const bool realized_differ =
        realized_hash_at[0] != realized_hash_at[1] && realized_hash_at[1] != realized_hash_at[2];
    std::printf("\nthreads equal: %s; records equal across s: %s; realized geometry differs across s: %s\n",
                threads_equal ? "yes" : "NO", scales_equal ? "yes" : "NO", realized_differ ? "yes" : "NO");
    failures += threads_equal && scales_equal && realized_differ ? 0 : 1;

    std::printf("\n## Violations (must be 0)\n\n| check | count |\n|---|---:|\n");
    const auto row = [](const char* name, std::uint64_t c) {
        std::printf("| %s | %llu |\n", name, static_cast<unsigned long long>(c));
    };
    row("adjacent planets < 10 mutual Hill radii (periapsis to apoapsis)", violations.planet_hill);
    row("planet periapsis inside the star's Roche limit or 2 stellar radii", violations.planet_star);
    row("moon periapsis inside the planet's fluid Roche limit", violations.moon_roche);
    row("moon apoapsis beyond 0.49 (1 - 1.03 e) Hill radii", violations.moon_hill);
    row("adjacent moons < 10 mutual Hill radii", violations.moon_spacing);
    row("systems with more than 255 generated bodies", violations.body_overflow);
    row("non-finite record fields", violations.non_finite);
    row("addresses that fail encode/decode/text round trip", address_failures);
    for (int k = 0; k < 3; ++k) {
        std::printf("| realized moons outside Roche..Hill at s_p = s_o = %g | %llu |\n", scales[k],
                    static_cast<unsigned long long>(realized_viol[k]));
    }
    failures +=
        violations.total() + address_failures + realized_viol[0] + realized_viol[1] + realized_viol[2] > 0 ? 1 : 0;
    std::printf(
        "\nInformational, separate factors: realized moon violations at s_p = 0.1, s_o = 0.01: %llu; at "
        "s_p = 0.01, s_o = 0.1: %llu\n",
        static_cast<unsigned long long>(split_viol[0]), static_cast<unsigned long long>(split_viol[1]));

    std::printf("\n## Summary\n\n| quantity | value |\n|---|---:|\n");
    std::printf("| planets | %llu (%.2f per system) |\n", static_cast<unsigned long long>(planets_total),
                static_cast<double>(planets_total) / n);
    std::printf("| moons | %llu (%.2f per planet) |\n", static_cast<unsigned long long>(moons_total),
                static_cast<double>(moons_total) / std::max(1.0, static_cast<double>(planets_total)));
    std::printf("| most bodies in one system | %llu (address limit 255) |\n",
                static_cast<unsigned long long>(max_bodies));
    std::printf("| most planets in one system | %llu (cap 30; %llu systems at the cap) |\n",
                static_cast<unsigned long long>(max_planets_seen), static_cast<unsigned long long>(capped));
    std::printf("| systems with a habitability anchor | %.2f %% |\n", 100.0 * static_cast<double>(anchors) / n);
    std::printf("| systems with >= 1 potentially habitable body | %.2f %% |\n",
                100.0 * static_cast<double>(systems_with_ph) / n);
    std::printf("| HZ planets tidally locked | %.1f %% |\n",
                100.0 * static_cast<double>(hz_locked) / std::max(1.0, static_cast<double>(hz_planets)));
    std::printf("| rocky planets keeping an atmosphere | %.1f %% |\n",
                100.0 * static_cast<double>(rocky_atm) / std::max(1.0, static_cast<double>(rocky)));
    std::printf(
        "\n### Per star class\n\n| class | systems | planets/system | %% with a gas giant | %% with an HZ planet | "
        "%% with a potentially habitable body |\n|---|---:|---:|---:|---:|---:|\n");
    for (std::size_t k = 0; k < 10; ++k) {
        const double sc = std::max(1.0, systems_by_class[k]);
        std::printf("| %s | %.0f | %.2f | %.1f | %.1f | %.1f |\n", star_class.labels[k].c_str(), systems_by_class[k],
                    planets_by_class[k] / sc, 100.0 * giant_by_class[k] / sc, 100.0 * hz_by_class[k] / sc,
                    100.0 * ph_by_class[k] / sc);
    }

    print_cat(star_class, out);
    print_cat(star_kind, out);
    print_hist(planets_per_system, out);
    print_cat(composition, out);
    print_hist(mass, out);
    print_hist(radius, out);
    print_hist(sma, out);
    print_hist(period, out);
    print_hist(ratio, out);
    print_hist(spacing, out);
    print_hist(teq, out);
    print_cat(temp_class, out);
    print_hist(hz, out);
    print_hist(ph, out);
    print_hist(moons_per_planet, out);
    hash_quality(seed);
    return failures == 0 ? 0 : 1;
}
