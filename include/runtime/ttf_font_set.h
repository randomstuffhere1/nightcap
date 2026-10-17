#ifndef MOCKTAIL_RUNTIME_TTF_FONT_SET_H_
#define MOCKTAIL_RUNTIME_TTF_FONT_SET_H_

#include <string>
#include <vector>

struct TTF_Font;

namespace mocktail {
namespace runtime {

// Owns one or more SDL3_ttf fonts, chained as fallbacks, along with the
// SDL3_ttf library initialisation they need.
//
// Both text overlays opened and closed fonts by hand and each tracked its own
// `ttf_initialized_` flag, leaving the pairing between TTF_Init and TTF_Quit up
// to whichever call site got there first. SDL3_ttf counts those calls itself, so
// pairing each successful TTF_Init with a TTF_Quit here is what keeps one
// overlay shutting down from pulling the library out from under the other.
class TtfFontSet final {
 public:
  TtfFontSet() = default;
  ~TtfFontSet();

  TtfFontSet(const TtfFontSet&) = delete;
  TtfFontSet& operator=(const TtfFontSet&) = delete;
  TtfFontSet(TtfFontSet&&) = delete;
  TtfFontSet& operator=(TtfFontSet&&) = delete;

  // Opens every file at `point_size` and chains them as fallbacks onto the
  // first. Files that do not open are skipped, so one missing font does not
  // cost the caller its text.
  //
  // On failure nothing is changed: the fonts already open stay open, because a
  // failed reopen must not leave the overlay with nothing to draw with.
  //
  // Returns false only when SDL3_ttf cannot be initialised or no file opened.
  bool Open(const std::vector<std::string>& files, float point_size);

  // Closes every font and releases the library if this was the last set.
  void Close();

  bool empty() const { return fonts_.empty(); }
  TTF_Font* primary() const { return fonts_.empty() ? nullptr : fonts_.front(); }
  const std::vector<TTF_Font*>& fonts() const { return fonts_; }

 private:
  std::vector<TTF_Font*> fonts_;
  bool initialised_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_TTF_FONT_SET_H_