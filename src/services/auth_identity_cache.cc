#include "services/auth_identity_cache.h"

#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
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

namespace mocktail::services {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumCacheBytes = 16U * 1024U;
constexpr int kCacheSchemaVersion = 1;
constexpr const char* kCacheFileName = ".auth-identity.json";

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
      directory / (".auth-identity.tmp-" + std::to_string(getpid()) + "-" +
                   std::to_string(++generation));
  // 0600: this record is keyed by a credential's digest, so it is not something
  // another local account should be able to read.
  const int descriptor = open(temporary.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
  if (descriptor < 0) {
    if (error != nullptr) *error = "cannot create auth identity cache";
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
      if (error != nullptr) *error = "cannot write auth identity cache";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    if (error != nullptr) *error = "cannot persist auth identity cache";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, directory / kCacheFileName,
                          filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    if (error != nullptr) *error = "cannot publish auth identity cache";
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

bool Signed(const Json& document, const char* key, std::int64_t* value) {
  const auto found = document.find(key);
  if (found == document.end() || !found->is_number_integer()) return false;
  *value = found->get<std::int64_t>();
  return true;
}

}  // namespace

std::string AuthCookieDigest(std::string_view cookie_value) {
  if (cookie_value.empty()) return {};
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  if (EVP_Digest(cookie_value.data(), cookie_value.size(), digest, &length,
                 EVP_sha256(), nullptr) != 1 ||
      length != 32U) {
    return {};
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(static_cast<std::size_t>(length) * 2U);
  for (unsigned int index = 0; index < length; ++index) {
    hex.push_back(kHex[digest[index] >> 4U]);
    hex.push_back(kHex[digest[index] & 0x0FU]);
  }
  return hex;
}

std::int64_t AuthCacheNowEpoch() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool CachedAuthIdentityFresh(const CachedAuthIdentity& cached,
                             std::int64_t interval_minutes,
                             std::int64_t now_epoch) {
  if (interval_minutes <= 0) return false;
  if (cached.cookie_digest.empty() || cached.username.empty() ||
      cached.user_id <= 0 || cached.checked_at_epoch <= 0) {
    return false;
  }
  if (cached.checked_at_epoch > now_epoch) return false;
  return now_epoch - cached.checked_at_epoch < interval_minutes * 60;
}

std::filesystem::path AuthIdentityCachePath() {
  const char* root = std::getenv("MOCKTAIL_DATA_ROOT");
  if (root == nullptr || root[0] == '\0') return {};
  return std::filesystem::path(root) / kCacheFileName;
}

bool ReadCachedAuthIdentity(CachedAuthIdentity* cached) {
  if (cached == nullptr) return false;
  const std::filesystem::path path = AuthIdentityCachePath();
  if (path.empty()) return false;
  const std::string raw = ReadBoundedFile(path);
  if (raw.empty()) return false;
  Json document = Json::parse(raw, nullptr, false, true);
  if (document.is_discarded() || !document.is_object() ||
      !document.contains("schema_version") ||
      !document["schema_version"].is_number_integer() ||
      document["schema_version"] != kCacheSchemaVersion) {
    return false;
  }
  CachedAuthIdentity parsed;
  if (!Text(document, "cookie_digest", &parsed.cookie_digest) ||
      !Text(document, "username", &parsed.username) ||
      !Signed(document, "user_id", &parsed.user_id) ||
      !Signed(document, "checked_at_epoch", &parsed.checked_at_epoch)) {
    return false;
  }
  (void)Text(document, "display_name", &parsed.display_name);
  *cached = std::move(parsed);
  return true;
}

bool WriteCachedAuthIdentity(const CachedAuthIdentity& cached,
                             std::string* error) {
  const std::filesystem::path path = AuthIdentityCachePath();
  if (path.empty()) {
    if (error != nullptr) *error = "no data root for the auth identity cache";
    return false;
  }
  const Json document{
      {"schema_version", kCacheSchemaVersion},
      {"cookie_digest", cached.cookie_digest},
      {"user_id", cached.user_id},
      {"username", cached.username},
      {"display_name", cached.display_name},
      {"checked_at_epoch", cached.checked_at_epoch},
  };
  return WriteAtomicFile(path.parent_path(), document.dump(2) + "\n", error);
}

}  // namespace mocktail::services