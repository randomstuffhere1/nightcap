#include "update/launch_cache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>

namespace mocktail::update {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumCacheBytes = 64U * 1024U;
constexpr const char* kLibraryReceiptName = ".library-verify.json";
constexpr const char* kVersionCacheName = ".version-check.json";
constexpr int kCacheSchemaVersion = 1;

std::string ReadBoundedFile(const std::filesystem::path& path,
                            bool* missing_ok) {
  if (missing_ok != nullptr) *missing_ok = false;
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (missing_ok != nullptr && errno == ENOENT) *missing_ok = true;
    return {};
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > kMaximumCacheBytes) {
    close(descriptor);
    return {};
  }
  std::string contents;
  contents.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes = read(descriptor, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      return {};
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return contents;
}

bool WriteAtomicFile(const std::filesystem::path& root,
                     const std::string& name, std::string_view contents,
                     std::string* error) {
  static std::uint64_t generation = 0;
  const std::filesystem::path temporary =
      root / ("." + name + ".tmp-" + std::to_string(getpid()) + "-" +
              std::to_string(++generation));
  const int descriptor = open(temporary.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    if (error != nullptr) *error = "cannot create launch cache";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      if (error != nullptr) *error = "cannot write launch cache";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (error != nullptr) *error = "cannot persist launch cache";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, root / name, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    if (error != nullptr) *error = "cannot publish launch cache";
    return false;
  }
  const int root_descriptor =
      open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (root_descriptor >= 0) {
    (void)fsync(root_descriptor);
    close(root_descriptor);
  }
  return true;
}

Json ParseCache(const std::string& contents) {
  if (contents.empty()) return Json();
  Json document = Json::parse(contents, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) return Json();
  if (!document.contains("schema_version") ||
      !document["schema_version"].is_number_integer() ||
      document["schema_version"] != kCacheSchemaVersion) {
    return Json();
  }
  return document;
}

bool Unsigned(const Json& document, const char* key, std::uint64_t* value) {
  const auto found = document.find(key);
  if (found == document.end() || !found->is_number_unsigned()) return false;
  *value = found->get<std::uint64_t>();
  return true;
}

bool Signed(const Json& document, const char* key, std::int64_t* value) {
  const auto found = document.find(key);
  if (found == document.end() || !found->is_number_integer()) return false;
  *value = found->get<std::int64_t>();
  return true;
}

bool Text(const Json& document, const char* key, std::string* value) {
  const auto found = document.find(key);
  if (found == document.end() || !found->is_string()) return false;
  *value = found->get<std::string>();
  return true;
}

bool ReadFingerprint(const Json& document, LibraryFingerprint* fingerprint) {
  return Unsigned(document, "device", &fingerprint->device) &&
         Unsigned(document, "inode", &fingerprint->inode) &&
         Unsigned(document, "size", &fingerprint->size) &&
         Signed(document, "mtime_seconds", &fingerprint->mtime_seconds) &&
         Signed(document, "mtime_nanoseconds", &fingerprint->mtime_nanoseconds);
}

Json FingerprintJson(const LibraryFingerprint& fingerprint) {
  return Json{{"device", fingerprint.device},
              {"inode", fingerprint.inode},
              {"size", fingerprint.size},
              {"mtime_seconds", fingerprint.mtime_seconds},
              {"mtime_nanoseconds", fingerprint.mtime_nanoseconds}};
}

std::int64_t NowEpochSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

bool ReadLibraryFingerprint(const std::filesystem::path& library,
                            LibraryFingerprint* fingerprint) {
  if (fingerprint == nullptr) return false;
  const int descriptor = open(library.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  struct stat metadata = {};
  const bool regular = fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
                       metadata.st_size >= 0;
  close(descriptor);
  if (!regular) return false;
  fingerprint->device = static_cast<std::uint64_t>(metadata.st_dev);
  fingerprint->inode = static_cast<std::uint64_t>(metadata.st_ino);
  fingerprint->size = static_cast<std::uint64_t>(metadata.st_size);
  fingerprint->mtime_seconds = static_cast<std::int64_t>(metadata.st_mtim.tv_sec);
  fingerprint->mtime_nanoseconds =
      static_cast<std::int64_t>(metadata.st_mtim.tv_nsec);
  return true;
}

bool LibraryReceiptCovers(const LibraryVerifyReceipt& receipt,
                          std::string_view payload_id,
                          std::string_view library_sha256,
                          const LibraryFingerprint& fingerprint) {
  return !receipt.payload_id.empty() && !receipt.library_sha256.empty() &&
         receipt.payload_id == payload_id &&
         receipt.library_sha256 == library_sha256 &&
         receipt.fingerprint == fingerprint;
}

bool ReadLibraryVerifyReceipt(const std::filesystem::path& data_root,
                              LibraryVerifyReceipt* receipt) {
  if (receipt == nullptr) return false;
  bool missing = false;
  const std::string raw =
      ReadBoundedFile(data_root / kLibraryReceiptName, &missing);
  const Json document = ParseCache(raw);
  if (document.is_null()) return false;
  LibraryVerifyReceipt parsed;
  const auto fingerprint = document.find("fingerprint");
  if (!Text(document, "payload_id", &parsed.payload_id) ||
      !Text(document, "library_sha256", &parsed.library_sha256) ||
      fingerprint == document.end() || !fingerprint->is_object() ||
      !ReadFingerprint(*fingerprint, &parsed.fingerprint)) {
    return false;
  }
  *receipt = std::move(parsed);
  return true;
}

bool WriteLibraryVerifyReceipt(const std::filesystem::path& data_root,
                               const LibraryVerifyReceipt& receipt,
                               std::string* error) {
  const Json document{{"schema_version", kCacheSchemaVersion},
                      {"payload_id", receipt.payload_id},
                      {"library_sha256", receipt.library_sha256},
                      {"fingerprint", FingerprintJson(receipt.fingerprint)}};
  return WriteAtomicFile(data_root, kLibraryReceiptName,
                         document.dump(2) + "\n", error);
}

bool ReadVersionCheckCache(const std::filesystem::path& data_root,
                           VersionCheckCache* cache) {
  if (cache == nullptr) return false;
  const Json document =
      ParseCache(ReadBoundedFile(data_root / kVersionCacheName, nullptr));
  if (document.is_null()) return false;
  VersionCheckCache parsed;
  if (!Text(document, "version_name", &parsed.version_name) ||
      !Unsigned(document, "version_code", &parsed.version_code) ||
      !Signed(document, "checked_at_epoch", &parsed.checked_at_epoch)) {
    return false;
  }
  if (parsed.version_name.empty() || parsed.version_code == 0) return false;
  *cache = std::move(parsed);
  return true;
}

bool WriteVersionCheckCache(const std::filesystem::path& data_root,
                            const VersionCheckCache& cache, std::string* error) {
  if (cache.version_name.empty() || cache.version_code == 0) {
    if (error != nullptr) *error = "refusing to cache an empty version";
    return false;
  }
  const Json document{{"schema_version", kCacheSchemaVersion},
                      {"version_name", cache.version_name},
                      {"version_code", cache.version_code},
                      {"checked_at_epoch", cache.checked_at_epoch}};
  return WriteAtomicFile(data_root, kVersionCacheName, document.dump(2) + "\n",
                         error);
}

bool VersionCheckCacheFresh(const VersionCheckCache& cache,
                            std::int64_t interval_hours,
                            std::int64_t now_epoch) {
  if (interval_hours <= 0) return false;
  if (cache.version_name.empty() || cache.version_code == 0) return false;
  if (cache.checked_at_epoch <= 0 || now_epoch <= 0) return false;
  if (cache.checked_at_epoch > now_epoch) {
    // A record from the future means the clock moved, so it is not evidence of
    // how old the last real check was; ask again.
    return false;
  }
  const std::int64_t interval_seconds = interval_hours * 3600;
  // The interval is a half-open window: a check exactly `interval_hours` old is
  // already due, which keeps "check every N hours" from drifting by one launch.
  return now_epoch - cache.checked_at_epoch < interval_seconds;
}

std::int64_t CacheNowEpoch() { return NowEpochSeconds(); }

}  // namespace mocktail::update