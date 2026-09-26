#ifndef MOCKTAIL_RUNTIME_ROBLOX_PRIVATE_SERVER_PASTE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_PRIVATE_SERVER_PASTE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mocktail/status.h"

namespace mocktail {
namespace runtime {

struct RobloxPrivateServerGameLink {
  // Roblox's public /games/<id>/... URL uses the place ID. It must remain the
  // launch target; the universe lookup is only used to obtain the display name.
  int64_t place_id = 0;
  std::string link_code;
  // The URL slug is only a display fallback. The games API remains the source
  // of the game name.
  std::string fallback_game_name;
};

std::optional<RobloxPrivateServerGameLink> ParseRobloxPrivateServerGameLink(
    std::string_view text);

struct RobloxPrivateServerGameDetails {
  int64_t place_id = 0;
  std::string name;
};

struct RobloxPrivateServerPasteResult {
  bool handled = false;
  bool approved = false;
  Status status = Status::Ok();
};

struct RobloxPrivateServerPasteActions {
  using MainTextEntryFn = bool (*)(void* context);
  using ResolveGameFn = Status (*)(void* context, int64_t place_id,
                                   RobloxPrivateServerGameDetails* details);
  using ConfirmFn = Status (*)(void* context, std::string_view game_name,
                               bool* approved);
  using LaunchFn = Status (*)(void* context, int64_t place_id,
                              std::string_view link_code);

  using ConfirmLinkFn = Status (*)(void* context,
                                   const RobloxPrivateServerGameLink& link,
                                   bool* approved);

  void* context = nullptr;
  MainTextEntryFn is_main_text_entry = nullptr;
  ResolveGameFn resolve_game = nullptr;
  ConfirmFn confirm = nullptr;
  LaunchFn launch = nullptr;
  // Optional fast path: confirmation receives the parsed link immediately and
  // may resolve the display name after the dialog is already visible.
  ConfirmLinkFn confirm_link = nullptr;

  bool complete() const {
    return is_main_text_entry != nullptr && launch != nullptr &&
           (confirm_link != nullptr ||
            (resolve_game != nullptr && confirm != nullptr));
  }
};

// Recognizes a complete private-server game URL, confirms it, and queues the
// matching private-server launch. A handled result consumes the paste whether
// the user declines or resolution fails, so the URL is never handed onward to
// an automatic join path. Results from text boxes inside an experience are not
// handled and therefore retain the normal paste behavior.
RobloxPrivateServerPasteResult HandleRobloxPrivateServerPaste(
    std::string_view text, const RobloxPrivateServerPasteActions& actions);

// Production callbacks used by the host runtime. They are kept separate from
// the policy above so link parsing and confirmation behavior remain testable
// without a display or network access.
Status ResolveRobloxPrivateServerGame(
    void* context, int64_t place_id,
    RobloxPrivateServerGameDetails* details);
Status ConfirmRobloxPrivateServerPaste(void* context,
                                       std::string_view game_name,
                                       bool* approved);
Status ConfirmRobloxPrivateServerPasteLink(
    void* context, const RobloxPrivateServerGameLink& link, bool* approved);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_PRIVATE_SERVER_PASTE_H_
