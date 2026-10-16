#include "runtime/startup_trace.h"

#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mocktail::runtime {
namespace {

std::mutex& TraceMutex() {
  static std::mutex mutex;
  return mutex;
}

// Origin and last reported mark, in milliseconds from CLOCK_MONOTONIC. The
// flag below is authoritative; an origin of zero is a legal timestamp.
std::uint64_t g_origin_ms = 0;
std::uint64_t g_last_ms = 0;
bool g_initialised = false;
bool g_enabled = false;

bool TraceFlagEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::uint64_t MonotonicMilliseconds() {
  timespec time{};
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
  return static_cast<std::uint64_t>(time.tv_sec) * 1000ULL +
         static_cast<std::uint64_t>(time.tv_nsec) / 1000000ULL;
}

void EmitLocked(std::string_view phase, std::uint64_t now) {
  const std::uint64_t total = now > g_origin_ms ? now - g_origin_ms : 0;
  const std::uint64_t delta = now > g_last_ms ? now - g_last_ms : 0;
  std::string line;
  FormatStartupTraceLine(phase, delta, total, &line);
  g_last_ms = now;
  std::fputs(line.c_str(), stdout);
  std::fflush(stdout);
}

}  // namespace

void InitStartupTrace() {
  const bool requested = TraceFlagEnabled("MOCKTAIL_STARTUP_TRACE") ||
                         TraceFlagEnabled("MOCKTAIL_TRACE_ALL") ||
                         TraceFlagEnabled("MOCKTAIL_FULL_TRACE");
  std::lock_guard<std::mutex> lock(TraceMutex());
  g_origin_ms = MonotonicMilliseconds();
  g_last_ms = g_origin_ms;
  g_initialised = true;
  g_enabled = requested;
}

void FormatStartupTraceLine(std::string_view phase, std::uint64_t delta_ms,
                            std::uint64_t total_ms, std::string* out) {
  if (out == nullptr) return;
  // Built by appending rather than by one snprintf into a fixed buffer: a
  // truncated phase name must still produce a newline-terminated line, and
  // snprintf would silently drop that trailing newline instead. The name is
  // capped here for the same reason, so a pathological one cannot grow the line
  // without bound.
  constexpr std::size_t kMaximumPhaseBytes = 200;
  out->assign("  [startup] ");
  out->append(phase.substr(0, kMaximumPhaseBytes));
  char numbers[96];
  const int written = std::snprintf(
      numbers, sizeof(numbers), " %llu ms (total %llu ms)\n",
      static_cast<unsigned long long>(delta_ms),
      static_cast<unsigned long long>(total_ms));
  if (written <= 0) {
    out->clear();
    return;
  }
  out->append(numbers, static_cast<std::size_t>(written) < sizeof(numbers)
                          ? static_cast<std::size_t>(written)
                          : sizeof(numbers) - 1);
}

void StartupTraceMark(std::string_view phase) {
  const std::uint64_t now = MonotonicMilliseconds();
  std::lock_guard<std::mutex> lock(TraceMutex());
  if (!g_enabled || !g_initialised) return;
  EmitLocked(phase, now);
}

}  // namespace mocktail::runtime