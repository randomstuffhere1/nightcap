#include "runtime/roblox_presence_resolver.h"

#include <curl/curl.h>


#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "net/curl_support.h"

namespace mocktail {
namespace runtime {
namespace {

// Matches the ceiling the other Roblox API readers use.
constexpr std::size_t kMaximumResponseBytes = 512U * 1024U;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

}  // namespace

// Reads mocktail's own persisted session cookie from
// $XDG_DATA_HOME/mocktail/auth/roblox.cookie (default
// ~/.local/share/mocktail/auth/roblox.cookie), matching the format
// mocktail's own auth code already writes there: a single line of the
// form ".ROBLOSECURITY=<value>". Strips the prefix so callers get just
// the bare value. Returns an empty string if no session is stored yet.
std::string ReadStoredRoblosecurityCookie() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return std::string();
  }
  const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
  const std::string base =
      (xdg_data_home != nullptr && xdg_data_home[0] != '\0')
          ? std::string(xdg_data_home)
          : std::string(home) + "/.local/share";
  const std::string path = base + "/mocktail/auth/roblox.cookie";

  std::ifstream file(path);
  if (!file.is_open()) {
    return std::string();
  }
  std::string line;
  std::getline(file, line);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  constexpr std::string_view kPrefix = ".ROBLOSECURITY=";
  if (line.size() > kPrefix.size() &&
      line.compare(0, kPrefix.size(), kPrefix) == 0) {
    return line.substr(kPrefix.size());
  }
  return line;
}

Status ResolveFollowUserPlace(int64_t user_id, const std::string& auth_cookie,
                              ResolvedFollowUserPlace* out) {
  if (out == nullptr || user_id <= 0) {
    return Invalid("Roblox presence lookup requires a positive userId");
  }
  if (auth_cookie.empty()) {
    return Invalid("Roblox presence lookup requires an authenticated session");
  }

  const net::EasyHandle curl(true);
  if (!curl) {
    return Invalid("Roblox presence lookup could not initialize a request");
  }

  const nlohmann::json body = {{"userIds", {user_id}}};
  const std::string payload = body.dump();
  std::string response;

  const std::string cookie_header = "Cookie: .ROBLOSECURITY=" + auth_cookie;
  // Reported rather than ignored, so a failed allocation cannot send the
  // .ROBLOSECURITY cookie request with no cookie attached.
  const net::HeaderList headers(
      std::vector<std::string>{"Content-Type: application/json", cookie_header});
  if (!headers.allocated()) {
    return Invalid("Roblox presence lookup could not build its request");
  }

  curl_easy_setopt(curl.get(), CURLOPT_URL,
                   "https://presence.roblox.com/v1/presence/users");
  curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  // This writer had no size bound at all, unlike the other Roblox API call
  // sites; the shared sink caps it at the same ceiling they use and checks
  // size * count for overflow.
  net::BoundedStringSink writer{&response, kMaximumResponseBytes, false};
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, net::WriteBoundedString);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &writer);
  net::TransportOptions options;
  options.timeout_ms = 10000L;
  options.follow_redirects = false;
  net::ApplyTransportOptions(curl.get(), options);

  const CURLcode result = curl_easy_perform(curl.get());
  long http_status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);

  if (result != CURLE_OK) {
    return Invalid("Roblox presence lookup failed to connect");
  }
  if (writer.exceeded) {
    return Invalid("Roblox presence lookup response exceeded its size limit");
  }
  if (http_status != 200) {
    return Invalid("Roblox presence lookup was rejected (status " +
                   std::to_string(http_status) + ")");
  }

  const nlohmann::json parsed = nlohmann::json::parse(
      response, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return Invalid("Roblox presence lookup returned malformed JSON");
  }

  if (!parsed.contains("userPresences") ||
      !parsed["userPresences"].is_array() ||
      parsed["userPresences"].empty()) {
    return Invalid("Roblox presence lookup returned no data for user");
  }

  const auto& presence = parsed["userPresences"][0];
  // userPresenceType: 0 offline, 1 online, 2 in-game, 3 in-studio.
  const int presence_type = presence.value("userPresenceType", 0);
  if (presence_type != 2) {
    return Invalid("User is not currently in a joinable Roblox experience");
  }
  if (!presence.contains("placeId") || presence["placeId"].is_null()) {
    return Invalid("Roblox presence lookup did not include a placeId");
  }

  out->place_id = presence.value("placeId", int64_t{0});
  if (presence.contains("gameId") && !presence["gameId"].is_null()) {
    out->game_instance_id = presence.value("gameId", std::string());
  }
  if (out->place_id <= 0) {
    return Invalid("Roblox presence lookup returned an invalid placeId");
  }
  return Status::Ok();
}

}  // namespace runtime
}  // namespace mocktail
