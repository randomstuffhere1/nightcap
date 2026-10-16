// Golden differential test for the ported kernels.
//
// The C++ implementation in src/graphics/texture_override.cc is the oracle; the
// Rust library backs mocktail_rs_resample_rgba. Every case here runs both and
// compares the destination byte for byte, so a "Rust is faster" claim that
// changes a single texel fails the build.
//
// The suite is deliberately deterministic: a plain LCG corpus rather than a
// random one, so a failure reproduces on any machine and any toolchain.
//
// Coverage, matching the branches in the C++ path:
//   * the one-texel-column fast path (integer upscales)
//   * the box-filter path (fractional and downscales), which is what rounds
//     with (sum + count / 2) / count
//   * the destination-row reuse shortcut
//   * strides wider than the image, as produced when the source is a slice of
//     a larger decoded buffer
//   * partial edge blocks, where source and destination dimensions are not
//     both multiples of four
//   * the rejection paths, which must leave the destination untouched

#include "mocktail/graphics/etc2_decoder.h"
#include "mocktail/graphics/mocktail_rs.h"
#include "mocktail/graphics/mocktail_rs_bridge.h"
#include "mocktail/graphics/texture_override.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

namespace etc2_decoder = mocktail::graphics;

constexpr std::uint64_t kSeed = 0x2545f4914f6cdd1dULL;

// Pins the A/B switches for the length of a scope, then puts them back.
//
// Without this the "reference" side of every comparison below would be whatever
// the dispatcher happens to dispatch to -- which, with the Rust kernels linked
// in and no environment override, is Rust. Comparing Rust against Rust passes
// and proves nothing. The switches are latched from the environment on first
// use, so these have to go through SetRustEtcToggle and friends rather than
// setenv.
class ScopedRustToggle {
 public:
  ScopedRustToggle(int etc, int resample)
      : etc_(etc), resample_(resample) {
    etc2_decoder::SetRustEtcToggle(etc);
    etc2_decoder::SetRustResampleToggle(resample);
  }
  ~ScopedRustToggle() {
    etc2_decoder::SetRustEtcToggle(-1);
    etc2_decoder::SetRustResampleToggle(-1);
  }
  ScopedRustToggle(const ScopedRustToggle&) = delete;
  ScopedRustToggle& operator=(const ScopedRustToggle&) = delete;

 private:
  int etc_;
  int resample_;
};

// Forces the C++ implementations, so the dispatcher really is the oracle.
class ScopedCppKernels : public ScopedRustToggle {
 public:
  ScopedCppKernels() : ScopedRustToggle(0, 0) {}
};

// A plain LCG, so the corpus is identical everywhere.
class Corpus {
 public:
  explicit Corpus(std::uint32_t seed) : state_(seed) {}

  std::uint8_t Byte() {
    state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<std::uint8_t>(state_ >> 56);
  }

  void Fill(std::uint8_t* out, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      out[i] = Byte();
    }
  }

 private:
  std::uint64_t state_;
};

struct ResampleCase {
  std::uint32_t source_width;
  std::uint32_t source_height;
  std::uint32_t width;
  std::uint32_t height;
  // Extra stride padding, in pixels, beyond source_width.
  std::uint32_t stride_padding;
  const char* name;
};

// Every dimension that changes which branch the C++ takes: multiples of four,
// primes, powers of two, one, and one larger than a multiple of four.
constexpr std::uint32_t kDimensions[] = {1, 2, 3, 4, 5, 7, 8, 9, 13, 16, 17,
                                         31, 32, 33, 64, 65, 128, 129, 256};

// Runs both implementations on the same source into two buffers and compares.
void ExpectResampleMatches(const ResampleCase& spec) {
  // Without this the two buffers below are both filled by Rust, because
  // ResampleRgba dispatches into the library by default, and the comparison is
  // a tautology.
  const ScopedCppKernels force_cpp;
  ASSERT_GT(spec.width, 0u);
  ASSERT_GT(spec.height, 0u);
  ASSERT_GT(spec.source_width, 0u);
  ASSERT_GT(spec.source_height, 0u);

  const std::size_t stride_pixels = spec.source_width + spec.stride_padding;
  const std::size_t stride_bytes = stride_pixels * 4;
  const std::size_t allocated = stride_bytes * spec.source_height;
  const std::size_t dest_bytes =
      static_cast<std::size_t>(spec.width) * spec.height * 4;

  Corpus corpus(static_cast<std::uint32_t>(kSeed ^
      (spec.source_width * 2654435761u) ^
      (spec.source_height * 40503u) ^ (spec.width * 97u) ^
      (spec.height * 63641362u) ^ spec.stride_padding));
  std::vector<std::uint8_t> source(allocated);
  corpus.Fill(source.data(), source.size());

  std::vector<std::uint8_t> cpp_dest(dest_bytes, 0xcd);
  std::vector<std::uint8_t> rs_dest(dest_bytes, 0xcd);

  mocktail::graphics::ResampleRgba(
      source.data(), spec.source_width, spec.source_height, stride_bytes,
      spec.width, spec.height, cpp_dest.data());
  ASSERT_EQ(0, mocktail_rs_resample_rgba(source.data(), spec.source_width,
                                         spec.source_height, stride_bytes,
                                         spec.width, spec.height, rs_dest.data()))
      << spec.name;

  EXPECT_EQ(0, std::memcmp(cpp_dest.data(), rs_dest.data(), dest_bytes))
      << spec.name << ": source " << spec.source_width << "x"
      << spec.source_height << " -> " << spec.width << "x" << spec.height
      << " with a stride of " << stride_bytes << " bytes differed from the C++ "
      << "implementation";
}

// ------------------------------------------------------------- etc2 blocks

// The reference implementations below are the C++ ones from
// src/graphics/etc2_decoder.cc, transcribed so this suite can compare without
// depending on the private helpers of that file. Both are the pre-port code.
constexpr std::int32_t kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14},   {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12},   {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11},   {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10},   {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},    {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},    {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},    {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},     {-3, -5, -7, -9, 2, 4, 6, 8},
};

inline std::uint8_t Clamp255(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

std::uint64_t ReferenceReadEacBits(const std::uint8_t* b) {
  std::uint64_t bits = 0;
  for (int i = 2; i < 8; ++i) {
    bits = (bits << 8) | b[i];
  }
  return bits;
}

int ReferenceEacIndex(std::uint64_t bits, int texel) {
  return static_cast<int>((bits >> (45 - texel * 3)) & 7);
}

// The ETC2_RGBA8 alpha merge as the C++ does it.
void ReferenceMergeAlpha(std::uint32_t* cols, const std::uint8_t* block) {
  const std::uint64_t alpha_bits = ReferenceReadEacBits(block);
  const int alpha_base = block[0];
  const int alpha_multiplier = block[1] >> 4;
  const auto& alpha_table = kEacModifiers[block[1] & 15];
  std::uint8_t alpha_palette[8];
  for (int i = 0; i < 8; ++i) {
    alpha_palette[i] =
        Clamp255(alpha_base + alpha_table[i] * alpha_multiplier);
  }
  for (int texel = 0; texel < 16; ++texel) {
    cols[texel] = (cols[texel] & 0x00FFFFFF) |
                  (static_cast<std::uint32_t>(
                       alpha_palette[ReferenceEacIndex(alpha_bits, texel)])
                   << 24);
  }
}

std::uint16_t ReferenceR11Modifier(const std::uint8_t* b, int multiplier,
                                   const int* modifier_table,
                                   int modifier_idx, bool is_signed) {
  const int modifier = modifier_table[modifier_idx];
  const int scaled = multiplier != 0 ? modifier * multiplier * 8 : modifier;
  if (!is_signed) {
    const int value = std::clamp(b[0] * 8 + 4 + scaled, 0, 2047);
    return static_cast<std::uint16_t>((value << 5) | (value >> 6));
  }
  const int base = std::max(-127, static_cast<int>(static_cast<std::int8_t>(b[0])));
  const int value = std::clamp(base * 8 + scaled, -1023, 1023);
  const int magnitude = value < 0 ? -value : value;
  const int expanded = (magnitude << 5) | (magnitude >> 5);
  return static_cast<std::uint16_t>(
      static_cast<std::int16_t>(value < 0 ? -expanded : expanded));
}

// The EAC R11/RG11 block decode as the C++ does it.
void ReferenceR11Block(int format, const std::uint8_t* block,
                       std::uint16_t* out) {
  const bool is_signed = format == 4 || format == 6;
  const int channels = (format == 5 || format == 6) ? 2 : 1;
  std::uint64_t channel_bits[2] = {0, 0};
  std::uint16_t channel_palette[2][8];
  for (int c = 0; c < channels; ++c) {
    const std::uint8_t* cb = block + c * 8;
    channel_bits[c] = ReferenceReadEacBits(cb);
    const int mult = cb[1] >> 4;
    const int* table = kEacModifiers[cb[1] & 15];
    for (int i = 0; i < 8; ++i) {
      channel_palette[c][i] =
          ReferenceR11Modifier(cb, mult, table, i, is_signed);
    }
  }
  for (int texel = 0; texel < 16; ++texel) {
    for (int channel = 0; channel < channels; ++channel) {
      out[texel * channels + channel] =
          channel_palette[channel][ReferenceEacIndex(channel_bits[channel],
                                                     texel)];
    }
  }
}

std::uint8_t MakeAlphaBlock(std::uint32_t index, std::uint8_t* block) {
  // A palette base and multiplier that reach every modifier table row, then
  // indices covering 0..7 across the block.
  block[0] = static_cast<std::uint8_t>(index * 31);
  block[1] = static_cast<std::uint8_t>(index * 7);
  std::uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    bits = (bits << 3) | (index + i) % 8;
  }
  for (int i = 0; i < 6; ++i) {
    block[2 + i] = static_cast<std::uint8_t>(bits >> (40 - i * 8));
  }
  return 0;
}

// The alpha merge is the only ported code with a hand-written SSSE3 path, so it
// gets the widest sweep: every multiplier table row, every index combination the
// 48-bit field can express in a structured walk, and every alpha byte value.
TEST(RustKernelParity, EacAlphaMergeMatchesEveryTableRow) {
  for (std::uint32_t index = 0; index < 4096; ++index) {
    std::uint8_t block[8];
    MakeAlphaBlock(index, block);

    std::uint32_t reference[16];
    std::uint32_t rust[16];
    // Colour texels that exercise the alpha byte being non-zero beforehand, so
    // "clear then set" cannot accidentally pass on a zero alpha.
    Corpus corpus(index);
    for (std::uint32_t t = 0; t < 16; ++t) {
      const std::uint32_t colour =
          (corpus.Byte() << 24) | (corpus.Byte() << 16) |
          (corpus.Byte() << 8) | corpus.Byte();
      reference[t] = colour;
      rust[t] = colour;
    }

    ReferenceMergeAlpha(reference, block);
    ASSERT_EQ(0, mocktail_rs_eac_merge_alpha(rust, block))
        << "alpha merge declined at index " << index;
    for (std::uint32_t t = 0; t < 16; ++t) {
      ASSERT_EQ(reference[t], rust[t])
          << "alpha merge differed at index " << index << " texel " << t;
    }
  }
}

TEST(RustKernelParity, EacAlphaMergeRejectsNull) {
  std::uint32_t texels[16] = {};
  std::uint8_t block[8] = {};
  EXPECT_NE(0, mocktail_rs_eac_merge_alpha(nullptr, block));
  EXPECT_NE(0, mocktail_rs_eac_merge_alpha(texels, nullptr));
}

TEST(RustKernelParity, R11BlocksMatchEveryTableRow) {
  for (int format = 3; format <= 6; ++format) {
    const std::size_t channels = (format == 5 || format == 6) ? 2 : 1;
    const std::size_t block_bytes = channels == 2 ? 16 : 8;
    for (std::uint32_t index = 0; index < 2048; ++index) {
      std::vector<std::uint8_t> block(block_bytes);
      for (std::size_t c = 0; c < channels; ++c) {
        MakeAlphaBlock(index + c * 977, block.data() + c * 8);
      }
      std::vector<std::uint16_t> reference(16 * channels, 0xbeef);
      std::vector<std::uint16_t> rust(16 * channels, 0xbeef);
      ReferenceR11Block(format, block.data(), reference.data());
      ASSERT_EQ(0, mocktail_rs_decode_r11_block(format, block.data(),
                                                block_bytes, rust.data()))
          << "r11 decode declined for format " << format << " index " << index;
      ASSERT_EQ(reference, rust)
          << "format " << format << " index " << index << " differed";
    }
  }
}

TEST(RustKernelParity, R11BlockRejectsBadArguments) {
  std::vector<std::uint8_t> block(16);
  std::vector<std::uint16_t> out(32, 0x1234);
  EXPECT_NE(0, mocktail_rs_decode_r11_block(3, nullptr, 16, out.data()));
  EXPECT_NE(0, mocktail_rs_decode_r11_block(3, block.data(), 16, nullptr));
  // Not an EAC R11 format at all.
  EXPECT_NE(0, mocktail_rs_decode_r11_block(0, block.data(), 16, out.data()));
  EXPECT_NE(0, mocktail_rs_decode_r11_block(99, block.data(), 16, out.data()));
  // R11 is eight bytes per block; claiming fewer is refused.
  EXPECT_NE(0, mocktail_rs_decode_r11_block(3, block.data(), 4, out.data()));
  // The sentinel proves nothing was written.
  for (std::uint16_t v : out) {
    EXPECT_EQ(0x1234, v);
  }
}

TEST(RustKernelParity, EacAlphaMergeRejectionLeavesTexelsUntouched) {
  std::uint8_t block[8];
  MakeAlphaBlock(7, block);
  std::uint32_t texels[16];
  for (int i = 0; i < 16; ++i) {
    texels[i] = 0xDEADBE00u | static_cast<std::uint32_t>(i);
  }
  std::vector<std::uint32_t> before(texels, texels + 16);
  // A well-formed call does write, so this asserts the merge is not a no-op,
  // which is what makes the parity assertions above meaningful.
  ASSERT_EQ(0, mocktail_rs_eac_merge_alpha(texels, block));
  bool changed = false;
  for (int i = 0; i < 16; ++i) {
    changed = changed || (texels[i] != before[i]);
  }
  EXPECT_TRUE(changed) << "the merge wrote nothing, so parity proves little";
}

// The interior path transposes column-major block data into row-major image
// rows, so it needs its own coverage: feeding it an edge block would write
// outside the buffer it was given.
//
// The reference here is DecodeEtcImageBlockRows itself on a full image, which is
// the only oracle that exercises the same edge handling and the same rounding.
TEST(RustKernelParity, R11InteriorPathMatchesWholeImageDecode) {
  for (int format = 3; format <= 6; ++format) {
    const std::uint32_t width = 16;
    const std::uint32_t height = 16;
    const std::size_t block_bytes = etc2_decoder::EtcBlockBytes(
        static_cast<etc2_decoder::EtcFormat>(format));
    const std::size_t texel_bytes = etc2_decoder::EtcDecodedTexelBytes(
        static_cast<etc2_decoder::EtcFormat>(format));
    const std::size_t source_bytes = (width / 4) * (height / 4) * block_bytes;
    const std::size_t dest_bytes = static_cast<std::size_t>(width) * height * texel_bytes;

    std::vector<std::uint8_t> source(source_bytes);
    Corpus corpus(0x1234'5678u + format);
    corpus.Fill(source.data(), source.size());

    // The whole-image decode is the oracle, and it has to be the C++ one: with
    // the Rust kernels linked in, DecodeEtcImageBlockRows dispatches the R11
    // family to them, and the two sides of this comparison would then be the
    // same code.
    const ScopedCppKernels force_cpp;
    std::vector<std::uint8_t> reference(dest_bytes, 0xcd);
    ASSERT_TRUE(etc2_decoder::DecodeEtcImageBlockRows(
        static_cast<etc2_decoder::EtcFormat>(format), source.data(),
        source_bytes, width, height, 0, height / 4, reference.data(),
        dest_bytes));

    // The interior path, one block at a time, into its own buffer.
    std::vector<std::uint8_t> interior(dest_bytes, 0xcd);
    for (std::uint32_t by = 0; by < height / 4; ++by) {
      for (std::uint32_t bx = 0; bx < width / 4; ++bx) {
        const std::uint8_t* block =
            source.data() + (by * (width / 4) + bx) * block_bytes;
        ASSERT_EQ(0, mocktail_rs_decode_r11_block_interior(
                          format, block, block_bytes,
                          interior.data() + (by * 4 * width + bx * 4) * texel_bytes,
                          width * texel_bytes))
            << "interior decode declined, format " << format;
      }
    }
    ASSERT_EQ(reference, interior)
        << "the interior block write differs from the whole-image decode for "
        << "format " << format;
  }
}

}  // namespace

TEST(RustKernelParity, LibraryIsLinked) {
  EXPECT_EQ(1, mocktail_rs_available());
  EXPECT_EQ(0, std::strncmp("0.3.", mocktail_rs_version(), 4));
}

// Integer upscales take the one-texel-column fast path and the row-reuse
// shortcut; these are the dimensions small_texture_upscale actually produces.
TEST(RustKernelParity, IntegerUpscalesMatch) {
  for (std::uint32_t sw : kDimensions) {
    for (std::uint32_t scale : {1u, 2u, 3u, 4u}) {
      for (std::uint32_t height_scale : {1u, 2u, 3u}) {
        ResampleCase spec{sw, sw, sw * scale, sw * height_scale, 0,
                          "integer upscale"};
        ExpectResampleMatches(spec);
      }
    }
  }
}

// Downscales and fractional scales take the box-filter path, which is the one
// whose rounding has to stay exact.
TEST(RustKernelParity, FractionalAndDownscalesMatch) {
  for (std::uint32_t sw : kDimensions) {
    for (std::uint32_t dw : kDimensions) {
      if (dw > sw * 2 && sw * 2 < 32u) {
        // Skip the very largest expansions; the matrix is already dense.
        continue;
      }
      ResampleCase spec{sw, sw, dw, dw, 0, "mixed scale"};
      ExpectResampleMatches(spec);
    }
  }
}

// A stride wider than the image, with no padding, exercises the same inner
// loops from a source buffer that is not tightly packed in y.
TEST(RustKernelParity, PaddedStridesMatch) {
  for (std::uint32_t padding : {0u, 1u, 3u, 4u, 15u}) {
    for (std::uint32_t sw : {1u, 4u, 5u, 16u, 33u}) {
      ResampleCase spec{sw, sw, sw * 3 + 1, sw * 2 + 1, padding,
                        "padded stride"};
      ExpectResampleMatches(spec);
    }
  }
}

// Dimensions that are not multiples of four force the partial-edge path at the
// image borders, which is the one branch the SIMD interior path skips.
TEST(RustKernelParity, PartialEdgesMatch) {
  constexpr std::uint32_t kPartial[] = {1, 2, 3, 5, 6, 7, 9, 11, 13, 15, 17};
  for (std::uint32_t sh : kPartial) {
    for (std::uint32_t dh : kPartial) {
      ResampleCase spec{sh, sh, dh, dh, 0, "partial edge"};
      ExpectResampleMatches(spec);
    }
  }
}

TEST(RustKernelParity, LargeImagesMatch) {
  ResampleCase specs[] = {
      {128, 128, 512, 512, 0, "128 to 512"},
      {512, 512, 128, 128, 0, "512 to 128"},
      {1920, 1080, 3840, 2160, 0, "full HD upscale"},
      {3840, 2160, 1920, 1080, 0, "full HD downscale"},
      {511, 1023, 1023, 511, 7, "prime dimensions with padding"},
      {4096, 4096, 64, 64, 0, "deep downscale"},
  };
  for (const ResampleCase& spec : specs) {
    ExpectResampleMatches(spec);
  }
}

// The dispatcher and the C++ oracle must agree whichever way the switch is set.
//
// ExpectResampleMatches already compares C++ against the FFI directly, which
// covers the kernels themselves. What it cannot cover is the dispatch decision:
// that the switch actually routes, that both routes produce the same image, and
// that clearing the override returns the process to the environment's choice.
// That is what regressed once -- the dispatcher latched its environment read,
// so an in-process switch had no effect and both arms of the comparison ran the
// same code while still reporting a clean pass.
TEST(RustKernelParity, DispatcherAgreesWithCppWhicheverWayTheSwitchIsSet) {
  constexpr std::uint32_t kWidth = 37;   // not a multiple of four
  constexpr std::uint32_t kHeight = 23;  // so the partial-edge path is taken
  const std::size_t source_bytes = kWidth * kHeight * 4;
  const std::size_t dest_bytes = 96 * 71 * 4;

  std::vector<std::uint8_t> source(source_bytes);
  Corpus corpus(0x51ed'2701u);
  corpus.Fill(source.data(), source.size());

  std::vector<std::uint8_t> cpp_dest(dest_bytes, 0xcd);
  {
    const ScopedCppKernels force_cpp;
    mocktail::graphics::ResampleRgba(source.data(), kWidth, kHeight,
                                     kWidth * 4, 96, 71, cpp_dest.data());
  }

  for (int setting : {0, 1}) {
    std::vector<std::uint8_t> dispatched(dest_bytes, 0xcd);
    {
      const ScopedRustToggle toggle(setting, setting);
      mocktail::graphics::ResampleRgba(source.data(), kWidth, kHeight,
                                       kWidth * 4, 96, 71,
                                       dispatched.data());
    }
    EXPECT_EQ(cpp_dest, dispatched)
        << "ResampleRgba disagreed with the C++ oracle while the resampler "
           "toggle was set to "
        << setting;
  }

  // And the override is gone afterwards, so the next test in this binary is not
  // silently running against a switch somebody left pinned.
  etc2_decoder::SetRustResampleToggle(1);
  std::vector<std::uint8_t> after_clear(dest_bytes, 0xcd);
  mocktail::graphics::ResampleRgba(source.data(), kWidth, kHeight, kWidth * 4,
                                   96, 71, after_clear.data());
  etc2_decoder::SetRustResampleToggle(-1);
  EXPECT_EQ(cpp_dest, after_clear)
      << "clearing the override did not return the dispatcher to the "
         "environment's choice";
}

// Rejection must leave the destination exactly as it was.
TEST(RustKernelParity, RejectionLeavesDestinationUntouched) {
  std::vector<std::uint8_t> source(4 * 4 * 4, 0x11);
  const std::size_t dest_bytes = 8 * 8 * 4;
  std::vector<std::uint8_t> dest(dest_bytes, 0xcd);

  struct Rejection {
    std::uint32_t sw, sh, w, h;
    std::size_t stride;
  };
  const Rejection rejections[] = {
      {0, 4, 8, 8, 16},  // zero source width
      {4, 0, 8, 8, 16},  // zero source height
      {4, 4, 0, 8, 16},  // zero destination width
      {4, 4, 8, 0, 16},  // zero destination height
      {4, 4, 8, 8, 0},   // null source or destination
      {4, 4, 8, 8, 4},   // stride too small for one source row
      {4, 4, 8, 8, 15},  // stride just short of source_width * 4
  };
  for (const Rejection& r : rejections) {
    std::fill(dest.begin(), dest.end(), 0xcd);
    const int rc = mocktail_rs_resample_rgba(source.data(), r.sw, r.sh, r.stride,
                                             r.w, r.h, dest.data());
    EXPECT_NE(0, rc) << "sw=" << r.sw << " sh=" << r.sh << " w=" << r.w
                     << " h=" << r.h << " stride=" << r.stride;
    for (std::size_t i = 0; i < dest_bytes; ++i) {
      EXPECT_EQ(0xcd, dest[i]) << "destination byte " << i << " was written";
    }
  }

  // A null source or destination is still refused rather than crashing.
  std::fill(dest.begin(), dest.end(), 0xcd);
  EXPECT_NE(0, mocktail_rs_resample_rgba(nullptr, 4, 4, 16, 8, 8, dest.data()));
  EXPECT_NE(0,
            mocktail_rs_resample_rgba(source.data(), 4, 4, 16, 8, 8, nullptr));
}
