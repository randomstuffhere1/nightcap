// Modified by vii from komaruworld/mocktail. See README "About this fork".
// Copyright 2026 Mocktail Project Authors
//
// Licensed under the Apache License, Version 2.0 (the "License")

#include "mocktail/graphics/vulkan_pipeline_cache.h"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace mocktail {
namespace graphics {
namespace {

namespace fs = std::filesystem;

PipelineCacheKey Key(std::string device = "NVIDIA GeForce RTX 4090",
                     std::string payload = "version-1234") {
  PipelineCacheKey key;
  key.device_name = std::move(device);
  key.vendor_id = 0x10DE;
  key.device_id = 0x2684;
  key.driver_version = 5500000;
  key.driver_info = "550.54.14";
  key.api_version = (1U << 22) | (3U << 12);
  key.payload_version = std::move(payload);
  return key;
}

std::vector<std::uint8_t> Payload(std::size_t size) {
  std::vector<std::uint8_t> payload(size);
  for (std::size_t index = 0; index < size; ++index) {
    payload[index] = static_cast<std::uint8_t>(index * 7 + 3);
  }
  return payload;
}

class VulkanPipelineCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = fs::temp_directory_path() /
                 ("nightcap-pipeline-cache-test-" +
                  std::to_string(::getpid()) + "-" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(directory_);
    fs::create_directories(directory_);
  }
  void TearDown() override { fs::remove_all(directory_); }

  fs::path directory_;
};

TEST(PipelineCacheMode, ParsesEveryDocumentedValue) {
  EXPECT_EQ(ParsePipelineCacheMode("on"), PipelineCacheMode::kOn);
  EXPECT_EQ(ParsePipelineCacheMode("off"), PipelineCacheMode::kOff);
  EXPECT_EQ(ParsePipelineCacheMode("clear"), PipelineCacheMode::kClear);
  EXPECT_EQ(ParsePipelineCacheMode("reset"), PipelineCacheMode::kClear);
  EXPECT_EQ(ParsePipelineCacheMode(""), PipelineCacheMode::kOn);
  EXPECT_EQ(ParsePipelineCacheMode("nonsense"), PipelineCacheMode::kOn);
}

TEST(PipelineCacheKey, IsFilenameSafeAndStable) {
  const std::string key = Key("AMD Radeon RX 7900 XTX (radeonsi)").ToString();
  for (const char character : key) {
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '-' || character == '_' || character == '.';
    EXPECT_TRUE(safe) << "unexpected character in key: " << key;
  }
  EXPECT_EQ(key, Key("AMD Radeon RX 7900 XTX (radeonsi)").ToString());
}

TEST(PipelineCacheKey, SeparatesPayloadsAndDevices) {
  // A Roblox update must not reuse the previous payload's blob.
  EXPECT_NE(Key("GPU", "version-1234").ToString(),
            Key("GPU", "version-1235").ToString());
  // Neither must a driver upgrade on the same card.
  PipelineCacheKey upgraded = Key();
  upgraded.driver_version += 1;
  EXPECT_NE(Key().ToString(), upgraded.ToString());
  PipelineCacheKey renamed = Key();
  renamed.driver_info = "550.55";
  EXPECT_NE(Key().ToString(), renamed.ToString());
  EXPECT_NE(Key("GPU A").ToString(), Key("GPU B").ToString());
}

TEST(PipelineCacheBlob, RoundTrips) {
  const std::vector<std::uint8_t> payload = Payload(4096);
  const std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-key", payload.data(), payload.size());
  std::vector<std::uint8_t> decoded;
  ASSERT_TRUE(DecodePipelineCacheBlob(encoded.data(), encoded.size(),
                                      "device-key",
                                      kMaxPipelineCachePayloadBytes, &decoded));
  EXPECT_EQ(decoded, payload);
}

TEST(PipelineCacheBlob, RoundTripsAnEmptyPayload) {
  const std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-key", nullptr, 0);
  std::vector<std::uint8_t> decoded;
  ASSERT_TRUE(DecodePipelineCacheBlob(encoded.data(), encoded.size(),
                                      "device-key",
                                      kMaxPipelineCachePayloadBytes, &decoded));
  EXPECT_TRUE(decoded.empty());
}

TEST(PipelineCacheBlob, RejectsABadMagic) {
  const std::vector<std::uint8_t> payload = Payload(64);
  std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-key", payload.data(), payload.size());
  encoded[0] = 'X';
  std::vector<std::uint8_t> decoded;
  EXPECT_FALSE(DecodePipelineCacheBlob(encoded.data(), encoded.size(),
                                       "device-key",
                                       kMaxPipelineCachePayloadBytes, &decoded));
}

TEST(PipelineCacheBlob, RejectsAnotherKey) {
  const std::vector<std::uint8_t> payload = Payload(64);
  const std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-a", payload.data(), payload.size());
  std::vector<std::uint8_t> decoded;
  EXPECT_FALSE(DecodePipelineCacheBlob(encoded.data(), encoded.size(),
                                       "device-b",
                                       kMaxPipelineCachePayloadBytes, &decoded));
}

TEST(PipelineCacheBlob, RejectsATruncatedFile) {
  const std::vector<std::uint8_t> payload = Payload(1024);
  const std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-key", payload.data(), payload.size());
  for (std::size_t keep : {std::size_t{0}, std::size_t{8}, std::size_t{40},
                           encoded.size() - 1}) {
    std::vector<std::uint8_t> decoded;
    EXPECT_FALSE(DecodePipelineCacheBlob(encoded.data(), keep, "device-key",
                                         kMaxPipelineCachePayloadBytes,
                                         &decoded))
        << "accepted a file truncated to " << keep << " bytes";
  }
}

TEST(PipelineCacheBlob, RejectsACorruptedPayload) {
  const std::vector<std::uint8_t> payload = Payload(256);
  std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-key", payload.data(), payload.size());
  encoded.back() ^= 0xFF;
  std::vector<std::uint8_t> decoded;
  EXPECT_FALSE(DecodePipelineCacheBlob(encoded.data(), encoded.size(),
                                       "device-key",
                                       kMaxPipelineCachePayloadBytes, &decoded));
}

TEST(PipelineCacheBlob, RejectsAnOversizePayloadWithoutAllocating) {
  const std::vector<std::uint8_t> payload = Payload(1024);
  std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob("device-key", payload.data(), payload.size());
  // A caller with a tighter cap than the file claims must refuse it, rather
  // than handing the engine more initial data than it agreed to.
  std::vector<std::uint8_t> decoded;
  EXPECT_FALSE(DecodePipelineCacheBlob(encoded.data(), encoded.size(),
                                       "device-key", 512, &decoded));
  EXPECT_TRUE(decoded.empty());
}

TEST_F(VulkanPipelineCacheTest, PersistsAndSeedsAcrossConfigure) {
  const std::vector<std::uint8_t> payload = Payload(8192);
  PipelineCacheStore writer;
  writer.Configure(directory_, Key(), PipelineCacheMode::kOn);
  // A first session is active but has nothing to seed from; it is the session
  // that has to produce the blob.
  ASSERT_TRUE(writer.enabled());
  EXPECT_FALSE(writer.has_seed());
  writer.Adopt(0x1000);
  ASSERT_EQ(writer.Persist(0x1000, payload.data(), payload.size()), true);
  EXPECT_TRUE(fs::exists(PipelineCacheFilePath(directory_, writer.key())));

  PipelineCacheStore reader;
  reader.Configure(directory_, Key(), PipelineCacheMode::kOn);
  EXPECT_TRUE(reader.enabled());
  EXPECT_TRUE(reader.has_seed());
  EXPECT_EQ(reader.seed(), payload);
  EXPECT_EQ(reader.seed_size(), payload.size());
}

TEST_F(VulkanPipelineCacheTest, OffNeitherReadsNorWrites) {
  const std::vector<std::uint8_t> payload = Payload(512);
  PipelineCacheStore seed_writer;
  seed_writer.Configure(directory_, Key(), PipelineCacheMode::kOn);
  seed_writer.Adopt(0x2000);
  ASSERT_TRUE(seed_writer.Persist(0x2000, payload.data(), payload.size()));

  PipelineCacheStore off;
  off.Configure(directory_, Key(), PipelineCacheMode::kOff);
  EXPECT_FALSE(off.enabled());
  EXPECT_FALSE(off.has_seed());
  const std::vector<std::uint8_t> other = Payload(64);
  EXPECT_FALSE(off.Persist(0x2000, other.data(), other.size()));
}

TEST_F(VulkanPipelineCacheTest, ClearRemovesTheStoredBlobAndStaysUncached) {
  const std::vector<std::uint8_t> payload = Payload(512);
  PipelineCacheStore writer;
  writer.Configure(directory_, Key(), PipelineCacheMode::kOn);
  writer.Adopt(0x3000);
  ASSERT_TRUE(writer.Persist(0x3000, payload.data(), payload.size()));
  const fs::path file = PipelineCacheFilePath(directory_, writer.key());
  ASSERT_TRUE(fs::exists(file));

  PipelineCacheStore clearing;
  clearing.Configure(directory_, Key(), PipelineCacheMode::kClear);
  EXPECT_FALSE(clearing.enabled());
  EXPECT_FALSE(clearing.has_seed());
  EXPECT_FALSE(fs::exists(file));
  // Nothing is written back during the session that cleared it.
  EXPECT_FALSE(clearing.Persist(0x3000, payload.data(), payload.size()));
  EXPECT_FALSE(fs::exists(file));
}

TEST_F(VulkanPipelineCacheTest, DiscardsAnUnreadableStoredBlob) {
  const fs::path file =
      PipelineCacheFilePath(directory_, Key().ToString());
  fs::create_directories(directory_);
  {
    std::ofstream garbage(file, std::ios::binary);
    ASSERT_TRUE(garbage.is_open());
    const std::string text = "not a pipeline cache";
    garbage.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  PipelineCacheStore store;
  store.Configure(directory_, Key(), PipelineCacheMode::kOn);
  // Still active, so this session writes a fresh blob, but with nothing to
  // seed the engine with.
  EXPECT_TRUE(store.enabled());
  EXPECT_FALSE(store.has_seed());
  // The unreadable file is dropped, so the next launch is a clean miss
  // rather than the same rejection forever.
  EXPECT_FALSE(fs::exists(file));
}

TEST_F(VulkanPipelineCacheTest, SeedsOnlyTheFirstCache) {
  const std::vector<std::uint8_t> payload = Payload(256);
  PipelineCacheStore store;
  store.Configure(directory_, Key(), PipelineCacheMode::kOn);
  ASSERT_TRUE(store.enabled());

  store.Adopt(0xAAAA);
  EXPECT_TRUE(store.Owns(0xAAAA));
  store.Adopt(0xBBBB);
  // A second cache must not start racing the same file.
  EXPECT_FALSE(store.Owns(0xBBBB));

  EXPECT_TRUE(store.Persist(0xAAAA, payload.data(), payload.size()));
  EXPECT_FALSE(store.Persist(0xBBBB, payload.data(), payload.size()));
  store.Forget(0xAAAA);
  EXPECT_FALSE(store.Owns(0xAAAA));
  EXPECT_FALSE(store.Persist(0xAAAA, payload.data(), payload.size()));
}

TEST_F(VulkanPipelineCacheTest, RefusesAnOversizeOrEmptyPersist) {
  PipelineCacheStore store;
  store.Configure(directory_, Key(), PipelineCacheMode::kOn);
  store.Adopt(0xC0DE);
  const std::vector<std::uint8_t> payload = Payload(32);
  EXPECT_FALSE(store.Persist(0xC0DE, nullptr, 0));
  EXPECT_FALSE(store.Persist(0xC0DE, payload.data(),
                             kMaxPipelineCachePayloadBytes + 1));
  EXPECT_TRUE(store.Persist(0xC0DE, payload.data(), payload.size()));
}

TEST_F(VulkanPipelineCacheTest, RegistryKeepsStoresPerDevice) {
  PipelineCacheRegistry registry;
  EXPECT_EQ(registry.StoreFor(0x1), nullptr);
  // The store is registered even when the mode leaves it inactive, so the
  // adapter never has to distinguish "no store" from "store disabled".
  EXPECT_FALSE(registry.ConfigureDevice(0x1, directory_, Key(),
                                        PipelineCacheMode::kOff));
  ASSERT_NE(registry.StoreFor(0x1), nullptr);
  EXPECT_FALSE(registry.StoreFor(0x1)->enabled());
  EXPECT_TRUE(registry.ConfigureDevice(0x2, directory_, Key(),
                                       PipelineCacheMode::kOn));
  EXPECT_TRUE(registry.StoreFor(0x2)->enabled());
  // Devices keep independent stores.
  EXPECT_TRUE(registry.StoreFor(0x2)->enabled());
  EXPECT_FALSE(registry.StoreFor(0x1)->enabled());
  registry.RemoveDevice(0x1);
  EXPECT_EQ(registry.StoreFor(0x1), nullptr);
  EXPECT_FALSE(registry.ConfigureDevice(0, directory_, Key(),
                                        PipelineCacheMode::kOn));
}

}  // namespace
}  // namespace graphics
}  // namespace mocktail
