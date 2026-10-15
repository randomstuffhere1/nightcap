#ifndef MOCKTAIL_UPDATE_COMPATIBILITY_CATALOG_H_
#define MOCKTAIL_UPDATE_COMPATIBILITY_CATALOG_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail::update {

// Optional, exact-build graphics fixups. Absent on every shipped profile: a
// profile has to opt in, and the runtime keeps its stock behavior otherwise.
// Kept here so the updater rejects a manifest whose fixup fields have the
// wrong type, before such a manifest can reach a runtime.
struct SupportedPayloadGraphicsFixups {
  int swapchain_fixup = 0;
  bool shader_fixups = false;
  bool use_new_text_renderer = false;
};

struct SupportedPayloadProfile {
  std::string version_name;
  std::uint64_t version_code = 0;
  std::string elf_build_id;
  SupportedPayloadGraphicsFixups graphics_fixups;
};

struct CompatibilityCatalogResult {
  std::vector<SupportedPayloadProfile> profiles;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

CompatibilityCatalogResult LoadCompatibilityCatalog(
    const std::filesystem::path& path);

std::optional<SupportedPayloadProfile> PreferredSupportedProfile(
    const std::vector<SupportedPayloadProfile>& profiles);

std::optional<SupportedPayloadProfile> FindSupportedProfile(
    const std::vector<SupportedPayloadProfile>& profiles,
    std::string_view version_name, std::uint64_t version_code = 0);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_COMPATIBILITY_CATALOG_H_
