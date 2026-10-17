#ifndef MOCKTAIL_RUNTIME_HOST_TOAST_OVERLAY_H_
#define MOCKTAIL_RUNTIME_HOST_TOAST_OVERLAY_H_

#include <array>
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
// back up the way it came. The hold lasts until the game is presenting frames
// steadily, so a slide-out never lands on a stutter and reads as the notice
// blinking away.
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

  // How many consecutive present intervals are kept to judge the game's frame
  // pacing before the notice is allowed to leave. Visible for the test.
  static constexpr std::size_t kPacingSamples = 6;
  // Whether a run of present intervals is regular enough that the leave slide
  // would be seen as a transition rather than a jump. Every interval has to be
  // short enough to land a frame or more inside the slide.
  static bool PresentIntervalsAreSteady(const std::int64_t* intervals_us,
                                        std::size_t count);

  // Where the card sits at a point in the notice's life, from when it was shown
  // and, once it has started to leave, when that began. Kept pure so the
  // timings are one place and not spread through the frame loop.
  static int ToastOffsetAt(std::int64_t started_us,
                           std::int64_t leave_started_us, std::int64_t now_us);
  // The leave is over once the card has been off the top edge for a slide.
  static bool ToastIsFinished(std::int64_t leave_started_us,
                              std::int64_t now_us);

 private:
  // Records one present, dropping the second query a single present can make
  // (the Vulkan fast path asks, then the compositor asks again). The intervals
  // between the kept presents are what the exit waits on.
  void RecordPresentLocked(std::int64_t now_us);
  bool PresentsAreSteadyLocked() const;

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
  // When the slide-out began, or zero while the card is still sliding in or
  // holding. Set only once the game is presenting steadily enough for the exit
  // to be seen, so the hold lasts as long as a stutter does.
  std::int64_t leave_started_us_ = 0;
  std::array<std::int64_t, kPacingSamples> presents_us_{};
  std::size_t present_count_ = 0;
  std::size_t present_next_ = 0;
  std::int64_t last_present_us_ = 0;
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