// Modified by vii from komaruworld/mocktail. See README "About this fork".

#include "mocktail/graphics/vulkan_pipeline_cache.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace mocktail {
namespace graphics {
namespace {

constexpr char kMagic[8] = {'N', 'C', 'P', 'P', 'C', '0', '1', '\0'};
// magic + version + header bytes + key bytes + payload crc, then the payload
// and whole-file lengths.
constexpr std::size_t kHeaderBytes = 8 + 4 * 4 + 8 * 2;
constexpr std::size_t kMaxKeyBytes = 512;
constexpr std::size_t kMaxSanitizedChars = 96;

void AppendU32(std::vector<std::uint8_t> *out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(std::vector<std::uint8_t> *out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

std::uint32_t ReadU32(const std::uint8_t *data) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(data[index]) << (index * 8);
  }
  return value;
}

std::uint64_t ReadU64(const std::uint8_t *data) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8);
  }
  return value;
}

// FNV-1a. It only has to keep distinct keys apart after sanitizing, so 64 bits
// of a non-cryptographic hash is the right size.
std::uint64_t HashKey(std::string_view key) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char character : key) {
    hash ^= static_cast<std::uint8_t>(character);
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint32_t Crc32(const std::uint8_t *data, std::size_t size) {
  static std::uint32_t table[256];
  static std::once_flag once;
  std::call_once(once, [] {
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0 ? (value >> 1) ^ 0xEDB88320U : value >> 1;
      }
      table[index] = value;
    }
  });
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    crc = table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::string Hex(std::uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int index = 15; index >= 0; --index) {
    text[static_cast<std::size_t>(index)] = kDigits[value & 0xFULL];
    value >>= 4;
  }
  return text;
}

std::string Sanitize(std::string_view text) {
  std::string result;
  for (const char character : text) {
    if (result.size() >= kMaxSanitizedChars) break;
    const bool safe = (character >= 'a' && character <= 'z') ||
                      (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') ||
                      character == '.' || character == '-' || character == '_';
    result.push_back(safe ? character : '_');
  }
  if (result.empty()) {
    result = "unknown";
  }
  return result;
}

bool WriteAll(int descriptor, const std::uint8_t *data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t written =
        ::write(descriptor, data + offset, size - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

}  // namespace

std::string PipelineCacheKey::ToString() const {
  const std::string identity = device_name + "|" + driver_info + "|" +
                               payload_version + "|" + Hex(vendor_id) + "|" +
                               Hex(device_id) + "|" + Hex(driver_version) + "|" +
                               Hex(api_version);
  return Sanitize(device_name) + "-" + Sanitize(payload_version) + "-" +
         Hex(HashKey(identity));
}

PipelineCacheMode ParsePipelineCacheMode(std::string_view value) {
  if (value == "off" || value == "0" || value == "false" ||
      value == "disabled") {
    return PipelineCacheMode::kOff;
  }
  if (value == "clear" || value == "reset") {
    return PipelineCacheMode::kClear;
  }
  return PipelineCacheMode::kOn;
}

const char *PipelineCacheModeName(PipelineCacheMode mode) {
  switch (mode) {
    case PipelineCacheMode::kOff:
      return "off";
    case PipelineCacheMode::kClear:
      return "clear";
    case PipelineCacheMode::kOn:
      break;
  }
  return "on";
}

std::vector<std::uint8_t> EncodePipelineCacheBlob(
    std::string_view key, const std::uint8_t *payload, std::size_t size) {
  const std::size_t key_size = std::min(key.size(), kMaxKeyBytes);
  const std::size_t body = (payload != nullptr) ? size : 0;
  std::vector<std::uint8_t> encoded;
  encoded.reserve(kHeaderBytes + key_size + body);
  encoded.insert(encoded.end(), std::begin(kMagic), std::end(kMagic));
  AppendU32(&encoded, kPipelineCacheFormatVersion);
  AppendU32(&encoded, static_cast<std::uint32_t>(kHeaderBytes));
  AppendU32(&encoded, static_cast<std::uint32_t>(key_size));
  AppendU32(&encoded, Crc32(payload, body));
  AppendU64(&encoded, body);
  AppendU64(&encoded, kHeaderBytes + key_size + body);
  encoded.insert(encoded.end(), key.begin(),
                 key.begin() + static_cast<std::ptrdiff_t>(key_size));
  if (body != 0) {
    encoded.insert(encoded.end(), payload, payload + body);
  }
  return encoded;
}

bool DecodePipelineCacheBlob(const std::uint8_t *file, std::size_t file_size,
                             std::string_view expected_key,
                             std::size_t max_payload_bytes,
                             std::vector<std::uint8_t> *payload) {
  if (file == nullptr || payload == nullptr || file_size < kHeaderBytes) {
    return false;
  }
  if (std::memcmp(file, kMagic, sizeof(kMagic)) != 0) {
    return false;
  }
  const std::uint8_t *header = file + sizeof(kMagic);
  if (ReadU32(header) != kPipelineCacheFormatVersion ||
      ReadU32(header + 4) != kHeaderBytes) {
    return false;
  }
  const std::uint32_t key_bytes = ReadU32(header + 8);
  const std::uint32_t expected_crc = ReadU32(header + 12);
  const std::uint64_t body_bytes = ReadU64(header + 16);
  const std::uint64_t claimed_file_bytes = ReadU64(header + 24);
  if (key_bytes > kMaxKeyBytes || expected_key.size() != key_bytes) {
    return false;
  }
  // Every length is checked against the bytes actually present before any of
  // it is read, so a truncated or lying header cannot walk off the buffer, and
  // a claimed payload never allocates.
  if (body_bytes > max_payload_bytes ||
      body_bytes > kMaxPipelineCachePayloadBytes) {
    return false;
  }
  if (claimed_file_bytes != kHeaderBytes + key_bytes + body_bytes ||
      claimed_file_bytes > file_size) {
    return false;
  }
  const std::uint8_t *key = file + kHeaderBytes;
  if (std::memcmp(key, expected_key.data(), key_bytes) != 0) {
    return false;
  }
  const std::uint8_t *body = key + key_bytes;
  if (Crc32(body, static_cast<std::size_t>(body_bytes)) != expected_crc) {
    return false;
  }
  payload->assign(body, body + static_cast<std::size_t>(body_bytes));
  return true;
}

std::filesystem::path PipelineCacheFilePath(
    const std::filesystem::path &directory, const std::string &key) {
  return directory / (key + ".vkpipelinecache");
}

void PipelineCacheStore::Configure(std::filesystem::path directory,
                                   PipelineCacheKey key,
                                   PipelineCacheMode mode) {
  key_ = key.ToString();
  file_ = PipelineCacheFilePath(directory, key_);
  seed_.clear();
  enabled_ = false;
  owned_cache_ = 0;
  if (mode == PipelineCacheMode::kClear) {
    std::error_code error;
    std::filesystem::remove(file_, error);
    return;
  }
  if (mode == PipelineCacheMode::kOff || directory.empty() || key_.empty()) {
    return;
  }
  // Persistence is active from the first session, which is the one that has
  // no blob to seed from and is therefore the one that has to write one.
  enabled_ = true;
  std::error_code error;
  if (!std::filesystem::exists(file_, error) || error) {
    return;
  }
  // One byte past the cap, so an oversize file is rejected on its reported
  // size rather than after reading it.
  constexpr std::size_t kReadCap =
      kMaxPipelineCachePayloadBytes + kHeaderBytes + kMaxKeyBytes;
  const std::uintmax_t reported = std::filesystem::file_size(file_, error);
  if (error || reported == 0 || reported > kReadCap) {
    return;
  }
  std::vector<std::uint8_t> contents(static_cast<std::size_t>(reported));
  const int descriptor = ::open(file_.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    return;
  }
  bool complete = true;
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        ::read(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0) {
      if (errno == EINTR) continue;
      complete = false;
      break;
    }
    if (bytes == 0) {
      complete = false;
      break;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  close(descriptor);
  if (!complete) {
    return;
  }
  if (!DecodePipelineCacheBlob(contents.data(), contents.size(), key_,
                               kMaxPipelineCachePayloadBytes, &seed_)) {
    // A blob this device cannot read is discarded, not retried every launch.
    seed_.clear();
    std::error_code remove_error;
    std::filesystem::remove(file_, remove_error);
    return;
  }
  enabled_ = true;
}

void PipelineCacheStore::Adopt(std::uintptr_t cache) {
  // The first cache the engine creates is its global one, and the only one
  // worth keeping: later caches are left alone rather than racing the file.
  if (owned_cache_ == 0) {
    owned_cache_ = cache;
  }
}

bool PipelineCacheStore::Owns(std::uintptr_t cache) const {
  return enabled_ && owned_cache_ != 0 && owned_cache_ == cache;
}

void PipelineCacheStore::Forget(std::uintptr_t cache) {
  if (owned_cache_ == cache) {
    owned_cache_ = 0;
  }
}

bool PipelineCacheStore::Persist(std::uintptr_t cache,
                                 const std::uint8_t *payload,
                                 std::size_t size) {
  if (!Owns(cache) || payload == nullptr || size == 0 ||
      size > kMaxPipelineCachePayloadBytes) {
    return false;
  }
  const std::vector<std::uint8_t> encoded =
      EncodePipelineCacheBlob(key_, payload, size);
  std::error_code error;
  std::filesystem::create_directories(file_.parent_path(), error);
  error.clear();
  const std::filesystem::path temporary = file_.string() + ".tmp";
  const int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    return false;
  }
  const bool wrote = WriteAll(descriptor, encoded.data(), encoded.size());
  const bool synced = wrote && fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return false;
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, file_, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    return false;
  }
  return true;
}

PipelineCacheStore *PipelineCacheRegistry::StoreFor(std::uintptr_t device) {
  auto entry = stores_.find(device);
  return entry == stores_.end() ? nullptr : &entry->second;
}

void PipelineCacheRegistry::RemoveDevice(std::uintptr_t device) {
  stores_.erase(device);
}

bool PipelineCacheRegistry::ConfigureDevice(std::uintptr_t device,
                                            const std::filesystem::path &directory,
                                            const PipelineCacheKey &key,
                                            PipelineCacheMode mode) {
  if (device == 0) {
    return false;
  }
  PipelineCacheStore &store = stores_[device];
  store.Configure(directory, key, mode);
  return store.enabled();
}

PipelineCacheMode PipelineCacheRegistry::ConfiguredMode() {
  static const PipelineCacheMode mode = ParsePipelineCacheMode(
      std::getenv("MOCKTAIL_PIPELINE_CACHE_MODE") != nullptr
          ? std::string_view(std::getenv("MOCKTAIL_PIPELINE_CACHE_MODE"))
          : std::string_view());
  return mode;
}

}  // namespace graphics
}  // namespace mocktail
