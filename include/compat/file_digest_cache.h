#ifndef MOCKTAIL_COMPAT_FILE_DIGEST_CACHE_H_
#define MOCKTAIL_COMPAT_FILE_DIGEST_CACHE_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace mocktail::compat {

// Identity of a file as the filesystem sees it: device, inode, size, and both
// mtime fields. Two observations that agree on all five describe the same file
// unless it was replaced by something that deliberately preserved them.
struct FileIdentity {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size = 0;
  std::int64_t mtime_seconds = 0;
  std::int64_t mtime_nanoseconds = 0;

  bool operator==(const FileIdentity& other) const {
    return device == other.device && inode == other.inode &&
           size == other.size && mtime_seconds == other.mtime_seconds &&
           mtime_nanoseconds == other.mtime_nanoseconds;
  }
};

// False when the path is missing, is not a regular file, or is a symlink. The
// read uses O_NOFOLLOW, so a symlink is a failure rather than a redirection.
bool ReadFileIdentity(const std::filesystem::path& path, FileIdentity* identity);

// A record that a file with this identity hashed to this value.
//
// This is a cache and nothing more. It exists because two independent checks in
// one launch each hash the 118 MiB libroblox.so, and every subsequent launch
// re-hashes it again. It can only ever make a check cheaper: an absent,
// unreadable, malformed, or mismatched record means the caller hashes the file
// itself. Nothing in the staging, promotion, or rollback path consults it.
struct FileDigestRecord {
  std::string path;
  std::string digest;
  FileIdentity identity;
};

bool FileDigestRecordCovers(const FileDigestRecord& record,
                           const std::string& expected_path,
                           const FileIdentity& identity);

// The cache file lives under MOCKTAIL_DATA_ROOT. When that is unset the cache
// is simply unavailable, and callers hash as they always did.
std::filesystem::path FileDigestCachePath();

bool ReadFileDigestRecord(FileDigestRecord* record);

bool WriteFileDigestRecord(const FileDigestRecord& record, std::string* error);

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_FILE_DIGEST_CACHE_H_