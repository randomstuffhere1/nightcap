#ifndef MOCKTAIL_RUNTIME_STARTUP_TRACE_H_
#define MOCKTAIL_RUNTIME_STARTUP_TRACE_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace mocktail::runtime {

// Records the monotonic origin that every startup phase is measured against.
// Call this once at the top of main(), before any other work, so the timeline
// includes the runtime's own initialisation and not just the phases it reports.
//
// The tracer is off unless MOCKTAIL_STARTUP_TRACE is set to a non-zero value.
// MOCKTAIL_TRACE_ALL and MOCKTAIL_FULL_TRACE enable it as well, matching the
// flags the update coordinator already reads, so one of those turns on both
// timelines at once.
void InitStartupTrace();

// Reports one phase boundary. `total_ms` is measured from InitStartupTrace, so
// the last mark before the first presented frame is the time to first frame.
void StartupTraceMark(std::string_view phase);

// The line the tracer prints, with the arithmetic done by the caller. Exposed
// so the format is covered by a unit test rather than only by a real launch.
void FormatStartupTraceLine(std::string_view phase, std::uint64_t delta_ms,
                            std::uint64_t total_ms, std::string* out);

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_STARTUP_TRACE_H_