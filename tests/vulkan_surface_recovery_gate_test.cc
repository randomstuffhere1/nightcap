#include "window/vulkan_surface_recovery_gate.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
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

  gate.NotifyOutOfDate();
  EXPECT_FALSE(gate.TakeRebindRequest(
      kFirstRequestTicks + VulkanSurfaceRecoveryGate::kRetryIntervalNs - 1,
      &request));
  ASSERT_TRUE(gate.TakeRebindRequest(
      kFirstRequestTicks + VulkanSurfaceRecoveryGate::kRetryIntervalNs,
      &request));
  EXPECT_EQ(request.observed_error_count, 2u);
  EXPECT_EQ(request.coalesced_error_count, 1u);
  EXPECT_TRUE(request.retry);
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

// The TEMPORARY unthrottled override. This exists so the one-second retry
// interval can be eliminated without a rebuild while it is being ruled out as
// the cause of a reported one-second resize. If it turns out not to be the
// cause -- which the evidence so far suggests, since this gate logs zero
// out-of-date results during a resize -- the override and this test both go.
TEST(VulkanSurfaceRecoveryGateTest, DefaultIntervalIsTheFullSecond) {
  EXPECT_EQ(VulkanSurfaceRecoveryGate::kRetryIntervalNs, 1000000000ULL);
  // Off unless the environment asks for it, so a stale value in the
  // environment cannot silently change the gate for an unrelated run.
  const char* override_value =
      std::getenv("MOCKTAIL_SURFACE_RECOVERY_UNTHROTTLED");
  if (override_value == nullptr || override_value[0] == '\0' ||
      std::strcmp(override_value, "0") == 0) {
    EXPECT_EQ(VulkanSurfaceRecoveryGate::RetryIntervalNs(),
              VulkanSurfaceRecoveryGate::kRetryIntervalNs);
  } else {
    EXPECT_EQ(VulkanSurfaceRecoveryGate::RetryIntervalNs(), 0u);
  }
}

}  // namespace
}  // namespace window
}  // namespace mocktail
