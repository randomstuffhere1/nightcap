#ifndef MOCKTAIL_WINDOW_VULKAN_SURFACE_RECOVERY_GATE_H_
#define MOCKTAIL_WINDOW_VULKAN_SURFACE_RECOVERY_GATE_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mocktail {
namespace window {

struct VulkanSurfaceRecoveryRequest {
  uint64_t observed_error_count = 0;
  uint64_t coalesced_error_count = 0;
  bool retry = false;
};

// Transfers out-of-date WSI evidence from a Vulkan render thread to the SDL
// main thread. A burst is coalesced into one rebind, while a swapchain that
// remains invalid can request another rebind after a bounded retry interval.
class VulkanSurfaceRecoveryGate final {
 public:
  static constexpr uint64_t kRetryIntervalNs = 1000000000ULL;

  // TEMPORARY diagnostic override, not a fix.
  //
  // MOCKTAIL_SURFACE_RECOVERY_UNTHROTTLED=1 removes the retry interval
  // entirely, so any newly reported out-of-date result rebinds immediately
  // instead of waiting out the remaining second.
  //
  // This exists because a resize was reported as taking about a second, and the
  // interval is the only one-second quantity in the surface path -- so it is
  // worth being able to eliminate it without a rebuild. Evidence so far says it
  // will change nothing: across a resize session this gate logged zero
  // out-of-date results, because the resize reaches the guest as a plain
  // surface rebind (141 events in, 141 delivered, 2 us each) and never goes
  // through WSI recovery. If this makes no difference, the interval is not the
  // cause and the toggle should be deleted rather than left in.
  static uint64_t RetryIntervalNs() {
    static const uint64_t interval = [] {
      const char* value = std::getenv("MOCKTAIL_SURFACE_RECOVERY_UNTHROTTLED");
      const bool unthrottled = value != nullptr && value[0] != '\0' &&
                               std::strcmp(value, "0") != 0;
      return unthrottled ? uint64_t{0} : kRetryIntervalNs;
    }();
    return interval;
  }

  VulkanSurfaceRecoveryGate() = default;
  VulkanSurfaceRecoveryGate(const VulkanSurfaceRecoveryGate&) = delete;
  VulkanSurfaceRecoveryGate& operator=(const VulkanSurfaceRecoveryGate&) =
      delete;

  bool Activate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_.load(std::memory_order_relaxed)) {
      return false;
    }
    out_of_date_count_.store(0, std::memory_order_relaxed);
    present_count_.store(0, std::memory_order_relaxed);
    handled_out_of_date_count_ = 0;
    present_count_at_last_rebind_ = 0;
    last_rebind_ticks_ns_ = 0;
    active_.store(true, std::memory_order_release);
    return true;
  }

  void Deactivate() {
    active_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    out_of_date_count_.store(0, std::memory_order_relaxed);
    present_count_.store(0, std::memory_order_relaxed);
    handled_out_of_date_count_ = 0;
    present_count_at_last_rebind_ = 0;
    last_rebind_ticks_ns_ = 0;
  }

  void NotifyOutOfDate() {
    if (active_.load(std::memory_order_acquire)) {
      out_of_date_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void NotifyPresent() {
    if (active_.load(std::memory_order_acquire)) {
      present_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  bool TakeRebindRequest(uint64_t now_ticks_ns,
                         VulkanSurfaceRecoveryRequest* request) {
    if (request == nullptr || !active_.load(std::memory_order_acquire)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.load(std::memory_order_relaxed)) {
      return false;
    }

    const uint64_t out_of_date_count =
        out_of_date_count_.load(std::memory_order_acquire);
    if (out_of_date_count == handled_out_of_date_count_) {
      return false;
    }
    const uint64_t present_count =
        present_count_.load(std::memory_order_acquire);
    const bool presented_since_last_rebind =
        present_count != present_count_at_last_rebind_;
    const bool retry_due =
        last_rebind_ticks_ns_ == 0 || now_ticks_ns < last_rebind_ticks_ns_ ||
        now_ticks_ns - last_rebind_ticks_ns_ >= RetryIntervalNs();
    if (!presented_since_last_rebind && !retry_due) {
      return false;
    }

    request->observed_error_count = out_of_date_count;
    request->coalesced_error_count =
        out_of_date_count - handled_out_of_date_count_;
    request->retry = last_rebind_ticks_ns_ != 0 && !presented_since_last_rebind;
    handled_out_of_date_count_ = out_of_date_count;
    present_count_at_last_rebind_ = present_count;
    last_rebind_ticks_ns_ = now_ticks_ns;
    return true;
  }

 private:
  std::atomic<bool> active_{false};
  std::atomic<uint64_t> out_of_date_count_{0};
  std::atomic<uint64_t> present_count_{0};
  std::mutex mutex_;
  uint64_t handled_out_of_date_count_ = 0;
  uint64_t present_count_at_last_rebind_ = 0;
  uint64_t last_rebind_ticks_ns_ = 0;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_VULKAN_SURFACE_RECOVERY_GATE_H_
