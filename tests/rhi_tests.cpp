#include "support/test_support.hpp"

#include <veach/rhi/submission_tracker.hpp>

#include <doctest/doctest.h>

using namespace veach;
using rhi::SubmissionId;

TEST_CASE("[frames.AC1] frame N waits only for frame N - frames_in_flight") {
    rhi::SubmissionTracker tracker(2);
    SubmissionId wait;

    CHECK(tracker.begin_frame(wait).slot == 1);  // frame 1
    CHECK(wait.value == 0);
    const SubmissionId f1 = tracker.next_submission();
    tracker.end_frame();

    CHECK(tracker.begin_frame(wait).slot == 0);  // frame 2: slot 0 never used, no wait
    CHECK(wait.value == 0);
    const SubmissionId f2 = tracker.next_submission();
    tracker.end_frame();

    static_cast<void>(tracker.begin_frame(wait));  // frame 3 reuses frame 1's slot: waits for frame 1, not 2
    CHECK(wait.value == f1.value);
    tracker.mark_completed(f1);
    static_cast<void>(tracker.next_submission());
    tracker.end_frame();

    static_cast<void>(tracker.begin_frame(wait));  // frame 4 reuses frame 2's slot
    CHECK(wait.value == f2.value);

    rhi::SubmissionTracker single(1);  // one frame in flight: every frame waits for the previous one
    static_cast<void>(single.begin_frame(wait));
    const SubmissionId s1 = single.next_submission();
    single.end_frame();
    static_cast<void>(single.begin_frame(wait));
    CHECK(wait.value == s1.value);
}

TEST_CASE("[frames.AC2] destroyed resources are released only after the GPU is done with them") {
    rhi::SubmissionTracker tracker(2);
    std::vector<int> released;

    tracker.retire([&] { released.push_back(0); });  // nothing in flight: released immediately
    CHECK(released == std::vector<int>{0});

    const SubmissionId a = tracker.next_submission();
    tracker.retire([&] { released.push_back(1); });  // may be in use by a
    const SubmissionId b = tracker.next_submission();
    tracker.retire([&] { released.push_back(2); });  // may be in use by a or b
    CHECK(tracker.pending_releases() == 2);

    tracker.collect();
    CHECK(released.size() == 1);

    tracker.mark_completed(a);
    tracker.collect();
    CHECK(released == std::vector<int>{0, 1});

    tracker.mark_completed(SubmissionId{1});  // stale reports never move the watermark back
    CHECK(tracker.completed().value == a.value);
    tracker.mark_completed(b);
    tracker.collect();
    CHECK(released == std::vector<int>{0, 1, 2});
}

TEST_CASE("[frames.AC3] the null device runs frames and kills destroyed handles at once") {
    auto device = std::move(*rhi::create_device({.frames_in_flight = 3}));
    CHECK(device->frames_in_flight() == 3);
    for (int i = 0; i < 5; ++i) {
        const rhi::FrameInfo frame = device->begin_frame();
        CHECK(frame.index == static_cast<std::uint64_t>(i + 1));
        CHECK(frame.slot == frame.index % 3);
        auto buffer = device->create_buffer({.size = 64, .memory = rhi::MemoryType::upload, .debug_name = "per-frame"});
        REQUIRE(buffer.has_value());
        CHECK(device->map(*buffer) != nullptr);
        device->submit(device->begin_commands("frame"), {});
        device->destroy(*buffer);
        CHECK(device->map(*buffer) == nullptr);  // the handle is dead immediately
        device->end_frame();
    }
}

TEST_CASE("[frames.AC4] frames_in_flight is validated and reaches the device") {
    const std::vector<std::string_view> args{"--headless"};
    auto config = resolve_config({.name = "t"}, args);
    REQUIRE(config.has_value());
    CHECK(config->project.frames_in_flight == 2);

    test::TempProject project("frames");
    project.write("veach.project.json", R"({"frames_in_flight": 9})");
    const std::string dir = project.path().string();
    const std::vector<std::string_view> bad{"--project", dir};
    auto rejected = resolve_config({.name = "t"}, bad);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().message.find("between 1 and 4") != std::string::npos);
}
