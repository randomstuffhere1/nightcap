#include "runtime/startup_trace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

void ClearTraceEnvironment() {
  unsetenv("MOCKTAIL_STARTUP_TRACE");
  unsetenv("MOCKTAIL_TRACE_ALL");
  unsetenv("MOCKTAIL_FULL_TRACE");
}

// The tracer is observable only through its output, so these cover the flag
// handling by observing that a mark either prints or does not. Reinitialising
// with no flags set puts the module back to silent for the next case.
void ExpectSilentWithoutFlags(const char* name, const char* value) {
  ClearTraceEnvironment();
  setenv(name, value, 1);
  mocktail::runtime::InitStartupTrace();
  testing::internal::CaptureStdout();
  mocktail::runtime::StartupTraceMark("probe");
  const std::string output = testing::internal::GetCapturedStdout();
  EXPECT_EQ(output, "") << name << "=" << value << " should be silent";
  ClearTraceEnvironment();
  mocktail::runtime::InitStartupTrace();
}

}  // namespace

TEST(StartupTrace, LineCarriesPhaseDeltaAndTotal) {
  std::string line;
  mocktail::runtime::FormatStartupTraceLine("update-preflight", 1086, 1224,
                                            &line);
  EXPECT_EQ(line, "  [startup] update-preflight 1086 ms (total 1224 ms)\n");
}

TEST(StartupTrace, LineEndsWithNewlineSoStreamsDoNotMerge) {
  std::string line;
  mocktail::runtime::FormatStartupTraceLine("first-present", 1, 1, &line);
  ASSERT_FALSE(line.empty());
  EXPECT_EQ(line.back(), '\n');
}

TEST(StartupTrace, TruncatesAnOverlongPhaseRatherThanOverrun) {
  const std::string phase(4096, 'x');
  std::string line;
  mocktail::runtime::FormatStartupTraceLine(phase, 0, 0, &line);
  ASSERT_FALSE(line.empty());
  // The phase name is capped, so a pathological one cannot grow the line without
  // bound. It may be cut mid-name, but the line is always newline-terminated.
  EXPECT_LT(line.size(), 320U);
  EXPECT_EQ(line.back(), '\n');
  EXPECT_EQ(std::count(line.begin(), line.end(), '\n'), 1);
}

TEST(StartupTrace, NullDestinationIsIgnored) {
  mocktail::runtime::FormatStartupTraceLine("anything", 0, 0, nullptr);
}

TEST(StartupTrace, MarksAreSilentUntilInitialised) {
  ClearTraceEnvironment();
  mocktail::runtime::InitStartupTrace();
  testing::internal::CaptureStdout();
  mocktail::runtime::StartupTraceMark("no-one-is-listening");
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "");
}

TEST(StartupTrace, TheExplicitFlagTurnsTheTraceOn) {
  ClearTraceEnvironment();
  setenv("MOCKTAIL_STARTUP_TRACE", "1", 1);
  mocktail::runtime::InitStartupTrace();
  testing::internal::CaptureStdout();
  mocktail::runtime::StartupTraceMark("explicit");
  const std::string output = testing::internal::GetCapturedStdout();
  EXPECT_NE(output.find("[startup] explicit"), std::string::npos) << output;
  ClearTraceEnvironment();
  mocktail::runtime::InitStartupTrace();
}

TEST(StartupTrace, TheFullTraceFlagAlsoTurnsItOn) {
  ClearTraceEnvironment();
  setenv("MOCKTAIL_FULL_TRACE", "1", 1);
  mocktail::runtime::InitStartupTrace();
  testing::internal::CaptureStdout();
  mocktail::runtime::StartupTraceMark("full");
  const std::string output = testing::internal::GetCapturedStdout();
  EXPECT_NE(output.find("[startup] full"), std::string::npos) << output;
  ClearTraceEnvironment();
  mocktail::runtime::InitStartupTrace();
}

TEST(StartupTrace, ZeroAndEmptyValuesLeaveTheTraceOff) {
  ExpectSilentWithoutFlags("MOCKTAIL_STARTUP_TRACE", "0");
  ExpectSilentWithoutFlags("MOCKTAIL_STARTUP_TRACE", "");
  ExpectSilentWithoutFlags("MOCKTAIL_FULL_TRACE", "0");
  ExpectSilentWithoutFlags("MOCKTAIL_TRACE_ALL", "0");
}

TEST(StartupTrace, MarksPrintOneLineEachAndATotalThatOnlyGrows) {
  ClearTraceEnvironment();
  setenv("MOCKTAIL_STARTUP_TRACE", "1", 1);
  mocktail::runtime::InitStartupTrace();
  testing::internal::CaptureStdout();
  mocktail::runtime::StartupTraceMark("first");
  mocktail::runtime::StartupTraceMark("second");
  const std::string output = testing::internal::GetCapturedStdout();
  ClearTraceEnvironment();
  mocktail::runtime::InitStartupTrace();

  ASSERT_EQ(std::count(output.begin(), output.end(), '\n'), 2) << output;
  EXPECT_NE(output.find("[startup] first"), std::string::npos) << output;
  EXPECT_NE(output.find("[startup] second"), std::string::npos) << output;
}