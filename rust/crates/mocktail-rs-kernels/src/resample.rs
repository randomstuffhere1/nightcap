// Box resampling of RGBA8 images.
//
// This mirrors `mocktail::graphics::ResampleRgba` in
// `src/graphics/texture_override.cc` byte for byte, including the
// `(sum + count / 2) / count` rounding and the row-reuse shortcut. It is
// deliberately the scalar reference version; a SIMD variant is only allowed to
// replace it once `kernel_parity_test` proves it identical.
//
// Two things in this file exist purely to keep it cheap at link time, and both
// are measured rather than assumed.
//
// # The scratch table is allocated with libc
//
// The obvious translation is a `Vec`. That is deliberately not used: `alloc`
// drags `core::fmt` and the panic-formatting machinery into the link, which is
// hundreds of KB in every image that references this kernel. The plain C
// `malloc` the host already provides is a drop-in for a fixed-size u64 table.
//
// # The divisions take NonZeroU64 denominators
//
// A plain `a / b` lowers to `core::panicking::panic_const_div_by_zero`, which
// reaches `std::panicking` and from there `std`'s backtrace machinery -- gimli,
// adler2, addr2line, memchr, rustc_demangle, and roughly 400 KB with them. Every
// denominator here is a validated non-zero dimension, so `NonZeroU64` states it
// and the check disappears. Verified: with the plain divisions the archive
// linked 426 KB; without them, the same link is a few KB.

use core::num::NonZeroU64;
use std::os::raw::c_void;

extern "C" {
    fn malloc(size: usize) -> *mut c_void;
    fn free(ptr: *mut c_void);
}

/// Owns the column table and frees it, so no early return leaks it.
///
/// No length field: `get` and `set` are only ever called with `2x` or `2x + 1`
/// for an `x` below the width the table was built from, which the caller
/// structure already guarantees.
struct ColumnTable {
    ptr: *mut u64,
}

impl ColumnTable {
    /// Allocates a table for `width` columns, or None when the size cannot be
    /// represented or the allocator refuses.
    fn new(width: usize) -> Option<ColumnTable> {
        let count = width.checked_mul(2)?;
        let bytes = count.checked_mul(std::mem::size_of::<u64>())?;
        // A texture dimension is at most 16384 on either current API, so
        // anything past this is a caller error rather than a texture.
        if bytes == 0 || bytes > (1usize << 30) {
            return None;
        }
        let raw = unsafe { malloc(bytes) as *mut u64 };
        if raw.is_null() {
            return None;
        }
        Some(ColumnTable { ptr: raw })
    }

    /// Reads `columns[index]`. The index is always `2x` or `2x + 1` for an `x`
    /// below the width the table was built for.
    #[inline]
    fn get(&self, index: usize) -> u64 {
        unsafe { std::ptr::read_unaligned(self.ptr.add(index)) }
    }

    /// Writes `columns[index]`.
    #[inline]
    fn set(&mut self, index: usize, value: u64) {
        unsafe { std::ptr::write_unaligned(self.ptr.add(index), value) };
    }
}

impl Drop for ColumnTable {
    fn drop(&mut self) {
        unsafe { free(self.ptr as *mut c_void) };
    }
}

/// Resamples `source_width * source_height` RGBA texels to `width * height`.
///
/// `source_stride_bytes` is the distance between source rows and may be larger
/// than `source_width * 4` when the source is a slice of a larger decoded
/// buffer. Returns false, writing nothing, when any argument is out of range.
pub fn resample_rgba(
    source: *const u8,
    source_width: usize,
    source_height: usize,
    source_stride_bytes: usize,
    width: usize,
    height: usize,
    destination: *mut u8,
) -> bool {
    if source.is_null()
        || destination.is_null()
        || width == 0
        || height == 0
        || source_width == 0
        || source_height == 0
        || source_stride_bytes < source_width * 4
    {
        return false;
    }
    let mut columns = match ColumnTable::new(width) {
        Some(columns) => columns,
        None => return false,
    };
    // Both are non-zero by the check above, which is what lets every division
    // below use a NonZero denominator.
    let width_nz = match NonZeroU64::new(width as u64) {
        Some(value) => value,
        None => return false,
    };
    let height_nz = match NonZeroU64::new(height as u64) {
        Some(value) => value,
        None => return false,
    };
    let source_width64 = source_width as u64;
    let source_height64 = source_height as u64;

    // Source box of destination column x is [columns[2x], columns[2x + 1]).
    // u64 throughout, because the C++ computes these in uint64_t before
    // narrowing, and the product of two u32 dimensions can reach 1.8e19.
    let mut one_texel_columns = true;
    for x in 0..width {
        let x0 = (x as u64) * source_width64 / width_nz;
        let x1 = std::cmp::max(
            x0 + 1,
            ((x + 1) as u64) * source_width64 / width_nz,
        );
        columns.set(2 * x, x0);
        columns.set(2 * x + 1, x1);
        one_texel_columns = one_texel_columns && x1 == x0 + 1;
    }

    // The case that actually runs in practice: an integer upscale, which is what
    // performance.small_texture_upscale produces (default 4).
    //
    // It never reaches the box filter, because every destination column is one
    // texel wide (x1 == x0 + 1 for all x whenever width == source_width * S), so
    // it takes the one-texel-column gather. That gather as written does a
    // dependent load of columns[2x] per *destination* pixel, and the same source
    // pixel is read S times in a row.
    //
    // Recognising the replication turns that into one load per *source* pixel
    // and a run of stores, with no table lookup in the loop at all, which also
    // removes the load-to-load dependency chain that serialised the old version.
    if let Some(scale) = integer_replicate_scale(source_width, width, source_height, height) {
        return replicate_image(
            source,
            source_stride_bytes,
            source_width,
            source_height,
            scale,
            destination,
        );
    }

    let row_bytes = width * 4;
    let mut previous_y0: u64 = 0;
    let mut previous_y1: u64 = 0;
    for y in 0..height {
        let y0 = (y as u64) * source_height64 / height_nz;
        let mut y1 = ((y + 1) as u64) * source_height64 / height_nz;
        if y1 <= y0 {
            y1 = y0 + 1;
        }
        let out_row = unsafe { destination.add(y * row_bytes) };
        if y > 0 && y0 == previous_y0 && y1 == previous_y1 {
            // Same source rows as the row above: the texels are identical.
            unsafe { std::ptr::copy_nonoverlapping(out_row.sub(row_bytes), out_row, row_bytes) };
            continue;
        }
        previous_y0 = y0;
        previous_y1 = y1;
        if one_texel_columns && y1 == y0 + 1 {
            let source_row = unsafe { source.add(y0 as usize * source_stride_bytes) };
            for x in 0..width {
                let px = unsafe { read_u32(source_row.add(columns.get(2 * x) as usize * 4)) };
                unsafe { write_u32(out_row.add(x * 4), px) };
            }
            continue;
        }
        for x in 0..width {
            let x0 = columns.get(2 * x);
            let x1 = columns.get(2 * x + 1);
            let sum = sum_box(source, source_stride_bytes, x0, x1, y0, y1);
            // Both spans are at least one texel, so the product is at least one
            // and the NonZero conversion cannot fail.
            let count = match NonZeroU64::new((y1 - y0).wrapping_mul(x1 - x0)) {
                Some(value) => value,
                None => return false,
            };
            let out = unsafe { destination.add((y * width + x) * 4) };
            if count.get() == 1 {
                for channel in 0..4 {
                    unsafe { *out.add(channel) = sum[channel] as u8 };
                }
            } else {
                let half = count.get() / 2;
                for channel in 0..4 {
                    unsafe { *out.add(channel) = ((sum[channel] + half) / count) as u8 };
                }
            }
        }
    }
    true
}

/// Sums the four channels over the source box `x0..x1` by `y0..y1`.
///
/// This is the whole cost of the box filter, so it is the part that is
/// vectorised. The four channels of one texel are independent, so a texel is
/// loaded once and accumulated into four 32-bit lanes rather than being walked
/// byte by byte -- the C++ does `for channel in 0..4 { sum[channel] += pixel[channel] }`,
/// which is four dependent byte loads per texel.
///
/// The 32-bit lanes are an optimisation, not the contract. The C++ accumulates in
/// `uint64_t`, so a box deep enough to overflow a lane falls back to the scalar
/// path and the two stay bit-identical for every input. 255 * texels is the
/// widest a single channel can sum to, and that overflows a u32 lane above
/// 16,843,009 texels -- a source of about 4097x4097, past any texture API limit,
/// but reachable by a caller that passes nonsense, so it is checked rather than
/// assumed.
#[inline]
fn sum_box(
    source: *const u8,
    stride: usize,
    x0: u64,
    x1: u64,
    y0: u64,
    y1: u64,
) -> [u64; 4] {
    const MAX_LANE_TEXELS: u64 = (u32::MAX as u64) / 255;
    let texels = (y1 - y0).wrapping_mul(x1 - x0);
    if texels == 0 || texels > MAX_LANE_TEXELS {
        return sum_box_scalar(source, stride, x0, x1, y0, y1);
    }
    #[cfg(target_arch = "x86_64")]
    {
        // SSE2 is part of the x86-64 baseline, so this needs no runtime check.
        // SAFETY: baseline instruction set; the pointer arithmetic stays inside
        // the caller's buffers, which the caller has already validated, and the
        // guard above bounds the accumulation to a lane.
        unsafe { sum_box_sse2(source, stride, x0, x1, y0, y1) }
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        sum_box_scalar(source, stride, x0, x1, y0, y1)
    }
}

#[inline]
fn sum_box_scalar(
    source: *const u8,
    stride: usize,
    x0: u64,
    x1: u64,
    y0: u64,
    y1: u64,
) -> [u64; 4] {
    let mut sum = [0u64; 4];
    for sy in y0..y1 {
        let row = unsafe { source.add(sy as usize * stride) };
        for sx in x0..x1 {
            let pixel = unsafe { row.add(sx as usize * 4) };
            for channel in 0..4 {
                sum[channel] += unsafe { *pixel.add(channel) as u64 };
            }
        }
    }
    sum
}

#[cfg(target_arch = "x86_64")]
#[inline]
unsafe fn sum_box_sse2(
    source: *const u8,
    stride: usize,
    x0: u64,
    x1: u64,
    y0: u64,
    y1: u64,
) -> [u64; 4] {
    use std::arch::x86_64::{
        _mm_add_epi32, _mm_cvtsi128_si32, _mm_cvtsi32_si128, _mm_setzero_si128,
        _mm_shuffle_epi32,
        _mm_unpacklo_epi16, _mm_unpacklo_epi8,
    };

    unsafe {
        // One texel per iteration, its four bytes widened into four 32-bit lanes
        // -- one lane per channel. That is the natural width here: the four
        // channels are independent, so they accumulate for free, whereas the
        // C++ walks them with a four-iteration inner loop of dependent byte
        // loads and adds.
        let zero = _mm_setzero_si128();
        let mut acc = zero;
        for sy in y0..y1 {
            let row = source.add(sy as usize * stride);
            for sx in x0..x1 {
                // Unaligned: a row stride is width * 4, which is not a multiple
                // of 16 for most widths, so rows are not 16-byte aligned.
                let packed = _mm_cvtsi32_si128(read_u32(row.add(sx as usize * 4)) as i32);
                let u16x4 = _mm_unpacklo_epi8(packed, zero);
                let u32x4 = _mm_unpacklo_epi16(u16x4, zero);
                acc = _mm_add_epi32(acc, u32x4);
            }
        }

        // Each lane now holds one channel's sum. The shuffle immediates have to
        // be compile-time constants, so this is unrolled rather than looped.
        let lane0 = _mm_cvtsi128_si32(_mm_shuffle_epi32(acc, 0b00_00_00_00));
        let lane1 = _mm_cvtsi128_si32(_mm_shuffle_epi32(acc, 0b01_01_01_01));
        let lane2 = _mm_cvtsi128_si32(_mm_shuffle_epi32(acc, 0b10_10_10_10));
        let lane3 = _mm_cvtsi128_si32(_mm_shuffle_epi32(acc, 0b11_11_11_11));
        [
            lane0 as u32 as u64,
            lane1 as u32 as u64,
            lane2 as u32 as u64,
            lane3 as u32 as u64,
        ]
    }
}

/// Reads a little-endian u32 from a possibly unaligned address.
#[inline]
unsafe fn read_u32(p: *const u8) -> u32 {
    std::ptr::read_unaligned(p as *const u32)
}

/// Writes a little-endian u32 to a possibly unaligned address.
#[inline]
unsafe fn write_u32(p: *mut u8, v: u32) {
    std::ptr::write_unaligned(p as *mut u32, v);
}

/// The integer scale factor when the destination is an exact multiple of the
/// source in both axes, which is what an upscale by a whole number produces.
///
/// Returns None for anything else, including a 1:1 copy, which the caller's
/// column table already handles without this path.
#[inline]
fn integer_replicate_scale(
    source_width: usize,
    width: usize,
    source_height: usize,
    height: usize,
) -> Option<usize> {
    if source_width == 0 || width % source_width != 0 {
        return None;
    }
    if source_height == 0 || height % source_height != 0 {
        return None;
    }
    let x_scale = width / source_width;
    let y_scale = height / source_height;
    // A single axis replicating is handled by the row-reuse shortcut, and the
    // one-texel-column path already covers the 1:1 case without the extra
    // bookkeeping this path adds.
    if x_scale < 2 || y_scale < 2 || x_scale != y_scale {
        return None;
    }
    Some(x_scale)
}


/// Writes the image as an integer `scale` by `scale` replication of every source
/// texel, which is what an integer upscale produces.
///
/// Bit-identical to the column-table path for the same inputs: when
/// `width == source_width * scale`, `x0` is exactly `x / scale` and
/// `x1 == x0 + 1` for every destination column, so each destination texel is the
/// source texel at `(x / scale, y / scale)`. `kernel_parity_test` covers every
/// integer upscale that lands here, so this is checked, not argued.
///
/// The win is in the inner loop rather than the algorithm. The gather reads
/// `columns[2x]` for each *destination* pixel, so a scale of four loads one
/// table entry and the same source texel four times. This reads the table zero
/// times and the source once per texel, and writes the run with whole registers.
fn replicate_image(
    source: *const u8,
    source_stride_bytes: usize,
    source_width: usize,
    source_height: usize,
    scale: usize,
    destination: *mut u8,
) -> bool {
    let out_row_bytes = source_width * scale * 4;
    for sy in 0..source_height {
        let source_row = unsafe { source.add(sy * source_stride_bytes) };
        // One destination row per source row; the rest of the scale is handled
        // by repeating the row, which is what the row-reuse shortcut does.
        let out_row = unsafe { destination.add(sy * scale * out_row_bytes) };
        replicate_row(source_row, source_width, scale, out_row);
        for copy in 1..scale {
            let repeated =
                unsafe { out_row.add((copy - 1) * out_row_bytes) };
            unsafe {
                std::ptr::copy_nonoverlapping(repeated, repeated.add(out_row_bytes), out_row_bytes)
            };
        }
    }
    true
}

/// Expands one source row into `scale` copies of each texel, writing `scale`
/// destination rows' worth of data into the first of them.
#[inline]
fn replicate_row(source_row: *const u8, source_width: usize, scale: usize, out: *mut u8) {
    for sx in 0..source_width {
        let pixel = unsafe { read_u32(source_row.add(sx * 4)) };
        unsafe { replicate_texel(pixel, scale, out.add(sx * scale * 4)) };
    }
}

/// Writes `scale` consecutive copies of one RGBA texel.
#[cfg(target_arch = "x86_64")]
#[inline]
unsafe fn replicate_texel(pixel: u32, scale: usize, out: *mut u8) {
    use std::arch::x86_64::{_mm_cvtsi32_si128, _mm_set1_epi32, _mm_storeu_si128};

    if scale >= 4 {
        // Four texels per store. The scale-4 case is the default setting, so it
        // is the one worth having a straight-line path for.
        let quad = _mm_set1_epi32(pixel as i32);
        let mut written = 0;
        while written + 4 <= scale {
            _mm_storeu_si128(
                out.add(written * 4) as *mut std::arch::x86_64::__m128i,
                quad,
            );
            written += 4;
        }
        while written < scale {
            write_u32(out.add(written * 4), pixel);
            written += 1;
        }
    } else {
        // Scales 2 and 3: three texels is twelve bytes, which is one 8-byte store
        // plus one 4-byte store rather than anything wider.
        if scale == 2 {
            let pair = _mm_cvtsi32_si128(pixel as i32);
            write_u32(out, pixel);
            write_u32(out.add(4), pixel);
            let _ = pair;
        } else {
            for i in 0..scale {
                write_u32(out.add(i * 4), pixel);
            }
        }
    }
}

#[cfg(not(target_arch = "x86_64"))]
#[inline]
unsafe fn replicate_texel(pixel: u32, scale: usize, out: *mut u8) {
    for i in 0..scale {
        write_u32(out.add(i * 4), pixel);
    }
}
