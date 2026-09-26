// Modified by vii from komaruworld/mocktail. See README "About this fork".
// Copyright 2026 Mocktail Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License")

#include "runtime/pump_stall_diagnostics.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uint64_t kMs = 1'000'000ULL;

PumpStallFrames Frames(std::initializer_list<const char*> names) {
  PumpStallFrames frames;
  for (const char* name : names) {
    frames.emplace_back(name);
  }
  return frames;
}

TEST(PumpStallTest, IgnoresStepsUnderTheThreshold) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  policy.minimum_sample_interval_ns = 0;
  recorder.Configure(policy);
  EXPECT_FALSE(recorder.ShouldSample(0));
  EXPECT_FALSE(recorder.ShouldSample(49 * kMs));
  EXPECT_TRUE(recorder.ShouldSample(50 * kMs));
  EXPECT_TRUE(recorder.ShouldSample(400 * kMs));
  // Every step is counted, sampled or not, so the ratio is meaningful.
  EXPECT_EQ(recorder.total_stalls(), 4U);
  EXPECT_EQ(recorder.total_sampled(), 2U);
}

TEST(PumpStallTest, TheDefaultCooldownAppliesBetweenSamples) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  // The shipped cooldown is 1 s, so two stalls in the same instant are one
  // sample. This is why a per-frame stall does not capture a stack per frame.
  recorder.Configure(policy);
  EXPECT_TRUE(recorder.ShouldSample(400 * kMs));
  EXPECT_FALSE(recorder.ShouldSample(400 * kMs));
  EXPECT_EQ(recorder.total_stalls(), 2U);
  EXPECT_EQ(recorder.total_sampled(), 1U);
}

TEST(PumpStallTest, CooldownStopsPerFrameSampling) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  // A stall that fires on every frame would otherwise capture every frame.
  policy.minimum_sample_interval_ns = 0;
  recorder.Configure(policy);
  EXPECT_TRUE(recorder.ShouldSample(100 * kMs));
  EXPECT_TRUE(recorder.ShouldSample(100 * kMs));
  EXPECT_EQ(recorder.total_sampled(), 2U);

  PumpStallRecorder limited;
  policy.minimum_sample_interval_ns = 30 * 1'000 * kMs;
  limited.Configure(policy);
  EXPECT_TRUE(limited.ShouldSample(100 * kMs));
  EXPECT_FALSE(limited.ShouldSample(100 * kMs));
  EXPECT_FALSE(limited.ShouldSample(100 * kMs));
  EXPECT_EQ(limited.total_sampled(), 1U);
  EXPECT_GT(limited.duplicates_suppressed(), 0U);
}

TEST(PumpStallTest, SameStackIsOneSignature) {
  const PumpStallFrames a = Frames({"mutex_lock", "guest_wait", "pump"});
  const PumpStallFrames b = Frames({"mutex_lock", "guest_wait", "pump"});
  EXPECT_EQ(PumpStallSignature(a), PumpStallSignature(b));

  const PumpStallFrames c = Frames({"read", "socket_wait", "pump"});
  EXPECT_NE(PumpStallSignature(a), PumpStallSignature(c));
  EXPECT_EQ(PumpStallSignature({}), "");
}

TEST(PumpStallTest, SignatureCoversTheTopFramesOnly) {
  // A deep engine stack must not make every call site its own signature, or
  // one stall fills the log with near-duplicates. The identifying frames are
  // at the top, so only those are compared.
  constexpr std::size_t kTopFrames = 6;
  PumpStallFrames first = Frames({"cond_wait", "frame_step"});
  PumpStallFrames second = Frames({"cond_wait", "frame_step"});
  // Identical for the top frames, then divergent all the way down.
  for (std::size_t index = 0; index < kTopFrames; ++index) {
    const std::string shared = "shared_" + std::to_string(index);
    first.emplace_back(shared);
    second.emplace_back(shared);
  }
  for (int index = 0; index < 40; ++index) {
    first.emplace_back("caller_a_" + std::to_string(index));
    second.emplace_back("caller_b_" + std::to_string(index));
  }
  EXPECT_EQ(PumpStallSignature(first), PumpStallSignature(second));

  // A difference at the very top is a different wait, which is the point.
  PumpStallFrames different = Frames({"read", "frame_step"});
  for (std::size_t index = 0; index < kTopFrames; ++index) {
    const std::string shared = "shared_" + std::to_string(index);
    different.emplace_back(shared);
  }
  for (int index = 0; index < 40; ++index) {
    different.emplace_back("caller_a_" + std::to_string(index));
  }
  EXPECT_NE(PumpStallSignature(first), PumpStallSignature(different));
}

TEST(PumpStallTest, RecordsAndDeduplicatesBySignature) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  policy.deduplicate_window_ns = 0;  // never suppress, to test the counter
  recorder.Configure(policy);
  recorder.ShouldSample(100 * kMs);
  const PumpStallFrames mutex = Frames({"mutex_lock", "guest_wait"});
  const std::string first = recorder.Record(mutex, 100 * kMs);
  EXPECT_FALSE(first.empty());
  EXPECT_NE(first.find("blocked 100 ms"), std::string::npos);
  EXPECT_NE(first.find("mutex_lock"), std::string::npos);
  EXPECT_EQ(recorder.signature_count(), 1U);

  recorder.ShouldSample(120 * kMs);
  const std::string again = recorder.Record(mutex, 120 * kMs);
  EXPECT_FALSE(again.empty());
  // A repeat says how many times the shape has been seen, which is what
  // separates a rare stall from a constant one.
  EXPECT_NE(again.find("2 times"), std::string::npos);
  EXPECT_EQ(recorder.signature_count(), 1U);
}

TEST(PumpStallTest, SuppressesARepeatInsideTheWindow) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  policy.deduplicate_window_ns = 60 * 1'000 * kMs;
  recorder.Configure(policy);
  const PumpStallFrames mutex = Frames({"mutex_lock", "guest_wait"});
  recorder.ShouldSample(100 * kMs);
  EXPECT_FALSE(recorder.Record(mutex, 100 * kMs).empty());
  recorder.ShouldSample(100 * kMs);
  // Same stack, still inside the window: counted, not printed again.
  EXPECT_TRUE(recorder.Record(mutex, 100 * kMs).empty());
  EXPECT_GE(recorder.duplicates_suppressed(), 1U);
  EXPECT_EQ(recorder.signature_count(), 1U);
}

TEST(PumpStallTest, KeepsDistinctShapesApart) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  policy.deduplicate_window_ns = 60 * 1'000 * kMs;
  policy.minimum_sample_interval_ns = 0;
  recorder.Configure(policy);
  recorder.ShouldSample(100 * kMs);
  EXPECT_FALSE(recorder.Record(Frames({"mutex_lock", "a"}), 100 * kMs).empty());
  recorder.ShouldSample(100 * kMs);
  // A different wait is new information and must be logged even though the
  // previous one is still inside its window.
  const std::string other = recorder.Record(Frames({"read", "b"}), 90 * kMs);
  EXPECT_FALSE(other.empty());
  EXPECT_NE(other.find("read"), std::string::npos);
  EXPECT_EQ(recorder.signature_count(), 2U);
}

TEST(PumpStallTest, CapsDistinctSignatures) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 50 * kMs;
  policy.minimum_sample_interval_ns = 0;
  policy.deduplicate_window_ns = 0;
  policy.maximum_signatures = 2;
  recorder.Configure(policy);
  for (int index = 0; index < 6; ++index) {
    recorder.ShouldSample(100 * kMs);
    // Past the cap a new shape is still logged rather than dropped, so the
    // log keeps showing what the thread is doing.
    const std::string name = "wait_" + std::to_string(index);
    PumpStallFrames frames{name, "x"};
    EXPECT_FALSE(recorder.Record(frames, 100 * kMs).empty());
  }
  EXPECT_EQ(recorder.signature_count(), 2U);
}

TEST(PumpStallTest, IgnoresAnEmptyStack) {
  PumpStallRecorder recorder;
  recorder.ShouldSample(100 * kMs);
  EXPECT_TRUE(recorder.Record({}, 100 * kMs).empty());
}

TEST(PumpStallTest, PolicyDefaultsAreUsable) {
  PumpStallRecorder recorder;
  PumpStallPolicy policy;
  policy.minimum_stall_ns = 0;
  policy.maximum_signatures = 0;
  policy.deduplicate_window_ns = 0;
  policy.minimum_sample_interval_ns = 0;
  recorder.Configure(policy);
  // A zero threshold would make every step a stall, including the empty polls
  // the pump makes at kilohertz rates, and a zero cap would drop every sample.
  // Both are clamped rather than honoured.
  EXPECT_FALSE(recorder.ShouldSample(0));
  EXPECT_TRUE(recorder.ShouldSample(1));
  PumpStallFrames frames = Frames({"a", "b"});
  for (int index = 0; index < 4; ++index) {
    recorder.ShouldSample(100 * kMs);
    EXPECT_FALSE(recorder.Record(frames, 100 * kMs).empty());
  }
  EXPECT_EQ(recorder.signature_count(), 1U);
}

TEST(PumpStallTest, ParsesAMapsLine) {
  AddressRegion region;
  ASSERT_TRUE(ParseMapsLine(
      "7f9c2a000000-7f9c2a021000 r-xp 00000000 08:01 1234 "
      "/home/you/.cache/mocktail/rbx_bin/libroblox.so",
      &region));
  EXPECT_EQ(region.start, 0x7f9c2a000000ULL);
  EXPECT_EQ(region.end, 0x7f9c2a021000ULL);
  EXPECT_EQ(region.base, 0x7f9c2a000000ULL);
  EXPECT_TRUE(region.executable);
  EXPECT_EQ(region.module, "libroblox.so");
  EXPECT_TRUE(region.Contains(reinterpret_cast<void*>(0x7f9c2a010000ULL)));
  EXPECT_FALSE(region.Contains(reinterpret_cast<void*>(0x7f9c2a030000ULL)));
}

TEST(PumpStallTest, MapsOffsetIsRelativeToTheImageBase) {
  AddressRegion region;
  // A later segment of the same image has a non-zero file offset, and the
  // frame's distance from the image base has to include it or two mappings of
  // one library would report different offsets for the same address.
  ASSERT_TRUE(ParseMapsLine(
      "55a000000000-55a000021000 r--p 00000000 08:01 99 /lib/libc.so.6",
      &region));
  ASSERT_EQ(region.base, 0x55a000000000ULL);
  ASSERT_TRUE(ParseMapsLine(
      "55a000021000-55a000123000 r-xp 000021000 08:01 99 /lib/libc.so.6",
      &region));
  EXPECT_EQ(region.base, 0x55a000021000ULL - 0x21000);
  EXPECT_TRUE(region.executable);
}

TEST(PumpStallTest, RejectsLinesThatAreNotMappings) {
  AddressRegion region;
  EXPECT_FALSE(ParseMapsLine("", &region));
  EXPECT_FALSE(ParseMapsLine("garbage", &region));
  EXPECT_FALSE(ParseMapsLine("7f9c2a000000 r-xp 0000 /lib/x.so", &region));
  // A reversed range contains nothing and must not be recorded.
  EXPECT_FALSE(ParseMapsLine(
      "7f9c2a021000-7f9c2a000000 r-xp 00000000 08:01 1 /lib/x.so",
      &region));
  EXPECT_FALSE(ParseMapsLine("7f9c2a0-7f9c2a0 r-xp 0 0 0 /lib/x.so",
                              &region));
  EXPECT_FALSE(ParseMapsLine(
      "7f9c2a000000-7f9c2a021000 r-xp 00000000 08:01 1 /lib/x.so",
      nullptr));
}

TEST(PumpStallTest, ANonExecutableMappingIsStillFound) {
  AddressRegion region;
  // Data mappings matter: a wait can be on a lock inside the guest's data
  // rather than its code.
  ASSERT_TRUE(ParseMapsLine(
      "7f9c30000000-7f9c30200000 rw-p 00000000 08:01 7 /lib/libc.so.6",
      &region));
  EXPECT_FALSE(region.executable);
  EXPECT_TRUE(region.Contains(reinterpret_cast<void*>(0x7f9c30100000ULL)));
  EXPECT_EQ(region.module, "libc.so.6");
}

TEST(PumpStallTest, ReadsThisProcessesOwnMappings) {
  const std::vector<AddressRegion> &regions = HostAddressRegions();
  // The pump's own image is mapped in any process that got this far, so its
  // absence would mean the parse is wrong rather than the map being unusual.
  bool found_self = false;
  for (const AddressRegion &region : regions) {
    if (region.module.find("pump_stall_diagnostics") != std::string::npos ||
        region.module.find("mocktail") != std::string::npos) {
      found_self = true;
      break;
    }
  }
  EXPECT_TRUE(found_self);
  // The main executable is always mapped too.
  bool found_executable = false;
  for (const AddressRegion &region : regions) {
    if (region.module.find("pump_stall_diagnostics_test") !=
        std::string::npos) {
      found_executable = true;
      break;
    }
  }
  EXPECT_TRUE(found_executable);
}

TEST(PumpStallTest, ResolvesAnAddressToItsImage) {
  const AddressRegion here =
      AddressRegion::For(reinterpret_cast<void*>(&ParseMapsLine));
  EXPECT_FALSE(here.module.empty());
  EXPECT_GT(here.offset_from_base, 0U);
  // An address in no mapping, such as one below every mapping, is reported as
  // unknown rather than attributed to whatever happens to be first.
  const AddressRegion nowhere = AddressRegion::For(reinterpret_cast<void*>(1));
  EXPECT_TRUE(nowhere.module.empty());
  EXPECT_EQ(nowhere.offset_from_base, 0U);
}

TEST(PumpStallTest, RecognisesThePumpsOwnFrames) {
  EXPECT_TRUE(IsPumpFrame("mocktail::runtime::ReportEnginePumpStall+0x40"));
  EXPECT_TRUE(IsPumpFrame("PumpRobloxMainThreadMessagesOnce(bool)"));
  EXPECT_TRUE(IsPumpFrame("mocktail::legacy::PumpStartupOwnerThread(void*)"));
  // A guest frame must not be mistaken for one of ours, or the trim would eat
  // the very frames that name the wait.
  EXPECT_FALSE(IsPumpFrame("libroblox.so+0x4a1b2c [pthread_cond_wait]"));
  EXPECT_FALSE(IsPumpFrame("main+0x50cf"));
}

TEST(PumpStallTest, TrimsThePumpsOwnFramesAndKeepsItsCaller) {
  // Shaped like a real unwind: capture helper, the pump, its caller, then the
  // frames that actually say where the thread is stuck.
  const PumpStallFrames unwind = Frames({
      "ReportEnginePumpStall",
      "PumpRobloxMainThreadMessagesOnce",
      "OwnedPthread::WaitFor",
      "libroblox.so+0x4a1b2c",
      "libc.so.6+0x111 [read]",
      "main",
  });
  PumpStallFrames identifying;
  std::string driven_from;
  ASSERT_TRUE(SplitPumpFrames(unwind, &identifying, &driven_from));
  EXPECT_EQ(driven_from, "OwnedPthread::WaitFor");
  ASSERT_EQ(identifying.size(), 3U);
  EXPECT_EQ(identifying[0], "libroblox.so+0x4a1b2c");
  EXPECT_EQ(identifying[2], "main");
}

TEST(PumpStallTest, TwoCallSitesToTheSameWaitStayDistinguishable) {
  // The same guest wait reached from the main loop and from a join loop is one
  // shape but two contexts, so the context is reported on its own line.
  const PumpStallFrames from_main = Frames({
      "ReportEnginePumpStall", "PumpRobloxMainThreadMessagesOnce", "main",
      "libroblox.so+0x4a1b2c"});
  const PumpStallFrames from_join = Frames({
      "ReportEnginePumpStall", "PumpRobloxMainThreadMessagesOnce",
      "OwnedPthread::WaitFor", "libroblox.so+0x4a1b2c"});
  PumpStallFrames a;
  PumpStallFrames b;
  std::string from_a;
  std::string from_b;
  ASSERT_TRUE(SplitPumpFrames(from_main, &a, &from_a));
  ASSERT_TRUE(SplitPumpFrames(from_join, &b, &from_b));
  EXPECT_EQ(a, b);
  EXPECT_EQ(PumpStallSignature(a), PumpStallSignature(b));
  EXPECT_EQ(from_a, "main");
  EXPECT_EQ(from_b, "OwnedPthread::WaitFor");
}

TEST(PumpStallTest, AStackWithNothingBelowThePumpIsReportedAsThin) {
  const PumpStallFrames unwind =
      Frames({"ReportEnginePumpStall", "PumpRobloxMainThreadMessagesOnce",
              "main"});
  PumpStallFrames identifying;
  std::string driven_from;
  EXPECT_FALSE(SplitPumpFrames(unwind, &identifying, &driven_from));
  EXPECT_TRUE(identifying.empty());
  EXPECT_EQ(driven_from, "main");

  const std::string line = FormatPumpStallWithCaller(
      driven_from, PumpStallFrames{std::string(kNoIdentifyingFrames)},
      63 * kMs);
  EXPECT_NE(line.find("blocked 63 ms"), std::string::npos);
  EXPECT_NE(line.find("driven from main"), std::string::npos);
  EXPECT_NE(line.find(kNoIdentifyingFrames), std::string::npos);
}

TEST(PumpStallTest, FormatNamesTheCallerAndTheFrames) {
  const std::string line = FormatPumpStallWithCaller(
      "OwnedPthread::WaitFor",
      Frames({"libroblox.so+0x4a1b2c", "libc.so.6+0x111 [read]"}), 347 * kMs);
  EXPECT_NE(line.find("347 ms"), std::string::npos);
  EXPECT_NE(line.find("driven from OwnedPthread::WaitFor"), std::string::npos);
  EXPECT_NE(line.find("#0 libroblox.so+0x4a1b2c"), std::string::npos);
  EXPECT_NE(line.find("#1 libc.so.6"), std::string::npos);
  // The pump's own frames must not be printed back at the reader.
  EXPECT_EQ(line.find("ReportEnginePumpStall"), std::string::npos);
}

TEST(PumpStallTest, FormatNamesTheWaitAndTheCount) {
  const std::string line =
      FormatPumpStall("sig", Frames({"cond_wait", "frame_step"}), 347 * kMs,
                      4, 4);
  EXPECT_NE(line.find("347 ms"), std::string::npos);
  EXPECT_NE(line.find("no CPU"), std::string::npos);
  EXPECT_NE(line.find("4 times"), std::string::npos);
  EXPECT_NE(line.find("#0 cond_wait"), std::string::npos);
  EXPECT_NE(line.find("#1 frame_step"), std::string::npos);
  // The first occurrence carries no count, which is what distinguishes it.
  EXPECT_EQ(
      FormatPumpStall("sig", Frames({"cond_wait"}), 100 * kMs, 1, 1)
          .find("times"),
      std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
