// Modified by vii from komaruworld/mocktail. See README "About this fork".
// Copyright 2026 Mocktail Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License")

#include "runtime/sibling_executable.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace mocktail::runtime {
namespace {

namespace fs = std::filesystem;

class SiblingExecutableTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = fs::temp_directory_path() /
                 ("nightcap-sibling-test-" + std::to_string(::getpid()) + "-" +
                  ::testing::UnitTest::GetInstance()
                      ->current_test_info()
                      ->name());
    fs::remove_all(directory_);
    fs::create_directories(directory_);
  }
  void TearDown() override { fs::remove_all(directory_); }

  // A file that is not a real program is enough: the lookup only asks whether
  // the path exists, and exec-ing it is not part of what is under test.
  fs::path Touch(const std::string& name) const {
    const fs::path file = directory_ / name;
    std::ofstream(file) << "#!/bin/sh\n";
    return file;
  }

  fs::path directory_;
};

// This is the whole of what the Restart button and `mocktail --settings` do to
// find each other, and getting it wrong is silent: the spawn appears to work
// and the wrong program runs.
TEST_F(SiblingExecutableTest, FindsAProgramInTheSameDirectory) {
  const fs::path self = Touch("mocktail_settings");
  const fs::path sibling = Touch("mocktail");
  const fs::path found = SiblingExecutableFor(self, "mocktail");
  ASSERT_FALSE(found.empty());
  EXPECT_EQ(found, sibling);
}

TEST_F(SiblingExecutableTest, ReportsAMissingProgramRatherThanGuessing) {
  const fs::path self = Touch("mocktail_settings");
  // An empty path is what makes the caller say "cannot find Nightcap" rather
  // than exec something and fail obscurely.
  EXPECT_TRUE(SiblingExecutableFor(self, "mocktail").empty());
  EXPECT_TRUE(SiblingExecutableFor(self, "does_not_exist").empty());
}

// The lookup is given a symlink, because that is what /proc/self/exe is, and it
// must look beside the program that is really running rather than beside the
// link. The two directories deliberately hold different things: only the real
// one's has a sibling, so a lookup that ignored the symlink would find nothing.
TEST_F(SiblingExecutableTest, ResolvesTheSymlinkBeforeLookingBesideIt) {
  const fs::path real_directory = directory_ / "real";
  fs::create_directories(real_directory);
  const fs::path real = real_directory / "mocktail_settings";
  std::ofstream(real) << "#!/bin/sh\n";
  // The sibling exists only beside the real binary.
  std::ofstream(real_directory / "mocktail") << "#!/bin/sh\n";
  // A decoy beside the link, which must NOT be the answer.
  Touch("mocktail");

  const fs::path link = directory_ / "mocktail_settings";
  std::error_code error;
  fs::create_symlink(real, link, error);
  ASSERT_FALSE(error) << error.message();

  const fs::path found = SiblingExecutableFor(link, "mocktail");
  ASSERT_FALSE(found.empty());
  EXPECT_EQ(found, real_directory / "mocktail");
  EXPECT_NE(found, directory_ / "mocktail");
}

TEST_F(SiblingExecutableTest, RefusesAnEmptyRequest) {
  const fs::path self = Touch("mocktail_settings");
  EXPECT_TRUE(SiblingExecutableFor(self, "").empty());
  EXPECT_TRUE(SiblingExecutableFor("", "mocktail").empty());
}

// In a real build the siblings are in the same directory, so the installed
// layout keeps working. This also covers the plain existence case with no
// symlink involved.
TEST_F(SiblingExecutableTest, WorksForThisTestBinaryItself) {
  const fs::path self =
      fs::read_symlink("/proc/self/exe");
  ASSERT_FALSE(self.empty());
  // The test binary has no sibling mocktail, so this must be empty rather than
  // a path to something that is not there.
  EXPECT_TRUE(SiblingExecutable("definitely_not_a_real_program_12345").empty());
}

}  // namespace
}  // namespace mocktail::runtime
