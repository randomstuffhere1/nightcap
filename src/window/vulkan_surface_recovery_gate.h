#ifndef MOCKTAIL_WINDOW_VULKAN_SURFACE_RECOVERY_GATE_H_
#define MOCKTAIL_WINDOW_VULKAN_SURFACE_RECOVERY_GATE_H_

#include <atomic>
#include <cstdint>
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
  // The retry interval starts here and doubles up to kRetryIntervalNs.
  //
  // A single flat interval cannot be right for both cases this gate serves. A
  // resize converges in a couple of frames, so waiting a second between
  // attempts made the window take a second to respond -- which is what it did,
  // because during a resize no present succeeds, so the "presented since the
  // last rebind" escape in TakeRebindRequest never opens and every attempt
  // after the first fell through to the interval. But a swapchain that is
  // genuinely broken must not be rebound at frame rate either.
  //
  // Backing off from a frame to a second gives the resize case what it needs
  // and still bounds the pathological one: four attempts reach the old interval
  // within about 120 ms, so a stuck surface costs a handful of extra rebinds
  // once and then settles back to one per second.
  static constexpr uint64_t kInitialRetryIntervalNs = 16000000ULL;
  static constexpr uint64_t kRetryIntervalNs = 1000000000ULL;

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
    retry_interval_ns_ = kInitialRetryIntervalNs;
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
    retry_interval_ns_ = kInitialRetryIntervalNs;
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
        now_ticks_ns - last_rebind_ticks_ns_ >= retry_interval_ns_;
    if (!presented_since_last_rebind && !retry_due) {
      return false;
    }

    // Captured before it is overwritten below: the first grant of a session has
    // no previous attempt to back off from, so it must leave the interval at
    // its initial value rather than doubling it.
    const bool first_grant = last_rebind_ticks_ns_ == 0;
    request->observed_error_count = out_of_date_count;
    request->coalesced_error_count =
        out_of_date_count - handled_out_of_date_count_;
    request->retry = !first_grant && !presented_since_last_rebind;
    handled_out_of_date_count_ = out_of_date_count;
    present_count_at_last_rebind_ = present_count;
    last_rebind_ticks_ns_ = now_ticks_ns;
    // A successful present is evidence the surface was healthy, so the next
    // out-of-date starts over from the short interval. Without that, one
    // unlucky stretch of broken presents would leave every later resize
    // waiting a second.
    if (presented_since_last_rebind) {
      retry_interval_ns_ = kInitialRetryIntervalNs;
    } else if (!first_grant) {
      // Clamped after doubling, not before: doubling 512 ms by itself would land
      // on 1024 ms, just past the cap, and the interval has to be a ceiling.
      retry_interval_ns_ =
          retry_interval_ns_ >= kRetryIntervalNs / 2
              ? kRetryIntervalNs
              : retry_interval_ns_ * 2;
    }
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
  // Doubles per failed attempt up to kRetryIntervalNs; see the constant's note.
  uint64_t retry_interval_ns_ = kInitialRetryIntervalNs;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_VULKAN_SURFACE_RECOVERY_GATE_H_
