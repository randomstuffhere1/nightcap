#include "runtime/host_toast_overlay.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>

namespace mocktail::runtime {
namespace {

// A steady 60 Hz display and a steady 30 Hz one are both regular enough for the
// leave slide to be seen; the notice holds until the game settles into one.
TEST(HostToastOverlayTest, SteadyFrameRatesAllowTheNoticeToLeave) {
  const std::int64_t sixty_hz[] = {16667, 16667, 16667, 16667, 16667};
  EXPECT_TRUE(HostToastOverlay::PresentIntervalsAreSteady(
      sixty_hz, std::size(sixty_hz)));

  const std::int64_t thirty_hz[] = {33333, 33333, 33333, 33333, 33333};
  EXPECT_TRUE(HostToastOverlay::PresentIntervalsAreSteady(
      thirty_hz, std::size(thirty_hz)));

  // Twenty-five frames a second is the slowest the slide still has frames in.
  const std::int64_t twenty_five_hz[] = {40000, 40000, 40000, 40000, 40000};
  EXPECT_TRUE(HostToastOverlay::PresentIntervalsAreSteady(
      twenty_five_hz, std::size(twenty_five_hz)));
}

TEST(HostToastOverlayTest, AStutterOrASlowGameKeepsTheNoticeUp) {
  // One frame that takes far longer than a slide lands no frame inside it, so
  // the notice waits rather than vanishing without its transition.
  const std::int64_t one_stutter[] = {16667, 16667, 16667, 16667, 140000};
  EXPECT_FALSE(HostToastOverlay::PresentIntervalsAreSteady(
      one_stutter, std::size(one_stutter)));

  const std::int64_t twenty_hz[] = {50000, 50000, 50000, 50000, 50000};
  EXPECT_FALSE(HostToastOverlay::PresentIntervalsAreSteady(
      twenty_hz, std::size(twenty_hz)));

  EXPECT_FALSE(HostToastOverlay::PresentIntervalsAreSteady(nullptr, 0));
}

// The card is above the top edge before its slide, rests at the top middle
// while it holds, and is above the top edge again once it has left.
TEST(HostToastOverlayTest, TheCardSlidesInHoldsAndSlidesOut) {
  const int rest = HostToastOverlay::ToastOffsetAt(0, 0, 1000000);
  EXPECT_GT(rest, 0);
  // Long after the slide-in and with no leave yet, the card is at rest.
  EXPECT_EQ(rest, HostToastOverlay::ToastOffsetAt(0, 0, 8000000));
  // The leave starts from rest and ends past the top edge.
  EXPECT_EQ(rest, HostToastOverlay::ToastOffsetAt(0, 8000000, 8000000));
  EXPECT_LT(HostToastOverlay::ToastOffsetAt(0, 8000000, 8130000), rest);
  EXPECT_LT(HostToastOverlay::ToastOffsetAt(0, 8000000, 9000000), 0);
}

TEST(HostToastOverlayTest, TheNoticeIsOnlyFinishedOnceTheLeaveHasRunOut) {
  // No leave has begun, however long the notice has been up.
  EXPECT_FALSE(HostToastOverlay::ToastIsFinished(0, 9000000));
  // One frame into the leave it is not over.
  EXPECT_FALSE(HostToastOverlay::ToastIsFinished(1000, 1100));
  // A second later it is.
  EXPECT_TRUE(HostToastOverlay::ToastIsFinished(1000, 1001000));
}

}  // namespace
}  // namespace mocktail::runtime
