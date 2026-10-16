#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/mocktail_rs.h"
#include "mocktail/graphics/mocktail_rs_bridge.h"

#include "rust_kernel_toggle.h"

// Guards the one place the C ABI and the C++ runtime can silently drift apart.
//
// mocktail_rs.h encodes EtcFormat's discriminants as plain integers for the
// FFI. If that enum gains or reorders a value in etc2_decoder.h, these
// static_asserts fail the build rather than leaving the Rust side decoding
// EAC R11 texels as ETC2 RGB8.
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEtc2Rgb8) ==
        MOCKTAIL_RS_ETC2_RGB8,
    "MOCKTAIL_RS_ETC2_RGB8 no longer matches mocktail::graphics::EtcFormat");
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEtc2Rgb8A1) ==
        MOCKTAIL_RS_ETC2_RGB8A1,
    "MOCKTAIL_RS_ETC2_RGB8A1 no longer matches mocktail::graphics::EtcFormat");
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEtc2Rgba8) ==
        MOCKTAIL_RS_ETC2_RGBA8,
    "MOCKTAIL_RS_ETC2_RGBA8 no longer matches mocktail::graphics::EtcFormat");
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEacR11) ==
        MOCKTAIL_RS_EAC_R11,
    "MOCKTAIL_RS_EAC_R11 no longer matches mocktail::graphics::EtcFormat");
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEacR11Signed) ==
        MOCKTAIL_RS_EAC_R11_SIGNED,
    "MOCKTAIL_RS_EAC_R11_SIGNED no longer matches mocktail::graphics::EtcFormat");
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEacRg11) ==
        MOCKTAIL_RS_EAC_RG11,
    "MOCKTAIL_RS_EAC_RG11 no longer matches mocktail::graphics::EtcFormat");
static_assert(
    static_cast<int>(mocktail::graphics::EtcFormat::kEacRg11Signed) ==
        MOCKTAIL_RS_EAC_RG11_SIGNED,
    "MOCKTAIL_RS_EAC_RG11_SIGNED no longer matches mocktail::graphics::EtcFormat");

namespace mocktail::graphics {

// Diagnostics only. The kernels pick their own path from the same runtime
// detection this reports.
int RustSimdLevel() { return mocktail_rs_simd_level(); }

void SetRustEtcToggle(int value) { internal::EtcToggle().Override(value); }
void SetRustPoolToggle(int value) { internal::PoolToggle().Override(value); }
void SetRustResampleToggle(int value) {
  internal::ResampleToggle().Override(value);
}

}  // namespace mocktail::graphics
