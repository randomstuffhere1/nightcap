// Modified by vii from komaruworld/mocktail. See README "About this fork".
#ifndef MOCKTAIL_GRAPHICS_VULKAN_PIPELINE_CACHE_H_
#define MOCKTAIL_GRAPHICS_VULKAN_PIPELINE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mocktail {
namespace graphics {

// The driver hands back a pipeline cache blob in a format only that exact
// driver build understands, so a stored blob is keyed on the payload and the
// device that produced it. A Roblox update or a driver upgrade lands on a
// different key, which misses, rather than feeding the driver a blob it would
// only have to reject.
struct PipelineCacheKey {
  std::string device_name;
  std::uint32_t vendor_id = 0;
  std::uint32_t device_id = 0;
  std::uint32_t driver_version = 0;
  std::string driver_info;
  std::uint32_t api_version = 0;
  std::string payload_version;

  // Filename-safe and collision-resistant: the readable parts are sanitized
  // and clamped, and a hash of the untruncated key is appended so two keys
  // that sanitize alike still land in separate files.
  std::string ToString() const;
};

enum class PipelineCacheMode {
  kOn,
  kOff,
  kClear,
};

PipelineCacheMode ParsePipelineCacheMode(std::string_view value);
const char *PipelineCacheModeName(PipelineCacheMode mode);

// A blob past this size is not worth the disk, and a hostile or corrupt
// header must not be able to make the loader allocate from a claimed length.
inline constexpr std::size_t kMaxPipelineCachePayloadBytes = 64U << 20;
inline constexpr std::uint32_t kPipelineCacheFormatVersion = 1;

// Both are pure so the format can be tested without a Vulkan device: a bad
// magic, an unknown version, a truncated file, a mismatched key, and a failed
// checksum all return false rather than throwing or trusting the header.
std::vector<std::uint8_t> EncodePipelineCacheBlob(
    std::string_view key, const std::uint8_t *payload, std::size_t size);
bool DecodePipelineCacheBlob(const std::uint8_t *file, std::size_t file_size,
                             std::string_view expected_key,
                             std::size_t max_payload_bytes,
                             std::vector<std::uint8_t> *payload);

std::filesystem::path PipelineCacheFilePath(
    const std::filesystem::path &directory, const std::string &key);

// Per-device persistence for the engine's pipeline cache. Tracks the first
// cache created on the device, because that is the engine's global cache and
// the one whose reuse removes first-visit shader compiles. Later caches are
// left alone rather than racing the same file.
class PipelineCacheStore {
 public:
  // `clear` removes any stored blob and runs the session uncached, so the
  // setting is a one-shot repair rather than a permanent off switch.
  void Configure(std::filesystem::path directory, PipelineCacheKey key,
                 PipelineCacheMode mode);

  bool enabled() const { return enabled_; }
  // Non-empty when a stored blob can seed a new pipeline cache.
  bool has_seed() const { return !seed_.empty(); }
  const std::vector<std::uint8_t> &seed() const { return seed_; }

  // Claims the first cache created on the device, which is the engine's
  // global one. Callers that supplied their own initial data must not adopt,
  // so the store never overwrites data it did not come from.
  void Adopt(std::uintptr_t cache);
  bool Owns(std::uintptr_t cache) const;
  void Forget(std::uintptr_t cache);
  // Writes only the adopted cache, and only when the payload is non-empty
  // and within the size cap. Returns whether a file was written.
  bool Persist(std::uintptr_t cache, const std::uint8_t *payload,
               std::size_t size);

  const std::filesystem::path &file() const { return file_; }
  const std::string &key() const { return key_; }
  // The blob loaded at Configure time, for the startup log line.
  std::size_t seed_size() const { return seed_.size(); }

 private:
  std::filesystem::path file_;
  std::string key_;
  std::vector<std::uint8_t> seed_;
  std::uintptr_t owned_cache_ = 0;
  bool enabled_ = false;
};

// One store per VkDevice, created in vkCreateDevice. The adapter holds this
// as a member; the store itself stays free of Vulkan types so its format and
// persistence logic can be tested on its own.
class PipelineCacheRegistry {
 public:
  // `cache` is a VkPipelineCache handle widened to an integer.
  PipelineCacheStore *StoreFor(std::uintptr_t device);
  void RemoveDevice(std::uintptr_t device);
  bool ConfigureDevice(std::uintptr_t device,
                       const std::filesystem::path &directory,
                       const PipelineCacheKey &key, PipelineCacheMode mode);
  // The process-wide mode, read once from MOCKTAIL_PIPELINE_CACHE_MODE.
  static PipelineCacheMode ConfiguredMode();

 private:
  std::unordered_map<std::uintptr_t, PipelineCacheStore> stores_;
};

}  // namespace graphics
}  // namespace mocktail

#endif  // MOCKTAIL_GRAPHICS_VULKAN_PIPELINE_CACHE_H_
