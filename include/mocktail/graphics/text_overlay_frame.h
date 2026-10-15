#ifndef MOCKTAIL_GRAPHICS_TEXT_OVERLAY_FRAME_H_
#define MOCKTAIL_GRAPHICS_TEXT_OVERLAY_FRAME_H_

#include <cstddef>
#include <cstdint>

// Process-local ABI for host compositors. Pixels are RGBA8; consumers must copy a
// matching revision before the producer clears its storage.
struct MocktailTextOverlayFrameInfo {
  static constexpr std::uint32_t kAbiVersion = 2;

  std::uint32_t abi_version = kAbiVersion;
  std::uint32_t visible = 0;
  std::uint64_t revision = 0;
  // Coordinates are logical, not swapchain pixels.
  std::uint32_t coordinate_width = 0;
  std::uint32_t coordinate_height = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_bytes = 0;
  std::uint64_t rgba_bytes = 0;
};

extern "C" {

// false guarantees no overlay; true still requires a synchronized query.
bool mocktail_text_overlay_may_present();
bool mocktail_text_overlay_query(MocktailTextOverlayFrameInfo* frame);
bool mocktail_text_overlay_copy(std::uint64_t revision, void* rgba,
                                std::size_t rgba_capacity);

// A second, host-owned layer, composited into the same frame as the one above.
// It carries what the host itself wants on screen, such as the notice about
// which region a session's server is in. Keeping it a separate layer rather than
// part of the text surface means a notice cannot be suppressed by, or suppress,
// a chat field, and that either can be absent while the other is shown.
//
// Pixels are RGBA8, exactly as above: straight (not premultiplied) alpha, so a
// consumer composites them with alpha-over.
struct MocktailHostOverlayFrameInfo {
  static constexpr std::uint32_t kAbiVersion = 1;

  std::uint32_t abi_version = kAbiVersion;
  std::uint32_t visible = 0;
  std::uint64_t revision = 0;
  // Coordinates are logical, not swapchain pixels.
  std::uint32_t coordinate_width = 0;
  std::uint32_t coordinate_height = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_bytes = 0;
  std::uint64_t rgba_bytes = 0;
};

// Unlike the text surface there is no "may present" flag: the host layer is
// asked about on every present, and answers "not visible" when it has nothing
// to say, which is the common case.
bool mocktail_host_overlay_query(MocktailHostOverlayFrameInfo* frame);
bool mocktail_host_overlay_copy(std::uint64_t revision, void* rgba,
                                std::size_t rgba_capacity);

}  // extern "C"

#endif  // MOCKTAIL_GRAPHICS_TEXT_OVERLAY_FRAME_H_
