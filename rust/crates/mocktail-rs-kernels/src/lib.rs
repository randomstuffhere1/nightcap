// Hot kernels for the Mocktail Vulkan stub.
//
// Everything crossing this boundary is a plain C ABI: raw pointers, sizes and
// integers. No Rust type is ever exposed, and no entry point may panic -- the
// C++ side is built -fno-exceptions, so a panic aborting here is the only
// acceptable outcome, and no unwind may cross into the caller.

#![allow(clippy::missing_safety_doc)]

use std::os::raw::{c_char, c_int, c_uint};

mod etc2;
mod pool;
mod resample;
mod simd;

pub use etc2::EtcFormat;

/// Reports whether the crate linked. Always 1 today; kept so callers can probe
/// before the kernels are wired up everywhere.
#[no_mangle]
pub extern "C" fn mocktail_rs_available() -> c_int {
    1
}

/// Version of the kernel crate, as a static string.
#[no_mangle]
pub extern "C" fn mocktail_rs_version() -> *const c_char {
    concat!(env!("CARGO_PKG_VERSION"), "\0").as_ptr() as *const c_char
}

/// Which SIMD class the running CPU offers.
///
/// 0 portable, 1 SSE2, 2 SSSE3, 4 NEON. Diagnostics only -- the kernels pick
/// their own path from the same detection, so this reports what they will do
/// rather than changing it.
#[no_mangle]
pub extern "C" fn mocktail_rs_simd_level() -> c_int {
    simd::level()
}

/// Decodes an interior EAC R11 / RG11 block straight into a destination image.
///
/// `destination` is the block's top-left texel and `destination_row_bytes` is
/// the distance between destination rows; the four rows written are exactly the
/// block's four texel rows, so this is only valid for a block that is not on the
/// right or bottom edge. Bit-for-bit equivalent to the EAC branch of
/// `DecodeEtcImageBlockRows`.
#[no_mangle]
pub unsafe extern "C" fn mocktail_rs_decode_r11_block_interior(
    format: c_int,
    block: *const u8,
    block_bytes: usize,
    destination: *mut u8,
    destination_row_bytes: usize,
) -> c_int {
    if block.is_null() || destination.is_null() {
        return 1;
    }
    let format = match EtcFormat::from_c(format) {
        Some(format) if format.is_eac_r11() => format,
        _ => return 1,
    };
    let needed = if matches!(
        format,
        EtcFormat::EacRg11 | EtcFormat::EacRg11Signed | EtcFormat::Etc2Rgba8
    ) {
        16
    } else {
        8
    };
    if block_bytes < needed {
        return 1;
    }
    let src = std::slice::from_raw_parts(block, needed);
    etc2::decode_r11_block_interior(format, src, destination, destination_row_bytes);
    0
}

/// Merges an EAC alpha sub-block into 16 decoded RGBA texels.
///
/// `texels` is 16 little-endian 0xAARRGGBB values laid out `x * 4 + y`, exactly
/// what `DecodeRgbBlock` produces, and `eac_block` is the first eight bytes of an
/// ETC2_RGBA8 block. Bit-for-bit equivalent to the merge in
/// `DecodeEtcImageBlockRows`. Returns 0 on success, non-zero without writing.
#[no_mangle]
pub unsafe extern "C" fn mocktail_rs_eac_merge_alpha(
    texels: *mut u32,
    eac_block: *const u8,
) -> c_int {
    if texels.is_null() || eac_block.is_null() {
        return 1;
    }
    let mut block = [0u8; 8];
    std::ptr::copy_nonoverlapping(eac_block, block.as_mut_ptr(), 8);
    // The caller guarantees sixteen texels, per the C++ side's `alignas(16)
    // std::uint32_t cols[16]`.
    let texels_ref: &mut [u32; 16] = &mut *(texels as *mut [u32; 16]);
    etc2::merge_eac_alpha(texels_ref, &block);
    0
}

/// Decodes one EAC R11 / RG11 block into `out`, `16 * channels` 16-bit values
/// laid out `x * 4 + y`; `channels` is 1 for R11 and 2 for RG11.
///
/// Bit-for-bit equivalent to the EAC branch of `DecodeEtcImageBlockRows`.
/// Returns 0 on success, non-zero without writing.
#[no_mangle]
pub unsafe extern "C" fn mocktail_rs_decode_r11_block(
    format: c_int,
    block: *const u8,
    block_bytes: usize,
    out: *mut u16,
) -> c_int {
    if block.is_null() || out.is_null() {
        return 1;
    }
    let format = match EtcFormat::from_c(format) {
        Some(format) if format.is_eac_r11() => format,
        _ => return 1,
    };
    let needed = if matches!(format, EtcFormat::EacRg11 | EtcFormat::EacRg11Signed) {
        16
    } else {
        8
    };
    if block_bytes < needed {
        return 1;
    }
    let channels = if matches!(format, EtcFormat::EacRg11 | EtcFormat::EacRg11Signed) {
        2
    } else {
        1
    };
    let _ = channels;
    etc2::decode_r11_block_raw(format, block, out);
    0
}

/// Box-resamples `source_width * source_height` RGBA texels to `width * height`,
/// writing row-major RGBA8 into `destination`.
///
/// Mirrors `mocktail::graphics::ResampleRgba` exactly, including its rounding:
/// `(sum + count / 2) / count`, and the row-reuse shortcut for destination rows
/// that map to the same source rows as the row above.
///
/// Returns 0 on success, non-zero when an argument is out of range (in which
/// case nothing is written).
#[no_mangle]
pub unsafe extern "C" fn mocktail_rs_resample_rgba(
    source: *const u8,
    source_width: c_uint,
    source_height: c_uint,
    source_stride_bytes: usize,
    width: c_uint,
    height: c_uint,
    destination: *mut u8,
) -> c_int {
    match resample::resample_rgba(
        source,
        source_width as usize,
        source_height as usize,
        source_stride_bytes,
        width as usize,
        height as usize,
        destination,
    ) {
        true => 0,
        false => 1,
    }
}
