#include "runtime/ttf_font_set.h"

#include <SDL3_ttf/SDL_ttf.h>

namespace mocktail {
namespace runtime {
namespace {

// Releases the fonts but deliberately leaves SDL3_ttf initialised, so a reopen
// does not have to bring the library back up between closing the old set and
// opening the new one.
void CloseFonts(std::vector<TTF_Font*>* fonts) {
  if (fonts->empty()) {
    return;
  }
  TTF_ClearFallbackFonts(fonts->front());
  for (auto iterator = fonts->rbegin(); iterator != fonts->rend(); ++iterator) {
    TTF_CloseFont(*iterator);
  }
  fonts->clear();
}

}  // namespace

TtfFontSet::~TtfFontSet() { Close(); }

bool TtfFontSet::Open(const std::vector<std::string>& files,
                      float point_size) {
  if (!initialised_) {
    if (!TTF_Init()) {
      return false;
    }
    initialised_ = true;
  }

  std::vector<TTF_Font*> opened;
  opened.reserve(files.size());
  for (const std::string& file : files) {
    TTF_Font* candidate = TTF_OpenFont(file.c_str(), point_size);
    if (candidate != nullptr) {
      opened.push_back(candidate);
    }
  }
  if (opened.empty()) {
    return false;
  }
  for (std::size_t index = 1; index < opened.size(); ++index) {
    (void)TTF_AddFallbackFont(opened.front(), opened[index]);
  }
  // Only once the replacement is known to be usable, so a failure above leaves
  // the fonts already on screen still open.
  CloseFonts(&fonts_);
  fonts_ = std::move(opened);
  return true;
}

void TtfFontSet::Close() {
  CloseFonts(&fonts_);
  if (initialised_) {
    // Paired with the successful TTF_Init above. SDL3_ttf counts these itself,
    // so the other overlay's set stays usable when one of them closes.
    TTF_Quit();
    initialised_ = false;
  }
}

}  // namespace runtime
}  // namespace mocktail