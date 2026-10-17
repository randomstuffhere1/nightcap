#ifndef MOCKTAIL_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_
#define MOCKTAIL_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_

#include <cstdint>
#include <vector>

namespace libc_shim {

// Converts one opaque ETC1 RGB block to an opaque BC1 RGB block.
void ConvertEtc1RgbBlockToBc1(const uint8_t *etc1, uint8_t *bc1);

// Rewrites a bundled ETC1 sky texture into BC1 in place, which is the only
// compressed format the desktop Vulkan path can sample. The result is memoised
// per process and, when MOCKTAIL_SKY_TRANSCODE_CACHE_DIR names a directory,
// on disk as well: converting the twelve bundled sky textures costs ~190 ms of
// CPU on the worker threads that load them, on every launch, for input that
// only changes when the payload does.
bool TranscodeEtc1SkyTextureForVulkan(const char *path,
                                      std::vector<unsigned char> *data);

// Test-only: drops the per-process memo so a test can observe the on-disk
// cache. Production code never needs to call this.
void ResetEtc1SkyTranscodeMemoryCache();

} // namespace libc_shim

#endif // MOCKTAIL_LIBC_SHIM_VULKAN_ETC1_SKY_TRANSCODER_H_
