#ifndef MOCKTAIL_UPDATE_LAUNCH_CACHE_H_
#define MOCKTAIL_UPDATE_LAUNCH_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace mocktail::update {

// Two caches exist only to keep a relaunch off the disk-hash and the network.
//
// Both are accelerators, never authorities: a missing, unreadable, stale, or
// mismatched entry falls straight through to the existing work (the full
// SHA-256, or the provider request), so the worst a corrupt cache can do is
// make a launch slow again. Nothing here weakens the checks that run when a
// payload is staged, promoted, or rolled back.

// Identity of libroblox.so as the filesystem sees it. Two launches that agree on
// this agree on the file unless it was replaced by something that preserved
// size, inode, and both mtime fields, which is not a realistic local edit.
struct LibraryFingerprint {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size = 0;
  std::int64_t mtime_seconds = 0;
  std::int64_t mtime_nanoseconds = 0;

  bool operator==(const LibraryFingerprint& other) const {
    return device == other.device && inode == other.inode &&
           size == other.size && mtime_seconds == other.mtime_seconds &&
           mtime_nanoseconds == other.mtime_nanoseconds;
  }
};

// False when the path is missing, not a regular file, or a symlink. The read
// uses O_NOFOLLOW, so a symlink is a failure rather than a redirection.
bool ReadLibraryFingerprint(const std::filesystem::path& library,
                            LibraryFingerprint* fingerprint);

struct LibraryVerifyReceipt {
  std::string payload_id;
  std::string library_sha256;
  LibraryFingerprint fingerprint;
};

// True when a receipt proves this exact file was already hashed and matched
// this hash. Every field must agree; a partial match is not a match.
bool LibraryReceiptCovers(const LibraryVerifyReceipt& receipt,
                          std::string_view payload_id,
                          std::string_view library_sha256,
                          const LibraryFingerprint& fingerprint);

bool ReadLibraryVerifyReceipt(const std::filesystem::path& data_root,
                              LibraryVerifyReceipt* receipt);

// A failed write is reported but is not an error the caller has to act on: the
// hash has already been compared, so the launch is still verified, it just pays
// the hash again next time.
bool WriteLibraryVerifyReceipt(const std::filesystem::path& data_root,
                               const LibraryVerifyReceipt& receipt,
                               std::string* error);

// The last successful provider answer. Caching the whole ProviderVersion would
// be enough, but only the two fields the coordinator compares are kept, so a
// stale record cannot carry a provider error string into a later run.
struct VersionCheckCache {
  std::string version_name;
  std::uint64_t version_code = 0;
  std::int64_t checked_at_epoch = 0;
};

bool ReadVersionCheckCache(const std::filesystem::path& data_root,
                           VersionCheckCache* cache);

bool WriteVersionCheckCache(const std::filesystem::path& data_root,
                            const VersionCheckCache& cache, std::string* error);

// A non-positive interval means "always ask", which is what 0 configures.
bool VersionCheckCacheFresh(const VersionCheckCache& cache,
                            std::int64_t interval_hours,
                            std::int64_t now_epoch);

// Wall-clock seconds since the epoch, used to stamp the version cache. Exposed
// so the coordinator and the cache agree on where "now" comes from.
std::int64_t CacheNowEpoch();

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_LAUNCH_CACHE_H_