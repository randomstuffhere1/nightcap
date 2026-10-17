#include "runtime/host_toast_overlay.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

// The overlay registers itself while it is initialized. The compositor reaches
// it through the ABI, which locks this first and the overlay's own mutex second,
// so registration takes them in that order too.
std::mutex g_active_toast_mutex;
HostToastOverlay* g_active_toast = nullptr;

namespace {

// The card's own look. Sized in logical pixels and scaled by the display, so it
// is the same size on the screen whatever the scale factor is.
constexpr int kCardHeightPx = 48;
constexpr int kPaddingXPx = 20;
constexpr int kIconSizePx = 18;
constexpr int kIconGapPx = 12;
constexpr int kTextSizePx = 14;
// The gap between the screen's top edge and the card at rest.
constexpr int kRestOffsetPx = 24;
constexpr int kMaximumTextWidthPx = 520;
// Down from above the top edge, held, then back up the way it came.
constexpr std::int64_t kSlideUs = 260000;
constexpr std::int64_t kHoldUs = 5000000;

constexpr std::uint8_t kCardRed = 31;
constexpr std::uint8_t kCardGreen = 31;
constexpr std::uint8_t kCardBlue = 34;
constexpr std::uint8_t kMarkRed = 77;
constexpr std::uint8_t kMarkGreen = 163;
constexpr std::uint8_t kMarkBlue = 255;

std::int64_t MonotonicMicroseconds() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Straight-alpha "source over destination". Both sides are unpremultiplied, so
// the colour has to be divided back out, which is what keeps the white text from
// picking up the colour of the card underneath it as the card fades.
void BlendPixel(std::uint8_t* destination, const std::uint8_t red,
                const std::uint8_t green, const std::uint8_t blue,
                double coverage) {
  if (coverage <= 0.0) {
    return;
  }
  const double source_alpha = std::min(1.0, coverage);
  const double inverse = 1.0 - source_alpha;
  const double destination_alpha = destination[3] / 255.0;
  const double out_alpha = source_alpha + destination_alpha * inverse;
  if (out_alpha <= 0.0) {
    return;
  }
  for (int channel = 0; channel < 3; ++channel) {
    const std::uint8_t source = channel == 0 ? red : (channel == 1 ? green : blue);
    const double source_unit = source / 255.0;
    const double destination_unit = destination[channel] / 255.0;
    const double premultiplied = source_unit * source_alpha +
                                destination_unit * destination_alpha * inverse;
    destination[channel] = static_cast<std::uint8_t>(
        std::clamp(premultiplied / out_alpha, 0.0, 1.0) * 255.0 + 0.5);
  }
  destination[3] = static_cast<std::uint8_t>(out_alpha * 255.0 + 0.5);
}

// Coverage of a rounded rectangle at a pixel centre, from the shape's distance
// field. Half a pixel of it is inside, so the edge is smooth rather than
// stepped.
double RoundedBoxCoverage(double px, double py, double center_x, double center_y,
                          double half_width, double half_height, double radius) {
  const double dx = std::fabs(px - center_x) - (half_width - radius);
  const double dy = std::fabs(py - center_y) - (half_height - radius);
  const double outside = std::hypot(std::max(dx, 0.0), std::max(dy, 0.0));
  const double inside = std::min(std::max(dx, dy), 0.0);
  const double distance = outside + inside - radius;
  return std::clamp(0.5 - distance, 0.0, 1.0);
}

// Coverage of the information mark: a ring with a dot above a stem.
double MarkCoverage(double px, double py, double center_x, double center_y,
                    double size) {
  const double radius = size / 2.0;
  const double stroke = std::max(1.0, size * 0.085);
  const double distance = std::hypot(px - center_x, py - center_y);
  const double ring = std::fabs(distance - (radius - stroke * 0.5)) -
                     stroke * 0.5;
  const double ring_coverage = std::clamp(0.5 - ring, 0.0, 1.0);
  const double dot =
      std::hypot(px - center_x, py - (center_y - radius * 0.34)) - radius * 0.14;
  const double dot_coverage = std::clamp(0.5 - dot, 0.0, 1.0);
  // The stem is a thick line with round ends.
  const double stem_top = center_y - radius * 0.02;
  const double stem_bottom = center_y + radius * 0.42;
  const double stem_half = stroke * 0.5;
  const double stem_dx = std::fabs(px - center_x) - stem_half;
  const double stem_dy = std::max(stem_top - py, py - stem_bottom);
  const double stem_outside =
      std::hypot(std::max(stem_dx, 0.0), std::max(stem_dy, 0.0));
  const double stem_inside = std::min(std::max(stem_dx, stem_dy), 0.0);
  const double stem_coverage =
      std::clamp(0.5 - (stem_outside + stem_inside), 0.0, 1.0);
  return std::clamp(std::max(std::max(ring_coverage, dot_coverage),
                             stem_coverage),
                    0.0, 1.0);
}

std::vector<std::string> ToastFontFiles() {
  std::vector<std::string> files;
  const char* configured = std::getenv("MOCKTAIL_TEXT_FONT");
  if (configured != nullptr && configured[0] != '\0') {
    files.emplace_back(configured);
  }
  if (!FcInit()) {
    return files;
  }
  FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans"));
  if (pattern == nullptr) {
    return files;
  }
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcFontSet* fonts = FcFontSort(nullptr, pattern, FcTrue, nullptr, &result);
  FcPatternDestroy(pattern);
  if (fonts == nullptr) {
    return files;
  }
  for (int index = 0; index < fonts->nfont; ++index) {
    FcChar8* file = nullptr;
    if (FcPatternGetString(fonts->fonts[index], FC_FILE, 0, &file) !=
        FcResultMatch) {
      continue;
    }
    files.emplace_back(reinterpret_cast<const char*>(file));
    if (files.size() >= 2) {
      break;
    }
  }
  FcFontSetDestroy(fonts);
  return files;
}

double SmoothStep(std::int64_t progress_us) {
  // 0 at the ends of the slide, fastest through the middle of it.
  const double scaled =
      std::clamp(progress_us / static_cast<double>(kSlideUs), 0.0, 1.0);
  return scaled * scaled * (3.0 - 2.0 * scaled);
}

}  // namespace

HostToastOverlay::~HostToastOverlay() { (void)Shutdown(); }

Status HostToastOverlay::Initialize(RobloxTextOverlayViewport viewport) {
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "host toast overlay viewport is invalid");
  }
  std::lock_guard<std::mutex> active_lock(g_active_toast_mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  viewport_ = viewport;
  initialized_ = true;
  // Registered from here rather than on first use, so a notice raised between
  // the first frame and the first notice still has somewhere to go.
  g_active_toast = this;
  return Status::Ok();
}

Status HostToastOverlay::UpdateViewport(RobloxTextOverlayViewport viewport) {
  if (!viewport.valid()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "host toast overlay viewport is invalid");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "host toast overlay is not initialized");
  }
  if (viewport.width == viewport_.width &&
      viewport.height == viewport_.height &&
      viewport.density_scale == viewport_.density_scale) {
    return Status::Ok();
  }
  viewport_ = viewport;
  // A different size or scale means the card is drawn at a different size.
  ReleaseFrameLocked();
  return Status::Ok();
}

Status HostToastOverlay::Shutdown() {
  std::lock_guard<std::mutex> active_lock(g_active_toast_mutex);
  std::lock_guard<std::mutex> lock(mutex_);
  if (g_active_toast == this) {
    g_active_toast = nullptr;
  }
  ReleaseFrameLocked();
  message_.clear();
  viewport_ = {};
  initialized_ = false;
  fonts_.Close();
  return Status::Ok();
}

void HostToastOverlay::Show(std::string_view message) {
  if (message.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return;
  }
  if (message == message_ && started_us_ != 0) {
    // The same notice again: show it again from the start rather than leaving
    // the player with whatever the previous one's animation had reached.
    started_us_ = MonotonicMicroseconds();
    return;
  }
  message_.assign(message);
  started_us_ = MonotonicMicroseconds();
  ReleaseFrameLocked();
}

void HostToastOverlay::ReleaseFrameLocked() {
  std::fill(rgba_.begin(), rgba_.end(), std::uint8_t{0});
  rgba_.clear();
  card_width_ = 0;
  card_height_ = 0;
  ++revision_;
  if (revision_ == 0) {
    ++revision_;
  }
}

int HostToastOverlay::ToastOffsetAt(std::int64_t elapsed_us) {
  if (elapsed_us <= 0) {
    return 0;
  }
  if (elapsed_us >= kSlideUs + kHoldUs + kSlideUs) {
    return -kCardHeightPx;
  }
  if (elapsed_us < kSlideUs) {
    // Down from above the screen's top edge: a whole card above the window,
    // easing to the resting gap.
    const double eased = SmoothStep(elapsed_us);
    return static_cast<int>(kRestOffsetPx - eased * kRestOffsetPx -
                            kCardHeightPx);
  }
  if (elapsed_us < kSlideUs + kHoldUs) {
    return kRestOffsetPx;
  }
  // Back up the way it came, so the whole of it clears the top edge.
  const double eased = SmoothStep(elapsed_us - kSlideUs - kHoldUs);
  return static_cast<int>(kRestOffsetPx -
                          eased * (kRestOffsetPx + kCardHeightPx));
}

bool HostToastOverlay::ToastIsFinished(std::int64_t elapsed_us) {
  return elapsed_us >= kSlideUs + kHoldUs + kSlideUs;
}

void HostToastOverlay::EnsureFontLocked() {
  const int scale = std::max(
      1, static_cast<int>(std::lround(viewport_.density_scale)));
  const int point_size = kTextSizePx * scale;
  if (fonts_.primary() != nullptr && font_point_size_ == point_size) {
    return;
  }
  if (fonts_.Open(ToastFontFiles(), static_cast<float>(point_size))) {
    font_point_size_ = point_size;
  }
}

bool HostToastOverlay::RasterizeLocked() {
  if (message_.empty() || !viewport_.valid()) {
    return false;
  }
  EnsureFontLocked();
  scale_ = std::max(1, static_cast<int>(std::lround(viewport_.density_scale)));
  const int card_height = kCardHeightPx * scale_;
  const int padding_x = kPaddingXPx * scale_;
  const int icon_size = kIconSizePx * scale_;
  const int icon_gap = kIconGapPx * scale_;

  int text_width = 0;
  SDL_Surface* text = nullptr;
  if (fonts_.primary() != nullptr) {
    const SDL_Color white = {255, 255, 255, 255};
    text = TTF_RenderText_Blended(fonts_.primary(), message_.data(),
                                  message_.size(), white);
    if (text != nullptr) {
      // What SDL_ttf hands back is not guaranteed to be RGBA in that order, and
      // reading it as though it were turns its transparent background into opaque
      // white and paints the card out.
      SDL_Surface* converted =
          SDL_ConvertSurface(text, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(text);
      text = converted;
    }
    if (text != nullptr) {
      text_width = text->w;
    }
  }
  // A notice with no text still shows its mark, so a failure to find a font
  // costs the message and not the card.
  const int maximum_text = kMaximumTextWidthPx * scale_;
  if (text_width > maximum_text) {
    SDL_DestroySurface(text);
    text = nullptr;
    text_width = maximum_text;
  }
  const int card_width = padding_x * 2 + icon_size + icon_gap + text_width;
  if (card_width <= 0 || card_height <= 0) {
    if (text != nullptr) {
      SDL_DestroySurface(text);
    }
    return false;
  }

  const std::size_t pixels =
      static_cast<std::size_t>(card_width) * card_height;
  std::vector<std::uint8_t> buffer(pixels * 4, 0);

  const double half_width = card_width / 2.0;
  const double half_height = card_height / 2.0;
  const double radius = half_height;
  const double mark_center_x = padding_x + icon_size / 2.0;
  for (int y = 0; y < card_height; ++y) {
    for (int x = 0; x < card_width; ++x) {
      const double px = x + 0.5;
      const double py = y + 0.5;
      const double coverage = RoundedBoxCoverage(px, py, half_width, half_height,
                                                 half_width, half_height,
                                                 radius);
      if (coverage <= 0.0) {
        continue;
      }
      std::uint8_t* pixel = buffer.data() + (static_cast<std::size_t>(y) *
                                                 card_width + x) * 4;
      BlendPixel(pixel, kCardRed, kCardGreen, kCardBlue, coverage);
      const double mark = MarkCoverage(px, py, mark_center_x, half_height,
                                       static_cast<double>(icon_size));
      if (mark > 0.0) {
        BlendPixel(pixel, kMarkRed, kMarkGreen, kMarkBlue, mark);
      }
    }
  }

  if (text != nullptr) {
    const int text_x = padding_x + icon_size + icon_gap;
    const int text_y = std::max(0, (card_height - text->h) / 2);
    for (int y = 0; y < text->h && text_y + y < card_height; ++y) {
      for (int x = 0; x < text->w && text_x + x < card_width; ++x) {
        const std::uint8_t* source =
            static_cast<const std::uint8_t*>(text->pixels) +
            static_cast<std::size_t>(y) * text->pitch + x * 4;
        const double alpha = source[3] / 255.0;
        if (alpha <= 0.0) {
          continue;
        }
        std::uint8_t* pixel =
            buffer.data() +
            (static_cast<std::size_t>(text_y + y) * card_width + text_x + x) * 4;
        BlendPixel(pixel, source[0], source[1], source[2], alpha);
      }
    }
    SDL_DestroySurface(text);
  }

  rgba_ = std::move(buffer);
  card_width_ = card_width;
  card_height_ = card_height;
  ++revision_;
  if (revision_ == 0) {
    ++revision_;
  }
  return true;
}

bool HostToastOverlay::QueryFrame(MocktailHostOverlayFrameInfo* frame) {
  if (frame == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  *frame = {};
  frame->revision = revision_;
  if (!initialized_ || message_.empty() || started_us_ == 0) {
    return false;
  }
  if (ToastIsFinished(MonotonicMicroseconds() - started_us_)) {
    message_.clear();
    started_us_ = 0;
    ReleaseFrameLocked();
    return false;
  }
  if (rgba_.empty() && !RasterizeLocked()) {
    return false;
  }
  const int logical_width = viewport_.coordinate_width();
  const int logical_height = viewport_.coordinate_height();
  const int scaled_rest = kRestOffsetPx * scale_;
  const int scaled_height = kCardHeightPx * scale_;
  // The card is centered in the window, and its top edge is where the animation
  // puts it. A negative y is fine: the compositor clips the part that has gone
  // past the screen's top edge.
  const int x = std::max(0, (logical_width - card_width_) / 2);
  const int offset = ToastOffsetAt(MonotonicMicroseconds() - started_us_);
  const int y = scaled_rest + (offset - kRestOffsetPx) * scale_;
  frame->visible = 1;
  frame->coordinate_width = static_cast<std::uint32_t>(logical_width);
  frame->coordinate_height = static_cast<std::uint32_t>(logical_height);
  frame->x = x;
  frame->y = y;
  frame->width = static_cast<std::uint32_t>(card_width_);
  frame->height = static_cast<std::uint32_t>(scaled_height);
  frame->row_bytes = frame->width * 4;
  frame->rgba_bytes = rgba_.size();
  return true;
}

bool HostToastOverlay::CopyFrame(std::uint64_t revision, void* rgba,
                                 std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rgba == nullptr || rgba_capacity < rgba_.size() || rgba_.empty() ||
      revision != revision_ || revision == 0) {
    return false;
  }
  std::memcpy(rgba, rgba_.data(), rgba_.size());
  return true;
}

bool ShowHostToast(std::string_view message) {
  HostToastOverlay* toast = nullptr;
  {
    // The overlay registers itself when it is initialized, so a notice raised
    // before the session has a window has nowhere to go and is dropped.
    std::lock_guard<std::mutex> lock(g_active_toast_mutex);
    toast = g_active_toast;
  }
  if (toast == nullptr) {
    return false;
  }
  toast->Show(message);
  return true;
}

}  // namespace runtime
}  // namespace mocktail

extern "C" bool mocktail_host_overlay_query(
    MocktailHostOverlayFrameInfo* frame) {
  std::lock_guard<std::mutex> lock(mocktail::runtime::g_active_toast_mutex);
  return mocktail::runtime::g_active_toast != nullptr &&
         mocktail::runtime::g_active_toast->QueryFrame(frame);
}

extern "C" bool mocktail_host_overlay_copy(std::uint64_t revision, void* rgba,
                                           std::size_t rgba_capacity) {
  std::lock_guard<std::mutex> lock(mocktail::runtime::g_active_toast_mutex);
  return mocktail::runtime::g_active_toast != nullptr &&
         mocktail::runtime::g_active_toast->CopyFrame(revision, rgba,
                                                      rgba_capacity);
}