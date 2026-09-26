// Modified by vii from komaruworld/mocktail. See README "About this fork".

#include "runtime/pump_stall_diagnostics.h"

#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

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

std::mutex &AddressRegionsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<AddressRegion> &CachedAddressRegions() {
  static std::vector<AddressRegion> regions;
  return regions;
}

}  // namespace

bool AddressRegion::Contains(const void* address) const {
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  return module.size() != 0 && value >= start && value < end;
}

AddressRegion AddressRegion::For(const void* address) {
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  for (const AddressRegion &region : HostAddressRegions()) {
    if (region.Contains(address)) {
      AddressRegion found = region;
      found.offset_from_base = value - region.base;
      return found;
    }
  }
  return AddressRegion{};
}

namespace {

// The part of a maps path that names the image. A path can end in " (deleted)"
// when a library was replaced underneath a running process, which is worth
// keeping: it explains a guest image that will not resolve.
std::string ModuleName(std::string_view path) {
  std::string_view trimmed = path;
  while (!trimmed.empty() && trimmed.front() == ' ') {
    trimmed.remove_prefix(1);
  }
  if (trimmed.empty()) {
    return std::string();
  }
  const std::size_t slash = trimmed.rfind('/');
  return std::string(slash == std::string_view::npos ? trimmed
                                                     : trimmed.substr(slash + 1));
}

bool ParseHex(std::string_view text, std::uintptr_t *out) {
  if (text.empty()) {
    return false;
  }
  std::uintptr_t value = 0;
  for (const char character : text) {
    std::uintptr_t digit = 0;
    if (character >= '0' && character <= '9') {
      digit = static_cast<std::uintptr_t>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      digit = static_cast<std::uintptr_t>(character - 'a' + 10);
    } else if (character >= 'A' && character <= 'F') {
      digit = static_cast<std::uintptr_t>(character - 'A' + 10);
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  *out = value;
  return true;
}

std::vector<std::string_view> SplitFields(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && line[index] == ' ') {
      ++index;
    }
    const std::size_t begin = index;
    while (index < line.size() && line[index] != ' ') {
      ++index;
    }
    if (index > begin) {
      fields.push_back(line.substr(begin, index - begin));
    }
  }
  return fields;
}

}  // namespace

bool ParseMapsLine(std::string_view line, AddressRegion *region) {
  if (region == nullptr) {
    return false;
  }
  const std::vector<std::string_view> fields = SplitFields(line);
  // start-end perms offset dev inode [pathname]
  if (fields.size() < 5) {
    return false;
  }
  const std::size_t dash = fields[0].find('-');
  if (dash == std::string_view::npos) {
    return false;
  }
  AddressRegion parsed;
  if (!ParseHex(fields[0].substr(0, dash), &parsed.start) ||
      !ParseHex(fields[0].substr(dash + 1), &parsed.end)) {
    return false;
  }
  if (parsed.end <= parsed.start) {
    return false;
  }
  // A mapping's file offset says where this segment sits inside the image, so
  // subtracting it from the mapping's start gives the image base. Without that,
  // the second segment of a library would report a different offset for the
  // same code than the first segment does.
  std::uintptr_t file_offset = 0;
  if (!ParseHex(fields[2], &file_offset) || file_offset > parsed.start) {
    file_offset = 0;
  }
  parsed.base = parsed.start - file_offset;
  // Perms are four characters, rwx plus a private/shared letter, so the
  // execute bit is the third.
  parsed.executable = fields[1].size() > 2 && fields[1][2] == 'x';
  if (fields.size() >= 6) {
    parsed.module = ModuleName(fields[5]);
  }
  *region = parsed;
  return true;
}

void RefreshHostAddressRegions() {
  std::vector<AddressRegion> regions;
  std::ifstream maps("/proc/self/maps");
  std::string line;
  while (std::getline(maps, line)) {
    AddressRegion region;
    if (ParseMapsLine(line, &region)) {
      regions.push_back(region);
    }
  }
  std::lock_guard<std::mutex> lock(AddressRegionsMutex());
  CachedAddressRegions() = std::move(regions);
}

const std::vector<AddressRegion> &HostAddressRegions() {
  {
    std::lock_guard<std::mutex> lock(AddressRegionsMutex());
    if (!CachedAddressRegions().empty()) {
      return CachedAddressRegions();
    }
  }
  // Read outside the lock: /proc can be slow and a stall sample must not hold
  // up the main thread while it is already blocked.
  RefreshHostAddressRegions();
  std::lock_guard<std::mutex> lock(AddressRegionsMutex());
  return CachedAddressRegions();
}

bool IsPumpFrame(std::string_view frame) {
  return frame.find("ReportEnginePumpStall") != std::string_view::npos ||
         frame.find("PumpRobloxMainThreadMessagesOnce") != std::string_view::npos ||
         frame.find("PumpStartupOwnerThread") != std::string_view::npos;
}

bool SplitPumpFrames(const PumpStallFrames &frames, PumpStallFrames *identifying,
                     std::string *driven_from) {
  if (identifying == nullptr) {
    return false;
  }
  identifying->clear();
  // A backtrace is ordered innermost first, so the pump's own frames are a run
  // at the top. Skip the whole run rather than stopping at the first match,
  // which would report the capture helper as the pump's caller.
  std::size_t index = 0;
  while (index < frames.size() && IsPumpFrame(frames[index])) {
    ++index;
  }
  // Whatever follows the run is the caller that reached the pump, which is
  // context rather than cause, so it is reported on its own.
  if (driven_from != nullptr) {
    *driven_from = index < frames.size() ? frames[index] : std::string();
  }
  const std::size_t begin = index + 1;
  if (begin < frames.size()) {
    identifying->assign(frames.begin() + static_cast<std::ptrdiff_t>(begin),
                        frames.end());
  }
  return !identifying->empty();
}

std::string FormatPumpStallWithCaller(std::string_view driven_from,
                                      const PumpStallFrames &identifying,
                                      std::uint64_t stall_ns) {
  std::string line = "  [stall] engine main-thread step blocked ";
  line += std::to_string(stall_ns / 1'000'000);
  line += " ms with no CPU";
  if (!driven_from.empty()) {
    line += ", driven from ";
    line += driven_from;
  }
  line += '\n';
  if (identifying.empty()) {
    line += "        ";
    line += kNoIdentifyingFrames;
    line += '\n';
    return line;
  }
  for (std::size_t index = 0; index < identifying.size(); ++index) {
    line += "        #";
    line += std::to_string(index);
    line += ' ';
    line += identifying[index];
    line += '\n';
  }
  return line;
}

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
