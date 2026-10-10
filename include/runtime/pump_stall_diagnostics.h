// Modified by vii from komaruworld/mocktail. See README "About this fork".
#ifndef MOCKTAIL_RUNTIME_PUMP_STALL_DIAGNOSTICS_H_
#define MOCKTAIL_RUNTIME_PUMP_STALL_DIAGNOSTICS_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail::runtime {

// A recorded stall: how long the engine's main-thread step blocked, and the
// stack that was on the thread while it did. A `--profile` trace already shows
// that these stalls burn no CPU, so the duration alone cannot say what the
// thread is waiting for. The stack can: a guest mutex, a socket read and a
// condition variable each look like a stall until you see the frames under it.

// One executable mapping from /proc/self/maps. A Release build carries no
// symbols for the guest, so a frame can often only be named by the image it
// falls in. That is still enough to tell the cases apart: a frame in the
// guest's own image means the engine is blocking, and one inside a system
// library's read or connect means it is the network.
struct AddressRegion {
  std::uintptr_t start = 0;
  std::uintptr_t end = 0;
  std::uintptr_t base = 0;
  bool executable = false;
  std::string module;

  std::uintptr_t offset_from_base = 0;
  bool Contains(const void* address) const;
  // The image containing `address`, or a region with an empty module name
  // when the address is in no known mapping.
  static AddressRegion For(const void* address);
};

// Parses one /proc/self/maps line. False for anything that is not a mapping, so
// a caller skips it rather than recording a region that contains nothing.
bool ParseMapsLine(std::string_view line, AddressRegion *region);

// The mappings of this process. Read once and cached, because a guest library
// can be mapped after the first sample; a miss is re-read rather than cached
// as absent, so a late-loaded image is still found.
const std::vector<AddressRegion> &HostAddressRegions();
void RefreshHostAddressRegions();

// Frames are captured by the caller, which owns the unwinding, so this type and
// the policy below stay testable without one.
using PumpStallFrames = std::vector<std::string>;

// Identifies a stall shape. Two stalls with the same signature are the same
// wait, however far apart they happened, so they are counted together instead
// of filling the log with near-duplicates.
std::string PumpStallSignature(const PumpStallFrames &frames);

// Frames that are identical on every sample and identify nothing: the capture
// helper and the pump itself. The pump's caller is kept separately, because two
// call sites reaching the same wait are otherwise indistinguishable.
bool IsPumpFrame(std::string_view frame);

// Splits an unwind into the frames below the pump and the caller that reached
// it. False when nothing identifying remains, which is the case for a pump
// called straight from the main loop.
bool SplitPumpFrames(const PumpStallFrames &frames, PumpStallFrames *identifying,
                     std::string *driven_from);

// One stall, including which call site drove the pump. `identifying` is the
// trimmed stack; when it is a single explanatory placeholder the line says so
// rather than implying a cause was found.
std::string FormatPumpStallWithCaller(std::string_view driven_from,
                                      const PumpStallFrames &identifying,
                                      std::uint64_t stall_ns);

// What a trimmed stack contains when the unwind carried nothing below the
// pump. The log has to say the stack was thin rather than imply a cause.
inline constexpr std::string_view kNoIdentifyingFrames =
    "blocked in host code; no frame below the pump";

// Decides what to do with one finished pump step.
//
// `minimum_stall_ns` is the threshold at which a step counts as a stall at
// all; below it the sampling cost is not worth it and the trace already shows
// the step. `deduplicate_window` suppresses a repeat of a signature seen that
// recently, because a stall that fires every frame would otherwise sample on
// every frame. The returned action is one of:
//
//   kIgnore        below the threshold, or a duplicate inside the window
//   kSample        capture a stack and record it
struct PumpStallDecision {
  enum class Action {
    kIgnore,
    kSample,
  };
  Action action = Action::kIgnore;
  bool is_new_signature = false;
};

// Everything the decision depends on, so it can be exercised directly.
struct PumpStallPolicy {
  std::uint64_t minimum_stall_ns = 50'000'000;   // 50 ms
  std::uint64_t deduplicate_window_ns = 30'000'000'000;  // 30 s
  // The shortest gap between two captures. Capturing a stack costs a walk of
  // the frame chain and a symbol lookup, so a stall that fires every frame is
  // sampled at this rate rather than every time.
  std::uint64_t minimum_sample_interval_ns = 1'000'000'000;  // 1 s
  // A hard cap on distinct signatures kept, so a pathological guest cannot
  // grow the table without bound. Past the cap, new signatures are sampled
  // but not retained for deduplication.
  std::size_t maximum_signatures = 32;
};

// Stateful so the deduplication window can be enforced across calls. Not
// thread-safe: the pump runs on one thread, which is the only thread whose
// stalls matter here.
class PumpStallRecorder {
 public:
  void Configure(PumpStallPolicy policy);

  // Call once per finished pump step with its wall duration. Returns whether
  // the caller should capture a stack.
  bool ShouldSample(std::uint64_t stall_ns);

  // Records a captured stack and returns the line to log, or an empty string
  // if the signature was already known. `total` is the running count for that
  // signature, which is what makes a rare stall and a constant one
  // distinguishable in the log.
  std::string Record(const PumpStallFrames &frames, std::uint64_t stall_ns);

  std::size_t signature_count() const { return entries_.size(); }
  // Steps that were long enough to be worth a capture.
  std::uint64_t total_stalls() const { return total_stalls_; }
  // Steps a capture was actually taken for, after the cooldown.
  std::uint64_t total_sampled() const { return total_sampled_; }
  // Stacks handed to Record, which is fewer than total_sampled only when a
  // capture was declined after the decision.
  std::uint64_t total_recorded() const { return total_recorded_; }
  // Distinct signatures suppressed as recent duplicates.
  std::uint64_t duplicates_suppressed() const { return duplicates_suppressed_; }

 private:
  struct Entry {
    std::string signature;
    std::uint64_t last_seen_ns = 0;
    std::uint64_t count = 0;
  };

  PumpStallPolicy policy_;
  std::vector<Entry> entries_;
  std::uint64_t now_ns_ = 0;
  // Starts past the cooldown so the first stall of a session is always
  // captured rather than suppressed against a zero timestamp.
  std::uint64_t last_sample_ns_ = ~0ULL / 2;
  std::uint64_t total_sampled_ = 0;
  std::uint64_t total_recorded_ = 0;
  std::uint64_t total_stalls_ = 0;
  std::uint64_t duplicates_suppressed_ = 0;
};

// Formats one stall for the log: the wait, the count for that shape, and the
// stack. Kept separate from Record so the format can be asserted in a test
// without a process to sample.
std::string FormatPumpStall(std::string_view signature,
                            const PumpStallFrames &frames,
                            std::uint64_t stall_ns, std::uint64_t count,
                            std::uint64_t total_for_signature);

// The process-wide recorder the pump uses. Exposed so the legacy runtime and
// its tests agree on one instance.
PumpStallRecorder &SharedPumpStallRecorder();

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_PUMP_STALL_DIAGNOSTICS_H_
