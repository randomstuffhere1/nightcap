#ifndef MOCKTAIL_RUNTIME_ROBLOX_GAME_URL_H_
#define MOCKTAIL_RUNTIME_ROBLOX_GAME_URL_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

// The public roblox.com game link form, https://www.roblox.com/games/<id>[/<slug>].
struct RobloxGameUrlLink {
  // The /games/ path carries the place ID and is authoritative for the launch
  // target. A universe lookup may only ever supply a display name.
  int64_t place_id = 0;
  // privateServerLinkCode (spelled linkCode by some share dialogs). Empty for
  // public links.
  std::string link_code;
  // accessCode for reserved servers. Empty for ordinary links.
  std::string access_code;
  // Display-only fallback derived from the URL slug.
  std::string fallback_game_name;
};

// Parses the roblox.com game URL form. Enforces the same origin allow-list as
// the PlaceLauncher URL so the launch and paste entry points cannot drift:
// https only, host roblox.com or www.roblox.com, no userinfo, no fragment,
// port 443 only, and a /games/<digits>[/<slug>] path. A code parameter that is
// present but malformed rejects the whole link rather than silently downgrading
// a private join to a public one. Returns nullopt for every other input.
std::optional<RobloxGameUrlLink> ParseRobloxGameUrl(std::string_view url);

// Roblox's deferred share form,
// https://www.roblox.com/share?code=<32 hex>&type=Server. Since October 2025
// Roblox generates this shorter link for private servers instead of the
// /games/<id>?privateServerLinkCode=... form, and every previously generated
// link keeps working.
struct RobloxShareLinkUrl {
  // The opaque deferred-link token. It carries no place ID and no link code:
  // only Roblox's authenticated LinkingService can turn it into a joinable
  // target.
  std::string code;
  // Share flavour, normalized to the casing Roblox documents ("Server").
  // Only a shared private server reaches the private-server paste path;
  // every other flavour is a different link form.
  std::string type;
};

// Parses the deferred share form under the same origin allow-list as
// ParseRobloxGameUrl, so the two cannot drift on host, port or path handling.
// Accepts both /share and the /share-links path Roblox redirects browsers to.
// Returns nullopt for every other input, including other share flavours.
std::optional<RobloxShareLinkUrl> ParseRobloxShareUrl(std::string_view url);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_GAME_URL_H_
