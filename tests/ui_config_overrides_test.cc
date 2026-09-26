// Modified by vii from komaruworld/mocktail. See README "About this fork".
// Copyright 2026 Mocktail Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License")

#include "runtime/ui_config_overrides.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace mocktail {
namespace runtime {
namespace {

namespace fs = std::filesystem;

class UiConfigOverridesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = fs::temp_directory_path() /
                 ("nightcap-ui-config-test-" + std::to_string(::getpid()) + "-" +
                  ::testing::UnitTest::GetInstance()
                      ->current_test_info()
                      ->name());
    fs::remove_all(directory_);
    fs::create_directories(directory_);
    file_ = directory_ / std::string(kUiConfigFileName);
  }
  void TearDown() override { fs::remove_all(directory_); }

  std::string Read(const fs::path& path) const {
    std::ifstream input(path);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  }

  void Write(const fs::path& path, const std::string& contents) const {
    std::ofstream output(path);
    output << contents;
  }

  fs::path directory_;
  fs::path file_;
};

// The window must never be able to write a key the loader would reject, and a
// value outside a field's accepted set has to be refused where it was typed.
TEST(UiConfigFieldsTest, RejectsValuesAFieldDoesNotAccept) {
  std::string error;
  EXPECT_TRUE(ValidateUiConfigValue("graphics.vsync", "off", &error))
      << error;
  EXPECT_TRUE(ValidateUiConfigValue("graphics.vsync", "auto", &error)) << error;
  EXPECT_FALSE(ValidateUiConfigValue("graphics.vsync", "maybe", &error));
  EXPECT_NE(error.find("graphics.vsync"), std::string::npos) << error;

  // A boolean takes only its two spellings, not "1" or "yes".
  EXPECT_TRUE(ValidateUiConfigValue("input.raw_mouse", "true", &error)) << error;
  EXPECT_FALSE(ValidateUiConfigValue("input.raw_mouse", "1", &error));
  EXPECT_FALSE(ValidateUiConfigValue("input.raw_mouse", "", &error));

  // A numeric field is range checked, so the window cannot write a value the
  // client would later reject.
  EXPECT_TRUE(
      ValidateUiConfigValue("performance.small_texture_upscale", "8", &error))
      << error;
  EXPECT_FALSE(
      ValidateUiConfigValue("performance.small_texture_upscale", "9", &error));
  EXPECT_FALSE(
      ValidateUiConfigValue("performance.small_texture_upscale", "0", &error));
  EXPECT_FALSE(
      ValidateUiConfigValue("performance.small_texture_upscale", "4x", &error));
  EXPECT_FALSE(ValidateUiConfigValue("performance.texture_memory_mb", "2048",
                                     &error));
}

TEST(UiConfigFieldsTest, RejectsAKeyTheWindowDoesNotManage) {
  std::string error;
  // runtime.roblox_library is a real config key, but a path is not something a
  // settings window should be writing.
  EXPECT_FALSE(
      ValidateUiConfigValue("runtime.roblox_library", "/tmp/x.so", &error));
  EXPECT_NE(error.find("unknown key"), std::string::npos) << error;
  EXPECT_FALSE(ValidateUiConfigValue("nonsense.key", "1", &error));
}

// Every field the window offers must be a key the loader accepts, or the
// window could write a setting that makes the next launch fail.
TEST(UiConfigFieldsTest, EveryFieldIsAKeyTheLoaderAccepts) {
  for (const UiConfigField& field : UiConfigFields()) {
    EXPECT_FALSE(field.key.empty());
    if (field.boolean) {
      std::string error;
      EXPECT_TRUE(ValidateUiConfigValue(field.key, "true", &error))
          << field.key << ": " << error;
      EXPECT_TRUE(ValidateUiConfigValue(field.key, "false", &error))
          << field.key << ": " << error;
    } else if (field.numeric) {
      std::string error;
      const std::string lowest = std::to_string(field.minimum);
      EXPECT_TRUE(ValidateUiConfigValue(field.key, lowest, &error))
          << field.key << ": " << error;
    } else if (field.free_text) {
      std::string error;
      EXPECT_TRUE(ValidateUiConfigValue(field.key, "A Device Name", &error))
          << field.key << ": " << error;
    } else {
      EXPECT_FALSE(field.allowed_values.empty()) << field.key;
      for (const std::string& value : field.allowed_values) {
        std::string error;
        EXPECT_TRUE(ValidateUiConfigValue(field.key, value, &error))
            << field.key << "=" << value << ": " << error;
      }
    }
    // A field with no help text is a field nobody can understand in the window.
    EXPECT_FALSE(field.help.empty()) << field.key;
  }
}

TEST(UiConfigFieldsTest, FieldKeysAreUnique) {
  std::vector<std::string> keys;
  for (const UiConfigField& field : UiConfigFields()) {
    keys.push_back(field.key);
  }
  std::sort(keys.begin(), keys.end());
  EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end());
}

TEST_F(UiConfigOverridesTest, RoundTripsThroughDisk) {
  UiConfigOverrides written;
  written.Set("graphics.vsync", "off");
  written.Set("input.raw_mouse", "true");
  written.Set("performance.texture_memory_mb", "2047");
  std::string error;
  ASSERT_TRUE(SaveUiConfigOverrides(file_, written, &error)) << error;
  EXPECT_TRUE(fs::exists(file_));

  UiConfigOverrides read;
  bool loaded = false;
  ASSERT_TRUE(LoadUiConfigOverrides(file_, &read, &loaded, &error)) << error;
  EXPECT_TRUE(loaded);
  ASSERT_EQ(read.values.size(), 3U);
  ASSERT_NE(read.Find("graphics.vsync"), nullptr);
  EXPECT_EQ(*read.Find("graphics.vsync"), "off");
  ASSERT_NE(read.Find("input.raw_mouse"), nullptr);
  EXPECT_EQ(*read.Find("input.raw_mouse"), "true");
  ASSERT_NE(read.Find("performance.texture_memory_mb"), nullptr);
  EXPECT_EQ(*read.Find("performance.texture_memory_mb"), "2047");
}

// config.yaml is 74% comments and is never rewritten, so the window's file has
// to stand on its own. It carries a header saying so.
TEST_F(UiConfigOverridesTest, FileExplainsWhereItSits) {
  UiConfigOverrides overrides;
  overrides.Set("graphics.vsync", "off");
  std::string error;
  ASSERT_TRUE(SaveUiConfigOverrides(file_, overrides, &error)) << error;
  const std::string contents = Read(file_);
  EXPECT_NE(contents.find("settings window"), std::string::npos) << contents;
  EXPECT_NE(contents.find("config.yaml"), std::string::npos) << contents;
  EXPECT_NE(contents.find("graphics:"), std::string::npos) << contents;
  EXPECT_NE(contents.find("vsync: off"), std::string::npos) << contents;
}

TEST_F(UiConfigOverridesTest, MissingFileIsNotAnError) {
  UiConfigOverrides overrides;
  bool loaded = true;
  std::string error;
  // A user who has never opened the window has no such file. That is the
  // normal case, not a failure.
  ASSERT_TRUE(LoadUiConfigOverrides(file_, &overrides, &loaded, &error))
      << error;
  EXPECT_FALSE(loaded);
  EXPECT_TRUE(overrides.empty());
}

// A file the window wrote must never fail to load. One that does was edited by
// hand, and it is reported rather than ignored, because ignoring it would drop
// settings the user believes are active.
TEST_F(UiConfigOverridesTest, AHandEditedFileIsReportedNotIgnored) {
  Write(file_, "graphics:\n  vsync: sometimes\n");
  UiConfigOverrides overrides;
  bool loaded = true;
  std::string error;
  EXPECT_FALSE(LoadUiConfigOverrides(file_, &overrides, &loaded, &error));
  EXPECT_NE(error.find("graphics.vsync"), std::string::npos) << error;
  EXPECT_FALSE(loaded);
  EXPECT_TRUE(overrides.empty());

  Write(file_, "graphics: not-a-mapping\n");
  EXPECT_FALSE(LoadUiConfigOverrides(file_, &overrides, &loaded, &error));
  EXPECT_FALSE(error.empty());

  Write(file_, "graphics:\n  vsync: [a, list]\n");
  EXPECT_FALSE(LoadUiConfigOverrides(file_, &overrides, &loaded, &error));

  Write(file_, "graphics:\n\tvsync: off\n");
  EXPECT_FALSE(LoadUiConfigOverrides(file_, &overrides, &loaded, &error));
}

TEST_F(UiConfigOverridesTest, RefusesToSaveAnInvalidValue) {
  UiConfigOverrides overrides;
  overrides.Set("graphics.vsync", "nonsense");
  std::string error;
  EXPECT_FALSE(SaveUiConfigOverrides(file_, overrides, &error));
  EXPECT_FALSE(fs::exists(file_)) << "a rejected value must not leave a file";
}

// Clearing every setting has to return the user to a clean state, which means
// no leftover file claiming to hold settings.
TEST_F(UiConfigOverridesTest, ClearingEverythingRemovesTheFile) {
  UiConfigOverrides overrides;
  overrides.Set("graphics.vsync", "off");
  std::string error;
  ASSERT_TRUE(SaveUiConfigOverrides(file_, overrides, &error)) << error;
  ASSERT_TRUE(fs::exists(file_));

  UiConfigOverrides cleared;
  ASSERT_TRUE(SaveUiConfigOverrides(file_, cleared, &error)) << error;
  EXPECT_FALSE(fs::exists(file_));

  // And saving nothing when there is no file is still fine.
  ASSERT_TRUE(SaveUiConfigOverrides(file_, cleared, &error)) << error;
  EXPECT_FALSE(fs::exists(file_));
}

TEST_F(UiConfigOverridesTest, SetReplacesRatherThanDuplicates) {
  UiConfigOverrides overrides;
  overrides.Set("graphics.vsync", "off");
  overrides.Set("graphics.vsync", "on");
  ASSERT_EQ(overrides.values.size(), 1U);
  ASSERT_NE(overrides.Find("graphics.vsync"), nullptr);
  EXPECT_EQ(*overrides.Find("graphics.vsync"), "on");

  overrides.Clear("graphics.vsync");
  EXPECT_EQ(overrides.Find("graphics.vsync"), nullptr);
  EXPECT_TRUE(overrides.empty());
}

TEST_F(UiConfigOverridesTest, FileIsNotWorldReadable) {
  UiConfigOverrides overrides;
  overrides.Set("audio.output_device", "HDA Intel PCH Speaker");
  std::string error;
  ASSERT_TRUE(SaveUiConfigOverrides(file_, overrides, &error)) << error;
  // config.yaml is 0600 and this can name a device, so it matches.
  const auto mode = fs::status(file_).permissions();
  EXPECT_EQ(mode & fs::perms::group_all, fs::perms::none);
  EXPECT_EQ(mode & fs::perms::others_all, fs::perms::none);
}

// A device name only exists on the user's machine, so the window cannot offer a
// list and has to accept what the user types.
TEST_F(UiConfigOverridesTest, ADeviceNameIsAcceptedAsTyped) {
  std::string error;
  EXPECT_TRUE(ValidateUiConfigValue("audio.output_device",
                                    "HDA Intel PCH Speaker", &error))
      << error;
  EXPECT_TRUE(ValidateUiConfigValue("audio.output_device", " Speakers ", &error))
      << error;
  // A line break would corrupt the one-key-per-line file.
  EXPECT_FALSE(
      ValidateUiConfigValue("audio.output_device", "a\nb", &error));
  EXPECT_FALSE(ValidateUiConfigValue("audio.output_device", "", &error));
  EXPECT_FALSE(ValidateUiConfigValue("audio.output_device",
                                     std::string(600, 'x'), &error));
}

TEST_F(UiConfigOverridesTest, ADeviceNameWithASpaceSurvivesTheRoundTrip) {
  UiConfigOverrides written;
  written.Set("audio.output_device", "HDA Intel PCH Speaker");
  std::string error;
  ASSERT_TRUE(SaveUiConfigOverrides(file_, written, &error)) << error;
  UiConfigOverrides read;
  bool loaded = false;
  ASSERT_TRUE(LoadUiConfigOverrides(file_, &read, &loaded, &error)) << error;
  ASSERT_NE(read.Find("audio.output_device"), nullptr);
  EXPECT_EQ(*read.Find("audio.output_device"), "HDA Intel PCH Speaker");
}

TEST(UiConfigFieldsTest, RenderGroupsKeysUnderTheirSection) {
  UiConfigOverrides overrides;
  overrides.Set("input.raw_mouse", "true");
  overrides.Set("graphics.vsync", "off");
  overrides.Set("graphics.backend", "vulkan");
  const std::string document = RenderUiConfigYaml(overrides);
  const std::size_t graphics = document.find("graphics:");
  const std::size_t input = document.find("input:");
  ASSERT_NE(graphics, std::string::npos);
  ASSERT_NE(input, std::string::npos);
  EXPECT_LT(graphics, input);
  EXPECT_NE(document.find("backend: vulkan"), std::string::npos);
  // Both graphics keys land under one heading rather than repeating it.
  EXPECT_NE(document.find("vsync: off"), std::string::npos);
  EXPECT_EQ(document.find("graphics:"), document.rfind("graphics:"));
}

// Toggling one setting must not reshuffle the file, so it does not produce a
// confusing diff for anyone who keeps it under version control.
TEST(UiConfigFieldsTest, OutputIsStableRegardlessOfInsertionOrder) {
  UiConfigOverrides first;
  first.Set("graphics.vsync", "off");
  first.Set("audio.output_device", "Speakers");
  first.Set("input.raw_mouse", "true");

  UiConfigOverrides second;
  second.Set("input.raw_mouse", "true");
  second.Set("graphics.vsync", "off");
  second.Set("audio.output_device", "Speakers");

  EXPECT_EQ(RenderUiConfigYaml(first), RenderUiConfigYaml(second));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
