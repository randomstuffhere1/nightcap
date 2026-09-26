// Modified by vii from komaruworld/mocktail. See README "About this fork".

#include "runtime/pump_stall_diagnostics.h"

#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mocktail::runtime {
namespace {

uint64_t NowNs() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(now.tv_nsec);
}

// Frames below the guest's own entry are noise: the guest is reached through
// the JNI environment, so every sample shares a long identical prefix and the
// interesting frame is the first one that differs.
constexpr std::size_t kSignatureFrames = 6;

}  // namespace

std::string PumpStallSignature(const PumpStallFrames &frames) {
  // Only the top frames take part, so a stall whose wait is in the same place
  // is one signature even if the engine's caller chain below it varies.
  std::string signature;
  for (std::size_t index = 0;
       index < frames.size() && index < kSignatureFrames; ++index) {
    if (index != 0) {
      signature += ';';
    }
    signature += frames[index];
  }
  return signature;
}

void PumpStallRecorder::Configure(PumpStallPolicy policy) {
  policy_ = policy;
  if (policy_.minimum_stall_ns == 0) {
    policy_.minimum_stall_ns = 1;
  }
  if (policy_.maximum_signatures == 0) {
    policy_.maximum_signatures = 1;
  }
}

bool PumpStallRecorder::ShouldSample(std::uint64_t stall_ns) {
  ++total_stalls_;
  now_ns_ = NowNs();
  if (stall_ns < policy_.minimum_stall_ns) {
    return false;
  }
  // The signature is not known until the stack is captured, so a repeat cannot
  // be recognised before paying for the capture. What can be checked is how
  // recently anything was sampled: a stall that fires every frame would
  // otherwise capture a stack on every frame. This is a cooldown, not
  // deduplication; Record decides whether a captured stack is new.
  if (now_ns_ - last_sample_ns_ < policy_.minimum_sample_interval_ns) {
    ++duplicates_suppressed_;
    return false;
  }
  last_sample_ns_ = now_ns_;
  ++total_sampled_;
  return true;
}

std::string PumpStallRecorder::Record(const PumpStallFrames &frames,
                                      std::uint64_t stall_ns) {
  ++total_recorded_;
  const std::string signature = PumpStallSignature(frames);
  if (signature.empty()) {
    return std::string();
  }
  for (Entry &entry : entries_) {
    if (entry.signature != signature) {
      continue;
    }
    ++entry.count;
    const bool inside_window =
        now_ns_ - entry.last_seen_ns < policy_.deduplicate_window_ns;
    entry.last_seen_ns = now_ns_;
    if (inside_window) {
      // Same wait as a recent one. Counted, but not logged again: the stack
      // is already on screen and repeating it every window buries anything
      // new.
      ++duplicates_suppressed_;
      return std::string();
    }
    return FormatPumpStall(signature, frames, stall_ns, entry.count,
                           entry.count);
  }
  if (entries_.size() >= policy_.maximum_signatures) {
    // Past the cap a new shape is still sampled, so the log keeps showing what
    // the thread is doing, but it is not retained and so is not deduplicated.
    return FormatPumpStall(signature, frames, stall_ns, 1, 1);
  }
  entries_.push_back(Entry{signature, now_ns_, 1});
  return FormatPumpStall(signature, frames, stall_ns, 1, 1);
}

std::string FormatPumpStall(std::string_view signature,
                            const PumpStallFrames &frames,
                            std::uint64_t stall_ns, std::uint64_t count,
                            std::uint64_t total_for_signature) {
  std::string line = "  [stall] engine main-thread step blocked ";
  line += std::to_string(stall_ns / 1'000'000);
  line += " ms with no CPU";
  if (count > 1) {
    line += " (";
    line += std::to_string(count);
    line += " times)";
  }
  line += "\n";
  for (std::size_t index = 0; index < frames.size(); ++index) {
    line += "        #";
    line += std::to_string(index);
    line += ' ';
    line += frames[index];
    line += '\n';
  }
  return line;
}

PumpStallRecorder &SharedPumpStallRecorder() {
  static PumpStallRecorder recorder;
  return recorder;
}

}  // namespace mocktail::runtime
