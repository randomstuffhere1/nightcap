#ifndef MOCKTAIL_RUNTIME_HOST_TOAST_OVERLAY_H_
#define MOCKTAIL_RUNTIME_HOST_TOAST_OVERLAY_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "mocktail/graphics/text_overlay_frame.h"
#include "mocktail/status.h"
#include "runtime/roblox_text_display_state.h"
#include "runtime/ttf_font_set.h"

namespace mocktail {
namespace runtime {

// The host's own notices, drawn into the game's own window rather than into a
// window of their own. A separate window sits on top of the game and takes the
// clicks aimed at it, which in a game means the game stops responding to the
// mouse for as long as the notice is up. Drawing into the frame the game is
// already presenting costs the game nothing and cannot intercept anything.
//
// The notice is a pill: a dark rounded card, an information mark, and the
// message. It slides down from the top middle of the screen, holds, and slides
// back up the way it came.
class HostToastOverlay final {
 public:
  HostToastOverlay() = default;
  ~HostToastOverlay();

  HostToastOverlay(const HostToastOverlay&) = delete;
  HostToastOverlay& operator=(const HostToastOverlay&) = delete;

  Status Initialize(RobloxTextOverlayViewport viewport);
  Status UpdateViewport(RobloxTextOverlayViewport viewport);
  Status Shutdown();

  // Shows a notice, replacing one already on screen. Safe to call from any
  // thread: the lookup that decides where the session's server is runs on a
  // worker.
  void Show(std::string_view message);

  bool QueryFrame(MocktailHostOverlayFrameInfo* frame);
  bool CopyFrame(std::uint64_t revision, void* rgba, std::size_t capacity);

 private:
  // Where the card sits at a point in the notice's life, given how long ago it
  // was shown, and whether that life is over. Static and free of state so the
  // timings are one place, not spread through the frame loop.
  static int ToastOffsetAt(std::int64_t elapsed_us);
  static bool ToastIsFinished(std::int64_t elapsed_us);

  void EnsureFontLocked();
  // Draws the card at its resting size once; the animation then only moves it,
  // so the pixels do not have to be drawn again for every frame.
  bool RasterizeLocked();
  void ReleaseFrameLocked();

  std::mutex mutex_;
  RobloxTextOverlayViewport viewport_;
  std::string message_;
  std::vector<std::uint8_t> rgba_;
  std::int64_t started_us_ = 0;
  std::uint64_t revision_ = 0;
  int card_width_ = 0;
  int card_height_ = 0;
  int scale_ = 1;
  bool initialized_ = false;
  TtfFontSet fonts_;
  int font_point_size_ = 0;
};

// Shows a notice through whichever overlay is active. A process with no overlay
// yet, or a session that never reaches a frame, shows nothing: there is nowhere
// on screen that is not the game's own.
bool ShowHostToast(std::string_view message);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_HOST_TOAST_OVERLAY_H_