#include "window/vulkan_surface_recovery_gate.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace mocktail {
namespace window {
namespace {

TEST(VulkanSurfaceRecoveryGateTest, IgnoresEvidenceOutsideActiveLifetime) {
  VulkanSurfaceRecoveryGate gate;
  VulkanSurfaceRecoveryRequest request;

  gate.NotifyOutOfDate();
  EXPECT_FALSE(gate.TakeRebindRequest(1, &request));

  ASSERT_TRUE(gate.Activate());
  EXPECT_FALSE(gate.Activate());
  gate.NotifyOutOfDate();
  gate.Deactivate();

  EXPECT_FALSE(gate.TakeRebindRequest(2, &request));
}

TEST(VulkanSurfaceRecoveryGateTest, CoalescesFirstOutOfDateBurst) {
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());

  gate.NotifyOutOfDate();
  gate.NotifyOutOfDate();
  gate.NotifyOutOfDate();

  VulkanSurfaceRecoveryRequest request;
  ASSERT_TRUE(gate.TakeRebindRequest(100, &request));
  EXPECT_EQ(request.observed_error_count, 3u);
  EXPECT_EQ(request.coalesced_error_count, 3u);
  EXPECT_FALSE(request.retry);
  EXPECT_FALSE(gate.TakeRebindRequest(101, &request));
}

TEST(VulkanSurfaceRecoveryGateTest, RateLimitsPersistentOutOfDateRetries) {
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyOutOfDate();

  VulkanSurfaceRecoveryRequest request;
  constexpr uint64_t kFirstRequestTicks = 1000;
  ASSERT_TRUE(gate.TakeRebindRequest(kFirstRequestTicks, &request));

  // The first retry is allowed after one frame, not after the old flat second:
  // this is the interval that made a resize take a second to respond, because
  // during a resize no present succeeds and so nothing else opens the gate.
  gate.NotifyOutOfDate();
  EXPECT_FALSE(gate.TakeRebindRequest(
      kFirstRequestTicks + VulkanSurfaceRecoveryGate::kInitialRetryIntervalNs - 1,
      &request));
  ASSERT_TRUE(gate.TakeRebindRequest(
      kFirstRequestTicks + VulkanSurfaceRecoveryGate::kInitialRetryIntervalNs,
      &request));
  EXPECT_EQ(request.observed_error_count, 2u);
  EXPECT_EQ(request.coalesced_error_count, 1u);
  EXPECT_TRUE(request.retry);
}

TEST(VulkanSurfaceRecoveryGateTest, BacksOffToTheCapForAPersistentlyBrokenSurface) {
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());

  VulkanSurfaceRecoveryRequest request;
  uint64_t now = 1000;
  gate.NotifyOutOfDate();
  ASSERT_TRUE(gate.TakeRebindRequest(now, &request));
  EXPECT_FALSE(request.retry) << "the first grant of a session is not a retry";

  // Each failed attempt doubles the interval, so the gate settles at the cap
  // rather than staying at frame rate. The schedule is 16ms, 32ms, 64ms, ...
  // capped at 1s.
  //
  // Every attempt needs fresh out-of-date evidence: the gate records what it
  // has handled, so a retry with no new report is a duplicate and is refused
  // regardless of elapsed time.
  uint64_t interval = VulkanSurfaceRecoveryGate::kInitialRetryIntervalNs;
  for (int attempt = 0; interval < VulkanSurfaceRecoveryGate::kRetryIntervalNs;
       ++attempt) {
    gate.NotifyOutOfDate();
    now += interval;
    ASSERT_TRUE(gate.TakeRebindRequest(now, &request))
        << "retry " << attempt << " was refused after " << interval << " ns";
    EXPECT_TRUE(request.retry);
    interval *= 2;
    ASSERT_LT(attempt, 32) << "the backoff never reached the cap";
  }

  // Capped: refused just short of a second, allowed at a second.
  const uint64_t capped = VulkanSurfaceRecoveryGate::kRetryIntervalNs;
  gate.NotifyOutOfDate();
  EXPECT_FALSE(gate.TakeRebindRequest(now + capped - 1, &request));
  ASSERT_TRUE(gate.TakeRebindRequest(now + capped, &request));
}

TEST(VulkanSurfaceRecoveryGateTest, DuplicateEvidenceIsRefusedRegardlessOfTime) {
  // The coalescing half of the contract: a rebind that already handled every
  // reported out-of-date must not be redone just because time has passed. This
  // is what stops the gate rebinding once per tick when a surface is healthy.
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyOutOfDate();

  VulkanSurfaceRecoveryRequest request;
  ASSERT_TRUE(gate.TakeRebindRequest(1000, &request));

  EXPECT_FALSE(gate.TakeRebindRequest(
      1000 + VulkanSurfaceRecoveryGate::kRetryIntervalNs, &request))
      << "a retry with no new out-of-date evidence should be refused";
}

TEST(VulkanSurfaceRecoveryGateTest, ResizeConvergesWithoutWaitingForTheCap) {
  // The reported symptom: a resize took about a second to respond, because the
  // retry interval was a flat second and nothing else could open the gate while
  // presents were failing. This is the sequence a resize produces.
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());

  VulkanSurfaceRecoveryRequest request;
  uint64_t now = 1000;
  for (int frame = 0; frame < 6; ++frame) {
    gate.NotifyOutOfDate();  // each present attempt fails while out of date
    now += VulkanSurfaceRecoveryGate::kInitialRetryIntervalNs;
    const bool rebound = gate.TakeRebindRequest(now, &request);
    if (rebound) {
      // The guest rebuilt and presented again.
      gate.NotifyPresent();
    }
  }

  // Well inside the old one-second interval.
  EXPECT_LT(now - 1000, VulkanSurfaceRecoveryGate::kRetryIntervalNs);
}

TEST(VulkanSurfaceRecoveryGateTest,
     SuccessfulPresentAllowsImmediateIndependentRecovery) {
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyOutOfDate();

  VulkanSurfaceRecoveryRequest request;
  ASSERT_TRUE(gate.TakeRebindRequest(1000, &request));
  gate.NotifyPresent();
  gate.NotifyOutOfDate();

  ASSERT_TRUE(gate.TakeRebindRequest(1001, &request));
  EXPECT_EQ(request.observed_error_count, 2u);
  EXPECT_EQ(request.coalesced_error_count, 1u);
  EXPECT_FALSE(request.retry);
}

TEST(VulkanSurfaceRecoveryGateTest, AcceptsConcurrentRenderThreadEvidence) {
  VulkanSurfaceRecoveryGate gate;
  ASSERT_TRUE(gate.Activate());

  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&gate]() {
      for (int notification = 0; notification < 1000; ++notification) {
        gate.NotifyOutOfDate();
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  VulkanSurfaceRecoveryRequest request;
  ASSERT_TRUE(gate.TakeRebindRequest(100, &request));
  EXPECT_EQ(request.observed_error_count, 4000u);
  EXPECT_EQ(request.coalesced_error_count, 4000u);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
