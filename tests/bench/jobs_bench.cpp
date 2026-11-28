// veach::JobSystem scaling (ADR 0073, VCH-015).
//   build/release/tests/veach-bench-jobs [--items N] [--grain G]
// The canonical workload of tests/jobs_tests.cpp ([jobs.AC1]) at 10^7 items: veach::hash per item, chained per chunk,
// chunks combined in index order. For 1, 2, 4, ... threads up to the default plus one: best of 5 runs in ms, the
// speedup over one thread, and the result, which must be the same on every line. Exit code 1 if it is not.

#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {

std::uint64_t run(veach::JobSystem& jobs, std::size_t items, std::size_t grain) {
    return jobs.parallel_reduce(
        "bench", items, grain, std::uint64_t{0},
        [](std::size_t begin, std::size_t end) {
            std::uint64_t h = 0;
            for (std::size_t i = begin; i < end; ++i) {
                h = veach::hash(h, veach::hash(0x5EED, i));
            }
            return h;
        },
        [](std::uint64_t acc, std::uint64_t part) { return veach::hash(acc, part); });
}

std::size_t arg(int argc, char** argv, std::string_view flag, std::size_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) {
            std::size_t value = fallback;
            const std::string_view text = argv[i + 1];
            std::from_chars(text.data(), text.data() + text.size(), value);
            return value;
        }
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t items = arg(argc, argv, "--items", 10'000'000);
    const std::size_t grain = arg(argc, argv, "--grain", 16'384);
    std::vector<std::uint32_t> counts;
    for (std::uint32_t t = 1; t < veach::default_job_threads(); t *= 2) {
        counts.push_back(t);
    }
    counts.push_back(veach::default_job_threads());
    counts.push_back(veach::default_job_threads() + 1);
    std::printf("%zu items, grain %zu (%zu chunks)\n", items, grain, veach::JobSystem::chunk_count(items, grain));
    std::printf("threads  best ms  speedup  result\n");
    double base = 0.0;
    std::uint64_t expected = 0;
    bool same = true;
    for (const std::uint32_t threads : counts) {
        veach::JobSystem jobs(threads);
        double best = 1e30;
        std::uint64_t result = 0;
        for (int rep = 0; rep < 5; ++rep) {
            const auto start = std::chrono::steady_clock::now();
            result = run(jobs, items, grain);
            const std::chrono::duration<double, std::milli> ms = std::chrono::steady_clock::now() - start;
            best = std::min(best, ms.count());
        }
        if (threads == 1) {
            base = best;
            expected = result;
        }
        same = same && result == expected;
        std::printf("%7u  %7.3f  %6.2fx  %016llx\n", threads, best, base / best,
                    static_cast<unsigned long long>(result));
    }
    std::printf(same ? "results identical\n" : "RESULTS DIFFER\n");
    return same ? 0 : 1;
}
