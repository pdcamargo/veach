// The job system (docs/specs/job-system/spec.md, ADR 0073).

#include "support/test_support.hpp"

#include <veach/core/dmath.hpp>
#include <veach/core/hash.hpp>
#include <veach/core/jobs.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using namespace veach;

namespace {

constexpr std::size_t items = 1'000'000;
constexpr std::uint64_t seed = 0x5EED;

struct Canonical {
    std::uint64_t chain = 0;  ///< Hash chain: depends on the order of every item and every chunk.
    double sum = 0.0;         ///< Floating-point sum: not associative, so it also depends on the order.
    bool operator==(const Canonical&) const = default;
};

/// The canonical workload: hash 10^6 items with veach::hash, reduced in index order.
Canonical canonical(JobSystem& jobs, std::size_t grain) {
    return jobs.parallel_reduce(
        "canonical", items, grain, Canonical{},
        [](std::size_t begin, std::size_t end) {
            Canonical part;
            for (std::size_t i = begin; i < end; ++i) {
                const std::uint64_t h = veach::hash(seed, i);
                part.chain = veach::hash(part.chain, h);
                part.sum += unit_double(h) - 0.5;
            }
            return part;
        },
        [](Canonical acc, Canonical part) {
            return Canonical{veach::hash(acc.chain, part.chain), acc.sum + part.sum};
        });
}

/// The same reduction written as a plain loop over the same chunks: what every thread count must reproduce.
Canonical reference(std::size_t grain) {
    Canonical acc;
    for (std::size_t begin = 0; begin < items; begin += grain) {
        Canonical part;
        for (std::size_t i = begin; i < std::min(items, begin + grain); ++i) {
            const std::uint64_t h = veach::hash(seed, i);
            part.chain = veach::hash(part.chain, h);
            part.sum += unit_double(h) - 0.5;
        }
        acc = Canonical{veach::hash(acc.chain, part.chain), acc.sum + part.sum};
    }
    return acc;
}

std::vector<std::uint32_t> thread_counts() {
    std::vector<std::uint32_t> counts{1, 2, 3, default_job_threads(), 2 * default_job_threads()};
    std::ranges::sort(counts);
    counts.erase(std::ranges::unique(counts).begin(), counts.end());
    return counts;
}

}  // namespace

TEST_CASE("[jobs.AC1] the canonical workload gives the same bits on 1, 2 and N threads") {
    constexpr std::size_t grain = 4096;
    const Canonical expected = reference(grain);
    for (const std::uint32_t threads : thread_counts()) {
        CAPTURE(threads);
        JobSystem jobs(threads);
        CHECK(jobs.thread_count() == threads);
        const Canonical got = canonical(jobs, grain);
        CHECK(got.chain == expected.chain);
        CHECK(std::bit_cast<std::uint64_t>(got.sum) == std::bit_cast<std::uint64_t>(expected.sum));

        // parallel_for into indexed slots.
        std::vector<std::uint64_t> slots(items);
        jobs.parallel_for("slots", items, 1000, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                slots[i] = veach::hash(seed, i);
            }
        });
        std::uint64_t chain = 0;
        for (const std::uint64_t h : slots) {
            chain = veach::hash(chain, h);
        }
        std::uint64_t expected_chain = 0;
        for (std::size_t i = 0; i < items; ++i) {
            expected_chain = veach::hash(expected_chain, veach::hash(seed, i));
        }
        CHECK(chain == expected_chain);
    }
    // The cut is part of the result: another grain changes the float sum. The thread count never does.
    CHECK(reference(1000).sum != expected.sum);
}

TEST_CASE("[jobs.AC2] chunks are fixed by count and grain, not by the thread count") {
    const auto cut = [](JobSystem& jobs, std::size_t count, std::size_t grain) {
        std::vector<std::pair<std::size_t, std::size_t>> ranges(JobSystem::chunk_count(count, grain));
        jobs.parallel_for("cut", count, grain,
                          [&](std::size_t begin, std::size_t end) { ranges[begin / grain] = {begin, end}; });
        return ranges;
    };
    JobSystem one(1);
    const auto expected = cut(one, 10'007, 100);
    REQUIRE(expected.size() == 101);
    CHECK(expected.front() == std::pair<std::size_t, std::size_t>{0, 100});
    CHECK(expected.back() == std::pair<std::size_t, std::size_t>{10'000, 10'007});
    for (const std::uint32_t threads : thread_counts()) {
        CAPTURE(threads);
        JobSystem jobs(threads);
        CHECK(cut(jobs, 10'007, 100) == expected);
    }
    CHECK(JobSystem::chunk_count(0, 7) == 0);
    CHECK(JobSystem::chunk_count(7, 7) == 1);
    CHECK(JobSystem::chunk_count(8, 7) == 2);
    int calls = 0;
    one.parallel_for("empty", 0, 16, [&](std::size_t, std::size_t) { ++calls; });
    CHECK(calls == 0);
    CHECK(one.parallel_reduce(
              "empty", 0, 16, 42, [](std::size_t, std::size_t) { return 1; }, [](int a, int b) { return a + b; }) ==
          42);
}

TEST_CASE("[jobs.AC3] jobs run nested parallel_for and task groups, and waits return when all are done") {
    JobSystem jobs(4);
    // A parallel_for whose chunks each run a parallel_for.
    std::vector<std::uint64_t> grid(256 * 256);
    jobs.parallel_for("rows", 256, 8, [&](std::size_t row_begin, std::size_t row_end) {
        for (std::size_t row = row_begin; row < row_end; ++row) {
            jobs.parallel_for("cells", 256, 32, [&](std::size_t begin, std::size_t end) {
                for (std::size_t col = begin; col < end; ++col) {
                    grid[row * 256 + col] = veach::hash(seed, row, col);
                }
            });
        }
    });
    bool all = true;
    for (std::size_t i = 0; i < grid.size(); ++i) {
        all = all && grid[i] == veach::hash(seed, i / 256, i % 256);
    }
    CHECK(all);

    // A task group whose jobs queue more jobs into the same group (what Jolt does).
    std::atomic<int> ran{0};
    {
        TaskGroup group(jobs);
        for (int i = 0; i < 16; ++i) {
            group.run("parent", [&] {
                ++ran;
                for (int j = 0; j < 4; ++j) {
                    group.run("child", [&] { ++ran; });
                }
            });
        }
        group.wait();
        CHECK(ran.load() == 16 * 5);
        group.run("again", [&] { ++ran; });
    }  // the destructor waits
    CHECK(ran.load() == 16 * 5 + 1);

    // wait_idle() drains jobs that nobody has waited for yet.
    std::optional<TaskGroup> pending(std::in_place, jobs);
    std::atomic<int> slow{0};
    for (int i = 0; i < 8; ++i) {
        pending->run("slow", [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ++slow;
        });
    }
    jobs.wait_idle();
    CHECK(slow.load() == 8);
    pending.reset();
}

TEST_CASE("[jobs.AC4] jobs come from --jobs and veach.project.json, one thread runs everything on the main thread") {
    test::TempProject project("jobs-config");
    project.write("veach.project.json", R"({"name": "jobs", "jobs": 3})");
    const std::string dir = project.path().string();
    const std::vector<std::string_view> from_file{"--project", dir};
    CHECK(resolve_config({.name = "t"}, from_file)->project.jobs == 3);
    const std::vector<std::string_view> from_cli{"--project", dir, "--jobs", "1"};
    CHECK(resolve_config({.name = "t"}, from_cli)->project.jobs == 1);
    const std::vector<std::string_view> inline_value{"--jobs=5"};
    CHECK(resolve_config({.name = "t"}, inline_value)->project.jobs == 5);
    CHECK(ProjectConfig{}.jobs == 0);

    const std::vector<std::string_view> too_many{"--jobs", "100000"};
    auto bad = resolve_config({.name = "t"}, too_many);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code == ErrorCode::cli_invalid_value);
    CHECK(bad.error().hint.find("256") != std::string::npos);
    const std::vector<std::string_view> not_a_number{"--jobs", "many"};
    CHECK(resolve_config({.name = "t"}, not_a_number).error().code == ErrorCode::cli_invalid_value);
    project.write("veach.project.json", R"({"jobs": 1000})");
    CHECK(resolve_config({.name = "t"}, from_file).error().message.find("jobs is 1000") != std::string::npos);

    CHECK(default_job_threads() >= 1);
    CHECK(default_job_threads() == std::max(1u, std::thread::hardware_concurrency() - 1));

    JobSystem single(1);
    CHECK(single.worker_count() == 0);
    CHECK(single.on_job_thread());
    CHECK(single.thread_index() == 0);
    const std::thread::id main = std::this_thread::get_id();
    std::atomic<bool> elsewhere{false};
    single.parallel_for("main only", 1000, 10,
                        [&](std::size_t, std::size_t) { elsewhere = elsewhere || std::this_thread::get_id() != main; });
    {
        TaskGroup group(single);
        group.run("main only", [&] { elsewhere = elsewhere || std::this_thread::get_id() != main; });
    }
    CHECK_FALSE(elsewhere.load());

    // With workers, jobs do leave the main thread, and each runs on a thread the job system owns.
    JobSystem many(4);
    std::atomic<bool> off_main{false};
    std::atomic<bool> unowned{false};
    many.parallel_for("spread", 64, 1, [&](std::size_t, std::size_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        off_main = off_main || std::this_thread::get_id() != main;
        unowned = unowned || !many.on_job_thread() || many.thread_index() >= many.thread_count();
    });
    CHECK(off_main.load());
    CHECK_FALSE(unowned.load());
    std::atomic<bool> outside{true};
    std::thread probe([&] { outside = many.on_job_thread(); });  // a thread the job system does not own
    probe.join();
    CHECK_FALSE(outside.load());
}

TEST_CASE("[jobs.AC5] the engine provides the job system from 'jobs' and drains it before tearing down the world") {
    test::LogCapture logs;
    test::TempProject project("jobs-engine");
    EngineConfig config = test::headless_config(project.path(), {}, 1);
    config.project.jobs = 3;
    Engine engine(std::move(config));
    REQUIRE(engine.init().has_value());
    REQUIRE(engine.start().has_value());
    JobSystem& jobs = engine.services().get<JobSystem>();
    CHECK(jobs.thread_count() == 3);
    CHECK(&services(engine.world()).get<JobSystem>() == &jobs);
    CHECK(logs.contains("init jobs (3 threads: main + 2 workers)"));
    static_cast<void>(engine.frame());

    // Jobs still running at shutdown finish before the world goes away.
    std::optional<TaskGroup> pending(std::in_place, jobs);
    std::atomic<int> done{0};
    for (int i = 0; i < 6; ++i) {
        pending->run("slow", [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ++done;
        });
    }
    std::optional<int> done_at_world_teardown;
    const auto sink = log::add_sink([&](const log::Record& r) {
        if (r.message == "shutdown ecs") {
            done_at_world_teardown = done.load();
        } else if (r.message == "shutdown platform") {
            pending.reset();  // a group must not outlive the job system, which goes next
        }
    });
    engine.shutdown();
    log::remove_sink(sink);
    CHECK(done_at_world_teardown == 6);
    std::vector<std::string> steps;
    for (const std::string& m : logs.messages("engine")) {
        if (m.starts_with("init ") || m.starts_with("shutdown ")) {
            steps.push_back(m.substr(0, m.find(" (")));
        }
    }
    REQUIRE(steps.size() >= 4);
    CHECK(steps[0] == "init core");
    CHECK(steps[1] == "init jobs");
    CHECK(steps[steps.size() - 2] == "shutdown jobs");
    CHECK(steps.back() == "shutdown core");
}

TEST_CASE("[jobs.AC6] canonical floating point gives the same bits on the workers as on the main thread") {
    // The FP environment (round to nearest, no flush to zero) is per thread; workers must match the main thread.
    using Sums = std::array<double, 2>;
    const auto work = [](std::size_t begin, std::size_t end) {
        Sums acc{0.0, 0.0};
        for (std::size_t i = begin; i < end; ++i) {
            const double x = unit_double(veach::hash(seed, i)) * 8.0 - 4.0;
            acc[0] += dmath::sin(x) * dmath::exp(x);
            acc[1] += 1e-310 * x;  // subnormal: a thread that flushes to zero would give 0
        }
        return acc;
    };
    const auto add = [](Sums a, Sums b) {
        return Sums{a[0] + b[0], a[1] + b[1]};
    };
    JobSystem one(1);
    const Sums expected = one.parallel_reduce("fp", 100'000, 1000, Sums{}, work, add);
    REQUIRE(expected[1] != 0.0);
    for (const std::uint32_t threads : thread_counts()) {
        CAPTURE(threads);
        JobSystem jobs(threads);
        const Sums got = jobs.parallel_reduce("fp", 100'000, 1000, Sums{}, work, add);
        CHECK(std::bit_cast<std::uint64_t>(got[0]) == std::bit_cast<std::uint64_t>(expected[0]));
        CHECK(std::bit_cast<std::uint64_t>(got[1]) == std::bit_cast<std::uint64_t>(expected[1]));
    }
}
