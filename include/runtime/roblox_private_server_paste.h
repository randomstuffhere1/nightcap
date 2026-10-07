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
  // Zero for a deferred /share?code=... link, which carries neither a place ID
  // nor a link code until Roblox's LinkingService resolves it.
  int64_t place_id = 0;
  std::string link_code;
  // The URL slug is only a display fallback. The games API remains the source
  // of the game name.
  std::string fallback_game_name;
  // Set only for a deferred /share?code=... link. Resolution turns this into
  // the place ID and link code the launch actually needs.
  std::string share_code;
};

// A fully resolved, joinable private-server target. A /games/... link fills
// this from the URL itself; a deferred /share?code=... link fills it from
// Roblox's LinkingService.
struct RobloxPrivateServerLaunchTarget {
  int64_t place_id = 0;
  std::string link_code;
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
                                   RobloxPrivateServerLaunchTarget* target,
                                   bool* approved);

  void* context = nullptr;
  MainTextEntryFn is_main_text_entry = nullptr;
  ResolveGameFn resolve_game = nullptr;
  ConfirmFn confirm = nullptr;
  LaunchFn launch = nullptr;
  // Optional fast path: confirmation receives the parsed link immediately,
  // resolves whatever the URL form requires, and reports the joinable target
  // it would launch. Nothing launches unless this reports an approved target.
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
    void* context, const RobloxPrivateServerGameLink& link,
    RobloxPrivateServerLaunchTarget* target, bool* approved);

// Turns a deferred /share?code=... link into a joinable place ID and link
// code using Roblox's authenticated LinkingService. Exposed so the resolution
// contract stays testable without a display. Fails closed: it never invents a
// target, so a rejected or expired link can never launch anything.
Status ResolveRobloxPrivateServerShareLink(
    std::string_view share_code, RobloxPrivateServerLaunchTarget* target,
    std::string* fallback_game_name);

// Parses a LinkingService resolve-link payload into a joinable target. Roblox
// answers with one populated object per invite flavour and nulls the rest; a
// shared private server arrives under "privateServerInviteData". Exposed so the
// response contract stays testable without a session or network access.
// Returns false when the payload names no live private server, which includes
// an expired, revoked or already-joined link.
bool ParseRobloxPrivateServerShareResponse(
    std::string_view json_body, RobloxPrivateServerLaunchTarget* target,
    std::string* fallback_game_name);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_PRIVATE_SERVER_PASTE_H_
