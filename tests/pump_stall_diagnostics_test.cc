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
