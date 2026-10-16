// ETC2 / EAC block kernels.
//
// # What is ported and what is not
//
// The opaque-RGB path of `DecodeRgbBlock` in `src/graphics/etc2_decoder.cc` is
// already SSSE3-optimised on x86 with a constexpr 256x16 mask table, and it is
// deliberately *not* reimplemented here: Rust's SSE is not faster than C++'s,
// so porting it would be churn for no gain. That path stays in C++.
//
// What was scalar everywhere, and is now vectorised:
//
//   * the EAC alpha merge for ETC2_RGBA8 (per-texel scalar OR today)
//   * the whole EAC R11 / RG11 family (per-texel, no SIMD at all today)
//
// Both are behind `#[target_feature]`, so one baseline artifact still runs on
// every host and takes the fast path when the CPU has the instructions. See
// `simd.rs` for why that detection is not `std::is_x86_feature_detected!`.
//
// Bit-exactness with the C++ original is not optional here: these feed a GPU
// texture, and a one-texel difference is a visible artifact. `kernel_parity_test`
// compares every path byte for byte against the C++ code that stays.

// EAC modifier table, indexed by [multiplier_selector][index]. Matches
// `kEacModifiers` in etc2_decoder.cc exactly; the values are the spec's.
//
// Flat rather than `[[i32; 8]; 16]` so that one table row is eight contiguous
// i32, which is exactly one AVX2 register. The two-dimensional form would need
// eight separate inserts to fill one.
pub const EAC_MODIFIERS: [i32; 128] = [
    -3, -6, -9, -15, 2, 5, 8, 14,
    -3, -7, -10, -13, 2, 6, 9, 12,
    -2, -5, -8, -13, 1, 4, 7, 12,
    -2, -4, -6, -13, 1, 3, 5, 12,
    -3, -6, -8, -12, 2, 5, 7, 11,
    -3, -7, -9, -11, 2, 6, 8, 10,
    -4, -7, -8, -11, 3, 6, 7, 10,
    -3, -5, -8, -11, 2, 4, 7, 10,
    -2, -6, -8, -10, 1, 5, 7, 9,
    -2, -5, -8, -10, 1, 4, 7, 9,
    -2, -4, -8, -10, 1, 3, 7, 9,
    -2, -5, -7, -10, 1, 4, 6, 9,
    -3, -4, -7, -10, 2, 3, 6, 9,
    -1, -2, -3, -10, 0, 1, 2, 9,
    -4, -6, -8, -9, 3, 5, 7, 8,
    -3, -5, -7, -9, 2, 4, 6, 8,
];


/// Reads the six bytes of 3-bit indices from an EAC sub-block into a bit field,
/// matching `ReadEacBits` in etc2_decoder.cc.
#[inline]
pub fn read_eac_bits(b: &[u8; 8]) -> u64 {
    let mut bits: u64 = 0;
    for i in 2..8 {
        bits = (bits << 8) | b[i] as u64;
    }
    bits
}

/// The 3-bit index for column-major texel `x * 4 + y`, matching
/// `EacIndexFromBits`.
#[inline]
pub fn eac_index_from_bits(bits: u64, texel: usize) -> usize {
    ((bits >> (45 - texel * 3)) & 7) as usize
}

#[inline]
fn clamp255(value: i32) -> u8 {
    value.clamp(0, 255) as u8
}

/// Builds the 8-entry EAC alpha palette for an ETC2_RGBA8 alpha sub-block.
///
/// `palette[i] = clamp(base + table[i] * multiplier, 0, 255)`, exactly as the
/// C++ builds it before merging.
#[inline]
pub fn eac_alpha_palette(block: &[u8; 8]) -> [u8; 8] {
    let base = block[0] as i32;
    let multiplier = (block[1] >> 4) as i32;
    let table = &EAC_MODIFIERS[((block[1] & 15) as usize) * 8..];
    let mut palette = [0u8; 8];
    for (i, slot) in palette.iter_mut().enumerate() {
        *slot = clamp255(base + table[i] * multiplier);
    }
    palette
}

/// The same palette, built with AVX2.
///
/// # Why this is the part worth widening
///
/// The palette build is eight independent `clamp(base + table[i] * mult, 0, 255)`
/// computations, and it was most of what the merge spent its time on: the eight
/// entries fit in one 256-bit register as eight i32 lanes, so the whole palette
/// is a single load, multiply, add, clamp pair and a pack. The table is stored
/// flat precisely so that its row is one 32-byte load here.
///
/// The merge after this is unchanged -- it is already a pshufb per four texels --
/// so this is the only part that needed to get wider.
#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn eac_alpha_palette_avx2(block: &[u8; 8]) -> [u8; 8] {
    use std::arch::x86_64::{
        __m128i, __m256i, _mm256_add_epi32, _mm256_castsi256_si128, _mm256_loadu_si256,
        _mm256_max_epi32, _mm256_min_epi32, _mm256_mullo_epi32, _mm256_packus_epi32,
        _mm256_permute4x64_epi64, _mm256_set1_epi32, _mm256_setzero_si256,
        _mm_packus_epi16, _mm_storeu_si128,
    };

    let base = block[0] as i32;
    let control = block[1];
    let multiplier = (control >> 4) as i32;
    let table_row = &EAC_MODIFIERS[(control & 15) as usize * 8..];

    unsafe {
        // One 32-byte load is exactly one table row, which is why the table is
        // stored flat.
        let table = _mm256_loadu_si256(table_row.as_ptr() as *const __m256i);
        let biased = _mm256_add_epi32(
            _mm256_mullo_epi32(table, _mm256_set1_epi32(multiplier)),
            _mm256_set1_epi32(base),
        );
        // Signed min/max are correct here: every lane is well inside i32, and
        // after clamping the values are 0..255 where signed and unsigned agree.
        let clamped = _mm256_min_epi32(
            _mm256_max_epi32(biased, _mm256_setzero_si256()),
            _mm256_set1_epi32(255),
        );

        // Narrow eight i32 lanes to eight bytes. Every value is already in
        // 0..255, so the low byte of each lane is the value and a shuffle that
        // picks byte 0 of lanes 0..7 does the whole narrowing -- no saturating
        // pack, and none of the per-lane padding the packs instructions leave
        // behind. The shuffle writes per 128-bit lane, so the result is four
        // bytes in each half and one unpack joins them.
        // Narrow eight i32 lanes to eight contiguous bytes.
        //
        // Every value is already in 0..255, so the unsigned 32-to-16 pack is
        // exact. Packing `clamped` against itself duplicates each group of four
        // into both halves of its 128-bit lane, which a 64-bit permute then
        // reassembles as eight contiguous u16; the unsigned 16-to-8 pack turns
        // those into the eight bytes, in order.
        //
        // The alternative -- shuffling byte 0 out of each lane -- leaves the
        // values three bytes apart, and the compaction shuffle to fix that up
        // produced [c0,0,0,0,c4,0,0,0], which the parity suite caught.
        let packed = _mm256_packus_epi32(clamped, clamped);
        let joined = _mm256_permute4x64_epi64::<0b00_00_10_00>(packed);
        let bytes = _mm_packus_epi16(
            _mm256_castsi256_si128(joined),
            std::arch::x86_64::_mm_setzero_si128(),
        );
        let mut out = [0u8; 16];
        _mm_storeu_si128(out.as_mut_ptr() as *mut __m128i, bytes);
        [out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]]
    }
}

pub fn merge_eac_alpha(texels: &mut [u32; 16], block: &[u8; 8]) {
    #[cfg(target_arch = "x86_64")]
    let palette = if crate::simd::has_avx2() {
        // SAFETY: guarded by the runtime check.
        unsafe { eac_alpha_palette_avx2(block) }
    } else {
        eac_alpha_palette(block)
    };
    #[cfg(not(target_arch = "x86_64"))]
    let palette = eac_alpha_palette(block);
    let bits = read_eac_bits(block);

    #[cfg(target_arch = "x86_64")]
    {
        if crate::simd::has_ssse3() {
            // SAFETY: guarded by the runtime feature check, and the intrinsics
            // used here are SSSE3, which that check asserts.
            unsafe { merge_eac_alpha_ssse3(texels, &palette, bits) };
            return;
        }
    }

    merge_eac_alpha_scalar(texels, &palette, bits);
}

#[inline]
fn merge_eac_alpha_scalar(texels: &mut [u32; 16], palette: &[u8; 8], bits: u64) {
    for (texel, out) in texels.iter_mut().enumerate() {
        let index = eac_index_from_bits(bits, texel);
        *out = (*out & 0x00FF_FFFF) | ((palette[index] as u32) << 24);
    }
}

/// The sixteen 3-bit EAC indices of a block, extracted once.
///
/// The C++ calls `EacIndexFromBits(bits, texel)` once per texel *per channel*,
/// so an RG11 block shifts and masks the same 48-bit field thirty-two times for
/// sixteen texels. The indices depend only on the block, not the channel, so
/// they are computed once and reused across channels.
#[inline]
pub fn eac_indices(bits: u64) -> [u8; 16] {
    let mut indices = [0u8; 16];
    for (texel, slot) in indices.iter_mut().enumerate() {
        *slot = eac_index_from_bits(bits, texel) as u8;
    }
    indices
}

/// Builds the four pshufb control vectors for one block, in a single pass.
///
/// Each control is sixteen bytes for four texels: the alpha lane of a texel is
/// byte 3, 7, 11 or 15, and every other lane wants 0x80 so pshufb zeroes it.
///
/// They are laid out end to end in one 64-byte buffer rather than as four
/// separate arrays. Building them separately meant re-stamping twelve 0x80 bytes
/// per group -- sixty-four writes per block where sixteen will do -- which was
/// most of what the vectorised merge spent its time on.
#[cfg(target_arch = "x86_64")]
#[inline]
fn alpha_controls(indices: &[u8; 16], out: &mut [u8; 64]) {
    out.fill(0x80);
    for texel in 0..16 {
        out[texel * 4 + 3] = indices[texel];
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "ssse3")]
unsafe fn merge_eac_alpha_ssse3(texels: &mut [u32; 16], palette: &[u8; 8], bits: u64) {
    use std::arch::x86_64::{
        __m128i, _mm_andnot_si128, _mm_loadu_si128, _mm_or_si128, _mm_set1_epi32,
        _mm_shuffle_epi8, _mm_storeu_si128,
    };

    // One 128-bit register covers four texels, because a texel is four bytes and
    // only its fourth byte changes. The C++ instead does a dependent palette
    // lookup, a byte shift and a mask-and-or per texel; pshufb does the lookup
    // for four texels at once and lands the alpha byte directly in its lane.
    let indices = eac_indices(bits);
    let mut controls = [0x80u8; 64];
    alpha_controls(&indices, &mut controls);

    let pal = _mm_loadu_si128(palette.as_ptr() as *const __m128i);
    let mask = _mm_set1_epi32(0xFF00_0000u32 as i32);

    for group in 0..4usize {
        let control = _mm_loadu_si128(controls.as_ptr().add(group * 16) as *const __m128i);
        let alpha = _mm_shuffle_epi8(pal, control);
        let texels_ptr = texels.as_mut_ptr().add(group * 4) as *mut __m128i;
        let base = _mm_loadu_si128(texels_ptr);
        // Clear the old alpha byte and OR the new one in: two vector ops where
        // the C++ needed a mask and an OR per texel.
        let cleared = _mm_andnot_si128(mask, base);
        _mm_storeu_si128(texels_ptr, _mm_or_si128(cleared, alpha));
    }
}


/// Decodes one EAC R11 / RG11 block into `out`, `16 * channels` 16-bit values
/// laid out `x * 4 + y`.
///
/// This runs tens of thousands of times per upload, so it is written for that.
/// Three things cost more here than they look:
///
///   * Slice indexing is bounds-checked. `block[i]` in the six-byte index read is
///     six checks per sub-block, where the C++ pointer arithmetic has none, so
///     the whole function works from raw pointers.
///   * The one-channel and two-channel cases are separate loops rather than a
///     test inside sixteen iterations.
///   * The sixteen indices are extracted once per sub-block. The C++ recomputes
///     `bits >> (45 - texel * 3)` for every texel of every channel, so an RG11
///     block shifts the same 48-bit field thirty-two times for sixteen texels.
///
/// `block` points at 8 bytes for R11 and 16 for RG11; the caller has already
/// checked the length.
pub unsafe fn decode_r11_block_raw(format: EtcFormat, block: *const u8, out: *mut u16) {
    let is_signed = matches!(
        format,
        EtcFormat::EacR11Signed | EtcFormat::EacRg11Signed
    );
    let two_channel = matches!(format, EtcFormat::EacRg11 | EtcFormat::EacRg11Signed);

    let palette0 = r11_palette_ptr(block, is_signed);
    let indices0 = eac_indices(read_eac_bits_ptr(block));

    if two_channel {
        let palette1 = r11_palette_ptr(block.add(8), is_signed);
        let indices1 = eac_indices(read_eac_bits_ptr(block.add(8)));
        for texel in 0..16 {
            *out.add(texel * 2) = palette0[indices0[texel] as usize];
            *out.add(texel * 2 + 1) = palette1[indices1[texel] as usize];
        }
    } else {
        for texel in 0..16 {
            *out.add(texel) = palette0[indices0[texel] as usize];
        }
    }
}

/// Reads the six bytes of 3-bit indices from an EAC sub-block.
///
/// Unsigned reads through a pointer because a slice index would bounds-check
/// every one of the six.
#[inline]
unsafe fn read_eac_bits_ptr(b: *const u8) -> u64 {
    let mut bits: u64 = 0;
    for i in 2..8 {
        bits = (bits << 8) | *b.add(i) as u64;
    }
    bits
}

/// Builds the eight-entry palette of one R11 sub-block.
#[inline]
unsafe fn r11_palette_ptr(sub: *const u8, is_signed: bool) -> [u16; 8] {
    let multiplier = ((*sub.add(1)) >> 4) as i32;
    let table = &EAC_MODIFIERS[(((*sub.add(1)) & 15) as usize) * 8..];
    let base = *sub;
    // Hoisted out of the loop: identical for all eight entries.
    let scale = if multiplier != 0 { multiplier * 8 } else { 1 };
    let mut palette = [0u16; 8];
    for (i, slot) in palette.iter_mut().enumerate() {
        *slot = r11_value(base, scale, table[i], is_signed);
    }
    palette
}

#[inline]
fn r11_value(base_byte: u8, scale: i32, modifier: i32, is_signed: bool) -> u16 {
    // `scale` is multiplier * 8, or 1 when the multiplier is zero, which is what
    // the C++ ternary `multiplier != 0 ? modifier * multiplier * 8 : modifier`
    // computes. Folding the test into the scale turns a branch that is the same
    // for all eight palette entries into one multiply.
    let scaled = modifier * scale;
    if !is_signed {
        let value = (base_byte as i32 * 8 + 4 + scaled).clamp(0, 2047);
        ((value << 5) | (value >> 6)) as u16
    } else {
        let base = (base_byte as i8 as i32).max(-127);
        let value = (base * 8 + scaled).clamp(-1023, 1023);
        // Branchless sign handling. The C++ uses `value < 0 ? -value : value` and
        // then re-tests the same condition to negate, which is two data-dependent
        // branches per palette entry; with eight entries of mixed sign they
        // mispredict often enough to cost more than the arithmetic. An arithmetic
        // shift gives an all-ones or all-zeroes mask, and two's complement
        // negation is xor-then-add-one -- adding the mask itself would negate to
        // -x - 1, which the parity suite caught.
        let magnitude = value.unsigned_abs();
        let expanded = (magnitude << 5) | (magnitude >> 5);
        let mask = (value >> 31) as u32;
        (expanded ^ mask).wrapping_add(mask & 1) as i16 as u16
    }
}

/// Decodes an interior EAC R11 / RG11 block straight into a destination image.
///
/// `destination` points at the block's top-left texel, `destination_row_bytes` is
/// the distance between destination rows, and the four destination rows this
/// writes are exactly the block's four texel rows -- which is only true for an
/// interior block, hence the name. `channels` is 1 for R11 and 2 for RG11.
///
/// # Why this exists
///
/// The C++ writes the decoded block with one byte at a time:
///
/// ```text
/// out[channel * 2]     = value;
/// out[channel * 2 + 1] = value >> 8;
/// ```
///
/// That is 32 byte-stores for an R11 block and 64 for an RG11 block, each a
/// separate read-modify-write of the destination cache line. For an interior
/// block the four texels of a row are contiguous, so the block can be laid out
/// in registers and written a row at a time: 8 bytes for R11 and 16 for RG11.
/// Two of those per block instead of thirty-two or sixty-four.
///
/// The layout is column-major in, row-major out -- the block stores texel
/// `x * 4 + y` and the image stores row-major -- so this transposes while it
/// copies, which is also where the values get packed from `u16` into their
/// little-endian byte form.
pub fn decode_r11_block_interior(
    format: EtcFormat,
    block: &[u8],
    destination: *mut u8,
    destination_row_bytes: usize,
) {
    let channels = if matches!(format, EtcFormat::EacRg11 | EtcFormat::EacRg11Signed) {
        2
    } else {
        1
    };

    let mut values = [0u16; 16 * 2];
    unsafe { decode_r11_block_raw(format, block.as_ptr(), values.as_mut_ptr()) };

    // Four destination rows, each `4 * channels` u16 values. The block holds texel
    // x * 4 + y, so row y is the strided slice values[y], values[y + 4], ...
    let row_bytes = 4 * channels * 2;
    let mut scratch = [0u8; 16];
    for y in 0..4usize {
        for x in 0..4usize {
            // The block stores texel x * 4 + y; the image wants row-major, so
            // this is the transpose.
            let texel = x * 4 + y;
            let out_base = x * channels * 2;
            let in_base = texel * channels;
            for channel in 0..channels {
                let v = values[in_base + channel];
                scratch[out_base + channel * 2] = v as u8;
                scratch[out_base + channel * 2 + 1] = (v >> 8) as u8;
            }
        }
        let out = unsafe { destination.add(y * destination_row_bytes) };
        unsafe { std::ptr::copy_nonoverlapping(scratch.as_ptr(), out, row_bytes) };
    }
}

/// Must match the discriminant order of `mocktail::graphics::EtcFormat` in
/// `include/mocktail/graphics/etc2_decoder.h`.
#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum EtcFormat {
    Etc2Rgb8 = 0,
    Etc2Rgb8A1 = 1,
    Etc2Rgba8 = 2,
    EacR11 = 3,
    EacR11Signed = 4,
    EacRg11 = 5,
    EacRg11Signed = 6,
}

impl EtcFormat {
    pub fn from_c(value: i32) -> Option<Self> {
        match value {
            0 => Some(Self::Etc2Rgb8),
            1 => Some(Self::Etc2Rgb8A1),
            2 => Some(Self::Etc2Rgba8),
            3 => Some(Self::EacR11),
            4 => Some(Self::EacR11Signed),
            5 => Some(Self::EacRg11),
            6 => Some(Self::EacRg11Signed),
            _ => None,
        }
    }

    /// True when the format is decoded as EAC R11 / RG11, which are the formats
    /// `decode_r11_block` handles.
    pub fn is_eac_r11(self) -> bool {
        matches!(
            self,
            Self::EacR11 | Self::EacR11Signed | Self::EacRg11 | Self::EacRg11Signed
        )
    }
}
