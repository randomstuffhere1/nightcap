#ifndef MOCKTAIL_GRAPHICS_MOCKTAIL_RS_H_
#define MOCKTAIL_GRAPHICS_MOCKTAIL_RS_H_

// C ABI of the Rust kernel library (rust/crates/mocktail-rs-kernels).
//
// Plain pointers and integers only: no Rust type crosses this boundary, and no
// entry point returns through Rust control flow. The C++ side is built
// -fno-exceptions, so the library is compiled panic=abort and its entry points
// must not panic; an abort is deliberate rather than an unwind into C++.
//
// Every entry point reports failure with a non-zero return and an untouched
// destination, matching the error-code style used elsewhere in the runtime.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1 (always). Present so callers can probe for the library before the
// kernels are wired up everywhere.
int mocktail_rs_available(void);

// Version of the kernel crate, as a NUL-terminated static string.
const char* mocktail_rs_version(void);

// Reports 1 when fast runtime-detected SIMD paths are available on this CPU, 0
// when only the portable fallback is. Diagnostics only; the kernel chooses the
// path itself.
int mocktail_rs_simd_level(void);

// ETcFormat from include/mocktail/graphics/etc2_decoder.h, by discriminant.
// The values must stay in step; the Rust side asserts them with an offset
// trick at build time, so a reordering there fails the build rather than
// silently mis-decoding a texture.
#define MOCKTAIL_RS_ETC2_RGB8 0
#define MOCKTAIL_RS_ETC2_RGB8A1 1
#define MOCKTAIL_RS_ETC2_RGBA8 2
#define MOCKTAIL_RS_EAC_R11 3
#define MOCKTAIL_RS_EAC_R11_SIGNED 4
#define MOCKTAIL_RS_EAC_RG11 5
#define MOCKTAIL_RS_EAC_RG11_SIGNED 6

// Box-resamples source_width x source_height RGBA texels to width x height,
// writing row-major RGBA8 into destination.
//
// source_stride_bytes is the distance between source rows and may be larger
// than source_width * 4 when the source is a slice of a wider buffer.
//
// Bit-for-bit equivalent to mocktail::graphics::ResampleRgba: the same
// (sum + count / 2) / count rounding, and the same destination-row reuse
// shortcut. Returns non-zero when a pointer is null, any dimension is zero, or
// the stride is too small, writing nothing in that case.
int mocktail_rs_resample_rgba(const uint8_t* source,
                              uint32_t source_width,
                              uint32_t source_height,
                              size_t source_stride_bytes,
                              uint32_t width,
                              uint32_t height,
                              uint8_t* destination);

// A C callback that runs one unit of work. Must be safe to call from several
// threads at once, per the contract on mocktail::graphics::RunOnDecodeWorkers.
typedef void (*mocktail_rs_work_fn)(void* context);

// Calls run(context) worker_count times, once on the caller and the rest on
// pooled workers that run alongside it, returning when every call has returned.
//
// Returns 0 when the work was spread across the pool, and non-zero when it ran
// on the caller alone. The C++ caller treats non-zero as "the pool declined" and
// drains the batch itself, so a non-zero return is not an error.
int mocktail_rs_run_work(mocktail_rs_work_fn run, void* context,
                         unsigned worker_count);

// Decodes an interior EAC R11 / RG11 block straight into a destination image.
//
// destination is the block's top-left texel and destination_row_bytes is the
// distance between destination rows. The four rows written are exactly the
// block's four texel rows, so this is only valid for a block that is not on the
// right or bottom edge. Bit-for-bit equivalent to the EAC branch of
// DecodeEtcImageBlockRows. Returns 0 on success, non-zero without writing.
int mocktail_rs_decode_r11_block_interior(int format, const uint8_t* block,
                                          size_t block_bytes, uint8_t* destination,
                                          size_t destination_row_bytes);

// Merges an EAC alpha sub-block into 16 decoded RGBA texels.
//
// texels is 16 little-endian 0xAARRGGBB values laid out x * 4 + y, exactly what
// DecodeRgbBlock produces, and eac_block is the first eight bytes of an
// ETC2_RGBA8 block. Bit-for-bit equivalent to the merge in
// DecodeEtcImageBlockRows. Returns 0 on success, non-zero without writing.
int mocktail_rs_eac_merge_alpha(uint32_t* texels, const uint8_t* eac_block);

// Decodes one EAC R11 / RG11 block into out, 16 * channels 16-bit values laid out
// x * 4 + y; channels is 1 for R11 and 2 for RG11.
//
// format is one of the MOCKTAIL_RS_EAC_* values above. Bit-for-bit equivalent to
// the EAC branch of DecodeEtcImageBlockRows. Returns 0 on success, non-zero
// without writing.
int mocktail_rs_decode_r11_block(int format, const uint8_t* block,
                                 size_t block_bytes, uint16_t* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // MOCKTAIL_GRAPHICS_MOCKTAIL_RS_H_
