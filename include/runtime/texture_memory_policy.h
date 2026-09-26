#ifndef MOCKTAIL_RUNTIME_TEXTURE_MEMORY_POLICY_H_
#define MOCKTAIL_RUNTIME_TEXTURE_MEMORY_POLICY_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

// Roblox sizes its texture budget for the Android guest, so a PC host still
// gets caps.videoMemory = 64 MiB. These publish a host-sized budget through
// Roblox's own FIntRenderForceVideoMemorySize override.

// Total usable host memory in bytes, or 0 when it cannot be read.
std::uint64_t DetectHostMemoryBytes();

// 0 leaves Roblox's budget alone; otherwise an eighth of host memory, clamped
// to a PC-class range.
std::uint64_t CalculateTextureMemoryBudgetBytes(std::uint64_t host_memory_bytes);

// The configured budget in bytes. `override_megabytes` of 0 means "decide from
// host memory"; anything else is used as given, up to the same ceiling the
// automatic path clamps to. A value the client cannot represent as an FInt is
// rejected rather than truncated.
inline constexpr std::uint64_t kMaximumTextureMemoryMegabytes = 65536;

std::uint64_t ResolveTextureMemoryBudgetBytes(std::uint64_t host_memory_bytes,
                                              std::uint64_t override_megabytes,
                                              bool* valid);

// A 0 budget, or a key already present, is preserved: an explicit fflags.json
// or environment override wins.
bool MergeTextureMemoryClientSettingsOverrides(std::uint64_t budget_bytes,
                                               std::string_view base_json,
                                               std::string* merged_json,
                                               std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_TEXTURE_MEMORY_POLICY_H_
