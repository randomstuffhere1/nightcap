#include "update/launch_cache.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using mocktail::update::CacheNowEpoch;
using mocktail::update::LibraryFingerprint;
using mocktail::update::LibraryReceiptCovers;
using mocktail::update::LibraryVerifyReceipt;
using mocktail::update::ReadLibraryFingerprint;
using mocktail::update::ReadLibraryVerifyReceipt;
using mocktail::update::ReadVersionCheckCache;
using mocktail::update::VersionCheckCache;
using mocktail::update::VersionCheckCacheFresh;
using mocktail::update::WriteLibraryVerifyReceipt;
using mocktail::update::WriteVersionCheckCache;

class LaunchCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("mocktail-launch-cache-" +
                  std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    ASSERT_TRUE(std::filesystem::create_directories(directory_));
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  std::filesystem::path Write(const std::string& name,
                              const std::string& contents) {
    const std::filesystem::path path = directory_ / name;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    stream.close();
    return path;
  }

  std::filesystem::path directory_;
  static int counter_;
};

int LaunchCacheTest::counter_ = 0;

LibraryFingerprint SampleFingerprint() {
  LibraryFingerprint fingerprint;
  fingerprint.device = 2050;
  fingerprint.inode = 14295857;
  fingerprint.size = 120604544;
  fingerprint.mtime_seconds = 1791638538;
  fingerprint.mtime_nanoseconds = 752148063;
  return fingerprint;
}

TEST_F(LaunchCacheTest, FingerprintRoundTripsThroughTheReceipt) {
  const std::string sha = std::string(64, 'a');
  LibraryVerifyReceipt written;
  written.payload_id = "3222-a67a91";
  written.library_sha256 = sha;
  written.fingerprint = SampleFingerprint();
  std::string error;
  ASSERT_TRUE(WriteLibraryVerifyReceipt(directory_, written, &error)) << error;

  LibraryVerifyReceipt read;
  ASSERT_TRUE(ReadLibraryVerifyReceipt(directory_, &read));
  EXPECT_EQ(read.payload_id, written.payload_id);
  EXPECT_EQ(read.library_sha256, written.library_sha256);
  EXPECT_EQ(read.fingerprint, written.fingerprint);
  EXPECT_TRUE(LibraryReceiptCovers(read, written.payload_id, sha,
                                   written.fingerprint));
}

TEST_F(LaunchCacheTest, ReceiptIsRefusedWhenTheFingerprintMoved) {
  const std::string sha = std::string(64, 'a');
  LibraryVerifyReceipt written;
  written.payload_id = "3222-a67a91";
  written.library_sha256 = sha;
  written.fingerprint = SampleFingerprint();
  std::string error;
  ASSERT_TRUE(WriteLibraryVerifyReceipt(directory_, written, &error)) << error;

  LibraryFingerprint edited = written.fingerprint;
  edited.size += 1;
  EXPECT_FALSE(LibraryReceiptCovers(written, written.payload_id, sha, edited));

  LibraryFingerprint retimed = written.fingerprint;
  retimed.mtime_nanoseconds += 1;
  EXPECT_FALSE(LibraryReceiptCovers(written, written.payload_id, sha, retimed));

  LibraryFingerprint swapped = written.fingerprint;
  swapped.inode += 1;
  EXPECT_FALSE(LibraryReceiptCovers(written, written.payload_id, sha, swapped));
}

TEST_F(LaunchCacheTest, ReceiptIsRefusedWhenAnyIdentityFieldDiffers) {
  const std::string sha = std::string(64, 'a');
  LibraryVerifyReceipt receipt;
  receipt.payload_id = "3222-a67a91";
  receipt.library_sha256 = sha;
  receipt.fingerprint = SampleFingerprint();

  EXPECT_FALSE(LibraryReceiptCovers(receipt, "3222-other", sha,
                                    receipt.fingerprint));
  EXPECT_FALSE(LibraryReceiptCovers(receipt, receipt.payload_id,
                                    std::string(64, 'b'), receipt.fingerprint));

  LibraryVerifyReceipt empty;
  EXPECT_FALSE(LibraryReceiptCovers(empty, receipt.payload_id, sha,
                                    receipt.fingerprint));
}

TEST_F(LaunchCacheTest, AMissingOrCorruptReceiptIsSimplyNotARecovery) {
  LibraryVerifyReceipt receipt;
  EXPECT_FALSE(ReadLibraryVerifyReceipt(directory_, &receipt));
  EXPECT_FALSE(ReadLibraryVerifyReceipt(directory_ / "absent", &receipt));

  // Each of these is a corrupt cache rather than a usable one. A launch that
  // cannot read its receipt must fall through to hashing, never to trusting.
  for (const char* contents : {
           "not json at all",
           "[]",
           "{}",
           R"({"schema_version": 2, "payload_id": "p", "library_sha256": "s",
               "fingerprint": {}})",
           R"({"schema_version": 1, "payload_id": 7, "library_sha256": "s",
               "fingerprint": {}})",
           R"({"schema_version": 1, "payload_id": "p", "library_sha256": "s"})",
           R"({"schema_version": 1, "payload_id": "p", "library_sha256": "s",
               "fingerprint": {"device": "x"}})",
       }) {
    const std::filesystem::path path = Write("corrupt.json", contents);
    std::error_code error;
    std::filesystem::rename(path, directory_ / ".library-verify.json", error);
    ASSERT_FALSE(error) << error.message();
    LibraryVerifyReceipt parsed;
    EXPECT_FALSE(ReadLibraryVerifyReceipt(directory_, &parsed))
        << "accepted corrupt receipt: " << contents;
  }
}

TEST_F(LaunchCacheTest, FingerprintReadsARealFileAndRefusesADirectory) {
  const std::filesystem::path path = Write("payload.bin", "hello");
  LibraryFingerprint fingerprint;
  ASSERT_TRUE(ReadLibraryFingerprint(path, &fingerprint));
  EXPECT_EQ(fingerprint.size, 5);

  // A directory is not the file the receipt describes, so it must not match.
  EXPECT_FALSE(ReadLibraryFingerprint(directory_, &fingerprint));
  EXPECT_FALSE(ReadLibraryFingerprint(directory_ / "absent", &fingerprint));
}

TEST_F(LaunchCacheTest, FingerprintRefusesASymlink) {
  const std::filesystem::path target = Write("target.bin", "hello");
  const std::filesystem::path link = directory_ / "link.bin";
  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  ASSERT_FALSE(error) << error.message();

  LibraryFingerprint fingerprint;
  // Opening with O_NOFOLLOW means a symlink is a failure, not a redirection to
  // somewhere else that happens to have the same size.
  EXPECT_FALSE(ReadLibraryFingerprint(link, &fingerprint));
}

TEST_F(LaunchCacheTest, VersionCacheRoundTrips) {
  VersionCheckCache written;
  written.version_name = "2.742.1059";
  written.version_code = 3222;
  written.checked_at_epoch = 1800000000;
  std::string error;
  ASSERT_TRUE(WriteVersionCheckCache(directory_, written, &error)) << error;

  VersionCheckCache read;
  ASSERT_TRUE(ReadVersionCheckCache(directory_, &read));
  EXPECT_EQ(read.version_name, written.version_name);
  EXPECT_EQ(read.version_code, written.version_code);
  EXPECT_EQ(read.checked_at_epoch, written.checked_at_epoch);
}

TEST_F(LaunchCacheTest, VersionCacheRefusesToStoreAnEmptyVersion) {
  VersionCheckCache empty;
  std::string error;
  EXPECT_FALSE(WriteVersionCheckCache(directory_, empty, &error));

  VersionCheckCache zero_code;
  zero_code.version_name = "2.742.1059";
  EXPECT_FALSE(WriteVersionCheckCache(directory_, zero_code, &error));
}

TEST_F(LaunchCacheTest, VersionCacheIsFreshOnlyInsideItsInterval) {
  VersionCheckCache cache;
  cache.version_name = "2.742.1059";
  cache.version_code = 3222;
  cache.checked_at_epoch = 1800000000;

  // The moment of the check is the start of the window, not the end of it.
  EXPECT_TRUE(VersionCheckCacheFresh(cache, 6, 1800000000));
  // 6 hours after the check is the boundary, and it is already stale.
  const std::int64_t six_hours = 1800000000 + 6 * 3600;
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 6, six_hours));
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 6, six_hours + 1));
  EXPECT_TRUE(VersionCheckCacheFresh(cache, 6, six_hours - 1));
  // A longer interval keeps the same record usable for longer.
  EXPECT_TRUE(VersionCheckCacheFresh(cache, 24, six_hours));
}

TEST_F(LaunchCacheTest, AZeroIntervalAlwaysAsks) {
  VersionCheckCache cache;
  cache.version_name = "2.742.1059";
  cache.version_code = 3222;
  cache.checked_at_epoch = CacheNowEpoch();
  // 0 is the "check every launch" setting, so even a just-written record is not
  // fresh.
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 0, CacheNowEpoch()));
  EXPECT_FALSE(VersionCheckCacheFresh(cache, -1, CacheNowEpoch()));
}

TEST_F(LaunchCacheTest, AnImpossibleTimestampIsNotFresh) {
  VersionCheckCache cache;
  cache.version_name = "2.742.1059";
  cache.version_code = 3222;
  cache.checked_at_epoch = 0;
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 6, CacheNowEpoch()));

  // A record from the future means the clock moved; it proves nothing about how
  // old the check really is.
  cache.checked_at_epoch = CacheNowEpoch() + 100000;
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 6, CacheNowEpoch()));
}

TEST_F(LaunchCacheTest, AnIncompleteVersionRecordIsNotFresh) {
  VersionCheckCache cache;
  cache.checked_at_epoch = CacheNowEpoch();
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 6, CacheNowEpoch()));
  cache.version_name = "2.742.1059";
  EXPECT_FALSE(VersionCheckCacheFresh(cache, 6, CacheNowEpoch()));
}

}  // namespace