#include "compat/file_digest_cache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>

namespace mocktail::compat {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumCacheBytes = 64U * 1024U;
constexpr int kCacheSchemaVersion = 1;
constexpr const char* kCacheFileName = ".file-digest.json";
constexpr const char* kDataRootVariable = "MOCKTAIL_DATA_ROOT";

std::string ReadBoundedFile(const std::filesystem::path& path) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return {};
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

bool WriteAtomicFile(const std::filesystem::path& directory,
                     std::string_view contents, std::string* error) {
  static std::uint64_t generation = 0;
  const std::filesystem::path temporary =
      directory / (".file-digest.tmp-" + std::to_string(getpid()) + "-" +
                   std::to_string(++generation));
  const int descriptor = open(temporary.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    if (error != nullptr) *error = "cannot create file digest cache";
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
      if (error != nullptr) *error = "cannot write file digest cache";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (error != nullptr) *error = "cannot persist file digest cache";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, directory / kCacheFileName, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    if (error != nullptr) *error = "cannot publish file digest cache";
    return false;
  }
  const int directory_descriptor =
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_descriptor >= 0) {
    (void)fsync(directory_descriptor);
    close(directory_descriptor);
  }
  return true;
}

bool Text(const Json& document, const char* key, std::string* value) {
  const auto found = document.find(key);
  if (found == document.end() || !found->is_string()) return false;
  *value = found->get<std::string>();
  return true;
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

bool ReadIdentity(const Json& document, FileIdentity* identity) {
  return Unsigned(document, "device", &identity->device) &&
         Unsigned(document, "inode", &identity->inode) &&
         Unsigned(document, "size", &identity->size) &&
         Signed(document, "mtime_seconds", &identity->mtime_seconds) &&
         Signed(document, "mtime_nanoseconds", &identity->mtime_nanoseconds);
}

Json IdentityJson(const FileIdentity& identity) {
  return Json{{"device", identity.device},
              {"inode", identity.inode},
              {"size", identity.size},
              {"mtime_seconds", identity.mtime_seconds},
              {"mtime_nanoseconds", identity.mtime_nanoseconds}};
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

}  // namespace

bool ReadFileIdentity(const std::filesystem::path& path, FileIdentity* identity) {
  if (identity == nullptr) return false;
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  struct stat metadata = {};
  const bool regular =
      fstat(descriptor, &metadata) == 0 && S_ISREG(metadata.st_mode) &&
      metadata.st_size >= 0;
  close(descriptor);
  if (!regular) return false;
  identity->device = static_cast<std::uint64_t>(metadata.st_dev);
  identity->inode = static_cast<std::uint64_t>(metadata.st_ino);
  identity->size = static_cast<std::uint64_t>(metadata.st_size);
  identity->mtime_seconds = static_cast<std::int64_t>(metadata.st_mtim.tv_sec);
  identity->mtime_nanoseconds =
      static_cast<std::int64_t>(metadata.st_mtim.tv_nsec);
  return true;
}

bool FileDigestRecordCovers(const FileDigestRecord& record,
                           const std::string& expected_path,
                           const FileIdentity& identity) {
  return !record.path.empty() && !record.digest.empty() &&
         record.path == expected_path && record.identity == identity;
}

std::filesystem::path FileDigestCachePath() {
  const char* root = std::getenv(kDataRootVariable);
  if (root == nullptr || root[0] == '\0') return {};
  return std::filesystem::path(root) / kCacheFileName;
}

bool ReadFileDigestRecord(FileDigestRecord* record) {
  if (record == nullptr) return false;
  const std::filesystem::path path = FileDigestCachePath();
  if (path.empty()) return false;
  const Json document = ParseCache(ReadBoundedFile(path));
  if (document.is_null()) return false;
  FileDigestRecord parsed;
  const auto identity = document.find("identity");
  if (!Text(document, "path", &parsed.path) ||
      !Text(document, "digest", &parsed.digest) ||
      identity == document.end() || !identity->is_object() ||
      !ReadIdentity(*identity, &parsed.identity)) {
    return false;
  }
  *record = std::move(parsed);
  return true;
}

bool WriteFileDigestRecord(const FileDigestRecord& record, std::string* error) {
  const std::filesystem::path path = FileDigestCachePath();
  if (path.empty()) {
    if (error != nullptr) *error = "no data root for the file digest cache";
    return false;
  }
  const Json document{{"schema_version", kCacheSchemaVersion},
                      {"path", record.path},
                      {"digest", record.digest},
                      {"identity", IdentityJson(record.identity)}};
  return WriteAtomicFile(path.parent_path(), document.dump(2) + "\n", error);
}

}  // namespace mocktail::compat